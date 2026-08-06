# RefractIR — Codebase Guide & Hands-on Walkthrough

## 1. What this project is (the mental model)

**RefractIR** (internally called *SymIR*) is a **CFG-based symbolic intermediate representation** — a small, LLVM-style language (files end in `.sir`) built for **program synthesis, symbolic execution, and SMT constraint generation** over **bit-vector (BV)** logic.

The core idea that drives everything: **a `.sir` program is a *template*, not a finished program.** It can declare **symbols** (`%?x`) — unknowns whose values an SMT solver will later choose. A symbol + a path through the CFG + `require`/`assume` properties = an SMT query. Solve it, and you get a concrete program.

Key semantic pillars (from `AGENTS.md` / `docs/SPEC_v0.2.3.md`):
- **Non-SSA, mutable** locals via `let mut`, explicit basic blocks (`^label`) and `br` branches — no structured nesting required.
- **Strict undefined behavior (UB)**: division by zero, overflow, OOB access, `undef` reads, bad pointer deref, FP NaN/∞ — all are fatal, modelable, and even *synthesizable* (`symirsolve --require-ub`).
- **Restricted, SMT-friendly expressions**: flat left-to-right, no parentheses.
- Types: `i1/i8/i16/i32/i64`, `f32/f64`, arrays, structs, `<N> T` SIMD vectors, and pointers `ptr T` (with `ptrindex`/`ptrfield` navigation).

## 2. Repository map

```
/workspace/                    <- THIS is the repo (the "vscode-refractir" folder inside is just a VSCode syntax-highlighting ext)
├── include/                   # headers (the map to the codebase)
│   ├── ast/                   #   AST types + SIRPrinter (canonical float I/O)
│   ├── frontend/              #   lexer, parser, typechecker, semchecker, link_resolver
│   ├── analysis/              #   CFG, dominators, loops, reducibility, structurizer, structured_lowering
│   ├── interp/                #   interpreter (value.hpp, memory, type_layout)
│   ├── backend/               #   C / WASM / Python codegen
│   ├── solver/                #   SMT integration (smt.hpp, bitwuzla_impl, alive_impl, symbolic_value)
│   └── reify/                 #   generators for rysmith/rylink/rytwin
├── src/                       # implementation, one .cpp per .hpp
├── docs/                      # SPEC_v0.2.2 (normative), SPEC_v0.2.3 (roadmap), per-tool guides, intrinsics/UB/float refs
├── examples/                  # real programs (bubble_sort, brainfuck, minicipher, …)
├── test/                      # the test corpus + test/lib/* runner scripts
├── alivesmt/                  # vendored Z3 backend (Alive2-derived)
└── Makefile                   # the build + test entry point
```

**Pipeline** (shared by all tools): `source → lexer → parser → CFG builder → typechecker → semchecker`; then each tool forks off (interp = bind symbols + run; symirc = lower + codegen; symirsolve = path-based symbolic execution → SMT → concrete `.sir`).

## 3. Environment status

The prebuilt binaries in `/workspace` were compiled on **glibc** (Ubuntu) and **cannot run in an Alpine container** (missing `__isoc23_strtol` etc.). Rebuild everything from source:

- Install `g++`, `python3`, `z3`, `cmake/ninja` via the distro package manager
- Build **bitwuzla 0.9.1 from source** (Alpine has no package) as a shared lib, install to `/usr/local`
- Rebuild all 6 tools with `make SOLVER=bitwuzla`

Verified state: **solver 350/363 (+13 by-design skips), all 15 examples, unit 130/130 rytwin, frontend all green**.

Two environment gotchas learned the hard way:
1. `make clean` **also needs** `SOLVER=bitwuzla` (the Makefile checks the solver *before* cleaning).
2. Old orphan `.o` files from a previous build layout aren't covered by `make clean` — delete `src/{backend,reify,solver}/*.o` leftovers if you see "undefined reference to `__isoc23_*`".

## 4. Building

```bash
# default = bitwuzla backend (recommended; the well-tested one)
make -j$(nproc)

# alt: Z3-based backend (HAS MODELING GAPS — floats, some aggregate/pointer
# patterns throw "Unknown/Unimplemented kind"; reify tools won't work)
make SOLVER=alivesmt -j$(nproc)

# clean (must pass SOLVER, see gotcha above)
make SOLVER=bitwuzla clean
```

## 5. The tools — hands-on

### 5.1 `symiri` — the reference interpreter (semantic oracle)
Runs `.sir` directly, enforcing strict UB. This is ground truth.

```bash
./symiri examples/bubble_sort.sir --sym %?missing=3     # bind a symbol; prints Result: 4
./symiri prog.sir --main @f0                            # different entry function
./symiri prog.sir --check                               # static checks only (no execution)
./symiri prog.sir --sym %?x=1 --sym %?v=1,2,3,4 --dump-trace   # vector syms per-lane; trace
```
Missing/extra `--sym` bindings = error. All symbols must be bound to run.

### 5.2 `symirc` — the compiler (C / WASM / Python)
Does **not** solve — it translates (symbols become extern provider hooks).

```bash
./symirc prog.sir --target c -o prog.c                  # labels+goto by default
./symirc prog.sir --target c --structured-lowering -o prog.c   # while/if instead of goto (requires reducible CFG)
./symirc prog.sir --target wasm -o prog.wat             # PC-dispatch loop (or structured with the flag)
./symirc prog.sir --target python -o prog.py            # structured only; rejects irreducible CFGs
./symirc prog.sir --emit-main -o prog.c                 # keep @main as main (else mangled refractir_main)
./symirc prog.sir --dump-domtree / --dump-loops / --dump-control-tree   # CFG analysis inspection
```
Full C round-trip: `symirc --emit-main` → compile with `gcc` + a tiny provider for the symbol → run. The bubble_sort C binary exited `4`, matching the interpreter exactly.

### 5.3 `symirsolve` — the concretizer (the heart of the project)
Turns a *symbolic* program into a *concrete* one by solving path constraints with SMT.

```bash
# Solve along one explicit execution path (deterministic):
./symirsolve prog.sir --path '^entry,^b1,^exit' -o concrete.sir

# Or let it sample random paths until one is SAT (add -j0 for all cores):
./symirsolve prog.sir --sample 100 --require-terminal -j0 -o concrete.sir

# Cool extra modes:
./symirsolve prog.sir --require-ub -o trap.sir        # synthesize a program that TRIGGERS UB
./symirsolve prog.sir --require-nonterm -o diverge.sir # synthesize a program that never terminates (lasso)
./symirsolve prog.sir --emit-model model.json -o c.sir # also dump symbol values as JSON
```
**Live demo (bubble_sort):** the program has `%?missing` in `[0,10]` and requires the final array to be `[1,2,3,4]`. Solving along the example's path returns `SAT` and rewrites `%ctx.data[1] = 3;` — `%?missing` is **3**, and the solved program interprets to `Result: 4`. Random sampling alone can't find it; the path (which swaps must happen) matters.

Modes in a nutshell: **default** = all UB-guards asserted true (UB-free model); `--require-ub` = negate the guard conjunction (guaranteed trap); `--require-nonterm` = constrain a loop-header's state to repeat bit-identically (guaranteed divergence).

### 5.4–5.6 The reifiers — compiler fuzzing machinery
These generate random RefractIR programs to **stress-test symiri/symirc against each other** (differential testing).

```bash
# rysmith: random LEAF functions (with --emit-desc, writes .sir + .json descriptors)
./rysmith --emit-desc -n 3 -o pool/

# rylink: compose the pool into WHOLE programs (call graphs), compile, validate
./rylink -n 1 --validate -i pool/ -o progs/            # wrote prog_<id>_0/program.sir, validated: OK

# rytwin: transform a program into a semantically-EQUIVALENT variant
./rytwin --p-twin 0.5 --validate -o twin.sir pool/func_<id>_<k>.sir
#   validated: OK (3 twin exec(s))
```
`rysmith`/`rylink`/`rytwin` are the tools that make the "compiler testing" part of the repo tick: generate a program → solve its symbols → interpret it → compile it → run the compiled binary → assert both agree. That's `make test-reify` at scale.

## 6. Testing

```bash
make test                    # full suite (slow; test-reify runs 100 programs)
make test-unit               # CLI/param/reify pipeline unit tests
make test-frontend           # lexer/parser/typechecker/semchecker via symiri --check + reducibility
make test-interp             # interpreter execution
make test-backends           # C/WASM/Python codegen + execution (needs gcc, a WASM runtime)
make cross-validation        # symiri vs compiled-C, both emission modes
make test-solver             # symirsolve + curated examples
make test-reify              # differential random testing (rysmith+rylink, 100 progs)
```
Or run a single suite directly: `python3 -m test.lib.run_interp_tests test/interp ./symiri`.

Test files are discovered as `.sir` and carry metadata tags in their headers: `// EXPECT: PASS|FAIL:<code>`, `// SOLVER_ARGS: --path '^entry,^exit'`, `// INTERP_ARGS: --sym ...`, `// SKIP: <TOOL>` (e.g. the 13 `// SKIP: SOLVER` tests that isolate unsupported advanced solver cases — by design, not failures).

## 7. Your first program — the loop to get comfortable

```sir
// tiny.sir
fun @main() : i32 {
  sym %?x : value i32 in [0, 10];
^entry:
  require %?x == 3, "x is 3";
  ret %?x;
}
```
1. `./symiri tiny.sir --sym %?x=3` → `Result: 3` (needs the binding)
2. `./symirsolve tiny.sir --sample 10 --require-terminal -o solved.sir` → `SAT`, `// SOLVED: ret=3`
3. `./symiri solved.sir` → runs with no bindings
4. `./symirc solved.sir --target c --emit-main -o t.c && gcc t.c && ./a.out; echo $?` → `3`

That's the whole synthesis loop. For richer examples, `examples/` is the best teacher — `ptr_swap.sir` (pointers), `feistel_cipher.sir` (calls + intrinsics), `minicipher_v022.sir` (vectors/structs/SIMD lanes), `brainfuck.sir` (a full interpreter written in the language).

## 8. Contributing norms (from AGENTS.md)

- **TDD is mandatory**: write 5 failing tests first, watch them fail, implement, watch them pass, add edge cases. Never disable failing tests.
- Keep the interpreter/solver/backends **clean and backend-independent** — they're shared by *all* tools.
- Format with `clang-format`, zero warnings, Conventional Commits, small reviewable commits, review before committing.
- **Float serialization invariant**: use `refractir::formatDouble` / `parseFloatLiteral` for any float text crossing a file/process boundary — never `std::stod`/`to_string`. (The two backends deliberately diverge for C/WAT grammar, with a comment pointing back.)

## 9. Next steps / where to dig

- Read `docs/SPEC_v0.2.3.md` — it's also the roadmap (marked `[Planned]`/`[Shipped]`).
- `docs/undefined.md` — the strict UB model, `docs/intrinsics.md` — standard intrinsics.
- Try `--require-ub` and `--require-nonterm` synthesis — the most unique features of the tool.
- If you want the WASM/Python backends tested, install a WASM runtime (`wasmtime`/`wasmer`) — `make test-backends` skips those targets without one.
