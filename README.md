# RefractIR

> [!WARNING]
> RefractIR is under active development: APIs and IR details are still evolving. [docs/SPEC_v0.2.3.md](./docs/SPEC_v0.2.3.md) is the normative specification and doubles as the roadmap for the v0.2.3 line, marking each feature `[Shipped]` or `[Planned]`.

RefractIR (internally SymIR) is a CFG-based symbolic intermediate representation for program synthesis, symbolic execution, and constraint generation for SMT solvers over bit-vector logic. It is a foundation for tools that reason about program semantics, explore execution paths, or synthesize code satisfying a stated property.

A RefractIR program is a template rather than a finished program: it may contain symbols, unknowns whose values a solver picks later under constraints drawn from a chosen execution path and from explicit properties on that path.

Core concepts:

* Path-oriented: designed for symbolic execution and path-based analysis.
* Symbolic by design: a program may be symbolic (solver-chosen unknowns marked with `?`) or fully concrete.
* Explicit control flow: basic blocks and `br` terminators, with no structured-nesting requirement.
* SMT-friendly: expressions are restricted to flat, left-to-right forms, so the generated bit-vector constraints stay predictable.
* Strict semantics: undefined behaviour (division by zero, out-of-bounds access, and the rest of [docs/undefined.md](./docs/undefined.md)) makes the executed path infeasible rather than merely suspect, which is what makes a wrong answer a bug report.

## 🛠️ Tools overview

| Tool | Purpose |
| :--- | :--- |
| `symiri` | Interpreter: execute `.sir` programs directly with concrete values or symbol bindings. |
| `symirc` | Compiler: translate `.sir` programs into C, WebAssembly, or Python. |
| `symirsolve` | Solver: concretize symbolic programs by solving path constraints via SMT. |
| `rysmith` | Generator: random RefractIR leaf functions for compiler testing. |
| `rylink` | Generator: random whole programs composed from a leaf-function pool. |
| `rytwin` | Transformer: an equivalent variant of a generated program. |

## 🚀 Getting started

### Prerequisites

The quickest path is a container:

```bash
docker build -t refractir:latest --build-arg UID=$(id -u) --build-arg GID=$(id -g) .
docker run -it --rm -v $(pwd):/workspace refractir:latest bash
```

Once inside, skip prerequisites below and jump directly to the [Building](#building) section.

To run RefractIR locally instead, the following tools are required:

- C++20 compatible compiler (GCC 10+ or Clang 10+)
- Bitwuzla (Required by default)
  - Install: https://github.com/bitwuzla/bitwuzla
- Z3 (Optional)
  - Install: https://github.com/Z3Prover/z3
- Python 3 (Optional, for the test suite and for running Python-target output)
- WASM runtime (Optional, for running WASM backend tests such as Wasmtime, Wasmer, or Node.js)

### Building

Build every tool, then run the test suite:

```bash
make -j$(nproc)
make test
```

### Usage

Interpret a concrete program, then a symbolic one with its symbols bound:

```bash
./symiri test/interp/complex_he_lcs.sir
./symiri test/interp/basic.sir --sym %?a=10 --dump-trace
```

Compile to each target:

```bash
./symirc examples/bubble_sort.sir --target c      -o out.c
./symirc examples/bubble_sort.sir --target wasm   -o out.wat
./symirc examples/bubble_sort.sir --target python -o out.py
```

Solve for symbol values, either along a path you name or over sampled paths. Every example ships the path that concretizes it in a sibling `_path.txt`:

```bash
./symirsolve examples/ptr_counter.sir --path "$(cat examples/ptr_counter_path.txt)" -o concrete.sir
./symirsolve examples/ptr_counter.sir --sample 100 --require-terminal -o concrete.sir
```

Generate random programs. `--emit-desc` writes the descriptors `rylink` needs:

```bash
./rysmith --emit-desc -n 100
./rylink -n 100
```

Emit an equivalent twin of a generated program. `rytwin` reads the `.state.json` sidecar when it is there and profiles the program in-process otherwise:

```bash
./rysmith -n 1 --emit-desc -o out/
./rytwin --p-twin 0.5 --validate -o out/<func>.twin.sir out/<func>.sir
```

### Switching SMT backends

The solver backend is selected at compile time through the `SOLVER` variable:

```bash
make SOLVER=bitwuzla   # default
make SOLVER=alivesmt
```

Bitwuzla is tuned for bit-vector and floating-point logic and is the faster choice for our symbolic execution. AliveSMT is a Z3-based backend derived from Alive2, an alternative where Z3's heuristics or theories are preferred.

## 📝 RefractIR example

A four-round substitution-permutation cipher, exercising structs and `ptrfield`, SIMD vectors, function calls and overloaded intrinsics in one file. The round state `@CipherState` is touched only through typed pointers projected from a single `addr`. A `<16> i8` S-box and a `<4> i32` round-constant table live in vector registers and are indexed per lane. `@popcount` is declared at both `i8` and `i32` widths, and the type checker pins each call site to the right overload. One plaintext byte is symbolic, and the solver picks a value driving the diffusion accumulator to a target.

```sir
struct @CipherState {
  s0:    i8;   // byte 0 of the 4-byte block
  s1:    i8;   // byte 1
  s2:    i8;   // byte 2 (initially symbolic)
  s3:    i8;   // byte 3
  acc:   i32;  // diffusion accumulator
  round: i32;  // round counter (0..3)
}

// Overloaded intrinsics: same name, distinct signatures.
intrinsic @popcount(%x: i8)  : i8;
intrinsic @popcount(%x: i32) : i32;
intrinsic @rotl(%x: i32, %n: i32) : i32;

// Substitute one byte through a 16-entry SIMD S-box.
fun @sbox(%v: i8) : i8 {
  let %tbl: <16> i8 = {0x6, 0xB, 0x5, 0x4, 0x2, 0xE, 0x7, 0xA,
                       0x9, 0xD, 0xF, 0xC, 0x3, 0x1, 0x0, 0x8};
  let mut %wide:   i32 = 0;
  let mut %nibble: i32 = 0;
  let mut %out:    i8  = 0;
  let %MASK:       i32 = 15;
^entry:
  %wide   = %v as i32;
  %nibble = %wide & %MASK;
  %out    = %tbl[%nibble]; // SIMD lane read
  ret %out;
}

// Round mix: rotl(acc XOR (byte_pop_sum + popcount(rc)), n).
fun @mix(%acc: i32, %byte_pop_sum: i32, %rc: i32, %round_idx: i32) : i32 {
  let mut %pop_rc:  i32 = 0;
  let mut %mix_in:  i32 = 0;
  let mut %xored:   i32 = 0;
  let mut %rotated: i32 = 0;
^entry:
  %pop_rc  = call @popcount(%rc);       // @popcount(i32) overload
  %mix_in  = %byte_pop_sum + %pop_rc;
  %xored   = %acc ^ %mix_in;
  %rotated = call @rotl(%xored, %round_idx);
  ret %rotated;
}

fun @main() : i32 {
  sym %?byte2 : value i8 in [32, 126];  // symbolic plaintext byte
  let %rconsts: <4> i32 = {-0x61C88647, -0x7A143595, -0x3D4D51CB, 0x27D4EB2F};
  let mut %st:  @CipherState = 0;
  let mut %pst: ptr @CipherState = null;
  let mut %p_s0:   ptr i8  = null; let mut %p_s1:   ptr i8  = null;
  let mut %p_s2:   ptr i8  = null; let mut %p_s3:   ptr i8  = null;
  let mut %p_acc:  ptr i32 = null;
  let mut %i:      i32 = 0; let mut %rc:    i32 = 0; let mut %rc_lo: i8 = 0;
  let mut %b0: i8 = 0; let mut %b1: i8 = 0; let mut %b2: i8 = 0; let mut %b3: i8 = 0;
  let mut %p0: i8 = 0; let mut %p1: i8 = 0; let mut %p2: i8 = 0; let mut %p3: i8 = 0;
  let mut %w0: i32 = 0; let mut %w1: i32 = 0; let mut %w2: i32 = 0; let mut %w3: i32 = 0;
  let mut %pop_sum: i32 = 0; let mut %acc: i32 = 0; let mut %r_idx: i32 = 0;
  let %N: i32 = 4; let %ONE: i32 = 1; let %TARGET: i32 = 51328;

^entry:
  // Project typed pointers into the state struct (no aggregate stores).
  %pst   = addr %st;
  %p_s0  = ptrfield %pst, s0; %p_s1 = ptrfield %pst, s1;
  %p_s2  = ptrfield %pst, s2; %p_s3 = ptrfield %pst, s3;
  %p_acc = ptrfield %pst, acc;
  store %p_s0, 0x12 as i8; store %p_s1, 0x34 as i8;
  store %p_s2, %?byte2;    store %p_s3, 0x78 as i8;
  store %p_acc, 0;
  br ^loop_cond;

^loop_cond:
  br %i < %N, ^loop_body, ^check;

^loop_body:
  %rc    = %rconsts[%i];                  // SIMD lane read
  %rc_lo = %rc as i8;
  %b0 = load %p_s0; %b1 = load %p_s1; %b2 = load %p_s2; %b3 = load %p_s3;
  %b0 = call @sbox(%b0); %b1 = call @sbox(%b1);
  %b2 = call @sbox(%b2); %b3 = call @sbox(%b3);
  %b0 = %b0 ^ %rc_lo;    %b1 = %b1 ^ %rc_lo;
  %b2 = %b2 ^ %rc_lo;    %b3 = %b3 ^ %rc_lo;
  %p0 = call @popcount(%b0); %p1 = call @popcount(%b1); // @popcount(i8)
  %p2 = call @popcount(%b2); %p3 = call @popcount(%b3);
  %w0 = %p0 as i32; %w1 = %p1 as i32; %w2 = %p2 as i32; %w3 = %p3 as i32;
  %pop_sum = %w0 + %w1 + %w2 + %w3;
  %acc   = load %p_acc;
  %r_idx = %i + %ONE;
  %acc   = call @mix(%acc, %pop_sum, %rc, %r_idx);
  store %p_s0, %b0; store %p_s1, %b1;
  store %p_s2, %b2; store %p_s3, %b3;
  store %p_acc, %acc;
  %i = %i + %ONE;
  br ^loop_cond;

^check:
  %acc = load %p_acc;
  require %acc == %TARGET, "minicipher hits target";
  ret 0;
}
```

The annotated source is [examples/minicipher_v022.sir](./examples/minicipher_v022.sir). More programs live in [examples](./examples/) and [test](./test/).

## 📁 Project structure

```text
.
├── include/          # Header files
│   ├── ast/          # AST definitions, cloning, construction
│   ├── frontend/     # Lexer, parser, type checker, parse-and-check pipeline
│   ├── analysis/     # CFG, dataflow, state sets, dominators/loops/structurizer, pass manager
│   ├── backend/      # C, WASM, and Python backends
│   ├── solver/       # SMT integration
│   └── reify/        # Reify generators (rysmith, rylink, rytwin)
├── src/              # Implementation files
├── docs/             # Tool and language documentation
├── test/             # Test suite and regression tests
└── Makefile          # Build system
```

## 🐞 Bug Showcases

Please refer to [LLVM](https://github.com/llvm/llvm-project/issues?q=is%3Aissue+author%3Azhendongsu+created%3A%3E2026-06-01) and [GCC](https://gcc.gnu.org/bugzilla/buglist.cgi?email1=zhendong&emailreporter1=1&emailtype1=substring&f1=creation_ts&o1=greaterthan&product=gcc&query_format=advanced&v1=2026-06-01).

## 📚 Documentation

* [Changelog](./CHANGELOG.md): release history from v0.0.1 onward.
* [Language specification (v0.2.3, current)](./docs/SPEC_v0.2.3.md): the normative spec; doubles as the v0.2.3 roadmap, marking each feature `[Shipped]` or `[Planned]`.
* [Floating-point model](./docs/float.md): the finite-only IEEE 754 value model and bit-exact text serialization.
* [Standard intrinsics reference](./docs/intrinsics.md): per-intrinsic signatures, SMT encodings, UB conditions, and per-backend lowering rules.
* [Undefined behaviour reference](./docs/undefined.md): the strict UB rules and how each tool enforces them.
* [CFG reducibility and structured control flow](./docs/reducibility.md): dominator trees, the reducibility check, loop forests, and the control-tree structuring behind the Python target and `--structured-lowering`.
* [symiri user guide](./docs/symiri.md): the reference interpreter.
* [symirc user guide](./docs/symirc.md): the C / WASM / Python translator.
* [symirsolve user guide](./docs/symirsolve.md): the SMT concretizer.
* [reify user guide](./docs/reify.md): rysmith, rylink, and rytwin.

## 📋 License

MIT.
