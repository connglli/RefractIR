#pragma once

#include <functional>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "ast/ast.hpp"
#include "solver/provenance.hpp"
#include "solver/smt.hpp"
#include "solver/symbolic_value.hpp"

namespace refractir {

  // How the collected UB-safety guards are used when solving a path.
  enum class SolvingMode {
    UBFree,         // Assert every safety guard true: the path executes with no UB (default).
    RequireUB,      // Assert NOT(and guards): the model triggers at least one UB on the path.
    RequireNonterm, // Lasso path: assert guards true AND the header state recurs after one
                    // lap, so the program diverges UB-free (see docs/reify.md).
    Unconstrained   // Leave UB guards unasserted: solve for a feasible path, UB either way.
  };

  /**
   * Performs path-based symbolic execution on the RefractIR program.
   * Generates SMT constraints for a selected path and uses an SMT solver
   * to find concrete values for symbolic variables that satisfy the path.
   */
  class SymbolicExecutor {
  public:
    struct Config {
      uint32_t timeout_ms = 0;
      uint32_t seed = 0;
      uint32_t num_threads = 1;
      uint32_t num_smt_threads = 1; // Number of threads for the SMT solver backend
      SolvingMode mode = SolvingMode::UBFree;
    };

    using SolverFactory = std::function<std::unique_ptr<smt::ISolver>(const Config &)>;

    explicit SymbolicExecutor(
        const Program &prog, const Config &config, SolverFactory solverFactory
    );

    /**
     * Tree-shaped concrete value extracted from the solver model for
     * an entry-function let at the end of the solved path. The
     * structure mirrors the RefractIR type the let was declared with:
     *   - Scalar Int / Float → `scalar` holds the concrete value.
     *   - Array / Vec        → `elems[i]` holds the i-th element.
     *   - Struct             → `fields[name]` holds each field.
     *   - Ptr                → `targetLocal` names the entry-function
     *                          let / param this pointer pointed to at
     *                          the end of the solved path, resolved
     *                          by inverting the FNV-1a hash that
     *                          tagged the SymbolicValue.prov_base.
     *                          Empty when the pointer's exit-time
     *                          tag does not match any local — pointer
     *                          arithmetic that crossed an object
     *                          boundary, undef pointer, etc.
     *   - Undef              → no defined value in the model.
     *
     * This is a general per-let model export. Consumers that need to
     * reconstruct end-of-path state (oracles, post-mortem dumps,
     * differential validators) read this map; the solver itself
     * makes no further use of it.
     */
    using ModelVal = std::variant<int64_t, double>;

    struct LetExitValue {
      enum class Kind { Int, Float, Array, Struct, Vec, Ptr, Undef } kind = Kind::Undef;
      ModelVal scalar;
      std::vector<LetExitValue> elems;
      std::map<std::string, LetExitValue> fields;
      // Ptr only: `targetLocal` is the entry-function let / param the
      // pointer's provenance base resolves to (FNV-1a inverse of the
      // model value of SymbolicValue::prov_base). `targetOffset` is
      // the address delta inside that object in packed bytes (the
      // scale `TypeUtils::packedSizeof` reports, shared with the
      // interpreter's memory model — decoders must walk it in bytes).
      // Together they pinpoint *which scalar leaf* the pointer points
      // at exit time, even when the body retargeted it mid-aggregate
      // via ptrfield / ptrindex / pointer arithmetic. Empty
      // targetLocal means the tag did not resolve to any local —
      // cross-object arithmetic, undef pointer, etc.
      std::string targetLocal;
      uint64_t targetOffset = 0;
    };

    /**
     * Represents the result of symbolic execution/solving.
     */
    struct Result {
      bool sat = false;
      bool unsat = false;
      bool unknown = false;
      std::string message;
      using ModelVal = SymbolicExecutor::ModelVal;
      std::unordered_map<std::string, ModelVal> model;
      // [v0.2.1] Per-lane model for vector syms. Populated only for syms
      // of vector type. Each entry holds N lane values matching the sym's
      // declared `<N> T` shape. Floats stored as bit-exact int64 too (so
      // a single map type fits both `<N> iM` and `<N> fM` cases —
      // consumers reinterpret as needed).
      std::unordered_map<std::string, std::vector<ModelVal>> vecModel;
      // [v0.2.2] Solved values for the entry function's parameters
      // (treated as symbols by the solver but printed verbatim in the
      // output). Empty when the entry has no parameters.
      std::unordered_map<std::string, ModelVal> paramModel;
      // [v0.2.2] Solved value for the entry function's `ret`
      // expression on the chosen path. `has_value() == false` when the
      // path's terminator is not a `ret <expr>;`.
      std::optional<ModelVal> retModel;
      // Concrete value of every entry-function let at the end of the
      // solved path. Keyed by let name (`%v0`, `%a0`, …). The tree
      // shape under each entry mirrors the let's declared type — see
      // LetExitValue. Empty for paths that did not produce a model.
      std::unordered_map<std::string, LetExitValue> letExitValues;
    };

    /**
     * Solves for a specific path in a function.
     * @param funcName The function to analyze.
     * @param path A sequence of block labels representing the path to execute.
     * @param fixedSyms Optional mapping to fix certain symbols to concrete values.
     */
    Result solve(
        const std::string &funcName, const std::vector<std::string> &path,
        const std::unordered_map<std::string, int64_t> &fixedSyms = {}
    );

    /**
     * Samples N paths randomly and tries to solve each of them.
     * Stops and returns the first SAT result found.
     */
    Result sample(
        const std::string &funcName, uint32_t n, uint32_t maxPathLen, bool requireTerminal,
        const std::vector<std::string> &prefixPath = {},
        const std::unordered_map<std::string, int64_t> &fixedSyms = {}
    );

    // SymbolicValue and SymbolicStore are defined in
    // solver/symbolic_value.hpp at namespace scope so the provenance
    // collaborator and the per-concern translation units can share them.

  private:
    const Program &prog_;
    Config config_;
    SolverFactory solverFactory_;

    // --- Symbolic evaluation helpers ---
    SymbolicValue
    mergeAggregate(const std::vector<SymbolicValue> &elements, smt::Term idx, smt::ISolver &solver);

    // [v0.2.1] Evaluate an Expr whose value is a vector, returning a
    // SymbolicValue of Kind::Vec. Used by AssignInstr when the LHS is
    // vector-typed. Handles CoefAtom/RValueAtom (whole-vector reference),
    // OpAtom (per-lane scalar arith with coef-broadcast), UnaryAtom,
    // CmpAtom (vec→<N>i1), and mask-form SelectAtom.
    SymbolicValue evalVecExpr(
        const Expr &e, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );

    // [v0.2.1] Per-atom evaluator for vector RHS. Pulled out so the
    // chain-application loop in evalVecExpr can lower each atom without
    // round-tripping through Expr (which is not copy-assignable).
    SymbolicValue evalVecExprAtom(
        const Atom &a, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );
    // Per-Atom-kind lane-wise helpers for evalVecExprAtom (src/solver/vec.cpp).
    // CoefAtom / RValueAtom stay inline in the dispatcher.
    SymbolicValue evalVecOpAtom(
        const OpAtom &arg, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );
    SymbolicValue evalVecUnaryAtom(
        const UnaryAtom &arg, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );
    SymbolicValue evalVecSelectAtom(
        const SelectAtom &arg, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );
    SymbolicValue evalVecCmpAtom(
        const CmpAtom &arg, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );
    SymbolicValue evalVecCastAtom(
        const CastAtom &arg, const VecType &vt, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );

    // Execute one instruction (assign / assume / require / store) of the
    // current block against the symbolic store and path condition. Extracted
    // from solve()'s per-instruction std::visit.
    void execInstr(
        const Instr &ins, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pathConstraints, std::vector<smt::Term> &ubGuards,
        std::vector<smt::Term> &requirements
    );

    SymbolicValue evalExpr(
        const Expr &e, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub, std::optional<smt::Sort> expectedSort = std::nullopt
    );
    SymbolicValue evalAtom(
        const Atom &a, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub, std::optional<smt::Sort> expectedSort = std::nullopt
    );
    // evalAtom dispatches on the Atom variant; each alternative's evaluation
    // lives in a dedicated evalXxxAtom helper (src/solver/expr.cpp). CoefAtom /
    // RValueAtom stay inline in the dispatcher. Signatures are tailored per
    // branch so no parameter is unused.
    SymbolicValue evalOpAtom(
        const OpAtom &arg, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub, std::optional<smt::Sort> expectedSort
    );
    SymbolicValue evalUnaryAtom(
        const UnaryAtom &arg, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );
    SymbolicValue evalSelectAtom(
        const SelectAtom &arg, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub,
        std::optional<smt::Sort> expectedSort
    );
    SymbolicValue evalCastAtom(
        const CastAtom &arg, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub
    );
    SymbolicValue evalCmpAtom(
        const CmpAtom &arg, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub
    );
    SymbolicValue evalAddrAtom(const AddrAtom &arg, smt::ISolver &solver, SymbolicStore &store);
    SymbolicValue evalPtrIndexAtom(
        const PtrIndexAtom &arg, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );
    SymbolicValue evalPtrFieldAtom(
        const PtrFieldAtom &arg, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );
    SymbolicValue evalLoadAtom(
        const LoadAtom &arg, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub
    );
    SymbolicValue evalCallAtom(
        const CallAtom &arg, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub
    );
    smt::Term evalCoef(
        const Coef &c, smt::ISolver &solver, SymbolicStore &store,
        std::optional<smt::Sort> expectedSort = std::nullopt
    );
    SymbolicValue evalSelectVal(
        const SelectVal &sv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub, std::optional<smt::Sort> expectedSort = std::nullopt
    );

    SymbolicValue evalLValue(
        const LValue &lv, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub, bool forWrite = false
    );
    void setLValue(
        const LValue &lv, const SymbolicValue &val, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );

    SymbolicValue muxSymbolicValue(
        smt::Term cond, const SymbolicValue &t, const SymbolicValue &f, smt::ISolver &solver
    );

    SymbolicValue updateLValueRec(
        const SymbolicValue &cur, std::span<const Access> accesses, const SymbolicValue &val,
        smt::Term pathCond, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub, int depth = 0
    );

    smt::Term evalCond(
        const Cond &c, smt::ISolver &solver, SymbolicStore &store, std::vector<smt::Term> &pc,
        std::vector<smt::Term> &ub
    );

    smt::Sort getSort(const TypePtr &t, smt::ISolver &solver);
    // Creates a fresh symbolic value for an *input* to the modeled
    // computation (a parameter, a `sym`, a havoc'd memory cell, or an
    // external-decl return). Because every RefractIR floating-point value is
    // finite (SPEC v0.2.2 §5), each scalar FP leaf is constrained finite by
    // pushing `notInf ∧ notNaN` onto `finiteSink` when a sink is supplied.
    // This is a *hard* domain invariant (path constraint), distinct from the
    // negatable FP-overflow guards on computed *results* — so RequireUB
    // negation can trigger a genuine finite→∞ overflow but can never select a
    // non-finite input, which is not a representable RefractIR value.
    SymbolicValue createSymbolicValue(
        const TypePtr &t, const std::string &name, smt::ISolver &solver,
        std::vector<smt::Term> *finiteSink = nullptr
    );

    SymbolicValue makeUndef(const TypePtr &t, smt::ISolver &solver);
    SymbolicValue broadcast(const TypePtr &t, smt::Term val, smt::ISolver &solver);
    SymbolicValue evalInit(
        const InitVal &iv, const TypePtr &t, smt::ISolver &solver, SymbolicStore &store,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );

    TypePtr resolveLValueType(const LValue &lv) const;
    std::string buildLValueKey(const LValue &lv) const;
    TypePtr resolveExprType(const Expr &e) const;
    TypePtr resolveAtomType(const Atom &a) const;
    TypePtr resolveSelectValType(const SelectVal &sv) const;

    // [v0.2.2] SMT lowering for built-in intrinsics.
    // Implemented in src/solver/intrinsics.cpp — the single source of
    // truth for solver-side intrinsic semantics. Adds UB path conditions
    // (e.g. x != 0 for @clz/@ctz) directly to `pc`.
    SymbolicValue callBuiltinIntrinsicSMT(
        const IntrinsicDecl &intr, std::vector<SymbolicValue> &argVals, smt::ISolver &solver,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );

    std::unordered_map<std::string, const StructDecl *> structs_;

    // Pointer dispatch helpers (v0.2.0):
    // Pointers are encoded as BV64 tags identifying their target local. The
    // current FunDecl is held per-solve via thread_local storage to support
    // load/store dispatch over candidate targets, while keeping sample() safe
    // for concurrent workers.
    static thread_local const FunDecl *currentFun_;

    // [v0.2.1] Per-solve pointer provenance store (rule 14, 19). Owns the
    // `ptr T` local → PtrProvenance map; see Provenance (solver/provenance.hpp).
    Provenance prov_;

    // [v0.2.2] §9.6.1 shared sym cache: a `sym` declared in a `fun`
    // body becomes one solver constant reused across every call to
    // that function on the path. Keyed by FunDecl*.
    std::unordered_map<const FunDecl *, std::unordered_map<std::string, SymbolicValue>> calleeSyms_;

    // [v0.2.2] Symbolic interprocedural call. Evaluates the callee
    // body on its straight-line CFG path and returns the symbolic
    // value of the ret expression. The caller's FunDecl and store are
    // passed in so that `store %p, %v` instructions inside the callee
    // can reach the caller-side `let mut` whose address backs `%p` —
    // see SPEC §9.6.1 step 4 ("Mem[T] reflects callee `store`s").
    SymbolicValue callFunction(
        const FunDecl &callee, std::vector<SymbolicValue> args, smt::ISolver &solver,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub, const FunDecl *callerFun = nullptr,
        SymbolicStore *callerStore = nullptr
    );

    // [v0.2.2 Phase 8] §9.6.2 contract-form decl expansion. `pre`
    // clauses join PC; a fresh ret_sym stands for the return value;
    // `post` clauses are assumed by joining PC. Memory havoc
    // (§9.6.2.4) is the next refinement and is currently a TODO --
    // pointer arguments are not yet havoc'd, so contracts that talk
    // about post-state pointee memory are sound only when the caller
    // has already constrained it.
    SymbolicValue callContract(
        const ExtDecl &decl, const std::vector<std::shared_ptr<Expr>> &argExprs,
        std::vector<SymbolicValue> args, smt::ISolver &solver, SymbolicStore &callerStore,
        std::vector<smt::Term> &pc, std::vector<smt::Term> &ub
    );

    // [v0.2.2] Currently active requirements vector (so nested callees
    // can push REQ terms). Set by solve() and consumed by callFunction.
    std::vector<smt::Term> *currentReq_ = nullptr;


    // [v0.2.2] Per-solve RNG used by callFunction to pick a branch when
    // a callee has a non-straight CFG. Seeded from config_.seed at the
    // start of solve(). A future sub-path syntax (see SPEC §13) will
    // replace this random sampling with a user-supplied callee path.
    std::mt19937 calleeRng_;

    // [v0.2.2] Caller frame snapshot for the current `callFunction`
    // activation.  Used by LoadAtom / StoreInstr handlers so that a
    // pointer parameter dereferenced inside a callee can land its read
    // / write on the caller-side `let mut` that owns the storage —
    // SPEC §9.6.1 step 4.  Both fields are null at the top level and
    // re-saved-and-restored around each nested call.
    const FunDecl *outerFun_ = nullptr;
    SymbolicStore *outerStore_ = nullptr;
  };

} // namespace refractir
