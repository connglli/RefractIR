# Semantic reification and the reify tools

Reify generates random programs by a technique called *semantic reification*. Syntactic reification works on program text; semantic reification works on program meaning, and separates two kinds of it: compile-time semantics, what a program *can* do, and runtime semantics, what a program *actually does* on one input.

Given an arbitrary control flow graph `g` and an arbitrary entry-to-exit path `pi` through it, reify produces a program `P`, an input `i`, and an output `o` such that:

1. `P` is syntactically and semantically correct for `i`;
2. `g` is the CFG of `P`;
3. `P(i)` deterministically follows `pi` and produces `o`.

The `g` fixes the compile-time semantics and the `pi` fixes the runtime semantics, so both are chosen rather than discovered.

That combination is what makes the output useful for compiler testing. A compiler must reason about every possible execution when it optimizes, even though the runtime semantics of one input are fixed, and semantic reification exposes errors in that reasoning while keeping each generated program deterministic and free of undefined behaviour on its stated input. Because `g` and `pi` are arbitrary, the generator reaches control structures and data flows that a grammar-driven generator does not: unbounded loops, irreducible regions, deep type mixing. And because `o` is known before the program is ever compiled, a miscompilation shows up as a wrong answer rather than as a disagreement between two compilers, so no pseudo-oracle is needed.

Reify separates *leaf function generation*, compact functions with no calls, from *whole-program generation*, which composes leaf functions under an arbitrary call graph. `rysmith` implements the first, `rylink` the second, and `rytwin` transforms either into an equivalent variant.

## Leaf function generation

Two of the clauses hold by construction: the program is built over `g` itself, so `g` is its CFG, and its statements are typed as they are generated, so it is syntactically correct. What is left is that `P` runs `pi` without UB and produces `o` on `i`, and that is a question about values.

Let `T` be that program with the values that matter left as symbols, and `T[x]` the program obtained by assigning `x` to them. The task is to find one `x`:

```
∃x. follows(T[x], pi) ∧ safe(T[x], pi) ∧ interesting(x)
```

`follows` is the conjunction of the branch conditions `pi` decides, and `safe` the UB guard of every operation `pi` executes; between them they carry what construction could not. `interesting` answers to no clause at all; it excludes degenerate values, and its job is to make the program worth compiling. Every conjunct is a bit-vector formula over `x`, so this is a first-order existential over values: decidable, and settled by a single query. A model is `P`, the values it gives the parameters are `i`, and running it produces `o`.

Which conjuncts are asserted decides what kind of program comes out. `safe` as stated gives a clean run, `¬safe` one that triggers UB, and a `pi` that returns to a loop header in the state it left one that diverges.

`rysmith` implements this:

```
S1. CFG generation   - random control-flow skeleton
S2. Path sampling    - random entry-to-exit walk through the CFG
S3. Program seeding  - populate all blocks with typed statements
S4. Concretization   - solve the symbols along the path via SMT
S5. Lowering         - emit concrete RefractIR, then C / WASM / Python
S6. Validation       - execute and compare the output against o
```

### S1: CFG generation

A random CFG starts as a spanning chain from entry through the interior blocks to exit, and then stochastically gains branch edges (a second successor pointing forward) and back edges (producing loops). The result is always connected and always has a path to exit.

A back edge may land past a loop header, which makes the CFG irreducible. When reducible CFGs are required, the CFG is repaired instead of resampled: a retreating edge whose target does not dominate its source is deleted, one per re-analysis pass, so every valid loop survives and only irreducible cycles are broken.

### S2: Path sampling

The execution path is a random walk from entry to exit, counting each back-edge traversal so loop iteration counts stay bounded. A walk that gets stuck escapes to exit along the shortest BFS route. The result is a sequence of block labels:

```
^entry -> ^b0 -> ^b3 -> ^b0 -> ^b4 -> ^exit
```

One CFG yields many distinct paths, differing in which branches are taken and how often each loop runs.

### S3: Program seeding

Every block gets typed statements, and a block's role decides what kind.

An on-path block, one that appears in `pi`, uses *symbolic* variables whose values the solver picks. Symbols are declared with a domain and a kind annotation (`coef`, `value`, `index`). Interest constraints, `require` statements excluding degenerate coefficients such as 0, 1 and -1, push the solver toward programs that are worth compiling.

An off-path block uses concrete random literals. It never executes, because the solver pins every on-path branch, so control never reaches an off-path successor. Off-path code is therefore left unconstrained and may contain UB: division by a variable that could be zero, signed overflow from a wide literal, an index that could leave its array. None of it reaches the differential oracle, and all of it is surface for the optimizer's dead-code elimination, alias analysis and vectorization to work over.

Off-path volume costs the solver nothing, which is why the volume knobs (`--n-stmts`, `--min-atoms`, `--max-atoms`) describe on-path blocks and off-path blocks scale them by `--off-path-multiplier`. On-path volume is the solver's bottleneck and tunes independently.

#### Types

Generation draws from the full RefractIR type lattice, each variable choosing independently: integer scalars `i8` through `i64` and arbitrary `iN`, floating-point `f32` and `f64`, arrays `[N] T` and structs `@Name { … }` up to a bounded nesting depth, vectors `<N> T`, and pointers `ptr T` including `ptr ptr T` chains. Mixed types appear within one function, and a scalar type boundary is crossed by an explicit cast atom, which is what exercises a compiler's promotion and narrowing paths.

A floating-point variable is initialized on-path by casting from an integer symbol, `(f32) %?s0`, which keeps the SMT problem in bit-vector theory. Off-path float code uses concrete literals.

#### Expressions

Expressions are generated type-directedly: given a target type `T`, the generator produces an expression of type `T`, and every atom in one expression shares that type. The repertoire covers linear terms with a symbolic coefficient (`coef_sym * var`), the bitwise and shift operators, `~var`, explicit casts, `load` through a pointer variable, `addr` of a local, a one-level `select`, and division or modulo by a concrete non-zero denominator.

Division and modulo keep a concrete denominator on-path, as in `%?s3 / 7`, which yields the divide-by-constant patterns that stress strength reduction. Off-path division uses any literal, zero included.

A symbolic coefficient is typed to match its expression context, so an `i64` expression takes a `coef i64` symbol and an `i32` expression a `coef i32` one. That reads as natural code, a 64-bit multiply against a 64-bit coefficient, and reaches type-specific optimization patterns.

#### Pointer initialization

`addr lv` is an expression atom, not a valid `let` initializer, so a pointer variable is declared `undef` and assigned in the entry block before anything else is generated:

```sir
fun @func0() : i32 {
  let mut %v0: i32 = %?s0;           // integer var, init from input sym
  let mut %p0: ptr i32 = undef;      // pointer var, init deferred
  let mut %pp0: ptr ptr i32 = undef; // depth-2 pointer, init deferred
^entry:
  %p0 = addr %v0;
  %pp0 = addr %p0;
  require %?s0 != 0, "nonzero input";
  ...
```

`^entry` is the first block on every path, so every pointer is definitely initialized whichever path is sampled.

### S4: Concretization

`symirsolve`, or the in-process symbolic executor when running under `rysmith`, executes `pi` symbolically: it collects the path conditions from branch terminators, the `require` constraints (interest constraints and UB guards), and the computed value of each assignment, encodes them as bit-vector constraints, and asks Bitwuzla for a satisfying assignment. The model is substituted back through `SIRPrinter` to give a fully concrete `.sir`. Off-path blocks pass through untouched, their literals needing no solving.

Solving the same symbolic template again, with a different solver or RNG seed, gives a structurally similar program with different numbers, so one control-flow structure yields many distinct optimization problems.

### S5: Lowering

`symirc` lowers the concrete `.sir` to C, WASM or Python:

```
rysmith -> concrete .sir -> symirc -t c      -> .c   -> gcc / clang (link -lm)
                         -> symirc -t wasm   -> .wat / .wasm
                         -> symirc -t python -> .py
```

The expected output `o` is the function's return value, a checksum over every live variable at exit.

### S6: Validation

The program is compiled and executed under `i`. An output other than `o` is a potential miscompilation:

```
Expected:        func0() = -847
Compiled (-O3):  func0() = -846   -> POTENTIAL BUG
```

Differential testing across compiler versions or optimization levels works the same way.

## Whole-program generation

S1 to S6 produce independent functions. To build a whole program, reify generates a random call graph and realizes each of its edges as a real call, without disturbing what the caller computes.

The leaves arrive already satisfying the three clauses, and composition adds a fourth: the call graph of `P` is the sampled DAG `h`. An edge of `h` is realized at a point where the caller already computes some value `v`, by routing that value through the callee. Writing `S` for the states the profiled run reaches that point in, the task is to find an argument expression `e1` and a result expression `e2` such that

```
∃e1. ∃e2. ∀s ∈ S. e2(f(e1(s))) = v(s)
```

`e1` builds the callee's parameters out of variables the caller has in scope, and `e2` turns the callee's return value back into the variable or constant the caller wanted. Neither may trap, and neither may the call between them, since the program has to stay UB-free on `i`. Both are unknown functions, so this is a second-order synthesis problem.

What collapses it is that `f` is known at the inputs it was concretized on and nowhere else. A non-constant `e1` would demand `f`'s behaviour at every state in `S`; behaviour nobody has computed, on inputs the callee was never proven UB-free for.

In `rylink`, `e1` is therefore *value*-constant, pinned to an input `f` was concretized on. Then `f(e1(s))` is the known `o` whatever `s` is, the callee runs on an input it was proven safe for, and `e2` is left alone and first-order: for a constant `v = c`, it is `+ (c - o)`.

Being pinned in value does not make `e1` a literal. It is drawn from a catalog of constructions, each of which reaches the same `i` from whatever the caller holds where the call lands, writing `σ₁…σₘ` for the distinct states the profiled run passes that point in:

```
literal-or-bias   e1 = %x + (i - x)                  %x steady at x across every σⱼ
xor-mask          e1 = i ^ ⋀ⱼ (g ^ gⱼ)               g a chosen XOR-fold, gⱼ its value at σⱼ
prime-interp      e1 = P(vars) - (i mod p) + i       P(σⱼ) = i mod p, P(t) ≠ i mod p off S
```

The first holds only where a variable does; the other two answer to every `σⱼ` at once, so the variables they read are free to move between visits.

`rylink` implements this:

```
W1. Pool ingest       - load a directory of rysmith (.sir + .json) pairs
W2. CG generation     - pick K functions, build a DAG call graph over them
W3. Bundle merge      - parse each .sir, union into one Program (dedup structs by name)
W4. Peephole rewrite  - per (caller, callee) edge, splice `call @callee(args) + (c - o)`
W5. Anti-optimization - rewrite every block by identities that cannot trap
W6. Lowering          - program.sir plus optional C / WASM / Python
W7. Validation        - run the bundled entry and check its outcome
```

Which realization a leaf brings is a choice among the `--n-concretes` concretizations `rysmith` emitted for it, and it fixes the `i` and the `o` the splice is built from. `CallRealizeTransform` consumes each rewrite site at most once across the whole program: composing two rewrites on one literal would build a left-to-right call chain, `f1() + f2() + …`, whose prefix sums can wrap even though each rewrite is individually sound in bit-vector arithmetic.

Every function in a bundle comes out of the same statement generator, so they read alike. W5 breaks that up by rewriting them with identities applied in the direction a compiler does not take: reversed peepholes, arithmetic and bitwise crossings, restructuring. The rule families live in [src/reify/antiopt](../src/reify/antiopt), driven by [include/reify/antiopt.hpp](../include/reify/antiopt.hpp). A bundled program is concrete, with no set of states to prove anything over, so only rules that cannot introduce a trapping operation apply; those hold whatever the state does, and so does any composition of them.

## Twin-program generation

A twin program is an equivalent variant: `f2(i) == f1(i)` for every input, with the same UB outcome. `rytwin` builds one from a program and the input that concretizes it, so the execution is deterministic and known, and no solver is involved anywhere.

Its unit is a *region*: the maximal single-entry region rooted at an executed block, covering every later block that the entry dominates on the executed path. For a region `R` and the entry state `s0` the profiled run reaches it in, the task is to find a guard `G` and a twin body `B` such that

```
∃G. ∃B. G(s0) ∧ ∀s. G(s) ⇒ R(s) ≡ B(s)
```

where `R(s) ≡ B(s)` holds when, started in `s` or `s0`, the region and the twin leave at the same block, neither hits UB on the way, and the states they leave in are bit-identical.

`G` and `B` are unknown *functions*, so this is a challenging, second-order synthesis problem under one ∀.

In `rytwin`, neither `G` nor `B` is searched for over a space of functions: T3 witnesses `B` by construction, and T4 draws `G` from a single template, a box around `s0`, so `G(s0)` holds for free. What is left of the ∀ is a first-order question about a fixed body and a fixed set of states.

`rytwin` implements this:

```
T1. State profile     - every initialized local at each on-path point, from the
                        .state.json sidecar (rysmith --emit-state) or by interpreting
T2. Region planning   - pick a dominance region whose live-in state a guard can state
T3. Trace flattening  - the statements the run executed, branches dropped, loops laid out
T4. Guard box         - a state-set pass opens each leaf as far as it can prove
T5. Anti-optimization - the W5 engine again, here licensed by the box
T6. Graft             - splice the guarded diamond; --validate spot-checks the box
```

The graft is a diamond at the region entry:

```
^X:           br call @__twg_<fn>_<X>(<state>) != 0  ->  ^X__twin  else  ^X__orig
^X__twin:     R'                            ->  br ^<exit>
^X__orig:     the region's entry block, unchanged - the region runs on from here
```

The guard takes over the region entry's own label, so no predecessor edge is rewritten, and the two arms rejoin at the exit the region left to.

The twin body is the executed trace, which makes it correct for every state that follows the same path UB-free, a far larger set than the one profiled state. The guard therefore states per leaf what the state-set pass proves: nothing at all (free), `lo <= x <= hi` (ranged), or `x == v` (pinned). Every comparison the guard makes is total, so the guard itself cannot trap.

Having a proof also buys the rewriting more than W5 gets. Rules that can trap are kept wherever the box clears them, and a family of rules reads the box's facts directly.

A region holding a non-intrinsic call is not twinned, since a callee could mutate state the frame diff does not see.

## rysmith

`rysmith` runs S1 to S5 in one process. It builds the program AST in memory, calls the symbolic executor in-process rather than as a subprocess, and writes concrete `.sir` through `SIRPrinter`. It can invoke `symiri` for S6 under `--validate`. Its subject is function generation; it does not test compilers itself.

```
rysmith [OPTIONS]
```

The full option list is `rysmith --help`, declared in [src/rysmith.cpp](../src/rysmith.cpp). The knobs group into type control (`--no-fp`, `--max-ptr-depth`, `--max-agg-nest`, …), generation volume (`--n-vars`, `--n-stmts`, `--min-atoms`, `--off-path-multiplier`), operator repertoire (`--no-divmod`, `--no-select`, `--no-intrinsics`, …), CFG shape (`--n-bbls`, `--p-branch`, `--p-backedge`), solver control (`--timeout`, `--seed`, `--max-retries`), and output (`-n`, `--n-concretes`, `--n-examples`, `-o`, `--target`, the `--emit-*` family). The sections below cover the modes whose semantics are not evident from the flag name.

```sh
# 10 functions, 3 concretizations each, all validated
rysmith -n 10 --n-concretes 3 --validate -o out/

# 10 functions, two distinct solved examples per concretization
rysmith -n 10 --n-examples 2 -o out/

# stress pointers and mixed types, no floats
rysmith -n 20 --no-fp --max-ptr-depth 2 --max-agg-nest 2 -o out/

# reproduce a run
rysmith -n 30 --seed 42 -o out/
```

### Output format

Each concrete `.sir` holds one function `@funcN` with every variable initialized to a concrete value. The `^exit` block folds every scalar leaf of every local and parameter, recursing through nested arrays, structs and vector lanes, into a running CRC32 state and returns it:

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

That return value is the expected output `o`. The solver never sees the CRC32 recurrence: it is asked for the cheaper sum-form contract, `%_chk = %_chk + atom`, and a post-solve rewriter replaces each accumulator step with a `@crc32_update` call before the file is written. After lowering to C, the function returns `o` whatever the compiler and optimization level, because the helper carries a function-local `static` table and a `static __attribute__((noinline))` qualifier that stop the optimizer folding the chain ([intrinsics.md](./intrinsics.md) §12.7).

`--emit-main` appends a `@main()` that calls the entry with the solver's parameter values and asserts the return against the captured checksum through `@check_chksum(EXPECTED, %r);`. The C lowering of `@check_chksum` aborts on mismatch, and that externally visible side effect anchors the whole call chain against interprocedural constant propagation, so the body survives `-O3 -flto`.

`--n-examples N` asks for `N` distinct input/output examples per concretized `.sir`. The first is the concretized solve; each further one comes from re-solving a clone of the same symbolic template armed with two splices into the entry block: one `require %?s == v` per solved sym pinning it to the value the emitted program embeds, and one exclusion require per collected input, `%paK != v` on a single parameter coordinate.

### Generating UB-triggering programs

By default the solver asserts each operation's safety guard, so the program executes cleanly and returns its checksum. `--require-ub` asks it to negate the conjunction of those guards instead, delegating to `symirsolve`'s RequireUB mode ([symirsolve.md](./symirsolve.md)), so the concretization triggers at least one UB on the sampled path. That is what exercises a downstream tool's UB detection.

`--require-ub` implies `--no-crc32`, and the reason matters. The solver reasons about the sum-form checksum, and one legitimate way to satisfy "at least one UB on this path" is to overflow that signed accumulator. The post-solve CRC32 rewriter would then replace `%_chk = %_chk + <leaf>` with a total `@crc32_update` call, which cannot overflow, deleting the very UB the solver just proved and leaving an emitted program that is UB-free. Keeping the sum form makes the emitted program byte-identical to the solved one, so a solver-found UB is guaranteed to trap under the interpreter. It costs nothing, because a UB-triggering program aborts before reaching a clean `ret` and its return-value oracle is vestigial anyway.

### Generating non-terminating programs

`--require-nonterm` generates programs that are UB-free and diverge on their input. The witness is a lasso: a stem from `^entry` to a loop header `^h`, then a cycle closing back at `^h`, carried as the finite prefix stem followed by lap. The mode implies `--require-reducible`, so every sampled back edge has a header that dominates it.

The certificate is a state fixed point. The mutable state at `^h` must be bit-identical on the first arrival and on the revisit, alongside the UB guards of the stem and the lap. A deterministic lap that begins and ends in the same state replays forever, so one finite SMT query certifies an infinite UB-free run. [symirsolve.md](./symirsolve.md) owns that query, including why its equality is value identity rather than IEEE `==`.

A random cycle almost never admits a fixed point on its own, so the cycle carries one additive correction symbol per mutable leaf it touches, `leaf = leaf + %?ntK`. The closer is uniform because `+` is the invertible operation in every domain:

| leaf type | symbol | why it closes |
|---|---|---|
| `iN` | `iN` | wrapping add is a bijection, so some value spans the gap |
| `fN` | `fN` | reaches the header value whenever the difference is representable |
| `ptr T` | `i64` | shifts the offset inside the pointee's object |

A shape that cannot close, a float too far from its header value or a pointer that changed object, comes back UNSAT and `rysmith` resamples. About 70% of seeds solve over the full type lattice, against 85% over integers alone; `--no-fp --max-ptr-depth 0` restores the higher rate.

`--max-lasso-period N` asks for a state that recurs only after `k` laps, with `k` drawn per attempt. Two constraints keep the orbit primitive: the state at lap `k` equals the header state, and the state at every earlier arrival does not. The corrections cannot supply a `k`-cycle by themselves, since `+` traps on overflow and the solver would answer `c = 0`, so a modular counter goes in the latch instead. Yield falls from about 70% at `k = 1` to 60% at `N = 4`.

The mode implies `--no-crc32`, since a diverging program has no return value and the post-solve oracle capture would hang waiting for one. Validation is a bounded replay instead: the interpreter runs the stem and the laps under a fuel bound and checks that two arrivals one orbit apart carry bit-identical state, that no UB fires, and that no `ret` is reached. `--emit-main` still applies, asserting a random `EXPECTED` the program never reaches, so a correct compilation hangs and a miscompiled one aborts. The descriptor records `outcome: diverge`, which `rylink` composes homogeneously and `rytwin` refuses.

C needs help to preserve divergence, because C11 §6.8.5p6 lets a compiler delete a side-effect-free loop, and gcc and clang both do so non-deterministically at `-O2`. `rysmith` plants an `@observe` beacon in the cycle, the identity on values and an observable `volatile` write in the C lowering ([intrinsics.md](./intrinsics.md) §12.7), so the loop survives at `-O0` and `-O2` under both compilers. WASM and Python have no forward-progress assumption and need no beacon.

### Dropping UB guards

The backends emit dynamic UB guards, and `symirc --no-ub-guards` drops them ([symirc.md](./symirc.md#omitting-ub-guards)). Those guards only ever fire on a UB path, so on a program the reify pipeline proves UB-free they are dead weight, and the tools drop them automatically rather than exposing the decision as a flag.

`rysmith` drops them whenever it is not in `--require-ub` mode, and records `has_ub` in each descriptor accordingly. `rylink` drops them only when every selected pool leaf has `has_ub: false`, since a bundle is UB-free exactly when all its leaves are; a descriptor without the field parses as `has_ub: true` and its guards are kept. `rytwin` drops them unconditionally, because a twin preserves equivalence over UB-free input and the interpreter it profiles with would have failed on any UB.

Each tool takes `--keep-ub-guards` to force them back on, which catches a program mislabeled UB-free that does trigger UB: it traps at runtime instead of misbehaving quietly.

## rylink

`rylink` reads a `rysmith` function pool, builds whole programs over it through W1 to W7, and optionally compiles and validates each one.

```
rylink [OPTIONS]
```

The full option list is `rylink --help`, declared in [src/rylink.cpp](../src/rylink.cpp).

### Pool homogeneity

Every whole program `rylink` builds has one well-defined behaviour, which requires the pool to be homogeneous: every leaf descriptor's `outcome` must agree, all `return`, all `trap`, or all `diverge`. A mixed pool has no well-defined fused behaviour, a returning caller splicing a call to a trapping or non-terminating callee, so `rylink` rejects it with an error. The common outcome becomes the program's outcome and decides what `--validate` asserts:

| Pool | Fused program | `--validate` asserts |
|---|---|---|
| `return` (default `rysmith`) | returns the entry's value; the peephole `call + (c - o)` preserves each rewritten literal | the entry returns its descriptor's solved `ret` |
| `trap` (`--require-ub`) | triggers UB, in the entry or in a spliced trapping callee | the program traps under `symiri` |
| `diverge` (`--require-nonterm`) | diverges; the entry is the unmodified diverging leaf, and its empty `ret` means no value-preserving call splices, so the callees ride along as compiler surface | the program diverges, by bounded replay on the entry's lasso header from the descriptor path |

UB guards are dropped for `return` and `diverge` bundles, both UB-free, and kept for `trap`. Under `--emit-main`, a `diverge` entry's `@main` asserts a random `i32` checksum: the entry never returns, so the check is unreachable, but the compiler still has to keep the computation.

### Reducible pools

When `--structured-lowering` is `true` or `random`, or the target is `python`, the composed program has to be reducible. Pool seeds may not be, so `rylink` discards every seed whose descriptor `reducible` flag is false before generating, a descriptor predating the flag parsing as false. If no reducible seed remains it aborts, pointing at `rysmith --require-reducible`. What survives composes into a reducible program by construction, since every inlined seed is reducible and the generated `@main` wrapper's CFG is trivial.

### Output layout

Each program lives in its own subdirectory:

```
rylink_out/
  prog_<id>_0/
    program.sir        # bundled RefractIR (header comments: ENTRY, CG, PARAMS, RETURN)
    common.h           # symirc --split-by-source artefacts, under --target c
    program.c
  prog_<id>_1/
    ...
```

The bundled `.sir` is the source of truth for every downstream consumer. Its header comments record the entry function, the call graph, the entry's solved parameter values and its expected return, which makes a bundle reproducible without the descriptor JSON.

```sh
# 1. build a pool of 200 leaf functions with descriptors
rysmith -n 200 --emit-desc -o pool/

# 2. 10 whole programs of about 4 functions each, all validated
rylink -n 10 --n-nodes 4 --validate -i pool/ -o progs/

# 3. C target with require checks kept
rylink -n 5 --target c --keep-require -i pool/ -o progs/

# 4. structured (goto-free) C over a reducible pool
rysmith -n 200 --emit-desc --require-reducible -o pool/
rylink -n 5 --target c --structured-lowering random -i pool/ -o progs/
```

## rytwin

`rytwin` takes a generated program `f1`, a `rysmith` leaf or a `rylink` whole program, and emits an equivalent `f2`: same result and same undefined-behaviour outcome for every input. A whole program is profiled from `@main` and twins are grafted into any function along the executed trace; the state capture is frame-aware, so a state is attributed to the right activation even when block labels repeat across functions.

```sh
rytwin <f1.sir> [OPTIONS]
```

The full option list is `rytwin --help`, declared in [src/rytwin.cpp](../src/rytwin.cpp). Only `f1` is passed positionally: the descriptor (`func_<id>_<i>.json`) and, when present, the state profile (`<stem>.state.json`) are read from `f1`'s directory following `rysmith`'s naming. Without a sidecar the profile is computed in-process, by interpreting `f1` on its solved input, taken from the descriptor realization or from `f1`'s own `// SOLVED:` header.

`rytwin` transforms UB-free terminating programs only, because it profiles `f1` by interpreting it. With a descriptor present, a `trap` or `diverge` input is rejected up front with a clear message, since profiling one would trap and the other would hang. Without a descriptor the profiling run is bounded by a block-step cap (`kNoDescProfileStepCap`, 3200): a terminating program finishes well inside it, a trapping one throws UB, and a non-terminating one hits the cap, all three reported as a clean failure rather than a hang.

### The twin unit

The twin unit is the maximal single-entry region rooted at an executed block: every later block the entry dominates on the executed path, up to the first block it does not, or the function's return.

The guard fires on the region's entry state, and the twin reproduces the region's net effect and jumps straight to the region exit, skipping every intermediate block and every loop iteration in between. A whole loop collapses when its header is the region entry. A straight-line run collapses to one block, which is the degenerate case of the same rule rather than a second mode.

A region is twinned only when its entry state is fully guardable and every covered block is free of non-intrinsic calls. Otherwise the window falls back to the single block at its entry.

### Region selection

`--twin-select` names a policy that assigns each eligible region a twin probability; every region is then twinned by an independent draw. A single block is the degenerate one-block region, so one rule covers both. The policies differ only in that probability.

`random`, the default, gives every region probability `--p-twin`.

`interesting` tilts the probability by how hard the region's twin is to prove equivalent. Each region scores `1000*(loop iterations collapsed) + 10*(distinct blocks) + 5*(changed leaves) + (entry fan-in)`, so collapsing a whole loop dominates. The score is normalized program-wide to `norm` in `[0,1]`, and the twin probability is `p = pTwin ^ exp((0.5 - norm) / T)` at a fixed softmax temperature `T = 0.5`. That is monotone in the score, and it is `1` at `--p-twin 1` and `0` at `--p-twin 0`, so `--p-twin` still sets the overall rate while the score biases which regions win it.

Overlapping regions resolve in trace order: the first region drawn claims its blocks, and a later region covering any claimed block is dropped. Twin bodies are synthesized only for the regions actually chosen. Selection spans the whole program, every function in the profiled trace, so on a `rylink` program `interesting` concentrates twins on the hardest regions across functions instead of scattering them uniformly.

```sh
# 1. generate a program (pointer-free, so more blocks are twin-eligible)
rysmith -n 1 --emit-desc --emit-main --max-ptr-depth 0 -o out/

# 2. emit an equivalent twin, twinning every eligible region, and self-validate
rytwin out/func_<id>_0.sir --p-twin 1.0 --validate -o out/twin.sir

# 3. differential test: compile both and compare
rytwin out/func_<id>_0.sir --p-twin 1.0 --target c --emit-main -o out/twin.sir
```

`--validate` runs `symiri` on `f1` and `f2` with the profiled input, asserts they agree and that at least one twin executed, and then spot-checks states sampled inside each guard's box, comparing the region against the twin body bit-exactly.

## Generation throughput

Three generator choices dominate the cost of a `rysmith` run: no trivially constant `lit op lit` atoms, store statements not counting toward `--n-stmts`, and indirect loads and stores. Each raises the solver work per function, and relaxing any of them would trade output quality for throughput. [.agents/notes/implementation/2026-08-05-rysmith-throughput.md](../.agents/notes/implementation/2026-08-05-rysmith-throughput.md) records what they cost together and what a per-change attribution would take.
