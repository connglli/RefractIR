# RefractIR: Agent Guideline

+ Knowledge Background: Optimizing Compilers, Program Analysis, Symbolic Execution, SMT (Bit-Vector Theory)
+ Implementation Language: C++ 20
+ Primary Source Directory: ./src


## Project Overview

RefractIR (internally called SymIR) is a **CFG-based symbolic intermediate representation** designed for:

- **Program synthesis**
- **Symbolic execution**
- **Constraint generation for SMT solvers (Bit-Vector logic)**

A RefractIR program is a **template**, not a fully concrete program. It may contain **symbols** (unknowns, marked with `?`) whose values are solved later by an SMT solver under constraints derived from:

1. A **specific execution path** through the CFG
2. Explicit **properties** (e.g., `require` and/or other user-provided properties) that must hold on that path

The key design goals are:

- SMT-friendliness (predictable BV constraints, minimal nonlinearity)
- Clear semantics with **strict undefined behavior (UB)**
- Explicit control-flow (CFG, no SSA, user-friendly)
- Simplicity and analyzability over expressiveness

RefractIR deliberately restricts expressions (flat, left-to-right, no parentheses) and uses **LLVM-style syntax** (`@`, `%`, `br`, basic blocks) while remaining language-agnostic.

The current formal specification is **[./docs/SPEC_v0.2.3.md](./docs/SPEC_v0.2.3.md)** (v0.2.3, **in progress** — the spec doubles as the release roadmap, with per-feature **[Shipped]**/**[Planned]** status). The v0.2.2 spec (function calls (`call`) and interprocedural execution; external declarations (`decl`) with `-I` link resolution and behavioral contracts (`pre`/`post`/`ret`); standard intrinsics) is preserved at [./docs/SPEC_v0.2.2.md](./docs/SPEC_v0.2.2.md), the v0.2.1 spec (aggregate pointers `ptr [N] T`, `ptr @S`, `ptrindex`, `ptrfield`; SIMD vector types `<N> T` with lane-wise arithmetic and `cmp`; floating-point value model; mask-based `select`) at [./docs/SPEC_v0.2.1.md](./docs/SPEC_v0.2.1.md), the v0.2.0 pointer baseline at [./docs/SPEC_v0.2.0.md](./docs/SPEC_v0.2.0.md), and the pre-pointer baseline at [./docs/SPEC_v0.1.0.md](./docs/SPEC_v0.1.0.md) for reference. The release history from v0.0.1 onward is summarized in [./CHANGELOG.md](./CHANGELOG.md).

## Language at a Glance

Key characteristics:

- **Non-SSA**, mutable locals via `let mut`
- **Symbols**: solver-chosen unknowns (`@?x`, `%?y`)
- **CFG-based**: explicit basic blocks (`^label`)
- **Expressions**:
  - Flat, left-to-right
  - `+`, `-` at expression level
  - `*`, `/`, `%` at atom level
- **Division/modulo**: round toward 0 (C / SMT `bvsdiv`, `bvsrem`)
- **select** expression:
  - Lazy (only selected arm evaluated)
  - Expression-level conditional; mask-based `select <N> i1` for per-lane blends (v0.2.1)
- **Floating-point**:
  - `f32` (IEEE 754 single) and `f64` (IEEE 754 double)
  - Finite-only domain: ±∞ and NaN are UB
  - `%` is C's `fmod` (truncated-quotient remainder)
  - All operations use RNE rounding
- **Pointers (v0.2.0)**:
  - `ptr T` type, scalar/pointer pointees
  - `addr lv` produces `ptr T` (requires `let mut` root)
  - `load p`, `store p, v` for read/write through pointers
  - `null` literal (typed by context)
  - Pointer arithmetic: `ptr T ± iN → ptr T`, `ptr T - ptr T → i64` (element distance)
  - No `sym` of pointer or aggregate (array/struct) type; no pointer/integer casts
- **Aggregate pointers (v0.2.1)**:
  - `ptr [N] T` / `ptr @S` — pointers to arrays and structs
  - `ptrindex <ptr [N] T>, <idx>` → `ptr T` (navigate array element)
  - `ptrfield <ptr @S>, <field>` → `ptr FieldType` (navigate struct field)
  - Packed `sizeof(@S) = Σ sizeof(field_i)`; no padding
  - No aggregate `load`/`store`; navigate to scalar/pointer leaves first
- **Vector types (v0.2.1)**:
  - `<N> T` — fixed-width SIMD vector of scalar elements
  - Lane-wise arithmetic: all scalar operators lift to vectors
  - `cmp <relop> lhs, rhs` — reified comparison → `i1` (scalar) or `<N> i1` (vector mask)
  - Lane access via subscript: `%v[%i]` reads/writes lane `i`
  - Whole-vector copy: `%v = %w;`
  - Not addressable: no `ptr <N> T`, no `addr` on vector locals
  - `sym` of vector type allowed (per-lane independent symbols)
- **Strict UB**:
  - Division/modulo by zero
  - Out-of-bounds array access
  - Reading `undef`
  - Null/uninitialised/out-of-bounds pointer dereference, cross-object pointer arithmetic or relational comparison
  - FP overflow/NaN, float-to-integer out-of-range
  - Vector lane-wise UB, out-of-bounds lane access (v0.2.1)

## Toolchain Overview

| Tool | Role |
|------|------|
| `symirc` | Translate `.sir` to C / WebAssembly / Python |
| `symiri` | Interpret `.sir` programs |
| `symirsolve` | Concretize symbolic programs using SMT |
| `rysmith` | Generate random RefractIR leaf functions (reify) |
| `rylink` | Compose leaf functions into whole programs (reify) |
| `rytwin` | Transform a generated program into a semantically-equivalent variant (reify) |

Documentation of each tool: [./docs/](./docs). A hands-on walkthrough (build, tool usage, first program) is at [./GUIDE.md](./GUIDE.md).

Remember: The interpreter, solver, and compiler backends are shared foundational for **all** RefractIR tooling. They should be kept **clean, correct, and well-tested**. They should also be kept **independent of any specific downstream tools**, with mentioning none in their implementation.

## Repo Layout Quirks

- `CLAUDE.md` and `GEMINI.md` are **symlinks to `AGENTS.md`** — edits to `AGENTS.md` propagate to all three automatically.
- `vscode-refractir/` is a standalone VSCode syntax-highlighting extension, not part of the toolchain.
- `alivesmt/` is a **vendored** Z3-based SMT backend (Alive2-derived). It is secondary: it has modeling gaps (floats, some aggregate/pointer patterns report "Unknown/Unimplemented kind") and the reify tools are not supported under it. **Bitwuzla is the default and the well-tested backend** — always build/test with `make SOLVER=bitwuzla`.
- The solver backend is selected at **compile time** via the `SOLVER` Makefile variable. `make clean` also needs `SOLVER=bitwuzla` — the Makefile probes the solver *before* cleaning, so a bare `make clean` after `make SOLVER=alivesmt` fails.
- There is **no CI** (no `.github/`). Quality gates are the local pre-commit hooks (`.pre-commit-config.yaml`): clang-format on C++ sources, ruff on Python, and a Conventional Commits check at the `commit-msg` stage.
- The `fuzz/` directory currently contains only a README; the `symirfuzz.py` it references does not exist yet.

## Compilation / Analysis Pipeline

### Shared Frontend Pipeline

```
Source (.sir)
  ↓
Lexer
  ↓
Parser → AST
  ↓
CFG Builder
  ↓
TypeChecker (BV-aware)
  ↓
Semantic Checker
```

### Tool-Specific Pipelines

`symirc`:
```
Checked AST + CFG
  ↓
Lowering (+ reducibility check & control-tree structuring for Python and C --structured-lowering)
  ↓
C / WASM / Python Code Generation
```

`symiri`:
```
Checked AST + CFG
  ↓
Symbol Binding (--sym)
  ↓
Interpreter Execution
```

`symirsolve`:
```
Checked AST + CFG
  ↓
Path-based Symbolic Execution
  ↓
SMT Solving
  ↓
Concrete .sir
```

### 1. Lexer
- Converts source text into tokens
- Handles identifiers with sigils (`@`, `%`, `@?`, `%?`, `^`)
- Handles comments and string literals
- No semantic knowledge

### 2. Parser
- Recursive-descent parser
- Builds a **typed, structured AST**
- Preserves source spans for diagnostics
- AST is analysis-oriented (not syntax-oriented)

### 3. CFG Builder
- Indexes basic blocks by label
- Builds successor and predecessor lists
- Validates `br` targets
- Computes traversal orders (e.g., reverse postorder)
- Forms the backbone for all dataflow analyses

### 4. TypeChecker (BV-aware)
- Maps RefractIR integer types to **SMT bit-vectors**
  - `i32` → `(_ BitVec 32)`
  - `i64` → `(_ BitVec 64)`
  - `iN`  → `(_ BitVec N)`
- Ensures:
  - Type correctness of expressions
  - Bitwidth compatibility
  - Correct typing of `select`
  - Assignment compatibility
  - Function return correctness
- Produces **typed annotations** for AST nodes
- Boolean conditions are treated separately from BV integers

### 5. Semantic Checker
Ensures program well-formedness beyond typing:

- Variables and symbols are declared before use
- No duplicate declarations
- Assignment only to `let mut` locals
- Parameters and symbols are immutable
- Definite initialization:
  - Parameters are initialized
  - `undef` is uninitialized
  - Reads before initialization are errors
- CFG consistency checks

### 6. Symbolic Execution / Constraint Generation

- Executes along a **user-selected path** (in `symirsolve` / SMT solver backend)
- Collects:
  - Path conditions from branches
  - Assumptions (`assume`)
  - Required properties (`require`)
- Applies **strict UB pruning**
- Produces BV and FP constraints suitable for SMT solvers (Bitwuzla/Z3)

### 7. Language Lower / Translator
- Translate a symbolic or concrete program into an existing language
- First-class support are C, WebAssembly, and Python.
- The Python target accepts only **reducible** CFGs and reconstructs genuine `while`/`if` control flow (dominator tree → reducibility check → loop forest → control tree → structured lowering; see [./docs/reducibility.md](./docs/reducibility.md)). The C backend emits labels+goto by default and offers the same structured reconstruction (`while`/`do-while`/`if`) behind `--structured-lowering`; the WASM backend emits a `$__pc`/`br_table` dispatch loop by default and, under `--structured-lowering`, reconstructs `block`/`loop`/`if` from the *unlowered* control tree using WASM's native multi-level `br`.
- For symbolic program translation, use external function declarations to indicate symbols.
  - C: `extern int func_name_symbol_name(...);`
  - WASM: `import func_name symbol_name (func func_name_symbol_name (....))`
  - Python: `func_name__symbol_name()` provider calls injected into module globals by the embedding


## Project Structure

Goto [./README.md](./README.md)


## Testing – TDD Approach (MANDATORY)

ALWAYS follow a strict Test-Driven Development discipline.

### Required workflow

1. Write **five failing tests** that expose the bug or demonstrate the desired behavior
2. Run the tests **one by one** to confirm they fail
3. Implement the fix or feature
4. Run the tests **one by one** to confirm they pass
5. Add the **smallest additional test** that covers edge cases, in the correct test directory

### Never

1. Disable failing tests
2. Modify tests to avoid triggering bugs
3. Add workarounds that bypass the real issue
4. Implement features without a test demonstrating them first

### How to Run Tests

The test suite is managed via the `Makefile` targets:

- `make test`: Runs the entire test suite sequentially.
- `make test-unit`: Runs unit tests for command-line arguments and reification pipelines.
- `make test-frontend`: Runs frontend validation tests (lexer, parser, type checker, semantic checker) using `symiri --check`, and CFG reducibility tests (dominator trees, loops, control trees) against `.sir.expected` files.
- `make test-interp`: Runs reference interpreter execution tests without checking mode.
- `make test-backends`: Runs compiler backend compilation and execution tests for C (goto and --structured-lowering modes), WASM, and Python targets.
- `make cross-validation`: Cross-validates interpreter execution outputs and UB behavior against compiled C binaries, in both goto and structured-lowering emission modes.
- `make test-solver`: Runs symbolic execution and SMT constraint solver tests.
- `make test-reify`: Runs differential random generation testing for rysmith and rylink (`-n 100 --seed 1234`); forward the parallelism you want with `make -jN test-reify`.

### Test-harness contract (know this before writing a test)

- Tests are **all `*.sir` files found recursively** under the `test/` directory, grouped by component (`test/{lexer,parser,cfgbuilder,typechecker,semchecker,reducibility,interp,xval,solver,compile,sbackend}`). Each suite is a `test/lib/run_*_tests.py` script; run one directly, e.g. `python3 -m test.lib.run_interp_tests test/interp ./symiri`.
- Metadata tags are read from the **first 10 lines** of each `.sir`: `// EXPECT: PASS` or `// EXPECT: FAIL:<subtype>`, plus `// INTERP_ARGS:`, `// COMPILER_ARGS:`, `// SOLVER_ARGS:` (tokenized), and `// SKIP: <TOOL>` (comma-separated tags; reason after `—`/`--`/`#`/`(`). A file with **no `EXPECT` tag is silently skipped** — don't write a test without one.
- `FAIL:<subtype>` maps to a specific exit code (see `test/lib/run_interp_tests.py`): `LexError`=2, `ParseError`=3, `StaticError`=4, `UndefinedBehavior`=5, `RequireViolation`=6. `symiri --check` is the frontend-only mode used by the lexer/parser/typechecker/semchecker suites.
- `make test-unit` is different: it runs `test/unit/run_*_tests.py` scripts that assert on a binary's stdout/sidecars/output layout, not on `.sir` metadata.
- `make test-backends` needs `gcc` and a WASM runtime (wasmtime/wasmer/node) — WASM targets are skipped without one. Interpreter and cross-validation tests have a 5s per-file timeout.

## Dependency Management

### C++
- Dependencies are managed **manually**
- Prefer header-only or standard-library-only solutions
- When introducing a new dependency:
  - Update `README.md`
  - Clearly document installation steps and versions

### Python (if used for tooling)
- Virtual environment: `./venv`
- Activate with:
  ```bash
  source venv/bin/activate
  ````

* Dependencies:
  * `requirements.txt` – runtime
  * `requirements.dev.txt` – development
* Always pin exact versions

## Principles and Best Practices

Always follow good practices:

1. Use git frequently and meaningfully
2. Follow **Conventional Commits**
3. Keep `README.md`, `AGENTS.md`, and `CHANGELOG.md` up to date, and the current spec `docs/SPEC_v0.2.3.md` (it doubles as the roadmap)
4. Fix **all compiler warnings**
5. Keep a clean, layered project structure
6. Write high-quality comments that explain *why*, not *what*
7, Keep CHANGELOG concise (multiple related entries can be summarized in one line)

Always check whether a design/implementation is *elegant*:

(1) It retains a minimalist core and a clean conceptual model.
(2) It is simple enough that an experienced developer can understand it within five minutes without any explanation.

Always keep in mind the following principles to make it elegant before designing any new feature or changing existing behavior. Consider these principles, think twice, and then design:

1. KISS: Keep It Simple, Stupid. Is this the simplest design that works?
2. SINE: Simplicity Is Not Enough. Is this design analyzable, testable, and solver-friendly?
3. DRY: Don't Repeat Yourself. Are there existing abstractions/implementations that can be reused?
4. YAGNI: You Ain't Gonna Need It. Do we really need this feature now, or is it speculative?
5. SOLID: Single Responsibility, Open/Closed, Liskov Substitution, Interface Segregation, Dependency Inversion. Does this design adhere to these principles?

## Floating-point serialization invariant (MANDATORY)

RefractIR carries `f32`/`f64` values bit-exactly across **every** boundary
that involves text — `.sir` source, descriptor JSON, SOLVED/PARAMS/RETURN
headers, model-dump files, and CLI positional args. The invariant is:

> **One canonical bit-exact format. One canonical parser. Used everywhere
> RefractIR text crosses a process or file boundary.**

### The two canonical entry points

- **`refractir::formatDouble(double)`** (`include/ast/ast.hpp`) — emits the
  shortest decimal string that round-trips via `std::to_chars(…,
  std::chars_format::shortest)`, with `.0` appended if neither `.` nor
  exponent is present so int/float dispatch on the resulting string is
  unambiguous and signed zero survives.
- **`refractir::parseFloatLiteral(std::string)`** (`include/ast/ast.hpp`) —
  uses `std::strtod` directly. Subnormals (returned values < `DBL_MIN`)
  are accepted; only true overflow to `±HUGE_VAL` raises. **Never call
  `std::stod`** anywhere in RefractIR — libstdc++ throws `out_of_range` on
  any `ERANGE`, including valid subnormals, and a perfectly representable
  denormal would abort the interpreter.

### Where the invariant must hold

All of these MUST go through the canonical pair:

- `SIRPrinter::printDouble` — `.sir` source emission.
- `rysmith` `fmtModelVal` — descriptor JSON + SOLVED header.
- `symirsolve` `fmtVal` — SOLVED header on solved programs.
- `symirsolve` model-dump JSON output.
- `reify::rewrite` `parseF64` — descriptor JSON read-back.
- Parser/Lexer float tokens — already routed through `parseFloatLiteral`.
- The interpreter's CLI positional-arg parser.

### Where it intentionally diverges

- `src/backend/c_backend.cpp` and `src/backend/wasm_backend.cpp` emit
  literals in **C** and **WAT** grammar respectively (suffixes, infinity
  syntax, etc.), so each backend has its own bit-exact formatter. Each
  carries a comment pointing back to `refractir::formatDouble` to flag the
  divergence as intentional.

### When you add a new producer or consumer

If you are about to write any of these patterns, **stop and use the
canonical pair instead**:

- `std::stod`, `std::stof`, `std::atof`
- `std::cout << double_value` with default precision
- `std::to_string(double)`
- `std::ostringstream` with explicit `precision(17)` or `max_digits10`
- `printf("%f", …)` or `printf("%g", …)` for FP output

`printf("%a", …)` is acceptable for **bit-exact xval output** (the
interpreter's `Result:` line uses this so the C-side `printf("Result:
%a\n", …)` can be compared byte-equal in the diff test). Hex-float form
is parseable by `strtod` and lossless by construction; it just isn't the
canonical *decimal* form.

## Before Starting Work

1. Review recent history:

   ```bash
   git log [--oneline] [--stat] [--name-only] # Show brief/extended history
   git show [--summary] [--stat] [--name-only] <commit> # Show brief/extended history of a commit
   git diff <commit> <commit> # Compare two different commits
   git checkout <commit> # Checkout and inspect all the details of a commit
   ```
2. Understand existing design decisions before changing behavior
3. For large tasks, commit incrementally with clear messages

## Before Saving Changes

ALWAYS:

1. Clear all compiler warnings
2. Format code with `clang-format`
3. Ensure all tests pass (timeouts excepted)
4. Check changes with `git status`
5. Split work into small, reviewable commits
6. Ask the user to review your changes before committing
7. Use Conventional Commit messages:

```text
<type>[optional scope]: <title>

<body>

[optional footer]
```

* Title ≤ 50 characters
* Body explains intent and design impact

**Remember:**
RefractIR prioritizes *clarity, analyzability, and solver-friendliness* over surface-level convenience.
Preserve these properties in every change.
