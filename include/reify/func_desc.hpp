#pragma once

// Function descriptor — sidecar metadata that rysmith writes
// next to each generated `.sir` file (`func_<id>_<i>.json`) so the
// `rylink` driver can wire callers/callees without re-parsing
// the SIR source. Both the writer (used by rysmith) and the reader
// (used by rylink) live here so the on-disk schema has a single
// canonical owner.

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ast/ast.hpp"

namespace refractir::reify {

  struct FuncDescriptor {
    // 6-char hex generation ID this descriptor was emitted under.
    std::string id;
    // Canonical sigil-prefixed function name, e.g. `@func_a3f9c2_0`.
    std::string name;
    // Return type, serialized in SIR surface syntax (e.g. `i32`,
    // `ptr i32`, `@struct_a3f9c2_0`, `<4> i32`).
    std::string retType;

    // Whether the function's CFG is reducible (computed from
    // the emitted program via the canonical DomTree/ReducibilityResult
    // analyses). Structuring consumers (rylink --structured-lowering,
    // the python target) must discard seeds where this is false.
    // Descriptors written before this field default to false —
    // conservatively "not known reducible".
    bool reducible = false;

    // Runtime outcome of the generated function on its solved
    // input, generalizing the old `has_ub` bool:
    // Return — UB-free, terminates, returns a value (default rysmith).
    // Trap — deliberately triggers UB (rysmith --require-ub).
    // Diverge — UB-free but never returns (rysmith --require-nonterm).
    // Consumers use it to drop the backends' dynamic UB guards (Return and
    // Diverge are UB-free), to avoid splicing a callee that never returns a
    // value (rylink discards Diverge — a call to it would hang), and to skip
    // twinning a non-terminating program (rytwin refuses Diverge — profiling
    // it would hang). Legacy descriptors carry `has_ub` instead and map on
    // read (has_ub:true → Trap, false → Return); an absent value defaults to
    // Trap, conservatively "may contain UB" so guards stay on.
    enum class Outcome { Return, Trap, Diverge };
    Outcome outcome = Outcome::Trap;

    // True when the concretized path deliberately hits UB (the old `has_ub`).
    [[nodiscard]] bool hasUb() const noexcept { return outcome == Outcome::Trap; }

    struct Param {
      std::string name; // e.g. `%pa0`
      std::string type; // SIR surface syntax
    };

    std::vector<Param> params;

    // Per-realization symValues carries the canonical, init-specific data;
    // callers who need the kind/type metadata should reach for the .sir
    // file's `sym` declarations directly.

    // Block-label path the solver concretized along (`["^entry", "^b1",
    // "^exit"]`). Consistent across all realizations of one descriptor
    // because rysmith samples the path once per attempt.
    std::vector<std::string> path;

    struct Struct {
      std::string name; // `@struct_<id>_<j>`

      struct Field {
        std::string name; // `f0`
        std::string type; // SIR surface syntax
      };

      std::vector<Field> fields;
    };

    std::vector<Struct> structs;

    // One per-concretization record. Carries the concrete .sir file
    // the solver produced for this realization plus the solver-
    // synthesised values for each parameter and the return
    // expression. Values are stringified in the same format
    // rysmith writes into the SOLVED header (decimal ints / hex
    // floats / std::to_string(double) full-precision), so a
    // consumer can hand them straight to `symiri ... -- val0 val1`.
    //
    // `paramValues` / `retValue` are the realization's primary example;
    // `extraExamples` carries the examples beyond it, present only under
    // `--n-examples > 1`. Each entry is a self-contained (parameter
    // values, expected return) pair of the same .sir body. JSON records
    // the primary as the realization's own `params`/`ret` and the rest
    // under `extra_examples`, written only when there is at least one.
    struct Realization {
      std::string file; // basename only, relative to descriptor dir
      // Parameter values keyed by the parameter's local-id (e.g.
      // `%pa0`). Empty when the function takes no parameters.
      std::vector<std::pair<std::string, std::string>> paramValues;
      // Sym values keyed by the sym's local-id (e.g. `%?s0`). One
      // entry per sym declared in *this* realization's program. The
      // sym set may differ across realizations because rysmith
      // re-generates statements per init; consult the companion
      // .sir file to recover sym kinds/types if you need them.
      std::vector<std::pair<std::string, std::string>> symValues;
      // Solved value of the `ret` expression on the chosen path.
      // Empty string when the path's terminator is not `ret <expr>;`.
      std::string retValue;

      // One concretized input/output pair of the .sir body.
      struct Example {
        // Parameter values keyed by the parameter's local-id.
        std::vector<std::pair<std::string, std::string>> paramValues;
        // Expected return value on that input.
        std::string retValue;
      };

      // The examples beyond the primary one. Empty unless rysmith ran
      // with `--n-examples > 1`; the primary is the fields above.
      std::vector<Example> extraExamples;
    };

    std::vector<Realization> realizations;
  };

  // Serialize a descriptor as JSON to `outPath`. Overwrites any
  // existing file. Throws on I/O error.
  void writeFuncDescriptor(const std::filesystem::path &outPath, const FuncDescriptor &d);

  // Convenience overload that builds the FuncDescriptor from an
  // in-memory Program + bookkeeping the caller already has. The
  // function named `@<funcName>` must exist in `prog.funs`; if it
  // doesn't, nothing is written and `false` is returned.
  bool writeFuncDescriptorFromProgram(
      const std::filesystem::path &outPath, const std::string &funcName,
      const refractir::Program &prog, const std::vector<std::string> &pathLabels,
      const std::vector<FuncDescriptor::Realization> &realizations, const std::string &genId,
      FuncDescriptor::Outcome outcome
  );

  // Parse a descriptor JSON. Returns nullopt on parse error (callers
  // typically log and skip). The JSON shape is intentionally narrow
  // (flat string-valued fields plus a few small arrays) so a tiny
  // hand-written parser is sufficient — no dependency added.
  [[nodiscard]] std::optional<FuncDescriptor> readFuncDescriptor(const std::filesystem::path &path);
  [[nodiscard]] std::optional<FuncDescriptor> parseFuncDescriptor(std::string_view json);

} // namespace refractir::reify
