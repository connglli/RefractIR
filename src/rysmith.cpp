/**
 * rysmith — C++ random RefractIR leaf-function generator.
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/build.hpp"
#include "ast/clone.hpp"
#include "ast/sir_printer.hpp"
#include "backend/emit.hpp"
#include "backend/py_vec_lowering.hpp"
#include "backend/wasm_vec_lowering.hpp"
#include "cxxopts.hpp"
#include "error.hpp"
#include "frontend/diagnostics.hpp"
#include "frontend/pipeline.hpp"
#include "interp/interpreter.hpp"
#include "reify/cfg_gen.hpp"
#include "reify/checksum.hpp"
#include "reify/common.hpp"
#include "reify/func_desc.hpp"
#include "reify/func_gen.hpp"
#include "reify/hyperparameters.hpp"
#include "reify/id_gen.hpp"
#include "reify/path_sampler.hpp"
#include "reify/state_profile.hpp"
#include "reify/var_catalogue.hpp"
#include "solver/solved_header.hpp"
#include "solver/solver.hpp"
#if defined(USE_BITWUZLA)
#include "solver/bitwuzla_impl.hpp"
#elif defined(USE_ALIVESMT)
#include "solver/alive_impl.hpp"
#endif

namespace fs = std::filesystem;
using namespace refractir;
using namespace refractir::reify;

// Parse "[lo, hi]" domain string
[[nodiscard]] static std::pair<std::int64_t, std::int64_t> parseDomain(const std::string &s) {
  if (s.size() < 5 || s.front() != '[' || s.back() != ']')
    throw std::invalid_argument("invalid domain (expected [lo, hi]): " + s);
  auto inner = s.substr(1, s.size() - 2);
  auto comma = inner.find(',');
  if (comma == std::string::npos)
    throw std::invalid_argument("invalid domain (missing comma): " + s);
  auto loStr = inner.substr(0, comma);
  auto hiStr = inner.substr(comma + 1);
  while (!loStr.empty() && std::isspace((unsigned char) loStr.back()))
    loStr.pop_back();
  while (!hiStr.empty() && std::isspace((unsigned char) hiStr.front()))
    hiStr = hiStr.substr(1);
  return {std::stoll(loStr), std::stoll(hiStr)};
}

// Try budget per extra example. A try that duplicates an earlier input or
// trips UB is rejected by the interpreter replay; a budget that never lands
// drops the example.
static constexpr int kExtraExampleTries = 3;

[[nodiscard]] static auto makeSolverFactory() {
  return [](const SymbolicExecutor::Config &cfg) -> std::unique_ptr<smt::ISolver> {
#if defined(USE_BITWUZLA)
    return std::make_unique<solver::BitwuzlaSolver>(cfg.timeout_ms, cfg.seed, cfg.num_smt_threads);
#elif defined(USE_ALIVESMT)
    return std::make_unique<solver::AliveSolver>(cfg.timeout_ms, cfg.seed, cfg.num_smt_threads);
#else
    (void) cfg;
    throw std::runtime_error("No solver backend compiled in");
#endif
  };
}

[[nodiscard]] static bool validateWithSymiri(
    const fs::path &sirPath, const std::string &funcName, const std::vector<std::string> &paramArgs,
    bool verbose, SolvingMode solMode = SolvingMode::UBFree
) {
  std::ifstream ifs(sirPath);
  if (!ifs)
    return false;
  std::stringstream ss;
  ss << ifs.rdbuf();
  std::string src = ss.str();
  try {
    Program prog = parseSource(src);

    if (!runAnalysisPasses(prog, /*verbose=*/false))
      return false;

    std::string canonical = funcName.empty() || funcName[0] == '@' ? funcName : "@" + funcName;

    // Send the interpreter's `Result:` line to a local sink unless verbose,
    // instead of redirecting the process-global std::cout (unsafe with
    // concurrent worker threads).
    std::stringstream sink;
    std::ostream &out = verbose ? std::cout : sink;

    try {
      Interpreter interp(prog, out);
      interp.run(canonical, {}, paramArgs);
      // Success (no exception) is correct only if we didn't require UB
      return (solMode != SolvingMode::RequireUB);
    } catch (const UndefinedBehaviorError &e) {
      if (verbose) {
        std::cout << "  (intercepted expected UB: " << e.what() << ")\n";
      }
      // UB exception is correct only if we required UB
      return (solMode == SolvingMode::RequireUB);
    } catch (...) {
      return false;
    }
  } catch (...) {
    return false;
  }
}

// One per concretized .sir file: the on-disk path, the certified examples
// (parameter values in declaration order plus the return each input produces)
// and the body's sym model values, so consumers (--validate, --emit-desc) read
// these instead of re-parsing the SOLVED header. `examples[0]` is the
// concretized solve; extras exist only under `--n-examples > 1`.
struct ConcreteFile {
  fs::path path;
  std::vector<FuncDescriptor::Realization::Example> examples;
  // Sym values of the body's model, in declaration order; one body
  // substitution serves every example.
  std::vector<std::pair<std::string, std::string>> symValues;
  // Lasso header (^-prefixed) for --require-nonterm, so the
  // bounded-replay divergence validation knows which block's state must recur,
  // and after how many laps (the orbit's period).
  std::string nontermHeader;
  int nontermPeriod = 1;
};

struct GenerateResult {
  std::vector<ConcreteFile> produced;
};

// Strip the trailing concrete-letter suffix (`a`..`z`) from a .sir stem
// to recover the base function name. Stems are either `func_<id>_<i>`
// (single concrete) or `func_<id>_<i><a..z>` (multi-concrete); drop one
// trailing lowercase letter when the char before it is a digit.
[[nodiscard]] static std::string getBaseFuncName(const fs::path &p) {
  std::string stem = p.stem().string();
  if (stem.size() >= 2) {
    char last = stem.back();
    char prev = stem[stem.size() - 2];
    if (last >= 'a' && last <= 'z' && std::isdigit(static_cast<unsigned char>(prev)))
      return stem.substr(0, stem.size() - 1);
  }
  return stem;
}

// Flatten a declaration-order (name, value) pair list into the bare value
// list that `symiri --` expects.
[[nodiscard]] static std::vector<std::string>
extractParamArgs(const std::vector<std::pair<std::string, std::string>> &paramValues) {
  std::vector<std::string> args;
  args.reserve(paramValues.size());
  for (const auto &pv: paramValues)
    args.push_back(pv.second);
  return args;
}

// One solve's parameter values over the entry's parameter list, in
// declaration order; a param the model lacks gets "0" so the list keeps
// every declared position. This is the one shape the SOLVED banner, the
// arming pins, and the symiri positional args all share.
[[nodiscard]] static std::vector<std::pair<std::string, std::string>> orderedParamValues(
    const FunDecl &entry,
    const std::unordered_map<std::string, SymbolicExecutor::Result::ModelVal> &paramModel
) {
  std::vector<std::pair<std::string, std::string>> pvs;
  pvs.reserve(entry.params.size());
  for (const auto &p: entry.params) {
    auto it = paramModel.find(p.name.name);
    pvs.emplace_back(p.name.name, it != paramModel.end() ? formatModelValue(it->second) : "0");
  }
  return pvs;
}

// Run the minimal CRC32 oracle for one set of exit-time values: build it
// from the rewritten prog, write it beside the output as a temp file, and
// capture its Result line under symiri on `paramArgs`. nullopt on any
// failure; the temp file is removed best-effort either way.
[[nodiscard]] static std::optional<std::string> runCrc32Oracle(
    const Program &prog, const std::string &funcName, const fs::path &outDir,
    const std::string &tempName,
    const std::unordered_map<std::string, SymbolicExecutor::LetExitValue> &letExitValues,
    const std::vector<std::string> &paramArgs
) {
  Program miniProg = buildMiniCrc32Prog(prog, funcName, letExitValues);
  auto tempPath = outDir / tempName;
  {
    std::ofstream tofs(tempPath);
    if (tofs) {
      SIRPrinter printer(tofs);
      printer.print(miniProg);
    }
  }
  auto captured = runSymiriCaptureResult(tempPath, "minimal_" + funcName, paramArgs);
  std::error_code ec;
  fs::remove(tempPath, ec); // best-effort; safe to leave on disk
  return captured;
}

// Everything a leaf-generation attempt reads that is fixed for the whole run.
// The three inputs that differ per leaf — its name, its variable-catalogue
// config (which carries the function index) and its RNG — stay parameters of
// generateLeaf.
struct LeafGenConfig {
  // CFG shape.
  int nBbls = 0;
  double pBranch = 0.0;
  double pBackedge = 0.0;
  bool requireReducible = false;

  // Path sampling.
  int maxLoopIter = 0;
  int minLoopIter = 0;

  // Function body.
  int nStmts = 0;
  double offPathMultiplier = 1.0;
  bool enableInterestCoefs = false;
  double pLargeCoef = 0.0;
  std::int64_t largeCoefThreshold = 0;
  std::int64_t coefLo = 0, coefHi = 0;
  std::int64_t valueLo = 0, valueHi = 0;
  std::int64_t indexLo = 0, indexHi = 0;
  ExprGenConfig exprCfg;
  bool enableIntrinsics = false;

  // Solving.
  std::uint32_t timeoutMs = 0;
  SolvingMode solMode = SolvingMode::UBFree;

  // Retry budget: `maxRetries` attempts per leaf, each solving `nConcretes`
  // independent concretizations.
  int maxRetries = 0;
  int nConcretes = 0;

  // Examples per concretized .sir. Example 1 is the concretized solve; extras
  // re-solve the template with fresh seeds and derive their output from the
  // re-solve's own exit-time model, so each is a distinct certified input.
  // UB-free mode only.
  int nExamples = 1;

  // Output. `genId` is the run's 6-char generation id, which names the
  // per-function descriptor; `emitDesc` writes the rylink-consumable
  // func_<id>_<i>.json sidecar beside each concrete .sir.
  fs::path outDir;
  bool keepSymbolic = false;
  bool verbose = false;
  bool emitDesc = false;
  bool emitMain = false;
  bool noCrc32 = false;
  std::string genId;

  // Non-terminating mode: sample a lasso instead of an entry-to-exit path and
  // splice cycle-closing corrections before solving. `maxLassoPeriod` caps the
  // orbit's period k; each attempt draws k from [1, maxLassoPeriod], so a run
  // mixes single-lap and multi-lap orbits.
  bool requireNonterm = false;
  int maxLassoPeriod = 1;
};

// `rng` is taken by value so the call can run in a detached thread.
// S1 of the pipeline: the leaf's control-flow graph. Non-terminating
// generation needs at least one loop, so it biases toward back edges and
// regenerates until the repaired CFG admits a lasso — bounded, because the
// attempt loop downstream fails cleanly when no lasso is ever found.
[[nodiscard]] static RyCFG buildLeafCFG(const LeafGenConfig &opts, std::mt19937 &rng) {
  GenCFGParams cfgParams;
  cfgParams.nBbls = opts.nBbls;
  cfgParams.pBranch = opts.pBranch;
  cfgParams.pBackedge = opts.requireNonterm ? std::max(opts.pBackedge, 0.5) : opts.pBackedge;
  cfgParams.requireReducible = opts.requireReducible;
  RyCFG cfg;
  for (int cfgTry = 0;; cfgTry++) {
    cfgParams.seed = rng();
    cfg = genCFG(cfgParams);
    if (!opts.requireNonterm)
      break;
    SampleLassoParams probe;
    probe.seed = rng();
    if (sampleLasso(cfg, probe).has_value() || cfgTry >= 20)
      break;
  }
  return cfg;
}

// S2: the execution path this attempt will concretize — a lasso for
// non-terminating generation, otherwise a random entry-to-exit walk whose
// loop-iteration ceiling decays on each retry. Returns nullopt when the
// sampler cannot produce one.
[[nodiscard]] static std::optional<std::vector<std::string>>
sampleAttemptPath(const LeafGenConfig &opts, const RyCFG &cfg, std::mt19937 &rng, int attempt) {
  if (opts.requireNonterm) {
    SampleLassoParams lassoParams;
    lassoParams.seed = rng();
    // Draw the period per attempt: a k > 1 orbit is a strictly harder
    // constraint (the state must avoid the header state for k-1 laps), so
    // retrying re-rolls it rather than being stuck on one hard k.
    lassoParams.period =
        opts.maxLassoPeriod <= 1
            ? 1
            : static_cast<int>(std::uniform_int_distribution<int>(1, opts.maxLassoPeriod)(rng));
    return sampleLasso(cfg, lassoParams);
  }
  SamplePathParams pathParams;
  pathParams.seed = rng();
  // Keep max >= min so retry decay can't violate the requested minimum.
  pathParams.maxLoopIter = std::max(opts.minLoopIter, opts.maxLoopIter - attempt);
  pathParams.minLoopIter = opts.minLoopIter;
  return samplePath(cfg, pathParams);
}

// The `// CFG:` / `// PATH:` banner every emitted .sir carries. Fixed for the
// whole attempt: only the per-concretization sym and parameter solutions
// differ.
static void writePathHeader(
    std::ostream &os, const RyCFG &cfg, const std::vector<std::string> &path, bool requireNonterm
) {
  os << "// CFG:\n";
  for (const auto &b: cfg.blocks) {
    os << "//   " << b.label;
    if (!b.succs.empty()) {
      os << " ->";
      for (const auto &succ: b.succs)
        os << " " << succ;
    }
    os << "\n";
  }
  os << (requireNonterm ? "// LASSO:" : "// PATH:");
  for (std::size_t k = 0; k < path.size(); k++)
    os << (k == 0 ? " " : " -> ") << path[k];
  os << "\n\n";
}

// One candidate input for an extra example: the re-solve's params in
// declaration order plus its own exit-time model and sum-form ret, the
// independent source the example's return value is derived from below.
struct ExtraCandidate {
  std::vector<std::pair<std::string, std::string>> params;
  std::unordered_map<std::string, SymbolicExecutor::LetExitValue> letExitValues;
  std::optional<SymbolicExecutor::Result::ModelVal> retModel;
};

// Find the generated entry function by name, or nullptr when absent.
[[nodiscard]] static FunDecl *findEntryFun(Program &prog, const std::string &funcName) {
  const std::string want = "@" + funcName;
  for (auto &f: prog.funs)
    if (f.name.name == want)
      return &f;
  return nullptr;
}

// Arm a trial clone with two splices into the entry block: one EQ require per
// solved sym pinning it to the value the emitted program embeds, so any SAT
// model of the armed clone matches the one body by construction, and one
// exclusion require per collected input, `%paK != v` on a single parameter
// coordinate, so the solve moves past every input gathered so far. The
// coordinate rotates with `tryIdx` so a param pinned by the branch conditions
// cannot starve the try budget.
static void armTrialClone(
    Program &trial, const std::string &funcName, const FunDecl &entry,
    const std::unordered_map<std::string, SymbolicExecutor::Result::ModelVal> &model,
    const std::vector<std::pair<std::string, std::string>> &primary,
    const std::vector<ExtraCandidate> &extras, int tryIdx
) {
  FunDecl *fun = findEntryFun(trial, funcName);
  if (!fun || fun->blocks.empty())
    return;

  auto litExpr = [](bool isFloat, const std::string &text) -> Expr {
    return isFloat ? buildExpr(buildCoefAtom(Coef{FloatLit{parseFloatLiteral(text), {}}}))
                   : buildExpr(buildCoefAtom(Coef{IntLit{parseIntegerLiteral(text), {}}}));
  };

  // Pin every solved sym to the value the emitted program embeds.
  for (const auto &s: entry.syms) {
    auto it = model.find(s.name.name);
    if (it == model.end())
      continue;
    const bool isFloat = s.type && std::holds_alternative<FloatType>(s.type->v);
    RequireInstr req;
    req.cond.lhs = buildExpr(buildCoefAtom(Coef{LocalOrSymId{SymId{s.name.name, {}}}}));
    req.cond.op = RelOp::EQ;
    req.cond.rhs = litExpr(isFloat, formatModelValue(it->second));
    req.message = "pinned sym";
    fun->blocks.front().instrs.emplace_back(std::move(req));
  }

  auto splice = [&](const std::vector<std::pair<std::string, std::string>> &pv, std::size_t coord) {
    // No parameters: no coordinate to exclude, and no modulo.
    if (fun->params.empty() || pv.size() < fun->params.size())
      return;
    const std::size_t c = coord % fun->params.size();
    const ParamDecl &pd = fun->params[c];
    RequireInstr req;
    req.cond.lhs = buildExpr(buildCoefAtom(Coef{LocalOrSymId{LocalId{pd.name.name, {}}}}));
    req.cond.op = RelOp::NE;
    const bool isFloat = pd.type && std::holds_alternative<FloatType>(pd.type->v);
    req.cond.rhs = litExpr(isFloat, pv[c].second);
    req.message = "distinct input";
    fun->blocks.front().instrs.emplace_back(std::move(req));
  };

  for (std::size_t e = 0; e < extras.size(); ++e)
    splice(extras[e].params, static_cast<std::size_t>(tryIdx) + e + 1);
  splice(primary, static_cast<std::size_t>(tryIdx));
}

// One FuncGenConfig per concretization: the run-wide knobs plus a fresh seed.
// The seed draw stays at the call site (`rng()`) so RNG consumption order is
// visible where the loop reads.
[[nodiscard]] static FuncGenConfig
makeFuncGenConfig(const LeafGenConfig &opts, const std::string &funcName, std::uint32_t seed) {
  FuncGenConfig fcfg;
  fcfg.funcName = funcName;
  fcfg.seed = seed;
  fcfg.nStmts = opts.nStmts;
  fcfg.offPathMultiplier = opts.offPathMultiplier;
  fcfg.enableInterestCoefs = opts.enableInterestCoefs;
  fcfg.enableInterestInits = true;
  fcfg.enableIntrinsics = opts.enableIntrinsics;
  fcfg.pLargeCoef = opts.pLargeCoef;
  fcfg.largeCoefThreshold = opts.largeCoefThreshold;
  fcfg.exprCfg = opts.exprCfg;
  fcfg.coefLo = opts.coefLo;
  fcfg.coefHi = opts.coefHi;
  fcfg.valueLo = opts.valueLo;
  fcfg.valueHi = opts.valueHi;
  fcfg.indexLo = opts.indexLo;
  fcfg.indexHi = opts.indexHi;
  return fcfg;
}

// Cycle-closing corrections for --require-nonterm, spliced into the lasso's
// latch so the header-state fixed point is solvable. The cycle spans the
// header (path.back()'s first occurrence) to the end; the latch is the block
// just before the terminating header revisit.
static void spliceLassoCorrections(
    Program &prog, const std::string &funcName, const std::vector<std::string> &path
) {
  const std::string &header = path.back();
  std::size_t firstHeaderIdx = 0;
  for (std::size_t j = 0; j < path.size(); ++j)
    if (path[j] == header) {
      firstHeaderIdx = j;
      break;
    }
  std::vector<std::string> cycleLabels;
  std::unordered_set<std::string> seenCycle;
  for (std::size_t j = firstHeaderIdx; j < path.size(); ++j)
    if (seenCycle.insert(path[j]).second)
      cycleLabels.push_back("^" + path[j]);
  std::string latchLabel = "^" + path[path.size() - 2];
  // The orbit's period is read off the path, exactly as the solver
  // reads it: k+1 arrivals at the header means k laps.
  int lassoPeriod = static_cast<int>(std::count(path.begin(), path.end(), header)) - 1;
  spliceNontermCorrections(prog, funcName, cycleLabels, latchLabel, lassoPeriod);
}

// The primary solve's results that extra-candidate solves build on: the
// pre-rewrite template program, its entry function, the sym model the emitted
// body embeds, and the primary input in declaration order.
struct SolvedTemplate {
  const Program &prog;
  const FunDecl &entry;
  const std::unordered_map<std::string, SymbolicExecutor::Result::ModelVal> &symModel;
  const std::vector<std::pair<std::string, std::string>> &primaryExample;
};

// Extra input candidates for `--n-examples > 1`: each comes from solving a
// distinctness-armed clone of the template (armTrialClone). This runs before
// the checksum rewrite, which turns `@crc32_update` into a non-intrinsic a
// re-solve cannot answer. UNSAT or a failed try budget drops the example: the
// caller simply gets fewer candidates.
[[nodiscard]] static std::vector<ExtraCandidate> solveExtraCandidates(
    const LeafGenConfig &opts, const std::string &funcName, int concreteIdx,
    const SolvedTemplate &solved, const SymbolicExecutor::Config &solverCfg,
    const std::vector<std::string> &pathLabels
) {
  std::vector<ExtraCandidate> extraCandidates;
  // The primary solve's seed: re-seed each (example, try) slot away from it,
  // since the same seed would re-answer the primary model instead of
  // proposing another input.
  const std::uint32_t slotSeed = solverCfg.seed;
  for (int j = 1; j < opts.nExamples; ++j) {
    for (int eTry = 0; eTry < kExtraExampleTries; ++eTry) {
      Program trialProg = cloneProgram(solved.prog);
      armTrialClone(
          trialProg, funcName, solved.entry, solved.symModel, solved.primaryExample,
          extraCandidates, eTry
      );
      SymbolicExecutor::Config extraCfg = solverCfg;
      // Distinct per (example, try). Mixed in 64 bits and folded down: a
      // 32-bit multiply wraps for large master seeds and an even stride is
      // not injective, so distinct slots could draw identical seeds.
      const std::uint64_t mixed =
          static_cast<std::uint64_t>(slotSeed) * (static_cast<std::uint64_t>(opts.nExamples) + 1) +
          (static_cast<std::uint64_t>(j) - 1) * kExtraExampleTries +
          static_cast<std::uint64_t>(eTry);
      extraCfg.seed = static_cast<std::uint32_t>(mixed ^ (mixed >> 32));
      SymbolicExecutor extraExec(trialProg, extraCfg, makeSolverFactory());
      SymbolicExecutor::Result extraRes;
      try {
        extraRes = extraExec.solve("@" + funcName, pathLabels);
      } catch (...) {
        if (opts.verbose)
          std::cerr << "[solver] concrete " << concreteIdx << " example " << j << ": exception\n";
        continue;
      }
      if (!extraRes.sat) {
        if (opts.verbose)
          std::cerr << "[solver] concrete " << concreteIdx << " example " << j << ": "
                    << (extraRes.unsat ? "UNSAT" : "UNKNOWN") << "\n";
        continue;
      }
      auto pvs = orderedParamValues(solved.entry, extraRes.paramModel);
      // Duplicates re-roll: rysmith wants distinct examples.
      if (pvs == solved.primaryExample)
        continue;
      bool dup = false;
      for (const auto &seen: extraCandidates)
        if (seen.params == pvs) {
          dup = true;
          break;
        }
      if (dup)
        continue;
      extraCandidates.push_back(
          {std::move(pvs), std::move(extraRes.letExitValues), std::move(extraRes.retModel)}
      );
      break;
    }
  }
  return extraCandidates;
}

// What the checksum rewrite plus the primary oracle capture produce.
struct PrimaryCapture {
  bool rewriteApplied = false;
  std::string expectedRet;
};

// Apply the checksum rewrite while we still hold the in-memory prog. The
// solver only saw the sum-based `%_chk = %_chk + ...` contract; rewriting now
// means every downstream consumer (SIRPrinter, symiri, symirc, the C / WASM
// backends) sees the opaque CRC32 form instead. The model from the solver
// (sym → value bindings) carries over unchanged because no symbols are
// introduced or removed by the rewrite. The pre-rewrite `res.retModel` (the
// solver's sum) is now stale and is dropped so the post-rewrite symiri value
// can take its place below.
//
// Capture failure (symiri exits non-zero or returns no parseable Result line)
// typically means the generated function tripped UB that the solver missed:
// nullopt, and the caller drops the concretization and tries the next one.
[[nodiscard]] static std::optional<PrimaryCapture> rewriteAndCapturePrimary(
    const LeafGenConfig &opts, Program &prog, const std::string &funcName, const FunDecl *entry,
    SymbolicExecutor::Result &res,
    const std::vector<std::pair<std::string, std::string>> &paramValues, const std::string &outName,
    const fs::path &concretePath, int concreteIdx, std::mt19937 &rng
) {
  std::size_t crcUpdates = 0;
  if (!opts.noCrc32) {
    crcUpdates = rewriteExitToCrc32Checksum(prog, funcName, res.letExitValues);
  }
  bool rewriteApplied = crcUpdates > 0;
  if (rewriteApplied)
    res.retModel.reset();

  // Capture the post-rewrite CRC32 return value via a MINIMAL oracle program.
  // The oracle:
  //   - shares struct decls and the @crc32_update intrinsic with
  //     the full program;
  //   - declares the same lets + params as the entry function,
  //     but each scalar / aggregate let-init is rewritten to its
  //     solver-known exit-time value (LetExitValue);
  //   - replays each pointer let's EXIT-time target via
  //     `%p = addr <targetLocal>;` (the solver's prov_base
  //     reverse-FNV'd back to a local name) so body-side
  //     pointer retargets — e.g. `%p0 = load %pp1;` — are
  //     captured;
  //   - then runs the verbatim exit block (load preamble +
  //     CRC32 chain + ret).
  // Driving the oracle from a SEPARATE program is intentional:
  // it makes `--validate` a real cross-check that the solver's
  // exit-time model + the minimal program == the interpreter's
  // execution of the full program. If we instead captured from
  // the full program itself, validate would compare X to X.
  std::string crcRetValue;
  if (rewriteApplied && entry) {
    auto captured = runCrc32Oracle(
        prog, funcName, opts.outDir, outName + ".oracle.tmp", res.letExitValues,
        extractParamArgs(paramValues)
    );
    if (!captured) {
      if (opts.verbose) {
        std::cerr << "[oracle] concrete " << concreteIdx
                  << ": symiri capture failed for the minimal "
                     "checksum oracle of "
                  << concretePath << "; skipping concretization\n";
      }
      return std::nullopt;
    }
    crcRetValue = std::move(*captured);
  }

  std::string expectedRet = !crcRetValue.empty()
                                ? crcRetValue
                                : (res.retModel.has_value() ? formatModelValue(*res.retModel) : "");

  // A diverging program has no captured return value, but --emit-main
  // still needs an expected checksum for @check_chksum. The entry call
  // never returns, so the check is unreachable at runtime and any
  // literal is sound; use a random i32 so the anchor value varies.
  if (opts.requireNonterm && opts.emitMain && expectedRet.empty())
    expectedRet = std::to_string(static_cast<std::int32_t>(rng()));

  return PrimaryCapture{rewriteApplied, std::move(expectedRet)};
}

// Each extra example's return value is derived from its candidate's own
// exit-time model through the same minimal-oracle recipe as the primary's: an
// independent source, so the validate-time run of the full program is a real
// cross-check for every example. Capture failure drops the example.
static void captureExtraReturns(
    const LeafGenConfig &opts, const Program &prog, const std::string &funcName, int concreteIdx,
    bool rewriteApplied, const std::string &outName, std::vector<ExtraCandidate> &candidates,
    std::vector<FuncDescriptor::Realization::Example> &examples
) {
  for (auto &cand: candidates) {
    std::string extraRet;
    if (rewriteApplied) {
      auto captured = runCrc32Oracle(
          prog, funcName, opts.outDir, outName + ".oracle.tmp", cand.letExitValues,
          extractParamArgs(cand.params)
      );
      if (!captured) {
        if (opts.verbose)
          std::cerr << "[examples] concrete " << concreteIdx << ": oracle rejected example "
                    << examples.size() << "\n";
        continue;
      }
      extraRet = std::move(*captured);
    } else {
      // No rewrite: the re-solve's sum model is the expectation, the
      // same source the primary uses here.
      extraRet = cand.retModel.has_value() ? formatModelValue(*cand.retModel) : "";
    }
    examples.emplace_back(
        FuncDescriptor::Realization::Example{std::move(cand.params), std::move(extraRet)}
    );
  }
}

// Replay every example in @main (one entry call plus @check_chksum each),
// then write the concrete .sir: one SOLVED line per example, the path header,
// and the printed program with syms substituted. False when the file cannot
// be opened. The @main push_back reallocates `prog.funs`, so the caller must
// treat any pointer into it as dangling afterward and read only snapshots.
[[nodiscard]] static bool writeConcreteSir(
    const LeafGenConfig &opts, Program &prog, const FunDecl *entry,
    const std::vector<FuncDescriptor::Realization::Example> &examples,
    const std::unordered_map<std::string, SymbolicExecutor::Result::ModelVal> &symModel,
    const fs::path &concretePath, const std::string &pathHeader
) {
  if (opts.emitMain && entry) {
    std::vector<MainExample> mainExamples;
    mainExamples.reserve(examples.size());
    for (const auto &ex: examples)
      mainExamples.emplace_back(extractParamArgs(ex.paramValues), ex.retValue);
    FunDecl mainFn = buildMainFunction(prog, *entry, mainExamples);
    prog.funs.push_back(std::move(mainFn));
  }

  std::ofstream ofs(concretePath);
  if (!ofs) {
    std::cerr << "error: cannot open " << concretePath << "\n";
    return false;
  }
  // One SOLVED line per example, so symiri can replay any of them
  // via `--main @f <file> -- <p0> <p1>`.
  for (const auto &ex: examples)
    writeSolvedHeader(ofs, ex.paramValues, ex.retValue);
  ofs << pathHeader;
  SIRPrinter printer(ofs, symModel);
  printer.print(prog);
  return true;
}

// Rewrite the per-function descriptor from every concretization produced so
// far. Runs after each new concrete file, so the final write wins.
static void refreshDescriptor(
    const LeafGenConfig &opts, const std::string &funcName, const Program &prog,
    const std::vector<std::string> &pathLabels, const std::vector<ConcreteFile> &produced
) {
  std::vector<FuncDescriptor::Realization> realizations;
  realizations.reserve(produced.size());
  for (const auto &x: produced) {
    FuncDescriptor::Realization z;
    z.file = x.path.filename().string();
    if (!x.examples.empty()) {
      z.paramValues = x.examples.front().paramValues;
      z.retValue = x.examples.front().retValue;
      z.extraExamples.assign(x.examples.begin() + 1, x.examples.end());
    }
    z.symValues = x.symValues;
    realizations.push_back(std::move(z));
  }
  auto descPath = opts.outDir / (funcName + ".json");
  FuncDescriptor::Outcome outcome =
      opts.solMode == SolvingMode::RequireUB        ? FuncDescriptor::Outcome::Trap
      : opts.solMode == SolvingMode::RequireNonterm ? FuncDescriptor::Outcome::Diverge
                                                    : FuncDescriptor::Outcome::Return;
  writeFuncDescriptorFromProgram(
      descPath, funcName, prog, pathLabels, realizations, opts.genId, outcome
  );
}

[[nodiscard]] static GenerateResult generateLeaf(
    const LeafGenConfig &opts, const VarGenConfig &varCfg, const std::string &funcName,
    std::mt19937 rng, std::uint32_t baseSeed
) {
  RyCFG cfg = buildLeafCFG(opts, rng);

  if (opts.verbose)
    std::cout << "[cfg] " << cfg.blocks.size() << " blocks\n";

  // Generate VarCatalogue (shared across all concretizations for the same CFG)
  VarCatalogue vars = genVarCatalogue(rng, varCfg);

  if (opts.verbose)
    std::cout << "[vars] " << vars.vars.size() << " vars, " << vars.structDecls.size()
              << " structs\n";

  for (int attempt = 0; attempt <= opts.maxRetries; attempt++) {
    std::optional<std::vector<std::string>> maybePath = sampleAttemptPath(opts, cfg, rng, attempt);
    if (!maybePath) {
      if (opts.verbose)
        std::cerr << "[sampler] attempt=" << attempt << " sample failed\n";
      continue;
    }
    const auto &path = *maybePath;

    if (opts.verbose)
      std::cout << "[sampler] attempt=" << attempt << " EP len=" << path.size() << "\n";

    // The `// CFG:` / `// PATH:` banner every emitted .sir carries, rendered
    // once per attempt. Fixed for the whole attempt: only the
    // per-concretization sym and parameter solutions differ.
    std::ostringstream pathHeader;
    writePathHeader(pathHeader, cfg, path, opts.requireNonterm);
    const std::string pathHeaderText = pathHeader.str();

    // Generate opts.nConcretes independently-seeded programs
    std::vector<ConcreteFile> produced;
    for (int concreteIdx = 0; concreteIdx < opts.nConcretes; concreteIdx++) {
      FuncGenConfig fcfg = makeFuncGenConfig(opts, funcName, rng());

      auto [prog, pathLabels] = genFunction(cfg, path, vars, fcfg);

      if (opts.requireNonterm)
        spliceLassoCorrections(prog, funcName, path);

      // Optionally dump symbolic program
      if (opts.keepSymbolic) {
        auto symPath = opts.outDir / (funcName + reify::rysmith::hp::kSymInfix +
                                      std::to_string(concreteIdx) + ".sir");
        std::ofstream ofs(symPath);
        ofs << pathHeaderText;
        SIRPrinter printer(ofs);
        printer.print(prog);
        if (opts.verbose)
          std::cout << "  symbolic: " << symPath << "\n";
      }

      // Validate the still-symbolic AST before handing it to the solver.
      // The module-level checks decide this: the per-function analyses only
      // warn here, and a symbol the generator declared but did not place is
      // an unused name they would report on every attempt.
      DiagBag diags;
      CheckOptions checkOpts;
      checkOpts.functionAnalyses = false;
      if (!checkProgram(prog, diags, checkOpts)) {
        if (opts.verbose) {
          std::cerr << "[validate] concrete " << concreteIdx
                    << ": generated program failed validation\n";
          for (const auto &d: diags.diags)
            if (d.level == DiagLevel::Error)
              std::cerr << "  error: " << d.message << "\n";
        }
        continue;
      }

      // Solve
      SymbolicExecutor::Config solverCfg;
      solverCfg.timeout_ms = opts.timeoutMs;
      solverCfg.seed = baseSeed + static_cast<std::uint32_t>(attempt * 100 + concreteIdx);
      solverCfg.num_threads = 1;
      solverCfg.num_smt_threads = 1;
      solverCfg.mode = opts.solMode;

      SymbolicExecutor executor(prog, solverCfg, makeSolverFactory());
      SymbolicExecutor::Result res;
      try {
        res = executor.solve("@" + funcName, pathLabels);
      } catch (const std::exception &e) {
        if (opts.verbose)
          std::cerr << "[solver] concrete " << concreteIdx << ": exception: " << e.what() << "\n";
        res.unknown = true;
        continue;
      } catch (...) {
        if (opts.verbose)
          std::cerr << "[solver] concrete " << concreteIdx << ": unknown exception\n";
        res.unknown = true;
        continue;
      }

      if (res.sat) {
        // Concrete suffix is a lowercase letter a..z so descriptor
        // consumers (rylink) can address a specific concretization by
        // `<funcName><letter>`. opts.nConcretes is clamped to [1, 26] at CLI
        // parse time, so concreteIdx is always in range.
        char letter = static_cast<char>('a' + concreteIdx);
        std::string outName = opts.nConcretes > 1 ? funcName + letter + ".sir" : funcName + ".sir";
        auto concretePath = opts.outDir / outName;

        // Snapshot every piece of entry metadata we'll need (params, syms)
        // into owning vectors here, BEFORE any later `prog.funs` mutation:
        // writeConcreteSir's optional `@main` push_back reallocates `funs`
        // and invalidates `entry`, and reading through it afterward returns
        // junk that feeds wrong CLI args to the validate-time symiri. The
        // checksum rewrite touches only the exit block, so `entry` stays
        // valid across it and the extra-candidate solves.
        const FunDecl *entry = findEntryFun(prog, funcName);
        std::vector<std::pair<std::string, std::string>> paramValuesCaptured;
        std::vector<std::pair<std::string, std::string>> symValuesCaptured;
        if (entry) {
          paramValuesCaptured = orderedParamValues(*entry, res.paramModel);
          for (const auto &s: entry->syms) {
            auto it = res.model.find(s.name.name);
            if (it != res.model.end())
              symValuesCaptured.emplace_back(s.name.name, formatModelValue(it->second));
          }
        }

        // The concretized primary example, in file order: more examples
        // are captured below and appended behind it.
        std::vector<std::pair<std::string, std::string>> primaryExample(
            paramValuesCaptured.begin(), paramValuesCaptured.end()
        );
        std::vector<ExtraCandidate> extraCandidates;
        if (opts.nExamples > 1 && entry) {
          const SolvedTemplate solved{prog, *entry, res.model, primaryExample};
          extraCandidates =
              solveExtraCandidates(opts, funcName, concreteIdx, solved, solverCfg, pathLabels);
        }

        auto primary = rewriteAndCapturePrimary(
            opts, prog, funcName, entry, res, paramValuesCaptured, outName, concretePath,
            concreteIdx, rng
        );
        if (!primary)
          continue;

        std::vector<FuncDescriptor::Realization::Example> examples;
        examples.emplace_back(
            FuncDescriptor::Realization::Example{
                std::move(paramValuesCaptured), std::move(primary->expectedRet)
            }
        );
        captureExtraReturns(
            opts, prog, funcName, concreteIdx, primary->rewriteApplied, outName, extraCandidates,
            examples
        );

        if (!writeConcreteSir(opts, prog, entry, examples, res.model, concretePath, pathHeaderText))
          continue;

        // `examples[0]` is the concretized solve; extras are oracle-certified.
        // Only snapshots are read below: the @main push_back inside
        // writeConcreteSir may have reallocated `prog.funs`.
        ConcreteFile cf;
        cf.path = concretePath;
        cf.examples = std::move(examples);
        cf.symValues = std::move(symValuesCaptured);
        // The lasso header is the path's final block; the bounded-replay
        // validation asserts its state recurs after the orbit's k laps, which
        // the path spells out as k+1 arrivals at that block.
        if (opts.requireNonterm) {
          cf.nontermHeader = pathLabels.back();
          cf.nontermPeriod =
              static_cast<int>(std::count(pathLabels.begin(), pathLabels.end(), cf.nontermHeader)) -
              1;
        }
        produced.push_back(std::move(cf));
        if (opts.emitDesc)
          refreshDescriptor(opts, funcName, prog, pathLabels, produced);
        if (opts.verbose)
          std::cout << "[emit] concrete " << concreteIdx << ": " << concretePath << "\n";
      } else if (opts.verbose) {
        std::cerr << "[solver] concrete " << concreteIdx << ": "
                  << (res.unsat ? "UNSAT" : "UNKNOWN") << "\n";
      }
    }

    if (!produced.empty())
      return GenerateResult{std::move(produced)};

    if (opts.verbose)
      std::cerr << "[solver] attempt=" << attempt << ": all concretizations failed, retrying\n";
  }

  return GenerateResult{};
}

// The command-line surface. Defined apart from main so the option table
// reads as one list rather than as the first ninety lines of the driver.
[[nodiscard]] static cxxopts::Options makeOptions() {
  cxxopts::Options opts("rysmith", "rysmith — C++ random RefractIR leaf-function generator");

  // clang-format off
    opts.add_options()
      ("n,n-funcs",         "Number of leaf functions to generate",
                            cxxopts::value<int>()->default_value("1"))
      // Type control
      ("no-fp",             "Disable f32/f64 types entirely")
      ("no-vec",            "Disable <N> T vector type generation")
      ("no-agg-ptr",        "Disable ptr [N] T / ptr @S aggregate pointer generation")
      ("max-ptr-depth",     "Maximum pointer nesting depth (0 disables pointers)",
                            cxxopts::value<int>()->default_value("2"))
      ("max-agg-nest",      "Maximum aggregate nesting depth",
                            cxxopts::value<int>()->default_value("2"))
      ("max-agg-elems",     "Maximum array size and struct field count",
                            cxxopts::value<int>()->default_value("3"))
      // Generation
      ("n-params",          "Number of scalar parameters per generated function (default: 3)",
                            cxxopts::value<int>()->default_value("3"))
      ("n-vars",            "Variables per function",
                            cxxopts::value<int>()->default_value("10"))
      ("n-stmts",           "Statements per block on path",
                            cxxopts::value<int>()->default_value("3"))
      ("min-atoms",         "Minimum atoms per generated expression",
                            cxxopts::value<int>()->default_value("1"))
      ("max-atoms",         "Maximum atoms per generated expression",
                            cxxopts::value<int>()->default_value("3"))
      ("off-path-multiplier", "Scale --n-stmts / --min-atoms / --max-atoms by this factor in off-path blocks (never executed; solver-free)",
                            cxxopts::value<double>()->default_value("2.0"))
      // Operators
      ("no-divmod",         "Disable integer division and modulo")
      ("no-select",         "Disable select ternary expressions")
      ("no-intrinsics",     "Disable intrinsic call generation")
      ("no-ptrarith",       "Disable pointer arithmetics")
      ("no-crc32",          "Disable CRC32 replacement of the original addition-based checksum")
      // CFG
      ("n-bbls",            "Basic blocks between entry and exit per CFG",
                            cxxopts::value<int>()->default_value("15"))
      ("p-branch",          "Probability of two-successor block",
                            cxxopts::value<double>()->default_value("0.5"))
      ("p-backedge",        "Probability of back-edge",
                            cxxopts::value<double>()->default_value("0.3"))
      ("require-reducible", "Only generate reducible CFGs (irreducible back edges are repaired away)")
      // Solver
      ("timeout",           "SMT solver timeout per attempt in ms",
                            cxxopts::value<std::uint32_t>()->default_value("2000"))
      ("require-ub",        "Force at least one UB to be triggered on the chosen path")
      ("require-nonterm",   "Generate UB-free programs that diverge on the sampled input (samples a lasso; implies --require-reducible and --no-crc32)")
      ("max-lasso-period",  "Cap the lasso orbit's period k under --require-nonterm; each attempt draws k from [1, N]. k > 1 means the header state recurs only after k laps",
                            cxxopts::value<int>()->default_value("1"))
      ("coef-domain",       "Domain for coef symbols",
                            cxxopts::value<std::string>()->default_value("[-2147483647, 2147483647]"))
      ("value-domain",      "Domain for value/constant symbols",
                            cxxopts::value<std::string>()->default_value("[-2147483647, 2147483647]"))
      ("index-domain",      "Domain for index symbols",
                            cxxopts::value<std::string>()->default_value("[1, 30]"))
      // Retry/concretizations
      ("n-concretes",       "Concretizations per template (different seeds)",
                            cxxopts::value<int>()->default_value("3"))
      ("n-examples",        "Input/output examples per concrete .sir; ub-free mode only",
                            cxxopts::value<int>()->default_value("1"))
      ("max-retries",       "Retry attempts on solver failure",
                            cxxopts::value<int>()->default_value("2"))
      ("max-loop-iter",     "Max loop iterations in the execution path (EP) sample",
                            cxxopts::value<int>()->default_value("3"))
      ("min-loop-iter",     "Require at least one loop in the EP to iterate this many times",
                            cxxopts::value<int>()->default_value("0"))
      ("p-large-coef",      "Fraction of new on-path coefs forced to |c| > --large-coef",
                            cxxopts::value<double>()->default_value("0.3"))
      ("large-coef",        "Magnitude threshold T for the |c| > T interest require (clamped per-coef to --coef-domain)",
                            cxxopts::value<std::int64_t>()->default_value("1048576"))
      // Output
      ("o,output-dir",      "Output directory",
                            cxxopts::value<std::string>()->default_value("rysmith_out"))
      ("target",            "Compile concrete .sir to target (sir, c, wasm, python); sir = no compilation",
                            cxxopts::value<std::string>()->default_value("sir"))
      ("vec-lowering",      "Vec-lowering strategy for C/WASM/Python backends (random|vecext|scalars|array|structscalars|structarray)",
                            cxxopts::value<std::string>()->default_value("random"))
      ("structured-lowering", "Structured lowering for the C (goto-free) and WASM (dispatch-free) targets: true|false|random; true/random imply --require-reducible",
                            cxxopts::value<std::string>()->default_value("false"))
      ("keep-require",      "Include require checks in compiled output (default: omitted)")
      ("keep-ub-guards",    "Keep dynamic UB guards in compiled output even for UB-free programs (default: false)")
      ("keep-symbolic",     "Write intermediate symbolic .sir files to disk")
      ("emit-desc",         "Emit per-function descriptor JSON (func_<id>_<i>.json) — needed by rylink")
      ("emit-state",        "Emit a func_<id>_<i>.state.json profile of the concrete state at each program point — consumed by rytwin. Value selects granularity: pbb (per basic block) or ppp (per program point)",
                            cxxopts::value<std::string>())
      ("emit-main",         "Generate a main wrapper in the output program")
      // Validation
      ("validate",          "Run symiri on each concrete .sir to validate")
      // Misc
      ("seed",              "Master RNG seed (default: random)",
                            cxxopts::value<std::uint32_t>())
      ("v,verbose",         "Verbose output")
      ("h,help",            "Print usage");
  // clang-format on
  return opts;
}

// The run-level switches the per-leaf loop consults after a leaf is solved:
// which backend to compile to, what to strip from the emitted program, and
// whether to validate it. Everything that shapes the leaf itself lives in
// LeafGenConfig.
struct RunConfig {
  int nFuncs = 1;
  std::string target = "sir";
  std::string vecLoweringOpt;
  std::string structuredLoweringOpt;
  bool emitMain = false;
  bool noRequire = false;
  bool noUbGuards = false;
  bool validate = false;
  int funcTimeoutMs = 0;
  std::string emitStateMode;
};

// Generate every leaf of the run, counting successes and failures. Each leaf
// runs on its own thread so a wall-clock timeout can abandon one that the
// solver cannot finish, without taking the run down with it.
static void runGenerationLoop(
    const LeafGenConfig &leafCfg, const VarGenConfig &varCfg, const RunConfig &run,
    std::mt19937 &rng, int &nOk, int &nFail
) {
  for (int i = 0; i < run.nFuncs; i++) {
    std::string funcName = std::string(reify::rysmith::hp::kFuncPrefix) + "_" + leafCfg.genId +
                           "_" + std::to_string(i);
    std::uint32_t funcSeed = rng();
    std::cout << "[" << (i + 1) << "/" << run.nFuncs << "] generating " << funcName
              << " (seed=" << funcSeed << ")\n";

    // Heap-allocated state lets us safely detach the thread on timeout without
    // dangling references. Leaked on timeout — bounded by run.nFuncs, cleaned at exit.
    struct FuncState {
      std::mt19937 rng;
      GenerateResult result;
      std::atomic<bool> done{false};
    };

    auto *state = new FuncState{std::mt19937(funcSeed), {}, false};

    // Per-function copy so funcIdx makes it into struct names
    // (`@struct_<id>_<funcIdx>_<j>`). Without this every sibling fun in
    // the same rysmith run would emit `@struct_<id>_0` etc, breaking
    // rylink's bundle merge on a name vs. content mismatch.
    VarGenConfig fnVarCfg = varCfg;
    fnVarCfg.funcIdx = i;

    std::thread t([&, state]() {
      state->result = generateLeaf(leafCfg, fnVarCfg, funcName, state->rng, funcSeed);
      state->done.store(true, std::memory_order_release);
    });

    bool timedOut = false;
    if (run.funcTimeoutMs > 0) {
      auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(run.funcTimeoutMs);
      while (!state->done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
          timedOut = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }

    if (timedOut) {
      t.detach(); // state leaked; thread completes or dies with the process
      std::cerr << "[TIMEOUT] " << funcName << " exceeded " << run.funcTimeoutMs
                << "ms wall clock\n";
      nFail++;
      continue;
    }
    t.join();
    GenerateResult genRes = std::move(state->result);
    delete state;

    if (genRes.produced.empty()) {
      std::cerr << "[FAIL] all attempts failed for " << funcName << " (seed=" << funcSeed << ")\n";
      nFail++;
      continue;
    }

    for (const auto &cf: genRes.produced) {
      const fs::path &p = cf.path;
      std::cout << "  concrete: " << p << "\n";

      if (run.target != "sir") {
        std::string ext = (run.target == "c") ? ".c" : (run.target == "python") ? ".py" : ".wat";
        fs::path outPath = p.parent_path() / (p.stem().string() + ext);
        std::string vecLowering = reify::pickVecLowering(rng, run.vecLoweringOpt, run.target);
        if (leafCfg.verbose && !vecLowering.empty())
          std::cout << "  vec-lowering: " << vecLowering << "\n";
        bool structured = (run.target == "c" || run.target == "wasm") &&
                          reify::pickStructuredLowering(rng, run.structuredLoweringOpt);
        if (leafCfg.verbose && structured)
          std::cout << "  structured-lowering: true\n";
        EmitOptions emitOpts;
        emitOpts.keepRequire = !run.noRequire;
        emitOpts.noUbGuards = run.noUbGuards;
        emitOpts.vecLowering = vecLowering;
        emitOpts.structuredLowering = structured;
        emitOpts.emitMain = run.emitMain;
        emitOpts.verbose = leafCfg.verbose;
        bool ok = emitSirFile(p, run.target, outPath, emitOpts);
        if (ok)
          std::cout << "  compiled: " << outPath << "\n";
        else
          std::cerr << "  compile FAIL: " << p << "\n";
      }
    }

    // Non-terminating programs run forever, so the symiri run.validate /
    // state-profile passes below would hang. --require-nonterm instead gets a
    // dedicated bounded-replay divergence check that confirms the header state
    // recurs after one lap; --emit-state is unsupported for it.
    if (leafCfg.requireNonterm && run.validate) {
      bool allOk = true;
      for (const auto &cf: genRes.produced) {
        const fs::path &p = cf.path;
        std::string baseFuncName = getBaseFuncName(p);
        std::vector<std::string> paramArgs = extractParamArgs(cf.examples.front().paramValues);
        bool ok =
            validateNontermDiverges(p, baseFuncName, paramArgs, cf.nontermHeader, cf.nontermPeriod);
        std::cout << "  validated: " << (ok ? "OK" : "FAIL") << "(" << p.filename() << ")\n";
        if (!ok) {
          nFail++;
          allOk = false;
          break;
        }
      }
      if (allOk)
        nOk++;
    } else if ((run.validate || !run.emitStateMode.empty()) && !leafCfg.requireNonterm) {
      const bool wantProfile = !run.emitStateMode.empty();
      const StateGranularity stGran =
          run.emitStateMode == "ppp" ? StateGranularity::Ppp : StateGranularity::Pbb;
      bool allOk = true;
      for (const auto &cf: genRes.produced) {
        const fs::path &p = cf.path;
        std::string baseFuncName = getBaseFuncName(p);
        // One symiri run per example, each Result asserted against that
        // example's own oracle-derived ret value: a real cross-check of
        // the rewriter, the CFG body, and intrinsic dispatch for every
        // example. The examples[0] run carries the rytwin state profile;
        // a trap is validated via the trap alone.
        bool ok = !run.validate; // nothing to fail when only profiling
        std::string mismatchReason;
        if (leafCfg.solMode == SolvingMode::RequireUB) {
          if (run.validate) {
            ok = validateWithSymiri(
                p, baseFuncName, extractParamArgs(cf.examples.front().paramValues), leafCfg.verbose,
                leafCfg.solMode
            );
            if (!ok)
              mismatchReason = "Expected UB but program executed successfully or failed statically";
          }
        } else {
          for (std::size_t ei = 0; ei < cf.examples.size(); ++ei) {
            // No consumer for further runs: validation is off and only
            // examples[0] carries the rytwin state profile.
            if (!run.validate && (ei > 0 || !wantProfile))
              break;
            const FuncDescriptor::Realization::Example &ex = cf.examples[ei];
            std::vector<std::string> paramArgs = extractParamArgs(ex.paramValues);
            StateProfile profile;
            auto observed = runSymiriCaptureResult(
                p, baseFuncName, paramArgs, (wantProfile && ei == 0) ? &profile : nullptr, stGran
            );
            if (wantProfile && ei == 0 && observed) {
              std::ofstream sofs(leafCfg.outDir / (p.stem().string() + ".state.json"));
              if (sofs)
                writeStateProfileJson(sofs, profile);
            }
            if (!run.validate)
              continue;
            if (!observed) {
              ok = false;
              mismatchReason = "symiri produced no Result line";
              break;
            }
            if (ex.retValue.empty()) {
              // No oracle to compare against (e.g. the rewrite was
              // skipped). Fall back to the exit-code check.
              ok = validateWithSymiri(p, baseFuncName, paramArgs, leafCfg.verbose, leafCfg.solMode);
              if (!ok)
                mismatchReason = "symiri exited non-zero";
              break;
            }
            if (*observed != ex.retValue) {
              ok = false;
              mismatchReason = "expected=" + ex.retValue + " observed=" + *observed;
              break;
            }
            ok = true;
          }
        }
        if (run.validate) {
          std::cout << "  validated: " << (ok ? "OK" : "FAIL") << " (" << p.filename() << ")";
          if (!ok && !mismatchReason.empty())
            std::cout << " [" << mismatchReason << "]";
          std::cout << "\n";
          if (!ok) {
            allOk = false;
            nFail++;
            break;
          }
        }
      }
      if (!run.validate || allOk)
        nOk++;
    } else {
      nOk++;
    }
  }
}

int main(int argc, char **argv) {
  cxxopts::Options opts = makeOptions();

  cxxopts::ParseResult result;
  try {
    result = opts.parse(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n" << opts.help() << "\n";
    return 1;
  }

  if (result.count("help")) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  // ---- Parse domains -------------------------------------------------------
  std::int64_t coefLo, coefHi, valueLo, valueHi, indexLo, indexHi;
  try {
    auto [clo, chi] = parseDomain(result["coef-domain"].as<std::string>());
    auto [vlo, vhi] = parseDomain(result["value-domain"].as<std::string>());
    auto [ilo, ihi] = parseDomain(result["index-domain"].as<std::string>());
    coefLo = clo;
    coefHi = chi;
    valueLo = vlo;
    valueHi = vhi;
    indexLo = ilo;
    indexHi = ihi;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  // ---- Setup ---------------------------------------------------------------
  std::uint32_t masterSeed = result.count("seed")
                                 ? result["seed"].as<std::uint32_t>()
                                 : static_cast<std::uint32_t>(std::random_device{}());
  std::cout << "rysmith: master seed = " << masterSeed << "\n";
  std::mt19937 rng(masterSeed);

  // 6-char hex generation ID — namespaces function and struct
  // names so multiple rysmith outputs link without rename. The ID is
  // always derived from the master seed (via genHexId) so two runs with
  // the same --seed reproduce the same ID; there is no CLI override.
  std::string genId = genHexId(rng);
  std::cout << "rysmith: generation id = " << genId << "\n";

  fs::path outDir = result["output-dir"].as<std::string>();
  fs::create_directories(outDir);

  // Type config
  TypeGenConfig typeCfg;
  typeCfg.enableFp = !result.count("no-fp");
  typeCfg.enableVec = !result.count("no-vec");
  typeCfg.enableAggPtr = !result.count("no-agg-ptr");
  typeCfg.maxPtrDepth = result["max-ptr-depth"].as<int>();
  typeCfg.maxAggNesting = result["max-agg-nest"].as<int>();
  typeCfg.maxAggElems = result["max-agg-elems"].as<int>();

  // --require-nonterm: generate diverging (⇑) programs. The type
  // lattice is unrestricted — spliceNontermCorrections closes every scalar
  // leaf of a touched let (integer, floating-point, or pointer) with the same
  // additive correction, so the header fixed point stays solvable over the
  // whole state.
  bool requireNonterm = result.count("require-nonterm") > 0;
  if (requireNonterm && result.count("require-ub")) {
    std::cerr << "error: --require-nonterm and --require-ub are mutually exclusive\n";
    return 2;
  }
  int maxLassoPeriod = result["max-lasso-period"].as<int>();
  if (maxLassoPeriod < 1) {
    std::cerr << "error: --max-lasso-period must be >= 1 (got " << maxLassoPeriod << ")\n";
    return 2;
  }
  if (maxLassoPeriod > 1 && !requireNonterm) {
    std::cerr << "error: --max-lasso-period requires --require-nonterm\n";
    return 2;
  }

  int minAtoms = result["min-atoms"].as<int>();
  int maxAtoms = result["max-atoms"].as<int>();
  if (minAtoms < 1) {
    std::cerr << "error: --min-atoms must be >= 1 (got " << minAtoms << ")\n";
    return 2;
  }
  if (maxAtoms < minAtoms) {
    std::cerr << "error: --max-atoms must be >= --min-atoms (got " << maxAtoms << " vs " << minAtoms
              << ")\n";
    return 2;
  }
  double offPathMultiplier = result["off-path-multiplier"].as<double>();
  if (offPathMultiplier < 0.0) {
    std::cerr << "error: --off-path-multiplier must be >= 0 (got " << offPathMultiplier << ")\n";
    return 2;
  }

  // Var config
  VarGenConfig varCfg;
  varCfg.nVars = result["n-vars"].as<int>();
  varCfg.nParams = result["n-params"].as<int>();
  varCfg.genId = genId;
  varCfg.typeConfig = typeCfg;

  // Expr config
  ExprGenConfig exprCfg;
  exprCfg.enableAllOps = true; // always enable all ops by default
  exprCfg.enableDiv = !result.count("no-divmod");
  exprCfg.enableSelect = !result.count("no-select");
  exprCfg.enablePtrArith = !result.count("no-ptrarith");
  exprCfg.enableFp = typeCfg.enableFp;
  exprCfg.minAtoms = minAtoms;
  exprCfg.maxAtoms = maxAtoms;
  bool enableIntrinsics = !result.count("no-intrinsics");

  int nFuncs = result["n-funcs"].as<int>();
  int nBbls = result["n-bbls"].as<int>();
  int nStmts = result["n-stmts"].as<int>();
  int maxLoopIter = result["max-loop-iter"].as<int>();
  int minLoopIter = result["min-loop-iter"].as<int>();
  // Clamp to [1, 26] — each concretization's file is named
  // with a lowercase-letter suffix `func_<id>_<i><a..z>.sir`, so
  // 26 is the natural cap. 0 is meaningless (no concretization).
  int nConcretes = result["n-concretes"].as<int>();
  if (nConcretes < 1) {
    std::cerr << "warning: --n-concretes clamped to 1 (was " << nConcretes << ")\n";
    nConcretes = 1;
  } else if (nConcretes > 26) {
    std::cerr << "warning: --n-concretes clamped to 26 (was " << nConcretes << ")\n";
    nConcretes = 26;
  }
  // Same [1, 26] budget as n-concretes: beyond the letter budget the
  // per-example re-solve and replay cost dominates anyway.
  int nExamples = result["n-examples"].as<int>();
  if (nExamples < 1) {
    std::cerr << "warning: --n-examples clamped to 1 (was " << nExamples << ")\n";
    nExamples = 1;
  } else if (nExamples > 26) {
    std::cerr << "warning: --n-examples clamped to 26 (was " << nExamples << ")\n";
    nExamples = 26;
  }
  int maxRetries = result["max-retries"].as<int>();
  double pBranch = result["p-branch"].as<double>();
  double pBackedge = result["p-backedge"].as<double>();
  bool requireReducible = result.count("require-reducible") > 0 || requireNonterm;
  bool enableInterestCoefs = true; // kept in code; not user-exposed
  double pLargeCoef = result["p-large-coef"].as<double>();
  if (pLargeCoef < 0.0 || pLargeCoef > 1.0) {
    std::cerr << "error: --p-large-coef must be in [0, 1] (got " << pLargeCoef << ")\n";
    return 2;
  }
  std::int64_t largeCoefThreshold = result["large-coef"].as<std::int64_t>();
  if (largeCoefThreshold < 0) {
    std::cerr << "error: --large-coef must be >= 0 (got " << largeCoefThreshold << ")\n";
    return 2;
  }
  std::uint32_t timeoutMs = result["timeout"].as<std::uint32_t>();
  SolvingMode solMode = result.count("require-ub") ? SolvingMode::RequireUB : SolvingMode::UBFree;
  if (requireNonterm)
    solMode = SolvingMode::RequireNonterm;
  // A trap has no output to record and a divergence one certifiable input,
  // so multi-example generation is UB-free mode only.
  if (nExamples > 1 && solMode != SolvingMode::UBFree) {
    std::cerr
        << "error: --n-examples > 1 requires the default UB-free mode (--require-ub and "
           "--require-nonterm programs have a single certifiable input and, for diverging ones, "
           "no output to record)\n";
    return 2;
  }
  // Wall-clock budget per function: retries × the n-concretes solves
  // (including the kExtraExampleTries re-solves per extra example), plus 50 ms
  // of non-solver overhead. Compilation runs outside the thread. Saturated:
  // large --max-retries/--timeout inputs would otherwise wrap the 32-bit cast
  // into a near-zero budget.
  std::uint32_t perConcreteSolves =
      1u + static_cast<std::uint32_t>(nExamples - 1) * kExtraExampleTries;
  std::uint32_t funcTimeoutMs = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      static_cast<std::uint64_t>(maxRetries + 1) * nConcretes * perConcreteSolves * timeoutMs + 50,
      0xFFFFFFFFu
  ));
  bool keepSymbolic = result.count("keep-symbolic") > 0;
  bool emitDesc = result.count("emit-desc") > 0;
  bool doValidate = result.count("validate") > 0;
  // --emit-main works under --require-nonterm too: the entry call never
  // returns, so the @check_chksum is unreachable at runtime — but the
  // compiler cannot prove the loop diverges, so it must keep the whole
  // computation alive against the abort() side effect. There is no real
  // return value to capture, so a random literal is used as the expected
  // checksum (see the emit-main block below).
  bool emitMain = result.count("emit-main") > 0;
  bool verbose = result.count("verbose") > 0;
  std::string emitStateMode =
      result.count("emit-state") ? result["emit-state"].as<std::string>() : "";
  if (!emitStateMode.empty() && emitStateMode != "pbb" && emitStateMode != "ppp") {
    std::cerr << "error: --emit-state must be 'pbb' or 'ppp' (got '" << emitStateMode << "')\n";
    return 2;
  }
  // --require-ub implies --no-crc32. The solver sees the sum-form
  // checksum (`%_chk = %_chk + <leaf>` per leaf, emitted by
  // buildSumChecksum). In RequireUB mode it is free to satisfy the
  // "at least one UB on the path" obligation by overflowing that
  // accumulator — a perfectly real signed-overflow UB in the program
  // it solved. But rewriteExitToCrc32Checksum then replaces every
  // `%_chk = %_chk + <leaf>` with a total `@crc32_update(...)` call,
  // deleting exactly that overflow. The emitted program would then be
  // UB-free even though the solver proved a UB, so symiri reports no
  // UB. Keeping the sum form (no crc32 rewrite) makes the program we
  // emit byte-identical to the one we solved, so a solver-found UB is
  // guaranteed to trap. This costs nothing: a UB-triggering program
  // aborts before it reaches a clean `ret`, so the crc32 return-value
  // oracle is vestigial for it anyway.
  // --require-ub and --require-nonterm both imply --no-crc32. RequireUB
  // needs the emitted program byte-identical to the solved one (the CRC32
  // rewrite would delete a solver-found accumulator overflow); RequireNonterm
  // never reaches ^exit, so the checksum is dead and its post-solve oracle
  // (which runs the program under symiri) would hang on the divergent loop.
  bool noCrc32 = result.count("no-crc32") > 0 || solMode == SolvingMode::RequireUB ||
                 solMode == SolvingMode::RequireNonterm;
  std::string target = result["target"].as<std::string>();
  bool noRequire = !result.count("keep-require");
  // UB-free generation (the default) produces programs the
  // solver guarantees never trigger UB on the concretized path, so the
  // backends' dynamic UB guards are dead weight — drop them. --require-ub
  // deliberately triggers UB, so its guards must stay; --keep-ub-guards
  // forces guards back on (e.g. to catch a mislabeled UB-free program
  // trapping at runtime instead of silently misbehaving).
  bool noUbGuards = (solMode == SolvingMode::UBFree) && result.count("keep-ub-guards") == 0;
  std::string vecLoweringOpt = result["vec-lowering"].as<std::string>();

  if (target != "sir" && target != "c" && target != "wasm" && target != "python") {
    std::cerr << "error: unknown target '" << target << "' (expected sir, c, wasm, python)\n";
    return 1;
  }

  std::string structuredLoweringOpt = result["structured-lowering"].as<std::string>();
  if (structuredLoweringOpt != "true" && structuredLoweringOpt != "false" &&
      structuredLoweringOpt != "random") {
    std::cerr << "error: unknown --structured-lowering '" << structuredLoweringOpt
              << "' (expected true, false, random)\n";
    return 1;
  }
  // Structuring consumers only accept reducible CFGs (mirrors symirc:
  // --structured-lowering and --target python imply the check — here,
  // the generator-side repair).
  if (structuredLoweringOpt != "false" || target == "python")
    requireReducible = true;
  // The python backend rejects vecext (no native SIMD value type);
  // catch an explicit request up-front instead of per-program.
  if (target == "python" && vecLoweringOpt != "random" && !makePyVecLowering(vecLoweringOpt)) {
    std::cerr << "error: python target does not support --vec-lowering '" << vecLoweringOpt
              << "' (try random|array|scalars|structscalars|structarray)\n";
    return 1;
  }
  // The WASM backend rejects struct-related strategies.
  if (target == "wasm" && vecLoweringOpt != "random" && !makeWasmVecLowering(vecLoweringOpt)) {
    std::cerr << "error: wasm target does not support --vec-lowering '" << vecLoweringOpt
              << "' (try random|vecext|array|scalars)\n";
    return 1;
  }

  // Sibling symiri is mandatory: the post-solve checksum rewrite uses
  // it to compute each concrete .sir's CRC32 return value, which lands
  // in the descriptor's `retValue` and gets asserted by rylink's
  // `@check_chksum(EXPECTED, …)` main wrapper. Falling back to the
  // solver's pre-rewrite model would put the SUM into the descriptor
  // while the on-disk program returns the CRC32, breaking every
  // compiled program downstream — so abort rather than silently emit
  // bad oracles. --validate's symiri usage rides on the same binary.
  // ---- Main loop -----------------------------------------------------------
  LeafGenConfig leafCfg;
  leafCfg.nBbls = nBbls;
  leafCfg.pBranch = pBranch;
  leafCfg.pBackedge = pBackedge;
  leafCfg.requireReducible = requireReducible;
  leafCfg.maxLoopIter = maxLoopIter;
  leafCfg.minLoopIter = minLoopIter;
  leafCfg.nStmts = nStmts;
  leafCfg.offPathMultiplier = offPathMultiplier;
  leafCfg.enableInterestCoefs = enableInterestCoefs;
  leafCfg.pLargeCoef = pLargeCoef;
  leafCfg.largeCoefThreshold = largeCoefThreshold;
  leafCfg.coefLo = coefLo;
  leafCfg.coefHi = coefHi;
  leafCfg.valueLo = valueLo;
  leafCfg.valueHi = valueHi;
  leafCfg.indexLo = indexLo;
  leafCfg.indexHi = indexHi;
  leafCfg.exprCfg = exprCfg;
  leafCfg.enableIntrinsics = enableIntrinsics;
  leafCfg.timeoutMs = timeoutMs;
  leafCfg.solMode = solMode;
  leafCfg.maxRetries = maxRetries;
  leafCfg.nConcretes = nConcretes;
  leafCfg.nExamples = nExamples;
  leafCfg.outDir = outDir;
  leafCfg.keepSymbolic = keepSymbolic;
  leafCfg.verbose = verbose;
  leafCfg.emitDesc = emitDesc;
  leafCfg.emitMain = emitMain;
  leafCfg.noCrc32 = noCrc32;
  leafCfg.genId = genId;
  leafCfg.requireNonterm = requireNonterm;
  leafCfg.maxLassoPeriod = maxLassoPeriod;

  RunConfig runCfg;
  runCfg.nFuncs = nFuncs;
  runCfg.target = target;
  runCfg.vecLoweringOpt = vecLoweringOpt;
  runCfg.structuredLoweringOpt = structuredLoweringOpt;
  runCfg.emitMain = emitMain;
  runCfg.noRequire = noRequire;
  runCfg.noUbGuards = noUbGuards;
  runCfg.validate = doValidate;
  runCfg.emitStateMode = emitStateMode;
  runCfg.funcTimeoutMs = funcTimeoutMs;

  auto wallStart = std::chrono::steady_clock::now();
  int nOk = 0, nFail = 0;

  runGenerationLoop(leafCfg, varCfg, runCfg, rng, nOk, nFail);

  // Single end-of-run orphan sweep.  When a per-function wall-clock
  // timeout fires, the detached worker may have already written some
  // concrete .sir files into outDir before we abandoned it.  The compile-
  // to-target step then skips that function, leaving the .sir on disk
  // with no matching .c/.wat.  Downstream consumers (reify-diff) see a
  // .sir without its companion and mis-classify it as a `cfail` symirc
  // bug instead of the rysmith timeout it really is.  A single pass after
  // the main loop is O(N) and handles every timed-out function uniformly
  // — much cheaper than scanning the whole directory inside each
  // [TIMEOUT] branch.  A residual race where the detached thread writes
  // a new file *after* this sweep is bounded: when rysmith returns, the
  // subprocess exits and the OS reaps any surviving worker.
  //
  // The `--keep-symbolic` switch emits `<stem><kSymInfix><N>.sir` files
  // (e.g. `func_ab12cd_42_sym0.sir`) that are pre-solve sources and
  // intentionally have no .c twin.  Detect them by the kSymInfix +
  // trailing-digits pattern and exclude from the orphan check.
  if (target != "sir") {
    std::string ext = (target == "c") ? ".c" : (target == "python") ? ".py" : ".wat";
    std::string symInfix = reify::rysmith::hp::kSymInfix;
    std::vector<fs::path> orphanSirs;
    std::error_code ec;
    for (auto &entry: fs::directory_iterator(outDir, ec)) {
      const auto &p = entry.path();
      if (p.extension() != ".sir")
        continue;
      auto stem = p.stem().string();
      // Skip --keep-symbolic preserve files: stem ends with kSymInfix
      // followed by one or more digits.
      auto symPos = stem.rfind(symInfix);
      if (symPos != std::string::npos && symPos + symInfix.size() < stem.size()) {
        std::string tail = stem.substr(symPos + symInfix.size());
        if (!tail.empty() &&
            std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isdigit(c); }))
          continue;
      }
      auto twin = p.parent_path() / (stem + ext);
      if (!fs::exists(twin, ec))
        orphanSirs.push_back(p);
    }
    for (const auto &p: orphanSirs)
      fs::remove(p, ec);
    if (!orphanSirs.empty())
      std::cerr << "[CLEANUP] removed " << orphanSirs.size()
                << " orphan .sir file(s) from timed-out functions\n";
  }

  auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
  double throughput = elapsed > 0 ? nOk / elapsed : 0.0;
  std::cout << "\nDone: " << nOk << " succeeded, " << nFail << " failed (total " << nFuncs << ")"
            << "  [" << elapsed << "s, " << throughput << " funcs/s]\n";

  return 0;
}
