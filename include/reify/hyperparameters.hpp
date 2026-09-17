#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <iterator>

// Central place to manage reify's *code-level* tunable hyperparameters.
//
// Scope:
//   - Probabilities / slot frequencies for atom-kind, type-kind, and var-phase
//     choices.
//   - Pools of "interest" literal values that the random generator draws from.
//   - Per-width concrete-literal coefficient ranges.
//
// Not in scope (these live elsewhere as struct defaults or CLI-tunable flags):
//   - n-funcs, n-vars, n-stmts, n-bbls, n-concretes, max-retries, max-loop-iter
//   - p-branch, p-backedge, timeouts, RNG seed
//   - coef-domain / value-domain / index-domain (symbolic-coef domains)
//   - max-ptr-depth, max-agg-nest, max-agg-elems
//
// Editing a value here changes behaviour for all rysmith runs without
// touching any CLI default.
namespace refractir::reify::rysmith::hp {

  // ===========================================================================
  // Atom-kind slot tables for genIntAtomOnPath / OffPath and genFloatAtomOnPath.
  //
  // Each generator draws s in [0, 99] and walks a series of "if (s < THRESH)"
  // checks. The slot reserved for a kind is [previous_thresh, this_thresh),
  // i.e. its share is (this_thresh - prev_thresh) percent. Order matters: the
  // first matching slot wins, with the final fallthrough used when no slot
  // matches (e.g. when a required precondition like "has an rvalue" fails).
  // ===========================================================================

  // --- genIntAtomOnPath: uses sym for coefs ---
  inline constexpr int kIntOnPath_CoefBareEnd = 40;  // standalone sym Coef
  inline constexpr int kIntOnPath_MulEnd = 60;       // sym * rval
  inline constexpr int kIntOnPath_BitwiseEnd = 70;   // sym & | ^ rval
  inline constexpr int kIntOnPath_ShiftEnd = 75;     // sym <</>>/>>> rval
  inline constexpr int kIntOnPath_UnaryNotEnd = 80;  // ~ rval
  inline constexpr int kIntOnPath_CastEnd = 85;      // var as T
  inline constexpr int kIntOnPath_DivModEnd = 90;    // sym / % rval (+ guard)
  inline constexpr int kIntOnPath_LoadEnd = 93;      // load %p
  inline constexpr int kIntOnPath_SelectEnd = 96;    // select cond, a, b
  inline constexpr int kIntOnPath_IntrinsicEnd = 99; // call @intrinsic(args)
  // fallthrough → standalone sym Coef

  // --- genIntAtomOffPath: concrete coefs only, no syms ---
  inline constexpr int kIntOffPath_ConcreteEnd = 25;  // bare int literal
  inline constexpr int kIntOffPath_MulEnd = 45;       // coef * var
  inline constexpr int kIntOffPath_BitwiseEnd = 55;   // coef & | ^ var
  inline constexpr int kIntOffPath_ShiftEnd = 63;     // coef << >> >>> var
  inline constexpr int kIntOffPath_CastEnd = 68;      // var as T
  inline constexpr int kIntOffPath_DivModEnd = 73;    // coef / % var
  inline constexpr int kIntOffPath_PlainRvalEnd = 78; // bare lvalue
  inline constexpr int kIntOffPath_LoadEnd = 85;      // load %p
  inline constexpr int kIntOffPath_SelectEnd = 93;    // select cond, a, b
  inline constexpr int kIntOffPath_IntrinsicEnd = 97; // call @intrinsic(args)

  // Probability that a drawn integer type is a custom width instead
  // of the standard i8/i16/i32/i64. The SPEC admits any `iN`; the
  // toolchain implements them via widen-and-mask in the backends — a
  // bug surface (cf. the crc32 sub-byte fix) that standard widths never
  // touch. Widths stay ≥ 12 so every literal pool in this file fits the
  // type's range, and the mix includes non-multiples of 8 (sub-byte
  // masking) alongside multi-byte customs.
  inline constexpr double kPOddIntWidth = 0.15;
  inline constexpr uint32_t kOddIntWidths[] = {12, 20, 24, 40, 48};
  inline constexpr std::size_t kOddIntWidthsSize = std::size(kOddIntWidths);

  // Probability that an off-path OpAtom coefficient is a same-type
  // LocalId (`%a * %b`) instead of a literal. Var-op-var is grammar-legal
  // for every operator, compiler-opaque (both sides are runtime values),
  // and solver-free off-path. On-path ops never use var coefs — var*var
  // is a nonlinear BV term.
  inline constexpr double kPOffPathVarCoef = 0.4;

  // Off-path FP atom slots. FP `/` and `%` (fmod) are typecheck-legal
  // (floats admit Mul / Div / Mod) but were never generated; off-path they
  // cost the solver nothing and cannot diverge interpreter-vs-backend
  // because the block never executes.
  inline constexpr int kFloatOffPath_ReadEnd = 35;      // bare same-type read
  inline constexpr int kFloatOffPath_MulEnd = 55;       // coef * var
  inline constexpr int kFloatOffPath_DivEnd = 68;       // coef / var
  inline constexpr int kFloatOffPath_ModEnd = 78;       // coef % var (fmod)
  inline constexpr int kFloatOffPath_IntrinsicEnd = 84; // call @intrinsic(args)
  // rest: concrete float literal
  // fallthrough → concrete int literal

  // --- genFloatAtomOnPath ---
  inline constexpr int kFloatOnPath_CastFromI32SymEnd = 50; // sym as fT
  inline constexpr int kFloatOnPath_MulLitEnd = 70;         // float lit * var
  inline constexpr int kFloatOnPath_CastFromVarEnd = 85;    // i32 var as fT
  inline constexpr int kFloatOnPath_SelectEnd = 95;         // select cond, a, b
  inline constexpr int kFloatOnPath_IntrinsicEnd = 98;      // call @intrinsic(args)
  // fallthrough → concrete float literal


  // ===========================================================================
  // Statement-level shape
  // ===========================================================================

  // Per-AssignInstr probability that one or more StoreInstrs are spliced in
  // *before* it. Rolled as a Bernoulli chain: each successful roll emits a
  // store and rolls again, so a single assignment slot may carry several
  // stores. Stores do not count against the `--n-stmts` budget — the budget
  // is the number of `AssignInstr`s per block, and stores are an independent
  // side stream meant to add aliasing for the compiler to chew on. Charging
  // them to the same budget would let a heavy store density starve the
  // assignment count.
  inline constexpr double kPStoreBeforeAssign = 0.25;

  // How many LHS picks `genBlockStmts` tries before giving up on the
  // current `nStmts` slot. The slot can fail to emit an AssignInstr when
  // the rolled LHS var is an aggregate with no usable scalar leaf
  // (array-of-nested-array, struct-with-only-ptr-fields). Without
  // retries those slots silently drop, so `--n-stmts N` produced
  // visibly fewer than N AssignInstrs per block. A handful of retries
  // pushes the skip rate from ~20% per slot to under 1‰.
  inline constexpr int kAssignTargetMaxAttempts = 5;

  // Probability that a generated RHS expression is allowed to keep a
  // fully-literal shape (`%x = 3;`, `%x = 3 + 5 - 2;`, `%x = 3 as i64;`,
  // and any expression where no atom reads a runtime LValue). Pure-
  // literal RHSs fold flat under SCCP, then DCE removes the def, and
  // the assignment contributes nothing to the compiler's optimization
  // surface. The complement (default 95%) of these expressions has one
  // atom rerolled until it carries a runtime data dependency. The
  // small `kPAllowAllLiteral` slice exists to keep the solver loose
  // when tight path constraints make non-trivial RHSs hard to satisfy.
  inline constexpr double kPAllowAllLiteral = 0.05;

  // Probability that a single bare-RValueAtom RHS (`%x = %y;`) survives
  // instead of being reshaped into a non-trivial expression. Copy
  // propagation is a distinct dataflow shape the optimizer exercises —
  // forbidding it outright homogenises every def into an arithmetic
  // join. A low rate reintroduces the shape without letting the body
  // collapse in bulk under SCCP; self-assigns stay impossible (the LHS is
  // excluded from every RHS pool).
  inline constexpr double kPAllowPlainCopy = 0.05;

  // Probability that a `ptr T_scalar` LHS reassign emits a pointer-
  // arithmetic RHS instead of the default single-atom `genPtrAtom` form.
  // Two provably in-bounds shapes (see genPtrArithRhs), drawn uniformly
  // across whatever sources are in scope; the slot falls through when
  // none exist:
  //   - array:  `ptrindex %ap, b ± d` off `%ap : ptr [N] T_scalar`, with
  //             b and b±d in [0, N-1] (real, loadable elements);
  //   - struct: `ptrfield %sp, f ± d` off `%sp : ptr @S`, stepping across
  //             a run of consecutive same-type fields so the result lands
  //             on a same-typed (loadable) field cell.
  // Both keep provenance inside the originating aggregate and use a
  // non-zero literal offset (so they also exercise the `ptr - iN` path).
  // A bare scalar `%p ± d` is NOT emitted: reify's default scalar ptr is
  // `addr %scalar`, a single-element object with no room to step.
  inline constexpr double kPPtrArith = 0.25;

  // Within the ON-PATH ptr-arith slot above, probability of emitting the
  // DIRECT pointer-variable form (`%p2 = %p1 ± d`) by reusing a pointer
  // already anchored to an array element earlier in the block, rather than
  // the combined ptrindex/ptrfield expression. The reused anchor pins %p1's
  // provenance and literal index, so `i ± d` is bounds-checkable locally
  // (def-use). This is the only on-path shape exercising `ptr_var ± iN` (a
  // pointer read from a variable, not a fresh navigation); it falls back to
  // the expression form when no live anchor exists.
  inline constexpr double kPPtrArithVarShare = 0.40;

  // Off-path (unexecuted) blocks are never run and never symbolically
  // constrained, so their pointer arithmetic can be arbitrary: any pointer
  // plus any offset, with no in-bounds reasoning (the result is never
  // dereferenced or UB-checked). The off-path ptr-arith slot emits
  // `<ptr atom> ± <stride>` with a stride drawn uniformly from
  // [1, kOffPathPtrStrideMax] — deliberately ranging past any array bound so
  // it stresses pointer/alias codegen the in-bounds on-path forms can't,
  // at zero solver cost.
  inline constexpr int64_t kOffPathPtrStrideMax = 64;

  // Probability that a `ptr F` (loadable F) reassign navigates an aggregate
  // to its leaf via a chain of >= 2 staged ptrindex/ptrfield steps
  // (tryEmitNavChain) — `ptr [N][M] F`, `ptr [N] @S`, struct-of-array, any
  // mix — when an aggregate-pointer source and the needed staging locals are
  // in scope (the var catalogue injects the latter). Plain in-bounds
  // navigation, not arithmetic, so NOT gated by --no-ptrarith; rolled after
  // the arithmetic slot declines, leaving that stream undisturbed.
  inline constexpr double kPNavChain = 0.50;

  // How many times `genExpr` / `genExprWithRequires` tries to roll a
  // non-trivial atom before giving up and accepting whatever the gen
  // returns. With the ~40-50% trivial rate of the on/off-path atom
  // dispatch tables, 5 retries pushes the post-check failure rate to
  // (~0.5)^5 ≈ 3% — the residual leak that survives alongside the
  // intentional `kPAllowAllLiteral` allowance.
  inline constexpr int kAtomRerollMaxAttempts = 5;

  // 50/50 between `+` and `-` when joining tail atoms in an expression.
  // The lone `coin(0, 1)` site in `genExpr` / `genExprWithRequires`
  // — kept as a named constant so a future "favour `-` for sign-bit
  // diversity" experiment doesn't need to grep for `coin(0, 1)`.
  inline constexpr double kPTailAddOpIsPlus = 0.5;

  // ===========================================================================
  // SelectVal arm distribution.
  //
  // `pickSelectVal` rolls a single `kind ∈ {0, 1, 2}` per arm:
  //   0 → same-typed scalar local (an LValue read)
  //   1 → fresh `nextValue(targetType)` sym (skipped for FP target types)
  //   2 → literal from the per-width concrete-int pool / kFloatLitPool
  // The default 1/3 / 1/3 / 1/3 split mirrors the original `kind(0, 2)`
  // uniform draw; exposing it lets a future experiment skew toward the
  // local arm (more cross-statement live-variable use) without touching
  // the implementation.
  // ===========================================================================
  inline constexpr int kSelectArm_LocalEnd = 33; // [0, 33)  → local
  inline constexpr int kSelectArm_SymEnd = 66;   // [33, 66) → sym, else lit

  // ===========================================================================
  // Type-kind probabilities in genRandomType (sum normalised at use-site).
  // depth-0 distribution; pArray/pStruct are zeroed past maxAggNesting and
  // pPtr is zeroed past maxPtrDepth.
  // ===========================================================================
  inline constexpr double kPTypeScalar = 0.50;
  inline constexpr double kPTypeArray = 0.20;
  inline constexpr double kPTypeStruct = 0.15;
  inline constexpr double kPTypePtr = 0.15;

  // Vector type probability at depth 0. Zeroed if enableVec=false.
  inline constexpr double kPTypeVec = 0.15;

  // When generating a pointer, probability the pointee is an
  // aggregate (array or struct) rather than a scalar. Zeroed if
  // enableAggPtr=false.
  inline constexpr double kPPtrAgg = 0.30;

  // ===========================================================================
  // Var catalogue split: fraction of cfg.nVars allocated to each phase.
  // The remainder after non-ptr + ptr1 goes to ptr-of-ptr vars.
  // ===========================================================================
  inline constexpr double kFracNonPtrVars = 0.65;
  inline constexpr double kFracPtr1Vars = 0.20;
  // remainder → ptr-of-ptr vars (~15%)

  // Fraction of nVars allocated to vec-typed locals.
  inline constexpr double kFracVecVars = 0.10;
  // Fraction of nVars allocated to aggregate-pointer locals.
  inline constexpr double kFracAggPtrVars = 0.10;

  // ===========================================================================
  // Vec atom slot thresholds for genExpr vec-target dispatch.
  // Slots: [0, kVecCopyEnd) = whole-vec copy
  //        [kVecCopyEnd, kVecSymMulEnd) = sym * vec (on-path only)
  //        [kVecSymMulEnd, kVecConcMulEnd) = concrete coef * vec
  //        [kVecConcMulEnd, 100) = fallback copy
  // ===========================================================================
  inline constexpr int kVecCopyEnd = 50;
  inline constexpr int kVecSymMulEnd = 70;
  inline constexpr int kVecConcMulEnd = 85;

  // Vec lane-write probability in genBlockStmts when LHS is vec.
  // When canWholeVec is true: probability of lane write vs whole-vec assign.
  inline constexpr int kVecLaneWriteProb = 40; // 40% lane write, 60% whole-vec

  // Vec tail-atom dispatch (`genExpr` vec branch, i > 0):
  //   [0, kVecTailCopyEnd) → vec-typed RValueAtom copy
  //   [kVecTailCopyEnd, kVecTailOpEnd) → OpAtom{Mul, concrete-coef, vec var}
  //   [kVecTailOpEnd, 100)  → broadcast scalar literal (drawn from the
  //                           per-width concrete-int pool / kFloatLitPool;
  //                           pre-consolidation this was a hardcoded
  //                           `[-8, 8]` uniform draw).
  inline constexpr int kVecTailCopyEnd = 40;
  inline constexpr int kVecTailOpEnd = 60;

  // Range for the concrete int Mul coef on a vec-typed atom
  // (`coef * %vec`). Narrow on purpose: large vec multipliers blow up
  // FMA contraction and break differential testing on lane-wise FP.
  inline constexpr std::int64_t kVecConcMulLo = -4;
  inline constexpr std::int64_t kVecConcMulHi = 4;

  // Probability that a struct field is an array (struct-of-arrays pattern).
  inline constexpr double kPStructFieldIsArray = 0.30;

  // ===========================================================================
  // Concrete int-literal coefficient ranges per bitwidth in genConcreteIntAtom.
  // Each entry is the inclusive [lo, hi] range used for that exact width and
  // covers the type's full representable signed range, so every width gets
  // equal access to boundary values (sign bit, INT_MIN-style wrap, etc.).
  //
  // These bound only the *literal* pool; symbolic coefs are bounded separately
  // by the --coef-domain / --value-domain / --index-domain CLI flags.
  // ===========================================================================
  inline constexpr std::int64_t kConcreteInt_I8_Lo = -128;
  inline constexpr std::int64_t kConcreteInt_I8_Hi = 127;
  inline constexpr std::int64_t kConcreteInt_I16_Lo = -32768;
  inline constexpr std::int64_t kConcreteInt_I16_Hi = 32767;
  inline constexpr std::int64_t kConcreteInt_I32_Lo = -2147483648;
  inline constexpr std::int64_t kConcreteInt_I32_Hi = 2147483647;
  inline constexpr std::int64_t kConcreteInt_I64_Lo = INT64_MIN;
  inline constexpr std::int64_t kConcreteInt_I64_Hi = INT64_MAX;
  // Used when targetType has no recognised width (shouldn't normally happen).
  inline constexpr std::int64_t kConcreteInt_Default_Lo = -4;
  inline constexpr std::int64_t kConcreteInt_Default_Hi = 8;

  // ===========================================================================
  // let-init value diversity (func_gen::makeInitVal).
  //
  // Scalar locals are seeded with a weighted mixture rather than a constant
  // `1`, so two generated programs start from different state and the C
  // optimizer can't fold every local to a known constant. The integer draw is
  // a three-bucket mixture over a [0,1) roll:
  //
  //   [0, pSmall)           "small"    — uniform in [-SmallMag, SmallMag],
  //                                      clamped to the type range. Dominant
  //                                      bucket; keeps most inits readable and
  //                                      SMT-friendly.
  //   [pSmall, pSmall+pMid) "mid"      — uniform over the full signed range of
  //                                      the type, supplying large-magnitude
  //                                      diversity (concrete constants only —
  //                                      no solver-variable / nonlinearity cost).
  //   [pSmall+pMid, 1)      "boundary" — type extremes and sign edges
  //                                      (0, ±1, min, max, min+1, max-1).
  //
  // Floats reuse kFloatLitPool (every entry is dyadic, bit-exact, and
  // f32-representable, so the FP differential-testing invariant still holds).
  inline constexpr double kInitInt_pSmall = 0.60;
  inline constexpr double kInitInt_pMid = 0.30; // remaining 0.10 → boundary
  inline constexpr std::int64_t kInitInt_SmallMag = 256;

  // ===========================================================================
  // Float literal pools.
  //
  // SAFETY-CRITICAL — DO NOT REPLACE WITH uniform_real_distribution.
  //
  // reify's output is differentially tested against compiled C/WASM. If a
  // generated FP expression isn't bitwise-deterministic across the RefractIR
  // interpreter, GCC (with/without FMA contraction), and WASM, we'd flag
  // legitimate rounding non-determinism as a compiler bug.
  //
  // Every entry below is a *dyadic rational* (p / 2^q) or a small odd int.
  // Sums, differences, products, and divisions of such values are themselves
  // dyadic and stay exactly representable in IEEE-754 binary64 until the
  // mantissa overflows — which doesn't happen for the 1–3 atom expressions
  // reify produces. Adding any non-dyadic value (e.g. 0.1, 0.3) here would
  // break differential testing.
  // ===========================================================================
  // Bare-literal float Coef pool (genConcreteFloatAtom).
  // Extended with non-power-of-2 dyadic rationals (1.5 = 3/2,
  // 2.5 = 5/2, 3.75 = 15/4, 1.125 = 9/8, 0.375 = 3/8, 1.75 = 7/4,
  // 0.625 = 5/8). Each is bit-exact across RefractIR / GCC / WASM and
  // composes cleanly with the existing power-of-2 entries; the extension
  // breaks the previous pure-power-of-2 pattern that the compiler could
  // recognise as `pow2 * var`-style strength-reduce candidates.
  inline constexpr double kFloatLitPool[] = {
      0.0,   1.0,   -1.0,   2.0,    -2.0,   4.0,  -4.0,  8.0,   -8.0,   0.5,  -0.5,
      0.25,  -0.25, 0.125,  -0.125, 3.0,    -3.0, 1.5,   -1.5,  2.5,    -2.5, 3.75,
      -3.75, 1.125, -1.125, 0.375,  -0.375, 1.75, -1.75, 0.625, -0.625,
  };
  inline constexpr std::size_t kFloatLitPoolSize = sizeof(kFloatLitPool) / sizeof(kFloatLitPool[0]);

  // Multiplicative float coefs used in float * var atoms (genFloatAtomOnPath
  // Mul slot). 0.0 is intentionally omitted to avoid trivial zero terms.
  // Small odd ints (3, 5) are dyadic-exact themselves; products like 3*5=15
  // stay exact until precision overflow.
  // Extended with the same non-power-of-2 dyadics as kFloatLitPool.
  inline constexpr double kFloatMulCoefPool[] = {
      2.0, -2.0, 4.0, -4.0, 8.0,  -8.0,  0.5,   -0.5,   0.25,  -0.25,  3.0,  -3.0,  5.0,   -5.0,
      1.5, -1.5, 2.5, -2.5, 3.75, -3.75, 1.125, -1.125, 0.375, -0.375, 1.75, -1.75, 0.625, -0.625,
  };
  inline constexpr std::size_t kFloatMulCoefPoolSize =
      sizeof(kFloatMulCoefPool) / sizeof(kFloatMulCoefPool[0]);

  // Cheap-linear replacement atoms (genCheapLinearAtom). When the
  // trivial-shape rewrite fires on-path, the replacement is a concrete-coef
  // multiply or a plain read instead of a slot-table reroll (whose
  // non-trivial outcomes are dominated by solver-expensive `sym * var`).
  // The int coef range is kept moderate so the on-path no-overflow
  // constraint `|coef * var| < 2^(N-1)` stays loose; 0 is rerolled away
  // (a `0 * var` term folds and drops the read).
  inline constexpr std::int64_t kCheapMulCoefLo = -16;
  inline constexpr std::int64_t kCheapMulCoefHi = 16;
  // Slot split inside genCheapLinearAtom: [0, kCheapAtom_MulEnd) → coef*var,
  // rest → plain same-type read.
  inline constexpr int kCheapAtom_MulEnd = 50;

  // ===========================================================================
  // Output naming
  //
  // Per-function artefacts share a common stem of the form
  // `<func-prefix>_<gen-id>_<i>` (e.g. `func_ab12cd_42`).  Concrete
  // .sir files append an optional init-letter (`a`..`z`) and the .sir
  // extension; the descriptor JSON sidecar is `<stem>.json`; the
  // compile-to-target output is `<stem><letter>.c` or `.wat`.
  //
  // The `--keep-symbolic` switch additionally emits pre-solve sources
  // named `<stem><sym-infix><N>.sir` (e.g. `func_ab12cd_42_sym0.sir`).
  // These files are intentionally NOT compiled to .c — downstream
  // cleanup logic must skip them when sweeping for compile orphans.
  //
  // Centralising the strings here means the generator and any external
  // consumer (test harnesses, cleanup sweeps, rylink bundlers) agree on
  // the layout without re-deriving it from scattered string literals.
  // ===========================================================================
  inline constexpr const char *kFuncPrefix = "func";
  inline constexpr const char *kSymInfix = "_sym";

} // namespace refractir::reify::rysmith::hp

// Central place to manage rylink's *code-level* tunable hyperparameters.
//
// Scope:
//   - Call-graph shape probabilities and out-degree caps.
//   - Rewrite engine acceptance probabilities and per-edge attempt caps.
//   - Fixed naming conventions for output artifacts (prog dir prefix,
//     entry .sir filename).
//
// Not in scope (these live elsewhere as struct defaults or CLI-tunable flags):
//   - n-progs, n-nodes, max-outdeg, RNG seed, generation id
//   - --target / --validate / --keep-require / I-O paths
//
// Editing a value here changes behaviour for all rylink runs without
// touching any CLI default.
namespace refractir::reify::rylink::hp {

  // ===========================================================================
  // Call-graph shape
  //
  // rylink builds a DAG over N nodes drawn from the pool. N comes from the
  // CLI (--n-nodes) and is shrunk to fit when the pool is smaller. The DAG
  // uses a fixed topological order: for every ordered pair (i, j) with
  // i < j we add edge i → j with probability kPEdge, capped so no node ever
  // exceeds kMaxOutDegree out-edges. The ordering guarantees acyclicity
  // (no recursion); the cap keeps individual callers from sprouting
  // unrealistically wide fan-outs.
  // ===========================================================================
  inline constexpr double kPEdge = 0.45;  // per-ordered-pair edge probability
  inline constexpr int kMaxOutDegree = 3; // hard out-degree cap per node

  // ===========================================================================
  // Anti-optimization
  //
  // How many rewrites the catalog may apply per block of a bundled program.
  // This is not rytwin's budget and should not be: rytwin spends its on one
  // region's trace, where a body that reads like its region is the one failure
  // mode that matters, while here every block of every function pays — and the
  // program that comes out is what a compiler has to chew through. Each kept
  // rewrite adds a statement or three, so the bundle grows by roughly this
  // many statements per block.
  // ===========================================================================
  inline constexpr std::size_t kAntiOptAttemptsPerBlock = 4;

  // ===========================================================================
  // Rewrite engine
  //
  // Per caller→callee edge, the engine enumerates rewrite *sites* in the
  // caller AST, filters them to the candidates whose callee matches, shuffles
  // them, and splices the first one that applies. An edge therefore lands at
  // exactly one site on the caller's executed path, drawn uniformly among
  // those that fit, and kMaxAttemptsPerEdge bounds the retries a candidate
  // that fails to apply is allowed.
  //
  // The one-site rule is a bound on the whole bundle, not a per-caller taste
  // setting. Calls made per activation multiply along every path through the
  // call graph, so admitting a second site on one edge does not add a call, it
  // adds a factor — and the composed program's cost grows exponentially in the
  // graph's depth instead of with its size.
  // ===========================================================================
  inline constexpr int kMaxAttemptsPerEdge = 32; // hard work budget per edge

  // Probability that an edge also gets a call site on the caller's executed
  // path. Every edge gets one in an unexecuted block regardless, so the
  // emitted call graph is the sampled DAG exactly either way; what this coin
  // decides is what the program pays for the edge at run time.
  //
  // That is the whole cost model. A caller's activations multiply by its
  // executed out-degree at every level of the graph, so the calls a run makes
  // go as (out-degree · p) raised to the graph's depth. With kMaxOutDegree 3
  // the process turns from dying out to compounding at p = 1/3, and the
  // measured cost at 64 nodes moves over four orders of magnitude as p goes
  // from 0.4 to 0.7. It is a steep knob, and it is the only one: nothing
  // downstream bounds a program whose branching already exceeds one.
  //
  // At 0.55 a 64-node bundle runs a few thousand calls and executes about
  // three quarters of its functions. The rest are compiled but never reached,
  // so a code-generation bug in them still has to be handled correctly while a
  // wrong *answer* there stays out of the checksum oracle's view. Lowering it
  // trades that reach for cost, steeply.
  inline constexpr double kPOnPathCall = 0.55;

  // Per call-argument choice between the two argument modes
  // Whether one call argument is stated in terms of what the caller holds
  // where the call lands, rather than as the value the solver picked. A plain
  // literal is the control case every richer construction is measured against,
  // so this is what keeps some of them in the output — and it is the only
  // place that decision is made, so the policies below are peers that either
  // state the target or pass.
  inline constexpr double kPReplaceParam = 0.85;

  // ===========================================================================
  // Dataflow policies
  //
  // How an argument is stated in terms of what the caller holds where the call
  // lands (reify/dataflow_policy.hpp). The knobs below bound the size of the
  // constructions, not their correctness: every one of them, at any setting,
  // yields an argument that evaluates to the callee's solved input.
  // ===========================================================================

  // How many of the caller's variables one argument may read. More of them
  // lowers the polynomial degree a given number of constraints needs, so a
  // wider probe is not a longer expression; what it costs is legibility.
  inline constexpr std::size_t kMaxProbeVars = 3;

  // Samples drawn to convince the xor policy its mask is non-zero somewhere.
  // A mask that vanishes everywhere leaves the target spelled out, and only a
  // sample can tell, since the domain is the whole width.
  inline constexpr int kMaskSamples = 8;

  // The smallest prime field worth interpolating over. The field shrinks with
  // the argument's width — 46337 at i32, 181 at i16, 11 at i8 — and below this
  // there are too few residues for the polynomial to say anything.
  inline constexpr std::int64_t kMinModulus = 11;

  // The most constraint points an interpolation will carry. Each one is
  // another monomial to evaluate in the emitted code, so a site whose block is
  // entered more often than this is left to a cheaper construction.
  inline constexpr std::size_t kMaxConstraintPoints = 5;

  // Redraws allowed when a monomial set gives a singular system, or when an
  // off-point lands on a state the run already visits.
  inline constexpr int kSolveAttempts = 8;

  // A ceiling on the total degree the monomial basis is grown to. It is a
  // backstop against an unbounded search, not a tuning choice: kMaxConstraintPoints
  // monomials are reached far below it at every probe width.
  inline constexpr int kMaxMonomialDegree = 16;

  // ===========================================================================
  // Per-program retry budget
  //
  // generateOne is deterministic given its rng, but each fork can still
  // hit a non-rewrite-related dead end downstream (e.g. an `--validate`
  // run that trips runtime UB inside off-path code spliced into a callee
  // via a now-on-path branch). 3 attempts is a small budget that
  // tolerates the occasional rng-induced miss without masking systemic
  // bugs (which would fail every fork and still surface).
  // ===========================================================================
  inline constexpr int kMaxAttemptsPerProg = 3;

  // ===========================================================================
  // Output naming
  //
  // The per-program output directory is `<prog-prefix>_<id>_<i>` (e.g.
  // `prog_ab12cd_0`). The bundled entry-program .sir inside that directory
  // is always `kEntrySirName`. These are exposed as named constants so the
  // rylink driver and any external test/inspection tool agree on the
  // layout without re-deriving it from string literals.
  // ===========================================================================
  inline constexpr const char *kProgPrefix = "prog";
  inline constexpr const char *kEntrySirName = "program.sir";

} // namespace refractir::reify::rylink::hp

// Central place to manage rytwin's *code-level* tunable hyperparameters.
//
// Scope:
//   - Budgets bounding how much work one twin site may cost.
//   - Parameters of the region-selection policies.
//
// Not in scope (these live elsewhere as struct defaults or CLI-tunable flags):
//   - p-twin, RNG seed, I-O paths, --validate / --verbose
//
// Editing a value here changes behaviour for all rytwin runs without
// touching any CLI default.
namespace refractir::reify::rytwin::hp {

  // ===========================================================================
  // Probing
  //
  // A probe runs one region on one state. The region's own blocks are few,
  // but a probed state can drive its loop far longer than the profiled one
  // did — or forever — so every run is bounded. This caps block entries, not
  // wall time, so it is deterministic across machines.
  // ===========================================================================
  inline constexpr std::uint64_t kProbeStepCap = 4096;

  // ===========================================================================
  // Region selection
  //
  // interestingPolicy tilts the twin probability toward the regions that are
  // harder to prove equivalent, via a softmax over the candidate scores. The
  // temperature is a middling tilt: it favours the harder regions without
  // collapsing to argmax, and as temp -> infinity the policy degenerates to
  // the uniform p-twin coin.
  // ===========================================================================
  inline constexpr double kInterestingTemp = 0.5;

  // ===========================================================================
  // Guard box
  //
  // How far the state-set pass is asked to widen a guard. Rounds bound the
  // lockstep doubling (each is one pass over the trace, so this is cheap); the
  // trim is a seeded percentage shaved off each proven run, because a maximal
  // run is the same run every time and makes guards recognizable.
  // ===========================================================================
  inline constexpr std::size_t kTwinBisectMaxRounds = 24;
  inline constexpr int kTwinBoxTrimPct = 25;

  // States sampled inside a guard's box by --validate. Corners come first
  // (a bound is where a box is most likely to be wrong), then interior points.
  inline constexpr std::size_t kTwinSpotChecks = 32;

  // ===========================================================================
  // Body disguise
  //
  // How hard a twin body is rewritten before it is grafted. Each application
  // costs one state-set pass over the body (to re-check it) and is rolled back
  // if that fails, so these bound work, not risk.
  // ===========================================================================
  inline constexpr std::size_t kTwinRewriteRounds = 3;
  inline constexpr std::size_t kTwinRewritesPerRound = 8;

} // namespace refractir::reify::rytwin::hp

// Central place to manage antiopt's *code-level* tunable hyperparameters.
//
// Scope:
//   - Self-test harness width for the catalog's self-check.
//
// Not in scope (these live elsewhere as struct defaults or CLI-tunable flags):
//   - Rule families, trap tiers, per-rule rewrite logic
//
// Editing a value here changes behaviour for all antiopt self-tests without
// touching any rule body.
namespace refractir::reify::antiopt::hp {

  // Bit-width the self-test harness uses for every rule example.  The harness
  // declares `%x/%y/%d/%e/%k` and `%pa0` as `iN` where `N` is this width, and
  // each rule's `selfTest(bits)` receives the same `N` to keep its example
  // type-consistent.  Changing this from 8 to 16 (or 32) makes the whole
  // catalog parametric without touching any rule body.
  inline constexpr int kSelfTestWidth = 8;

} // namespace refractir::reify::antiopt::hp
