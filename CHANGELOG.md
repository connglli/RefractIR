# Changelog

Notable changes to RefractIR (internally SymIR), newest first. Language-level changes are defined normatively by the matching `docs/SPEC_v*.md`; entries here summarize them alongside toolchain milestones.

## [v0.2.3] - in progress

Spec: [docs/SPEC_v0.2.3.md](./docs/SPEC_v0.2.3.md), which doubles as the release roadmap and marks each feature `[Shipped]` or `[Planned]`.

### Added

- Python compilation target (`symirc --target python`): genuine `while`/`if` control flow, a boxed pointer and aggregate memory model with runtime UB traps, lane-list vectors, and symbol providers through module globals. Accepts reducible CFGs only.
- CFG structuring analyses: dominator trees, reducibility check, loop forest, control-tree builder, and structured lowering with `while`/`do-while` peepholes and header-test loop rotation ([docs/reducibility.md](./docs/reducibility.md)).
- `symirc --structured-lowering`: reconstructed `while`/`do-while`/`if` on the C target, and `block`/`loop`/`if` on the WASM target, where the native multi-level `br` consumes the unlowered control tree and needs no guard flags. The `$__pc`/`br_table` dispatch loop stays the WASM default and still accepts irreducible CFGs.
- WASM SIMD-128 vector lowering: vector locals live in native `v128` registers by default, with shapes wider than 16 bytes split across registers.
- `symirc --vec-lowering` strategy families: C `vecext|scalars|array|structscalars|structarray`, Python all but `vecext`, WASM `vecext|array|scalars`; the chosen strategy is stamped into the emitted module.
- `symirc --no-ub-guards`: omit the backends' dynamic UB guards on all three targets. Sound only for UB-free programs, where the guards never fire; value semantics are unchanged. The reify tools drive it from the descriptor's `has_ub` field, and `--keep-ub-guards` forces it back on.
- WASM checksum intrinsics `@crc32_update` (table-free LFSR loop) and `@check_chksum`, with no host imports, so every shipped intrinsic lowers on every compiled target.
- Horizontal vector reductions `@reduce_add`/`min`/`max`/`and`/`or`/`xor` (spec §12.4): a sequential per-step-UB fold from `<N> T` to `T`, order-independent for min/max via the `@fmin`/`@fmax` tie-break, across the interpreter, the solver and all three targets. `@reduce_mul` is intentionally absent, being nonlinear.
- Non-terminating program generation (`symirsolve --require-nonterm`, `rysmith --require-nonterm`): UB-free diverging functions from a lasso witness whose header state is a fixed point, closed by a uniform `leaf = leaf + %?ntK` correction over integer, float and pointer leaves. `rylink` fuses a homogeneous pool into a whole program of the same outcome, and `rytwin` refuses one. Adds the descriptor `outcome` field generalizing `has_ub`, the `@observe` beacon intrinsic, and `symiri --max-bbl-steps`.
- UB-directed generation: `symirsolve --require-ub` and `rysmith --require-ub` solve for symbol values that trigger UB on the chosen path; `rysmith --no-crc32` skips the checksum oracle.
- `rytwin` (new tool): emits a semantically equivalent variant of a generated program by grafting guarded twin regions, with no SMT solver involved. The twin unit is a whole dominance region, collapsing straight-line runs and entire loops into one guarded block that jumps to the region exit, and its body is the region's own executed trace. An interval pass classifies each guard leaf free, ranged or pinned, and `--validate` spot-checks states sampled inside the resulting box.
- Anti-optimization engine ([include/reify/antiopt.hpp](./include/reify/antiopt.hpp)): identities applied in the opposite direction to a compiler's, re-checked over the states a guard admits and rolled back otherwise, so a rule that can trap is kept exactly where the box proves it cannot. Used by `rylink` on bundled programs and by `rytwin` on twin bodies.
- `rytwin --twin-select interesting`: a selection policy that softmax-tilts each region's twin probability by how hard its twin is to prove equivalent, concentrating twins on the hardest regions.
- Per-lane vector symbol binding, `symiri --sym '%?v=1,2,3,4'`.
- Reify pipeline support for the Python target, structured lowering with per-program strategy sweeps, and reducible CFG generation.
- `rysmith --n-examples`: more than one input/output example per concretized `.sir`.
- `make install` target.

### Changed

- `rysmith --n-inits` renamed to `--n-concretes`; the old flag no longer parses.
- Internal modularization: the interpreter, the solver and the C/WASM backends split into cohesive translation units with extracted collaborators (`TypeLayout`, `Memory`, `Provenance`) and decomposed visitor dispatches; backend files renamed to a target-prefix convention; C vector lowering made target-specific. The shared mechanism the reify tools had accumulated moved to the components that own it: the parse-and-check pipeline, the type and access-path queries, the byte-offset walk and pointer provenance, AST cloning and construction, the emit driver, declaration identity, the solved-program header, and the state-set analysis.

### Fixed

- Linking a library with `-I` merged its declarations by name. An intrinsic overload the primary also declared under a different signature was dropped, so the library body stopped typechecking; a struct the primary declared differently displaced the library's, and when the two agreed on field types but not their order the result still typechecked and read fields at the wrong offsets. Both are keyed by the declaration's identity now, and a struct conflict is rejected.
- WASM vector call boundary: arguments were passed as a garbage byte-load rather than an address, and vector returns emitted invalid WASM. Both use a defined memory ABI (caller-owned spill slots plus a hidden sret parameter) preserving by-value semantics.
- Object extents in the solver were measured in scalar leaves rather than packed bytes, a scale that agrees with the interpreter only when every scalar in an object has the same width. Mixed-width structs both hid real out-of-bounds pointer arithmetic and rejected valid programs, and the `--emit-main` checksum oracle decoded exit-time pointer offsets on a third differently-scaled copy of the model, with the Python backend's leaf-slot memory a fourth. All four measure with `TypeUtils::packedSizeof`, and Python's buffer is byte-indexed with a `_PAD` sentinel that traps on read of a wider leaf's interior bytes.
- Float literals reached the solver through a decimal rendering, which dropped the sign of `-0.0` and truncated every value to six fraction digits. All four construction sites and both solver backends build FP constants from the `double` itself, bit-exactly.
- `rylink` composed programs whose cost was exponential in the call graph's depth: an edge could be spliced at several sites on the caller's executed path, and the per-match acceptance coin was tuned to keep the expected count in [1, 2), at or above the branching factor from which a run never terminates in practice. An edge now takes at most one executed site, chosen with probability `--p-onpath-call`, and is realized off-path regardless, so the emitted call graph is the sampled DAG whatever the coin says. A 64-node bundle went from ~10⁹ calls (hanging `--validate`) to a few thousand.
- New UB rule 27 (spec §7.5): dereferencing a pointer to a returned activation's local. The solver tagged a pointer by the local's name alone, so a dead frame's pointer resolved against any live cell that shared the name, a load through it read the wrong object rather than being ruled out; tags now name the activation too. `symirc` does not enforce the rule. The interpreter kept every activation's provenance objects for the life of the run, so a dead frame's locals stayed readable and, since provenance lookup is a linear scan, a program's cost grew quadratically in its own call count. An activation now releases its objects on return, keeping only the provenance ids reserved.
- Rule 3 (read of `undef`) enforced on a load through a pointer to an uninitialized cell, in the interpreter and the solver.
- Symbolic floating-point inputs constrained finite by the solver, per the finite-only FP domain (spec §2.9).
- Literal bit-width inference propagated, so UB overflow detection sees the resolved width.
- C backend: inline `cmp`-atom masks and vector symbol initialization.
- `symirc --no-ub-guards` no longer affects `@check_chksum` and `require`.

## [v0.2.2] - 2026-06-29

Spec: [docs/SPEC_v0.2.2.md](./docs/SPEC_v0.2.2.md).

### Added: language

- Function calls: the `call` atom with left-to-right argument evaluation and interprocedural execution, threading path conditions, store and memory through callees, with callee UB pruning the calling path. Recursion, indirect calls and variadics are rejected.
- External declarations (`decl`) in two mutually exclusive forms: contract (`pre`/`post`/`ret` clauses as the callee's specification for solver reasoning, with pointer-argument memory havoc) and link (signature resolved to a body in another `.sir` file via `-I`).
- Intrinsics (`intrinsic`): toolchain-defined built-ins with fixed interpreter, SMT and backend lowerings, shipping the §12 baseline and the full P0 tier (integer extras, bit-manipulation, the overflow-aware family, the FP IEEE family) with per-width overload resolution ([docs/intrinsics.md](./docs/intrinsics.md)).
- Checksum primitives `@crc32_update` and `@check_chksum` for the reify pipeline's opaque return-value oracle, lowering to C at this point and to WASM in v0.2.3.
- Signed `i1 = {0, -1}` value convention, and strict signed-range literal checking with no silent narrowing.

### Added: toolchain

- `rylink` (new tool): whole-program generator composing `rysmith` leaf functions, with per-artifact output, `--split-by-source`, and differential cross-validation batches.
- `-I` link resolution across `symiri` / `symirc` / `symirsolve`, the `test/lib/std` stdlib, entry-point positional arguments, the bit-exact `SOLVED`/`PARAMS`/`RETURN` headers, and `--emit-main`.
- Solver: contract-form `decl` expansion, random-path sampling for branchy callees, and contract memory havoc for direct pointer arguments.
- Reify: intrinsic generation, the opaque checksum rewrite, and a broad set of generator controls (`--min/max-atoms`, `--large-coef`, `--off-path-multiplier`, noinline and noclone probabilities).

## [v0.2.1] - 2026-05-27

Spec: [docs/SPEC_v0.2.1.md](./docs/SPEC_v0.2.1.md).

### Added: language

- SIMD vector types `<N> T`: lane-wise arithmetic, lane access by subscript, whole-vector copy, and per-lane independent vector symbols. Vectors are pure value types and are not addressable.
- Reified comparisons (`cmp <relop>`): `i1` for scalars and `<N> i1` masks for vectors, with mask-based `select` for per-lane blends.
- Aggregate pointers `ptr [N] T` and `ptr @S` with `ptrindex` / `ptrfield` navigation, packed struct layout, UB rules 14 to 19 for provenance and typed-access mismatch, and the vector UB rules.
- Atom-form initializers for non-aggregate locals, and the finalized floating-point value model.

### Added: toolchain

- C backend vector-lowering strategies (`vecext`, `array`, `scalars`, `structarray`, `structscalars`), full v0.2.1 support in the WASM backend, and per-lane vector SMT encoding in the solver.
- Reify: vector and aggregate-pointer generation with a random per-program vec-lowering strategy.

## [v0.2.0] - 2026-05-23

Spec: [docs/SPEC_v0.2.0.md](./docs/SPEC_v0.2.0.md).

### Added: language

- Pointers: the `ptr T` type, `addr` requiring a `let mut` root, `load` / `store`, context-typed `null`, pointer arithmetic (`ptr T ± iN` and `ptr T - ptr T` element distance), and strict provenance UB rules including rule 15 for struct-field provenance.
- Float `%` redefined from IEEE remainder to C `fmod` truncated-quotient semantics, and `shl` result overflow classified as signed-overflow UB.

### Added: toolchain

- `rysmith` (new tool): the C++ reify pipeline generating random leaf functions with pointer support, AoS/SoA type diversity, and `--target c/wasm` compilation of concrete output.
- Solver pointer support through a tagged BV64 encoding, typed process exit codes with `EXPECT: FAIL:<subtype>` test-framework support, and `symirc --no-require`.

## [v0.1.0] - 2026-05-18

Spec: [docs/SPEC_v0.1.0.md](./docs/SPEC_v0.1.0.md), the draft v0 spec renamed under semantic versioning. First complete implementation of the language and toolchain.

### Added

- Language core: a non-SSA CFG-based IR with mutable locals (`let mut`), explicit symbols, flat left-to-right expressions, lazy `select`, strict UB, `as` casts, bitwise operators, hex/octal/binary literals, multidimensional arrays with brace initialization, and finite-only `f32`/`f64` floating-point.
- `symiri` reference interpreter with strict UB checks and execution tracing.
- `symirc` compiler with C and WebAssembly (WAT) backends.
- `symirsolve` SMT concretizer: path-based symbolic execution, an abstract solver interface with Bitwuzla and AliveSMT (Z3) backends, and random path sampling with multi-threading.
- Frontend stack: lexer, recursive-descent parser, CFG builder, BV-aware typechecker, semantic checker with definite-initialization analysis, pass manager and dataflow framework, and clang-style diagnostics.
- Examples (ciphers, sorts, a robot navigator), the `.sir` VS Code extension, and the automated test suite.

## [v0.0.1] - 2026-01-23

Repository bootstrap: the draft v0 language specification (`SPEC_v0.md`, later `SPEC_v0.1.0.md`), design documents for `symirc` / `symiri` / `symirsolve`, and the project guides. No implementation.
