# Semantic Reification and Reify

Reify's technique is called *Semantic Reification*, a paradigm for random program generation. Unlike syntactic reification, which operates primarily on syntax, semantic reification centers on program semantics. It distinguishes between two kinds of semantics: compile-time semantics (what a program *can* do) and runtime semantics (what a program *actually does*). The key insight is reformulating random program generation to capture both:

Given an *arbitrary* control flow graph (CFG) $g$ to capture compile-time semantics and an *arbitrary* entry-to-exit path $\pi$ within $g$ (called an execution path or EP) to capture runtime semantics, Reify produces a program $P$, input $i$, and output $o$ satisfying:

1. $P$ is both syntactically and semantically correct for $i$;
2. $g$ corresponds to the CFG of $P$;
3. executing $P(i)$ deterministically follows $\pi$ and produces $o$.

**Why this matters for compiler testing.** Although runtime semantics are fixed for a given input, compilers must reason about all possible executions when optimizing. Semantic reification exposes bugs in that reasoning while guaranteeing every generated program behaves deterministically and is free of undefined behavior *on the specified input*. Allowing arbitrary CFGs and EPs produces complex data flows and diverse control structures, enriching the behaviors available for compiler optimization passes. Compared to existing generators, Reify: (1) inherently supports arbitrary control flow including unbounded loops and irreducible regions; (2) ensures well-definedness and guaranteed termination under the generated input; (3) produces an expected output, enabling direct correctness validation without pseudo-oracles.


## Implementation

Given $g$ and $\pi$, Reify populates each basic block with random statements and jump terminators, then uses *symbolic execution* to derive a path condition and compute an input $i$ that forces $P$ to follow $\pi$ and produce $o$. The symbolic execution explores only the single EP $\pi$, avoiding the path explosion of full symbolic execution.

Reify separates *leaf function generation* (compact functions with no calls) from *whole-program generation* (combining leaf functions into programs with arbitrary call graphs). This document describes the current leaf function generation pipeline and the `rysmith` tool that implements it and the `rylink` whole-program generator.


## Leaf Function Generation

```
S1. CFG Generation   — random control-flow skeleton
S2. Path Sampling    — random entry-to-exit walk through the CFG
S3. Program Seeding  — populate all blocks with typed statements using RefractIR
S4. Concretization   — solve symbolic variables along the EP via SMT
S5. Lowering         — emit concrete RefractIR, then lower to C / WASM
S6. Validation       — compile and execute; compare output to expected
```


### S1: CFG Generation

A random CFG is generated with a configurable number of interior blocks. The structure begins as a spanning chain (entry → b0 → … → b_{n−1} → exit), then stochastically adds branch edges (second successors pointing forward) and back edges (producing loops). The result is always connected with a guaranteed path to exit.

Back edges may land past a loop header and make the CFG **irreducible**. When reducible CFGs are required, the CFG is repaired: retreating edges whose target does not dominate their source are deleted, one per re-analysis pass, so every valid loop survives and only irreducible cycles are broken.


### S2: Path Sampling

An execution path is sampled by a random walk from entry to exit. Back edges are counted per traversal to bound loop iterations. If the walk gets stuck, BFS finds the shortest escape to exit. The path is a sequence of block labels, e.g.:

```
^entry → ^b0 → ^b3 → ^b0 → ^b4 → ^exit
```

The same CFG can yield many distinct paths with different loop iteration counts.


### S3: Program Seeding

This is the core generation step. Every block in the CFG is populated with typed statements using RefractIR. The generation distinguishes two roles:

**On-path blocks** (those appearing in $\pi$): statements use *symbolic variables* whose values will be determined by the SMT solver. Symbols are declared with domains and kind annotations (`coef`, `value`, `index`). Interest constraints — `require` statements that exclude trivial values like 0, 1, −1 from coefficients — push the solver toward diverse, non-degenerate programs.

**Off-path blocks** (those not in $\pi$): statements use *concrete random literals*. These blocks are never executed under the generated input — the solver pins every on-path branch, so control never enters an off-path successor — but the compiler still compiles them. Off-path code is therefore deliberately left unconstrained and may contain UB (division by a variable that could be zero, signed overflow from wide literals, out-of-bounds-capable accesses, etc.). Because off-path code never runs, this UB never reaches the differential oracle; it simply maximizes the diversity of IR presented to optimization passes such as DCE, alias analysis, and vectorization.

Because off-path volume costs the solver nothing, the volume knobs (`--n-stmts`, `--min-atoms`, `--max-atoms`) describe **on-path** blocks, and off-path blocks scale them by `--off-path-multiplier` (default 2×). This buys compiler-facing surface for free and lets on-path volume — the solver's bottleneck — be tuned independently.

#### Type system

Reify uses the full RefractIR type lattice. Each variable independently draws its type from:

| Category | Types |
|---|---|
| Integer scalars | `i8`, `i16`, `i32`, `i64` (and arbitrary `iN`) |
| Floating-point | `f32`, `f64` (disable with `--no-fp`) |
| Arrays | `[N] T` for any element type `T` (depth-bounded) |
| Structs | `@Name { f0: T0; f1: T1; … }` with heterogeneous field types |
| Pointers | `ptr T` for any `T`, including `ptr ptr T` chains |

Mixed types appear within the same function. Scalar type boundaries are crossed with explicit `CastAtom` nodes (sign-extension, truncation, integer-to-float, float-to-integer), which directly test compiler type promotion and narrowing paths.

Floating-point variables are initialized on-path by casting from an integer symbol (`(f32) %?s0`), keeping the SMT problem in BV theory. Off-path float code uses concrete literals.

#### Expression diversity

Expressions are generated *type-directedly*: given a target type `T`, the generator produces an `Expr` of type `T`. All atoms in a single `Expr` share the same type. The atom repertoire includes:

- `coef_sym * var` — linear with symbolic coefficient (on-path)
- `coef_sym & var`, `| var`, `^ var`, `<< var`, `>> var`, `lshr var` — bitwise / shift
- `~var` — bitwise NOT
- `(T) src` — explicit cast from another type
- `load ptr_var` — dereference a pointer variable
- `addr lv` — take the address of a local (produces `ptr T`)
- `select (cond) ? a : b` — lazy ternary (one level deep)
- `coef_sym / concrete_nonzero` — integer division with concrete denominator
- `coef_sym % concrete_nonzero` — integer modulo with concrete denominator

Division and modulo use concrete non-zero denominators on-path (e.g., `%?s3 / 7`), producing div-by-constant patterns that stress compiler strength-reduction. Off-path division uses any concrete literal including zero.

#### Pointer initialization

`addr lv` is an expression atom, not a valid `let` initializer. Pointer variables are therefore declared as `undef` and assigned in the entry block before any other generation:

```sir
fun @func0() : i32 {
  let mut %v0: i32 = %?s0;        // integer var, init from input sym
  let mut %p0: ptr i32 = undef;   // pointer var, init deferred
  let mut %pp0: ptr ptr i32 = undef;  // depth-2 pointer, init deferred
  ...
^entry:
  %p0 = addr %v0;                 // concrete address assignment
  %pp0 = addr %p0;                // ptr ptr chain
  require %?s0 != 0, "nonzero input";
  ...
```

Since `^entry` is always the first block on every path, this guarantees definite initialization for all pointer variables regardless of which path is sampled.

#### On-path coef symbols

Symbolic coefficients are typed to match the expression context. An expression of type `i64` uses a `coef i64` symbol; one of type `i32` uses a `coef i32` symbol. This produces more natural programs (a 64-bit multiply with a 64-bit coefficient) and tests type-specific optimization patterns.


### S4: Concretization

`symirsolve` (or the in-process `SymbolicExecutor` when using `rysmith`) performs path-directed symbolic execution along $\pi$:

1. Executes each on-path block symbolically, collecting:
   - Path conditions from branch terminators
   - `require` constraints (interest constraints, UB guards)
   - Computation results for each assignment
2. Encodes everything as SMT constraints in bitvector theory
3. Calls Bitwuzla to find a satisfying assignment for all symbols
4. Substitutes the model into the program via `SIRPrinter`, emitting a fully concrete `.sir`

The off-path blocks pass through untouched — their concrete literals need no solving.

Multiple concretizations of the same symbolic template (different solver seeds, or re-generation with a different RNG seed) produce structurally similar programs with different numeric values, exploring distinct optimization opportunities from the same control-flow structure.


### S5: Lowering

The concrete `.sir` file is lowered to C, WASM, or Python by `symirc`:

```
rysmith  →  concrete .sir  →  symirc -t c  →  .c  →  gcc / clang (link with -lm)
                           →  symirc -t wasm →  .wat / .wasm
                           →  symirc -t python →  .py
```

The generated C code is suitable for direct compilation and execution under the generated input $i$. The expected output $o$ is the return value of the function (the checksum over all live variables at exit).


### S6: Validation

The generated program is compiled with the target compiler and executed under $i$. If the output differs from $o$, Reify reports a potential miscompilation.

```
Expected:  func0() = -847
Compiled (-O3):  func0() = -846   → POTENTIAL BUG
```

Differential testing across compiler versions or optimization levels is also supported.


## Whole-Program Generation

The leaf generation pipeline (S1–S6) produces independent functions. To build a complete program, Reify generates a random call graph (CG) and applies *semantics-preserving peephole rewriting*: a constant `c` in a caller is replaced with `f(i) + (c − o)`, where `f(i) = o`. This establishes an inter-procedural call while preserving the constant's value at runtime.

Whole-program generation is implemented by `rylink`, described below. The pipeline:

```
W1. Pool ingest        — load a directory of rysmith-emitted (.sir + .json) pairs
W2. CG generation      — pick K functions and build a DAG call graph over them
W3. Bundle merge       — parse each .sir, union into one Program (dedup structs by name)
W4. Peephole rewrite   — for each (caller, callee) edge, splice `call @callee(args) + (c − o)`
W5. Lowering           — emit program.sir + optional symirc --split-by-source C/WASM
W6. Validation         — symiri runs the bundled entry with its solved params; check return
```

Each chosen leaf function brings its own solved realization (one of the `--n-inits` rysmith concretizations) so the rewrite expression `call + (c − o)` is semantically equivalent to the original literal at runtime. The call-realization transform (`CallRealizeTransform`, a whole-program `Transform`) consumes each rewrite site at most once across the entire program; composing two rewrites on the same literal would produce a left-to-right call chain (`f1() + f2() + …`) whose prefix sums can wrap in unintended ways even though each individual rewrite is BV-sound.

## Twin-Program Generation

A twin program of a given program is its equivalent variant. Twin-program generation is based on leaf functions, too.

Given a leaf function `f1` together with the exact input `i` that concretizes it, the whole execution is deterministic and known. `rytwin` obtains, for each on-path program point, the concrete value of every initialized local/parameter — the state the program passes through — from the `.state.json` sidecar when `f1` was generated with `rysmith --emit-state`, and otherwise by interpreting `f1` on `i` in-process. For a chosen basic block `B`, let `s` be the state at `B`'s entry and `s' = B(s)` the state at its exit. Twin-program generation synthesizes a **twin block** `B'` whose net effect from `s` is exactly `s'` and grafts a guarded diamond:

```
^X  (guard):  br call @__twg_<fn>_<X>(<state>) != 0  ->  ^X__twin  else  ^X__orig
^X__twin:     B'   ->  ^X__merge      (reproduces B's effect at s)
^X__orig:     B    ->  ^X__merge      (the original block body)
^X__merge:    <B's original terminator>
```

The guard fires only when the live-in state equals `s`, so on the profiled input the twin runs (producing exactly what `B` would) and on every other state the original runs — hence full equivalence. The guard is a generated function `@__twg_<fn>_<label> : i1`, one per twin site. It consumes the **entire** definitely-initialized state at `B`'s entry — not only `B`'s read set — as a conjunction of per-leaf equalities. The conjunction is total (no UB) and collision-free, so it preserves the equivalence on all inputs, not just the profiled one, and it cannot fire on any state other than `s`. Scalar roots cross into the guard by value, vector roots per-lane, and aggregate roots by address (`ptr [N] T` / `ptr @S` parameters, navigated inside the guard with in-bounds `ptrindex`/`ptrfield` + `load`). Each candidate block is twinned with probability `--p-twin`.

`B'` is generated the same way rysmith generates blocks: random statements with `%?` symbols over the live state (UB-safety `require`s spliced automatically), one fresh additive correction symbol per touched leaf so any target stays reachable, and one equality `require` per leaf pinning the final state to `s'`. The resulting single-block mini-program is solved in-process with the SMT solver, concretized by printing with the model and re-parsing, and then verified **bit-exactly** by re-running the interpreter (the solver's FP equality is IEEE and would conflate `+0.0`/`-0.0`). The scaffolding equality requires are stripped from the graft; the UB-safety requires remain (they hold on the guarded state). On UNSAT/timeout the attempt is retried with fresh statements (`--twin-retries`), and when every attempt fails — or with `--no-twin-smith` — `B'` falls back to a constant reconstruction of the leaves `B` writes.

**Pointers and memory**. The state profile records each pointer leaf's provenance — the originating local and the byte offset of the pointee cell — so memory-op blocks (`load`/`store`/`addr`/`ptr`-navigation) are twin candidates like any other. A block's effect is the bit-exact state diff `s -> s'` (store-through-pointer effects surface as diffs of the pointee root), changed pointer leaves are reconstructed with `addr <root>[path]` / `null`, and the guard compares pointer values with `==` against caller-reconstructed expected pointers (equality is defined across objects, so the check is total). Memory-op blocks require the entire frame state to be guardable — a load can observe any root through a pointer.

**Limitations**. Blocks containing non-intrinsic calls are not twinned (a callee handed a pointer into an outer frame could mutate state the frame diff does not see), and solver-generated twin bodies currently fall back to constant reconstruction when the block's state contains pointer leaves.

## Tool: rysmith

`rysmith` implements S1–S5 in a single in-process C++ binary. It builds RefractIR program ASTs directly in memory, calls `SymbolicExecutor` in-process (no subprocess), and emits concrete `.sir` files via `SIRPrinter`. It can optionally invoke `symiri` for S6 validation. The main focus is function generation. It does not test the compilers directly.

### Usage

```
rysmith [OPTIONS]
```

### Options

#### Type control

| Flag | Default | Description |
|---|---|---|
| `--no-fp` | off | Disable `f32`/`f64` types entirely |
| `--max-ptr-depth N` | 2 | Maximum pointer nesting depth (`ptr ptr T` = depth 2) |
| `--max-agg-nest N` | 2 | Maximum aggregate nesting depth |
| `--max-agg-elems N` | 3 | Maximum array size and struct field count |

#### Generation

| Flag | Default | Description |
|---|---|---|
| `--n-vars N` | 10 | Total variables per function (types drawn independently) |
| `--n-stmts N` | 3 | Statements per on-path block |
| `--off-path-multiplier F` | 2.0 | Scale `--n-stmts` / `--min-atoms` / `--max-atoms` by `F` in off-path blocks |

#### Operators

| Flag | Default | Description |
|---|---|---|
| `--no-divmod` | off | Disable integer division and modulo |
| `--no-select` | off | Disable `select` ternary expressions |

#### CFG

| Flag | Default | Description |
|---|---|---|
| `--n-bbls N` | 15 | Basic blocks between entry and exit per CFG |
| `--p-branch F` | 0.5 | Probability of a two-successor (branch) block |
| `--p-backedge F` | 0.3 | Probability of a back edge (loop) from a non-entry/exit block |

#### Solver

| Flag | Default | Description |
|---|---|---|
| `--timeout N` | 2000 | SMT solver timeout per attempt (ms) |
| `--seed N` | random | Master RNG seed |
| `--require-ub` | off | Generate programs that **trigger** UB on the sampled path instead of UB-free ones (see below). Implies `--no-crc32`. |
| `--require-nonterm` | off | Generate UB-free programs that **diverge** (⇑) on the sampled input instead of terminating ones (see below). Samples a lasso instead of an entry-to-exit path; implies `--require-reducible` and `--no-crc32`; pairs with `--max-lasso-period`. The full type lattice is available — integer, float, and pointer leaves are all closeable. |
| `--no-crc32` | off | Keep the sum-form checksum (`%_chk = %_chk + <leaf>`) in the emitted program instead of rewriting it to `@crc32_update` calls |

#### Output

| Flag | Default | Description |
|---|---|---|
| `-n, --n-funcs N` | 1 | Number of leaf functions to generate |
| `--n-inits N` | 3 | Concretizations per CFG+path template |
| `--max-loop-iter N` | 1 | Max iterations of any single loop in the sampled path |
| `--min-loop-iter N` | unset | If set, force at least one loop in the path to iterate ≥ N times (rejects loop-free CFGs) |
| `--max-retries N` | 2 | Retry attempts on solver failure (simpler path each time) |
| `-o, --output-dir PATH` | `reify_out` | Output directory for `.sir` files |
| `--target sir\|c\|wasm\|python` | `sir` | Optionally compile each concrete `.sir` in-process (`python` implies `--require-reducible`) |
| `--require-reducible` | off | Only generate reducible CFGs (irreducible back edges are repaired away) |
| `--structured-lowering true\|false\|random` | `false` | Structured lowering for the C (goto-free) and WASM (dispatch-free) targets, resolved per program; `true`/`random` imply `--require-reducible` |
| `--vec-lowering <s>` | `random` | Vector lowering strategy, resolved per program; `random` sweeps the target's set (C: all five; python: all but `vecext`) |
| `--keep-require` | off | Include `require` checks in compiled output |
| `--keep-ub-guards` | off | Keep the backends' dynamic UB guards in compiled output even for UB-free programs. By default UB-free generation (i.e. without `--require-ub`) drops them — see below |
| `--keep-symbolic` | off | Write intermediate symbolic `.sir` to disk |
| `--validate` | off | Run `symiri` on each concrete `.sir` and check its `Result:` line matches the descriptor's captured CRC32 retValue |
| `--emit-main` | off | Append a `@main()` wrapper that calls the entry with its solver-synthesised params and asserts the CRC32 retValue via `@check_chksum` |
| `--emit-desc` | off | Emit per-function descriptor JSON (`func_<id>_<i>.json`) used by `rylink`; records a `reducible` bool computed from the emitted function so structuring consumers can filter seeds, and a `has_ub` bool (true under `--require-ub`) so `rylink` knows whether the leaf's UB guards can be dropped |
| `--emit-state pbb\|ppp` | off | Emit a `func_<id>_<i>.state.json` profile of the concrete state at each program point (`pbb` = per basic-block entry, `ppp` = per program point) — loaded by `rytwin` when present, sparing it the in-process profiling run |
| `-v, --verbose` | off | Verbose progress output |

### Example

```sh
# Generate 10 diverse functions, 3 concretizations each, validate all
rysmith -n 10 --n-inits 3 --validate -o out/

# Stress pointer and mixed-type generation, disable floats
rysmith -n 20 --no-fp --max-ptr-depth 2 --max-agg-nest 2 -o out/

# Reproduce a specific run
rysmith -n 30 --seed 42 -o out/
```

### Output format

Each concrete `.sir` file is a valid RefractIR program containing one function `@funcN`. All variables are initialized to concrete integer or float values. The `^exit` block folds **every** scalar leaf of every let-init local and every parameter — recursing through nested arrays, structs, and vector lanes — into a running CRC32 state and returns it:

```sir
intrinsic @crc32_update(%state: i32, %val: i32) : i32;

fun @func0(%pa0: i32) : i32 {
  let mut %v0: i32 = 7;
  let mut %v1: i64 = -3;
  let mut %p0: ptr i32 = undef;
  let mut %_chk: i32 = 0;
^entry:
  %p0 = addr %v0;
  ...
^exit:
  %_chk = 0;
  %_chk = call @crc32_update(%_chk, %v0);
  %_chk = call @crc32_update(%_chk, %v1 as i32);
  %_chk = call @crc32_update(%_chk, %pa0);
  ret %_chk;
}
```

The return value is the expected output $o$. Internally rysmith asks the solver for the cheaper sum-based contract (`%_chk = %_chk + atom`), then a post-solve rewriter replaces every accumulator step with a `@crc32_update` call before the .sir is written; the solver never has to encode the CRC32 recurrence. After lowering to C with `symirc -t c`, executing the function should always return this value regardless of compiler version or optimization level — the helper carries a function-local `static` lookup table and a `static __attribute__((noinline))` qualifier (see `docs/intrinsics.md` §12.7) so the optimizer cannot fold the chain.

With `--emit-main`, rysmith additionally appends a `@main()` wrapper that calls `@func0` with the solver-synthesised parameter values and asserts the return matches the captured CRC32 via `@check_chksum(EXPECTED, %r);`. The C lowering of `@check_chksum` aborts on mismatch (`fprintf(stderr, …); abort();`) — that externally-visible side effect anchors the entire call chain against IPA-CP, so the compiler cannot fold the body away even at `-O3 -flto`.

### Generating UB-triggering programs (`--require-ub`)

By default every generated program is UB-free on its input: the solver asserts each operation's safety guard, so the concretization executes cleanly and returns the checksum. With `--require-ub`, rysmith instead asks the solver to **negate** the conjunction of those guards (delegated to `symirsolve`'s RequireUB mode — see [symirsolve.md](./symirsolve.md)), so the concretization is guaranteed to trigger at least one UB on the sampled path. This is used to exercise the UB-detection of downstream tools.

`--require-ub` **implies `--no-crc32`.** The solver reasons about the *sum-form* checksum (`%_chk = %_chk + <leaf>`, the cheap contract above), and one legitimate way to satisfy "at least one UB on the path" is to overflow that signed accumulator. The post-solve CRC32 rewriter, however, replaces every `%_chk = %_chk + <leaf>` with a total `@crc32_update(...)` call — which cannot overflow — so it would silently *delete* the very UB the solver just proved, leaving the emitted program UB-free. Keeping the sum form (`--no-crc32`) makes the program rysmith emits byte-identical to the one it solved, so a solver-found UB is guaranteed to trap under the interpreter. This costs nothing: a UB-triggering program aborts before it reaches a clean `ret`, so its CRC32 return-value oracle is vestigial anyway.

### Generating non-terminating programs (`--require-nonterm`)

By default every generated program terminates and returns the checksum. With `--require-nonterm`, rysmith instead generates programs that are UB-free but **diverge** (⇑) on their input — the concretization runs forever. The witness is no longer a finite entry-to-exit path but a *lasso*: a finite stem ρ from `^entry` to a loop header `^h`, followed by a cycle γ that closes back at `^h`. The infinite execution ρ·γ^ω is represented by the finite prefix ρ·γ — the stem plus one lap, ending on the revisit of `^h` — and nothing materializes the infinite unrolling. The mode implies `--require-reducible` so every sampled back edge is a genuine loop header (its target dominates its source).

The core is a **state fixed point**. Symbolic execution walks the lasso once and snapshots the complete mutable state at `^h`'s entry on the first arrival (σ_h) and again on the revisit (σ_h′, the state after one lap). It then asserts σ_h′ = σ_h — a bit-exact equality over every `let mut` leaf (scalars, aggregate/vector lanes, and pointer addresses; parameters and syms are immutable and cannot change) — together with the ordinary UB-safety guards collected over the stem and the one lap. Because execution is deterministic, a lap that starts in σ_h and returns to σ_h replays identically forever: the same branches are taken (so control stays on γ), the same operations stay UB-free, and the same state recurs. One finite SMT query therefore certifies an infinite, UB-free execution. This is delegated to `symirsolve`'s RequireNonterm mode (see [symirsolve.md](./symirsolve.md)) — the divergent analogue of RequireUB: where RequireUB *negates* the safety guards, RequireNonterm *asserts* them and adds the header recurrence. A one-lap fixed point is the default; `--max-lasso-period N` asks for longer orbits (see below).

**Recurrence is value identity, not IEEE comparison.** For float leaves the two differ on exactly one pair: `+0.0` and `-0.0` compare equal under `==` but are distinct values (SPEC §2.9). A lap that turns `-0.0` into `+0.0` has changed the state and must not be certified as recurring, so the constraint is built with SMT `=` rather than `fp.eq`, and the bounded-replay validator compares raw bits. Both agree; `test/solver/nonterm_fp_signed_zero_{fabs,sat}.sir` are a minimal pair differing only in that sign bit, one UNSAT and one SAT. See [float.md](./float.md) §2.1.

A freely random cycle rarely admits a fixed point, so the cycle blocks are seeded like `rytwin`'s twin bodies: random statements over the live state plus **one fresh additive-correction symbol per mutable leaf the cycle touches**, giving the solver the freedom to restore each leaf to its entry value. (RefractIR's `+ - * <<` trap on signed overflow, so a bare `x = x + c` would force `c = 0`; the corrections carry their own no-overflow guards, and the body is biased toward the overflow-safe `^ & | lshr` mixers.) To avoid emitting a trivial `while (1)`, the sampler keeps a live-looking exit edge on the cycle that the data never takes, and at least one intermediate lap state is required to differ from σ_h — so the loop genuinely computes even though it recurs.

The correction is `leaf = leaf + %?ntK` for **every** scalar leaf, whatever its type — the closer is uniform because `+` is the invertible operation in each domain:

| leaf type | correction symbol | why it closes |
|-----------|-------------------|---------------|
| `iN` | `iN` | wrapping add is a bijection, so some `%?nt` always spans the gap |
| `fN` | `fN` | add reaches the header value whenever the difference is representable; the asserted finite-result guard keeps the lap UB-free |
| `ptr T` | `i64` | pointer arithmetic shifts the offset inside the pointee's object (there is no `sym` of pointer type, so the symbol is the element-distance integer) |

Aggregate and vector leaves are reached by recursion (every leaf of a *touched* let is corrected, which also closes writes through a dynamic index, whose target leaf is not statically known). Two shapes are inherently unclosable and simply surface as UNSAT, after which rysmith resamples: a float whose lap-end and header values are too far apart for any representable delta, and a pointer that changes *object* over the lap (cross-object arithmetic is itself UB). That costs yield — roughly **70%** of seeds solve over the full type lattice versus **85%** over integers alone; passing `--no-fp --max-ptr-depth 0` restores the higher rate when throughput matters more than state variety.

`--require-nonterm` **implies `--no-crc32`.** A diverging program never reaches `^exit`, so it has no return value and the CRC32 return-value oracle is vestigial; more to the point, the post-solve oracle capture runs the program under `symiri`, which would *hang* on a divergent one. Validation is therefore not a return-value diff but a **bounded replay**: the interpreter runs the stem and a few laps under a fuel bound (reusing the per-block state-capture hook that backs `--emit-state`) and confirms that two arrivals at `^h` exactly one orbit (`k` laps) apart carry bit-identical state, that no UB fires, and that no `ret` is reached. The gap is the orbit's period, not one: under `--max-lasso-period` consecutive arrivals deliberately differ, so a validator comparing neighbours would reject every `k > 1` program. The pair compared is the **last** such pair within the budget, not the first: a local declared `= undef` does not enter the store until its first assignment, so the header's first visit carries a smaller, partly undefined state whenever the header is the entry block that initializes the pointers. Late visits are past all initialization, so the comparison stays strictly bit-exact with no undef exemption — and two identical header states one orbit apart in a deterministic program are themselves a complete proof that the orbit repeats forever. The `--emit-desc` descriptor records the leaf's **`outcome`** (`return` / `trap` / `diverge`, generalizing the old `has_ub` bool), which downstream tools key on: `rylink` requires a homogeneous pool and composes a `diverge` pool into a diverging whole program, while `rytwin` refuses a `diverge` (or `trap`) input. See the `rylink` and `rytwin` sections for the full behavior.

**Period-k orbits (`--max-lasso-period N`).** By default the header state recurs after a single lap. `--max-lasso-period N` draws the orbit's period `k` from `[1, N]` per attempt, asking instead for a state that returns only after `k` laps. The period needs no separate solver flag: the sampler emits the cycle `k` times, so the header appears `k+1` times in the path, and every layer reads `k` off the path itself — the solver, the correction splicer, and the bounded-replay validator all count arrivals.

Two constraints make the orbit *primitive*, i.e. genuinely period `k` rather than something shorter:

| constraint | asserted over | purpose |
|---|---|---|
| σ_k = σ_h | every `let mut` leaf | the orbit closes |
| σ_i ≠ σ_h for `0 < i < k` | the whole state (≥ 1 leaf differs) | no earlier recurrence |

Without the second, a period-1 program would satisfy a period-4 request and the knob would silently do nothing. It also rejects a *divisor* period — a two-cycle offered for `k = 4` fails because σ₂ = σ_h.

The additive corrections cannot supply a k-cycle on their own. Closing after `k` laps with one correction value would need the leaf to wrap around (`k·c ≡ 0` with no intermediate hitting the header value), and RefractIR's `+` **traps on signed overflow**, so the solver is forced to `c = 0` — which recurs after one lap, not `k`. A random body almost never supplies a k-cycle by chance, so for `k > 1` rysmith splices a modular counter into the latch:

```
let mut %_ntper: i32 = 0;      // the counter
let mut %_ntmod: i32 = k;      // its modulus, never written
  ...
  %_ntper = %_ntper + 1;       // 0,1,…,k-1,k — bounded by k, never overflows
  %_ntper = %_ntper % %_ntmod; // k wraps back to 0
```

`%_ntper` is zero only at every k-th arrival, which is exactly what the intermediate-arrival constraint needs: that constraint asks for the *state* to differ, i.e. at least one leaf, so every other leaf is free to stay put and still satisfy it. (The modulus lives in its own never-written local because an atom is `coef OP rval`, which cannot spell `%v % 3` with the literal on the right; being invariant, it recurs trivially.) Yield falls gently with the cap — about **70%** of seeds solve at `k = 1` and **60%** at `--max-lasso-period 4`, with the realized periods spread roughly evenly over `1..N`.

**`--emit-main` for divergence.** `--emit-main` still applies: rysmith appends a `@main` that calls the entry and asserts its return via `@check_chksum(EXPECTED, %r)`. The entry call never returns, so the check is unreachable at runtime — but the compiler cannot prove the loop diverges, so it must keep the whole computation alive against `@check_chksum`'s `abort()` side effect. There is no captured return value to assert, so `EXPECTED` is a **random** `i32` (any literal is sound, since the check is never reached). This makes `--require-nonterm --emit-main` a compilable whole-program differential-testing artifact: a correct compilation hangs, while a miscompilation that lets the loop terminate reaches the check and aborts.

**Target soundness.** Once a diverging program is compiled, a build that *terminates* is a miscompilation. WASM and Python preserve the divergence unconditionally — neither has a forward-progress assumption. **C does not on its own**: C11 §6.8.5p6 / C++ `mustprogress` let a compiler assume a *side-effect-free* loop terminates, and gcc/clang at `-O2` exploit this to delete a diverging loop — **non-deterministically**, so the same source compiles to a terminating binary on some runs and a diverging one on others. Neither the constant-condition loop form (`for(;;)`/`goto`) nor the loop's own computation reliably prevents it. So rysmith plants one **`@observe` beacon** in the cycle (`%leaf = call @observe(%leaf);` — see `docs/intrinsics.md` §12.7): `@observe` is the identity in value, so the header fixed point is unchanged, but its C lowering performs an observable `volatile` write, making the loop observably side-effecting so the optimizer must keep it. With the beacon, a diverging program stays infinite under gcc and clang at `-O0`/`-O2` (verified across recompiles, goto and structured lowering); WASM and Python lower `@observe` to the identity, needing no beacon.

### Dropping UB guards for UB-free output

The C/WASM/Python backends emit dynamic UB guards (`symirc --no-ub-guards`; see [symirc.md](./symirc.md#omitting-ub-guards---no-ub-guards-v023)). Because those guards only ever fire on a UB path, a program the reify pipeline proves UB-free renders them dead weight, so the tools **drop them automatically** rather than exposing a flag:

- **rysmith** drops the guards whenever it is not in `--require-ub` mode, and records `has_ub` in each `--emit-desc` descriptor accordingly.
- **rylink** drops them only when *every* selected pool leaf has `has_ub: false` — a bundle is UB-free iff all its leaves are. Legacy descriptors without the field parse as `has_ub: true`, so their guards are conservatively kept.
- **rytwin** drops them unconditionally: a twin is equivalence-preserving over UB-free input, and the interpreter it profiles `p1` with would itself fail on any UB.

Each tool takes **`--keep-ub-guards`** to force the guards back on — useful for catching a mislabeled UB-free program that *does* trigger UB, which then traps at runtime instead of silently misbehaving.


## Tool: rylink

`rylink` reads a rysmith function pool, builds whole programs over it, and (optionally) compiles and validates each one following W1-W5.

### Pool outcome (homogeneity)

Every whole program rylink builds has a single well-defined behavior, so the pool must be **homogeneous**: every leaf's descriptor `outcome` must be the same — all `return`, all `trap`, or all `diverge`. A mixed pool has no well-defined fused behavior (a returning caller splicing a `call` to a trapping or non-terminating callee), so rylink **rejects it with an error**. The common outcome becomes the whole program's outcome and selects how `--validate` checks each program:

| Pool | Fused program | `--validate` asserts |
|---|---|---|
| **`return`** (default rysmith) | returns the entry's value; peephole `call + (c − o)` preserves each rewritten literal | the entry returns its descriptor's solved `ret` |
| **`trap`** (`--require-ub`) | triggers UB — the entry (or a spliced trap callee) traps | the program **traps** under `symiri` |
| **`diverge`** (`--require-nonterm`) | diverges — the entry is the unmodified diverging leaf (its empty `ret` means no value-preserving call splices, so callees ride along as compiler surface) | the program **diverges** (bounded-replay on the entry's lasso header, taken from the descriptor path) |

UB guards are dropped for `return`/`diverge` bundles (both UB-free) and kept for `trap`. With `--emit-main`, a `diverge` entry's `@main` uses a random `i32` checksum (the entry never returns, so the check is unreachable, but the compiler must keep the computation — see the `--emit-main` note under `--require-nonterm`).

When `--structured-lowering` is `true`/`random` — or the target is `python` — seed programs may not be reducible (older pools, or runs without `rysmith --require-reducible`), so rylink **discards every pool seed whose descriptor's `reducible` flag is false** before generation (descriptors predating the flag parse as false and are conservatively discarded too). If no reducible seeds remain, rylink aborts with a pointer to `rysmith --require-reducible`. The composed program is then reducible by construction: every inlined seed is, and the generated `@main` wrapper's CFG is trivial.

### Usage

```
rylink [OPTIONS]
```

### Options

| Flag | Default | Description |
|---|---|---|
| `-i, --input-dir PATH` | `rysmith_out` | Directory of rysmith-emitted `(.sir + .json)` pairs (`rysmith --emit-desc`) |
| `-o, --output-dir PATH` | `rylink_out` | Root; each program lands in `<root>/prog_<id>_<i>/` |
| `-n, --n-progs N` | 1 | Number of whole programs to generate |
| `--id HEX6` | random | 6-hex-char generation ID prefix |
| `--seed N` | random | RNG seed |
| `--n-nodes N` | 4 | Target number of call-graph nodes per program |
| `--max-outdeg N` | 3 | Maximum out-degree per CG node |
| `--target sir\|c\|wasm\|python` | `c` | `c` uses `symirc --split-by-source`; `python` emits a single `program.py`; `sir` skips lowering |
| `--structured-lowering true\|false\|random` | `false` | Structured lowering for the C (goto-free) and WASM (dispatch-free) targets, resolved per program |
| `--vec-lowering <s>` | `random` | Vector lowering strategy, resolved per program from the target's set (C: all five; python: all but `vecext`) |
| `--keep-require` | off | Keep `require` checks in C/WASM output |
| `--keep-ub-guards` | off | Keep the dynamic UB guards even when the bundle is UB-free (default: dropped — see *Dropping UB guards* above) |
| `--validate` | off | Run `symiri` on each emitted program and assert the entry returns its descriptor's solved value |
| `-v, --verbose` | off | Per-init log lines (`validated: OK`, `symirc FAIL`, etc.) |

### Output layout

Each program lives in its own subdirectory:

```
rylink_out/
  prog_<id>_0/
    program.sir        # bundled RefractIR (header comments: ENTRY, CG, PARAMS, RETURN)
    common.h           # symirc --split-by-source artefacts (when --target c)
    program.c
  prog_<id>_1/
    ...
```

The bundled `.sir` is the source of truth for every downstream consumer. Header comments record the entry function, the call graph, the solved parameter values for the entry, and the expected return value — making each bundle reproducible without consulting the descriptor JSON.

### Example

```sh
# 1. Build a pool of 200 leaf functions with descriptors
rysmith -n 200 --emit-desc -o pool/

# 2. Generate 10 whole programs of ~4 functions each, validate every one
rylink -n 10 --n-nodes 4 --validate -i pool/ -o progs/

# 3. C target with require checks kept
rylink -n 5 --target c --keep-require -i pool/ -o progs/

# 4. Structured (goto-free) C over a reducible pool
rysmith -n 200 --emit-desc --require-reducible -o pool/
rylink -n 5 --target c --structured-lowering random -i pool/ -o progs/
```

## Tool: rytwin

`rytwin` is an **equivalence-preserving program transformer**. Given a generated program `f1` (a rysmith leaf or a rylink whole program), it emits an equivalent program `f2` such that `f1(i) == f2(i)` for **every** input `i` — same result, same undefined-behaviour outcome. Whole programs are profiled from `@main`, and twins are grafted into any function along the executed trace; the state capture is frame-aware, so states are attributed to the right activation even when block labels repeat across functions.

For a step-by-step hand-verification of a single twin — exact commands, the two versions of the block, feeding the input to both programs, and checking the guard fires on the profiled state — see [Hands-on: verifying a rytwin twin by hand](./rytwin_handcheck.md).

rytwin only transforms **UB-free terminating** programs, because it profiles `f1` by interpreting it on its solved input. When the descriptor is present, a `trap` (`--require-ub`) or `diverge` (`--require-nonterm`) input is **rejected up front** with a clear message (profiling one would trap, the other would hang). Without a descriptor the profiling run is **bounded** by a block-step cap (`kNoDescProfileStepCap`, 3200): a terminating program finishes well within it, a trapping one throws UB, and a non-terminating one hits the cap — all reported as a clean failure rather than a hang.

### Usage

```sh
rytwin <f1.sir> [OPTIONS]
```

The descriptor (`func_<id>_<i>.json`) and, when present, the state profile (`<stem>.state.json`) are read from `f1`'s directory following rysmith's naming, so only `f1` is passed positionally. Without a sidecar the profile is computed in-process: rytwin interprets `f1` on its solved input (the descriptor realization, or `f1`'s `// SOLVED:` header when no descriptor is present).

| Flag | Default | Description |
|---|---|---|
| `-o, --output PATH` | — | Output `.sir` (`f2`) |
| `--p-twin P` | 0.5 | Probability of grafting a twin for each candidate block |
| `--no-twin-smith` | off | Disable rysmith-style twin generation; reconstruct the post state with constants |
| `--twin-stmts N` | 3 | Random statements per generated twin |
| `--twin-retries N` | 3 | Generation attempts per twin before falling back |
| `--twin-guard exact\|bijection` | `exact` | Guard surface (see *Guard styles* below) |
| `--twin-scope block\|region` | `block` | Twin unit (see *Twin scope* below) |
| `--twin-select random\|interesting` | `random` | Region-selection policy (see *Region selection* below) |
| `--seed N` | random | RNG seed |
| `--target sir\|c\|wasm` | `sir` | Optionally compile `f2` via the in-process backend |
| `--validate` | off | Run `symiri` on `f1` and `f2` with the profiled input, assert they agree, and assert at least one twin block actually executed |
| `-v, --verbose` | off | Log each twin decision (grafted / skipped / rejected, with reason) to stderr |
| `--keep-require` | off | Keep `require` checks in compiled output |
| `--keep-ub-guards` | off | Keep the dynamic UB guards in the compiled twin (default: dropped — the twin is assumed UB-free; see *Dropping UB guards* above) |
| `--emit-main` | off | Keep `@main` un-mangled in compiled output |

### Guard styles

`--twin-guard` selects how the guard function checks the live-in state
against `s`. Both styles are **exact and collision-free** — they differ only
in surface form, so the equivalence holds identically for either.

- **`exact`** (default) — a conjunction of per-leaf `operand == const`. It is
  plainly readable as "state == s", so a reader (or a solver, or a
  pattern-matching compiler) can deduce the equivalence of the twin and the
  original almost by inspection.
- **`bijection`** — each integer leaf is first run through a *nonlinear
  bijection* on `iW` before the comparison, and the constant is pre-mixed the
  same way. Because a bijection collides with nothing, `mix(x) == mix(s)` iff
  `x == s`, so the guard fires on exactly `s` just as `exact` does. The
  bijection is built only from overflow-safe operators — `x ^= x >>> a` and
  the nonlinear `x ^= (x >>> a) & (x >>> b)` — since RefractIR's `+ - * <<`
  are strict-signed (overflow is UB), which rules out the usual
  multiply/rotate mixers. The guard body is then an opaque `>>> & ^` chain
  against constants that bear no visible relation to `s`, so recovering `s`
  (to prove twin ≡ orig) requires inverting a nonlinear map rather than
  reading off literals. Float and pointer leaves have no bijective integer
  primitive and stay exact.

### Twin scope

`--twin-scope` selects how much of the CFG each twin replaces.

- **`block`** (default) — one basic block, as above: the guard fires on the
  block's entry state, the twin reproduces its effect, and control resumes at
  the block's observed successor.
- **`region`** — the maximal single-entry region rooted at a block: every
  later block the entry *dominates* on the executed path, up to the first
  block it does not (or the function's return). The guard fires on the
  region's entry state, the twin reproduces the region's **net** effect and
  jumps straight to the region exit, **skipping every intermediate block and
  every loop iteration in between**. A whole loop collapses when its header
  is the region entry; a straight-line run collapses to a single block; a
  single-block region is the `block` case. This is sound by the same argument
  as `block`: RefractIR is deterministic, so the full entry state fixes the
  entire continuation, and the twin is a memoized shortcut for exactly that
  state — valid on every input that reaches the entry in that state, not only
  the profiled one. A region is only twinned when its entry state is fully
  guardable and every skipped block is free of non-intrinsic calls (a callee
  could mutate outer-frame state the net diff does not see); otherwise it
  falls back to a single-block twin.

### Region selection

`--twin-select` is a **policy that assigns each eligible region a twin
probability**; every region is then twinned by an independent draw (a single
block is the degenerate one-block region, so the same rule covers both). The
two policies differ only in that probability:

- **`random`** (default) — every region gets probability `--p-twin` (uniform).
- **`interesting`** — the probability is tilted by how hard the region's twin
  is to prove equivalent. Each region is scored
  `1000·(loop iterations collapsed) + 10·(distinct blocks) + 5·(changed
  leaves) + (entry fan-in)` — so collapsing a whole loop dominates — the score
  is normalized to `norm ∈ [0,1]` program-wide, and the twin probability is
  `p = pTwin ^ exp((0.5 − norm) / T)` with a fixed softmax temperature
  `T = 0.5`. That is monotone in the score, and is `1` at `--p-twin 1` (twin
  all) and `0` at `--p-twin 0` (twin none), so `--p-twin` still sets the
  overall rate while the score biases *which* regions win it.

Overlapping regions are resolved in trace order — the first region drawn
claims its blocks, and later regions covering any claimed block are dropped.
Twin bodies are synthesized only for the regions actually chosen. Selection
spans the **whole program** (all functions in the profiled trace), so on a
rylink program `interesting` concentrates twins on the hardest regions across
functions rather than scattering them uniformly.

### Example

```sh
# 1. Generate a program (pointer-free here, so more blocks are twin-eligible)
rysmith -n 1 --emit-desc --emit-main --max-ptr-depth 0 -o out/

# 2. Emit an equivalent twin, twinning every eligible block, and self-validate
rytwin out/func_<id>_0.sir --p-twin 1.0 --validate -o out/twin.sir

# 3. Differential test: compile both and compare
rytwin out/func_<id>_0.sir --p-twin 1.0 --target c --emit-main -o out/twin.sir
```

## Known Issues

The following commits, together, cause a 3-5x rysmith performace degradation:

1. 14343fc completely removed trivial "lit op lit" atoms.
2. e390437 excluded store statements from counting into --n-stmts.
3. 7118748 introduced indirect store and load.

Limiting them would bring back some trivial patterns that might not be
bad for compiler testing, and would allow more performant generation.
