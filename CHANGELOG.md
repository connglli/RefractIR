# Changelog

All notable changes to RefractIR (internally SymIR) are documented in
this file, newest first. Language-level changes are normatively defined
by the corresponding `docs/SPEC_v*.md`; entries here summarize them
together with toolchain milestones.

## [v0.2.3] — In progress

Spec: [docs/SPEC_v0.2.3.md](./docs/SPEC_v0.2.3.md) — the spec doubles
as the release roadmap. The toolchain and backend work below has
shipped; the language-surface additions carried over from the v0.2.2
§13 plan — in the intended implementation order, easiest first:
horizontal `@reduce_*` intrinsics, `shuffle`, addressable vectors
(`ptr <N> T`, whole-vector `load`/`store`), vectors in aggregates,
and function attributes (`inline`/`noinline`/`pure`/`const`) — are
designed in the spec. `@reduce_*` is now **partially shipped**
(frontend + interpreter + solver; backend lowering still **[Planned]**);
the rest remain **[Planned]**. Relaxed SIMD and `noreturn` were
considered and dropped (spec §13).

### Added

- **Non-terminating program generation** (`rysmith --require-nonterm`,
  `symirsolve --require-nonterm`): generate UB-free **diverging** leaf
  functions from a lasso witness with a header-state fixed point
  (`σ_h′ = σ_h`), closed by a uniform `leaf = leaf + %?ntK` correction
  over every integer, float, and pointer leaf; `rylink` fuses a
  **homogeneous** pool (all-return / all-trap / all-diverge) into a
  whole program of that outcome; `rytwin` transforms only UB-free
  terminating inputs. Adds the descriptor `outcome` field (generalizing
  `has_ub`), the `@observe` observability-beacon intrinsic (identity
  value over any `iN`/`fN`; observable `volatile` write in the C
  lowering), and `symiri --max-bbl-steps`.
- **Python compilation target** (`symirc --target python`): emits
  genuine `while`/`if` control flow, a boxed pointer/aggregate memory
  model with runtime UB traps, lane-list vectors, and symbol providers
  via module globals. Accepts only **reducible** CFGs.
- **CFG structuring analyses**: dominator trees, reducibility check,
  loop-forest identification, control-tree builder, and structured
  lowering transforms (`while`/`do-while` peepholes, header-test loop
  rotation) — see [docs/reducibility.md](./docs/reducibility.md).
- **`symirc --structured-lowering`** (C target): reconstructed
  `while`/`do-while`/`if` emission instead of labels+`goto`.
- **`symirc --structured-lowering`** (WASM target): reconstructed
  `block`/`loop`/`if` emission instead of the `$__pc`/`br_table`
  dispatch loop. WASM's native multi-level `br` consumes the
  *unlowered* control tree directly — no guard flags. The dispatch
  loop stays the default and still accepts irreducible CFGs.
- **WASM SIMD-128 vector lowering**: vector locals live in native
  `v128` registers by default; shapes wider than 16 bytes split across
  registers. Fulfils the v0.2.2 §13 plan.
- **Vector-lowering strategy families** (`symirc --vec-lowering`):
  C `vecext|scalars|array|structscalars|structarray`, Python
  `array|scalars|structarray|structscalars`, WASM
  `vecext|array|scalars`; defaults `vecext` (C, WASM) / `array`
  (Python). The chosen strategy is stamped into the emitted module.
- **WASM checksum intrinsics**: `@crc32_update` (table-free LFSR loop)
  and `@check_chksum` (trap on mismatch) now lower on WASM with no
  host imports — every shipped intrinsic lowers on every compiled
  target.
- **Horizontal vector reductions** (`@reduce_add`/`min`/`max`/`and`/`or`/
  `xor`, spec §12.4 V1): fold an `<N> T` vector to a scalar `T` across the
  interpreter, solver, and all three compiled targets. Sequential
  per-step-UB fold; min/max order-independent via the `@fmin`/`@fmax`
  tie-break. Backends lower it to a per-shape helper (C takes the lanes as
  scalar parameters, so every `--vec-lowering` strategy works; WASM reads
  the packed spill by address; Python folds the lane list). `@reduce_mul`
  is intentionally absent (nonlinear).
- **`rytwin`** (new tool): transforms a generated program into a
  semantically-equivalent variant via SMT-checked twin blocks, driven
  by `rysmith --emit-state` per-program-point state profiles.
  `--twin-guard bijection` optionally hides each twin's guard behind a
  collision-free nonlinear `iW` bijection (opaque `>>> & ^`, no literal
  `state == s`), so deducing twin ≡ orig needs a solver. `--twin-scope
  region` widens the twin unit from a single block to a whole dominance
  region — collapsing straight-line runs and entire loops into one guarded
  block that jumps straight to the region exit. `--twin-select interesting`
  swaps the uniform `--p-twin` coin for a selection policy that softmax-tilts
  each region's twin probability by how hard its twin is to prove equivalent
  (loop-collapse dominating), concentrating twins on the hardest regions.
- **Multi-size DLP guard tiers** (`--twin-guard bijection`): the guard's
  discrete-logarithm trapdoor now draws from a seven-tier table of verified
  (prime, primitive-root) pairs spanning 8/10/16/20/32/48/62 bits (28 pairs,
  all re-checked from scratch: GMP `factor` primality + `bc` square-and-
  multiply primitive-root witness). Each integer leaf
  picks the smallest tier whose square-and-multiply depth covers its width and
  whose primes exceed its profiled value (so the `x < P` range gate still
  fires), with a 30% jump to a strictly larger tier for diversity. Working
  types scale with the tier (`i16`/`i32`/`i64`/`i128` casts) while the DLP
  check stays collision-free on the gated domain and UB-free on every input
  (`mulBits > 2·primeBits` so products never overflow the signed working type).
- **UB-directed generation**: `symirsolve --require-ub` and
  `rysmith --require-ub` solve for symbol values that *trigger* UB on
  the chosen path; `rysmith --no-crc32` skips the checksum oracle.
- **Per-lane vector symbol binding**: `symiri --sym '%?v=1,2,3,4'`.
- Reify pipeline: Python-target and structured-lowering support with
  per-program strategy sweeps; reducible CFG generation.
- **`symirc --no-ub-guards`**: omit the backends' dynamic UB guards
  (null/OOB pointer traps, integer div/rem-by-zero traps, FP-finiteness
  traps, intrinsic preconditions) across C, WASM, and Python. Sound only
  for UB-free programs, where the guards never fire; value semantics are
  preserved. The reify tools drive it automatically — `rysmith`/`rylink`
  drop the guards for UB-free output (tracked per descriptor via a new
  `has_ub` field), `rytwin` drops them for its UB-free twin, and
  `--keep-ub-guards` forces them back on.
- `make install` target.

### Changed

- Internal modularization: interpreter, solver, and the C/WASM
  backends split into cohesive translation units with extracted
  collaborators (`TypeLayout`, `Memory`, `Provenance`) and decomposed
  visitor dispatches; backend files renamed to a target-prefix
  convention; C vector lowering made target-specific.

### Fixed

- WASM vector call boundary: arguments were passed as a garbage
  byte-load instead of an address and vector returns emitted invalid
  WASM; both now use a defined memory ABI (caller-owned spill slots +
  hidden sret parameter) preserving by-value semantics.
- Rule 3 (read of `undef`) enforced on loads through a pointer to an
  uninitialized cell, in both the interpreter and the solver.
- Symbolic floating-point inputs constrained finite by the solver
  (finite-only FP domain, spec §2.9).
- Object extents in the solver were measured as one unit per scalar
  **leaf** rather than packed **bytes**, a scale that matches the
  interpreter only while every scalar in an object has the same width.
  Mixed-width structs both hid real out-of-bounds pointer arithmetic and
  rejected valid programs, and the `--emit-main` checksum oracle decoded
  the solver's exit-time pointer offsets on a third, differently-scaled
  copy of the model. The Python backend's memory was a fourth, indexed
  by leaf slot. All four now measure with `TypeUtils::packedSizeof`;
  Python's buffer is byte-indexed, with a `_PAD` sentinel for the
  interior bytes of a wider leaf that traps if read.
- Float **literals** reached the solver through a decimal rendering,
  which dropped the sign of `-0.0` (a decimal parses via a real, which
  has no signed zero) and truncated every value to six fraction digits
  (`std::to_string` formats with `%f`, so `1e-7` became `0.0`). All four
  construction sites and both solver backends now build FP constants
  from the `double` itself, bit-exactly.
- Literal bit-width inference propagated so UB overflow detection sees
  the resolved width.
- C backend: inline `cmp`-atom masks and vector symbol initialization.

## [v0.2.2] — 2026-06-29

Spec: [docs/SPEC_v0.2.2.md](./docs/SPEC_v0.2.2.md).

### Added — language

- **Function calls**: the `call` atom with left-to-right argument
  evaluation and interprocedural execution (path conditions, store,
  and memory threaded through callees; callee UB prunes the calling
  path). Recursion, indirect calls, and variadics are rejected.
- **External declarations** (`decl`): contract form
  (`pre`/`post`/`ret` clauses as the callee's specification for
  solver reasoning, with pointer-argument memory havoc) and link form
  (signature resolved to a body in another `.sir` file via `-I`
  search paths); the two forms are mutually exclusive.
- **Intrinsics** (`intrinsic`): toolchain-defined built-ins with fixed
  interpreter, SMT, and backend lowerings. Shipped the §12 baseline
  (`@abs`, `@min`, `@max`, `@clz`, `@ctz`, `@popcount`) and the full
  P0 tier: integer extras (`@abs_diff`, `@signum`, `@clamp`,
  `@midpoint`), bit-manipulation (`@parity`, `@bswap`, `@bitreverse`,
  `@rotl`, `@rotr`, `@is_pow2`, `@ilog2`), the overflow-aware family
  (`@wrapping_*`, `@saturating_*`, `@div_euclid`, `@rem_euclid`), and
  the FP IEEE family (`@fabs` … `@fract`, `@recip`), with per-width
  overload resolution. See [docs/intrinsics.md](./docs/intrinsics.md).
- **Checksum primitives** `@crc32_update` / `@check_chksum` for the
  reify pipeline's opaque return-value oracle (C-only lowering at the
  time; WASM followed in v0.2.3).
- Signed `i1 = {0, -1}` value convention; strict signed-range literal
  checking (no silent narrowing).

### Added — toolchain

- **`rylink`** (new tool): whole-program generator composing rysmith
  leaf functions, with per-artifact output, `--split-by-source`, and
  differential cross-validation batches.
- `-I` link resolution across `symiri` / `symirc` / `symirsolve`; the
  `test/lib/std` stdlib; entry-point positional arguments and the
  bit-exact `SOLVED`/`PARAMS`/`RETURN` headers; `--emit-main`.
- Solver: contract-form `decl` expansion, random-path sampling for
  branchy callees, contract memory havoc for direct pointer
  arguments.
- Reify: intrinsic generation, the opaque checksum rewrite, and a
  broad set of generator controls (`--min/max-atoms`, `--large-coef`,
  `--off-path-multiplier`, noinline/noclone probabilities, …).

## [v0.2.1] — 2026-05-27

Spec: [docs/SPEC_v0.2.1.md](./docs/SPEC_v0.2.1.md).

### Added — language

- **SIMD vector types** `<N> T`: lane-wise arithmetic, lane access via
  subscript, whole-vector copy, per-lane independent vector symbols;
  vectors are pure value types (not addressable).
- **Reified comparisons** (`cmp <relop>`): `i1` results for scalars,
  `<N> i1` masks for vectors; **mask-based `select`** for per-lane
  blends.
- **Aggregate pointers**: `ptr [N] T` and `ptr @S` with `ptrindex` /
  `ptrfield` navigation, packed struct layout, and UB rules 14–19
  (provenance, typed-access mismatch); vector UB rules (lane-wise UB,
  out-of-bounds lane access).
- Atom-form initializers for non-aggregate locals; finalized
  floating-point value model documentation.

### Added — toolchain

- C backend vector-lowering strategies (`vecext`, `array`, `scalars`,
  `structarray`, `structscalars`); full v0.2.1 support in the WASM
  backend; per-lane vector SMT encoding in the solver.
- Reify: vector and aggregate-pointer generation with random
  per-program vec-lowering strategy selection.

## [v0.2.0] — 2026-05-23

Spec: [docs/SPEC_v0.2.0.md](./docs/SPEC_v0.2.0.md).

### Added — language

- **Pointers**: `ptr T` type, `addr` (requires a `let mut` root),
  `load` / `store`, context-typed `null`, pointer arithmetic
  (`ptr T ± iN`, `ptr T - ptr T` element distance), and strict
  provenance UB rules including rule 15 (struct-field provenance).
- Float `%` redefined from IEEE remainder to C `fmod` (truncated
  quotient) semantics; `shl` result overflow classified as
  signed-overflow UB.

### Added — toolchain

- **`rysmith`** (new tool): the C++ reify pipeline generating random
  leaf functions with pointer support, AoS/SoA type diversity, and
  `--target c/wasm` compilation of concrete output.
- Solver pointer support via tagged BV64 encoding; typed process exit
  codes with `EXPECT: FAIL:<subtype>` test-framework support;
  `symirc --no-require`.

## [v0.1.0] — 2026-05-18

Spec: [docs/SPEC_v0.1.0.md](./docs/SPEC_v0.1.0.md) (the draft v0 spec
renamed under semantic versioning). First complete implementation of
the language and toolchain:

### Added

- **Language core**: non-SSA CFG-based IR with mutable locals
  (`let mut`), explicit symbols (`@?x`, `%?y`), flat left-to-right
  expressions, lazy `select`, strict UB (division by zero, signed
  overflow, out-of-bounds access, read of `undef`), `as` casts,
  bitwise operators, hex/octal/binary literals, multidimensional
  arrays with brace initialization, and finite-only `f32`/`f64`
  floating-point.
- **`symiri`** reference interpreter with strict UB checks and
  execution tracing.
- **`symirc`** compiler with C and WebAssembly (WAT) backends.
- **`symirsolve`** SMT concretizer: path-based symbolic execution,
  abstract solver interface with Bitwuzla and AliveSMT (Z3) backends,
  random path sampling with multi-threading.
- Frontend stack: lexer, recursive-descent parser, CFG builder,
  BV-aware typechecker, semantic checker with definite-initialization
  analysis, pass manager and dataflow framework, clang-style
  diagnostics.
- Examples (ciphers, sorts, robot navigator, …), the `.sir` VS Code
  extension, and the automated test suite.

## [v0.0.1] — 2026-01-23

Repository bootstrap: the draft v0 language specification
(`SPEC_v0.md`, later `SPEC_v0.1.0.md`), design documents for
`symirc` / `symiri` / `symirsolve`, and project guides. No
implementation yet.
