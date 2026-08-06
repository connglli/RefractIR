# Hands-on: verifying a `rytwin` twin by hand

This guide walks through a single `rytwin` run on a tiny program and
hand-checks the three claims the tool makes:

1. it emits a **twin** of the input program,
2. the twin and the original are **equivalent** (same result for the same input),
3. the guard that chooses between twin and original is **satisfied by the input
   that was used to profile the program** (and only by that input).

Everything below is the actual output of the current build (seed 1, `--twin-guard
bijection`), produced from the repo root. Prerequisites: the tools are built
(`make SOLVER=bitwuzla`) and, if bitwuzla lives in a non-system prefix, the
dynamic loader points at it (export `LD_LIBRARY_PATH` accordingly). The input
fixture is committed at `test/unit/fixtures/rytwin_handcheck/tiers.sir`; the
`twin.sir` produced below is a generated artifact (gitignored — delete and
re-create it with the rytwin command whenever you want a fresh one).

## 1. The input program

`test/unit/fixtures/rytwin_handcheck/tiers.sir` — a one-parameter leaf. On entry
`%a` is 9 and `%b` is 100; the single `^work` block adds the input `%pa0` to
`%a`. The `// SOLVED: %pa0=5` header tells rytwin the concrete input used to
profile it:

```sir
// SOLVED: %pa0=5
fun @tiers(%pa0: i8) : i8 {
  let mut %a: i8 = 9;
  let mut %b: i8 = 100;
^entry:
  br ^work;
^work:
  %a = %a + %pa0;
  br ^exit;
^exit:
  ret %a;
}
```

With the profiled input `%pa0=5` the program computes `%a = 9 + 5 = 14` and
returns `14`.

## 2. Produce the twin

```bash
./rytwin test/unit/fixtures/rytwin_handcheck/tiers.sir --p-twin 1.0 --seed 1 --twin-guard bijection -o twin.sir
```

Output:

```
rytwin: wrote "twin.sir" (1 twin(s), entry @tiers)
```

`twin.sir` contains the original `@tiers` plus one extra function,
`@__twg_tiers_work`, the guard. (The file is ~870 lines because the guard
unrolls one square-and-multiply round per bit of the value it checks.)

## 3. The two versions of the block

The original `^work` block:

```sir
^work:
  %a = %a + %pa0;
  br ^exit;
```

In the twin, `^work` is replaced by a **guarded diamond**: first a call to the
guard over the live state at the block's entry (`%a`, `%b`, `%pa0`); if the
guard returns non-zero, run the synthesized twin block, otherwise run the
original:

```sir
^work:
  br call @__twg_tiers_work(%a, %b, %pa0) != 0, ^work__twin, ^work__orig;

^work__twin:                                  # net effect on the profiled state
  require %a != 0, "div nonzero";             #   (a,b) = (14,100), same as original
  %b = %pa0 + -1 % %a;
  %b = %a + %a;
  %b = -1 as i8 + -1 + %pa0;
  %a = %a + 5;
  %b = %b + 97;
  br ^work__merge;

^work__orig:                                  # the original block, preserved verbatim
  %a = %a + %pa0;
  br ^work__merge;

^work__merge:
  br ^exit;
```

The twin block is solver-synthesized garbage that only *looks* different: on the
profiled state its net effect is exactly the original's. Starting from
`(a,b) = (9,100)`, `%pa0 = 5`:

- original: `a = 9+5 = 14` → `(14,100)`
- twin: `b = 5 + (-1 % 9) = 5 + (-1) = 4` (`%` binds tighter than `+`),
  `b = 9+9 = 18`, `b = -1-1+5 = 3`, `a = 9+5 = 14`, `b = 3+97 = 100` → `(14,100)`

Same state after the block, so the two paths merge onto identical exits.

## 4. Verification: feed the same input to both programs

`rytwin`'s own flag does this for you:

```bash
./rytwin test/unit/fixtures/rytwin_handcheck/tiers.sir --p-twin 1.0 --seed 1 --twin-guard bijection --validate -o twin.sir
```

```
rytwin: wrote "twin.sir" (1 twin(s), entry @tiers)
rytwin: validated: OK (1 twin exec(s))
```

`validated: OK` means the interpreter ran the original and the twin on the same
input and both returned `14`. Do it by hand with `symiri` — entry-function
parameters are passed as **positional CLI args after the input file** (symbols
would be bound with `--sym %?name=value`):

```bash
$ ./symiri test/unit/fixtures/rytwin_handcheck/tiers.sir --main @tiers 5
Result: 14

$ ./symiri twin.sir --main @tiers 5
Result: 14
```

Equivalence must hold for *every* input, not just the profiled one. On any other
input the guard fails and the original block runs, so the twin degrades to the
original:

```bash
$ for i in 3 9; do
    echo -n "$i: original="; ./symiri test/unit/fixtures/rytwin_handcheck/tiers.sir --main @tiers $i 2>/dev/null | grep Result
    echo -n "$i: twin=     "; ./symiri twin.sir --main @tiers $i 2>/dev/null | grep Result
  done
3: original=Result: 12
3: twin=     Result: 12
9: original=Result: 18
9: twin=     Result: 18
```

(The `warning: Unused local: %b` noise on stderr is harmless; it is suppressed
above with `2>/dev/null`.)

## 5. The guard: is it satisfied by the input we have?

The guard is just a function returning `i1`. Run it directly with the state at
`^work`'s entry **on the profiled input** — `%a=9, %b=100, %pa0=5` — and it
must return *true* (an `i1` true prints as signed `-1`; the diamond tests
`!= 0`, so `-1` means "take the twin"):

```bash
$ ./symiri twin.sir --main @__twg_tiers_work 9 100 5
Result: -1     # true  → guard fires → twin block runs
```

Change any one leaf and the guard must fail (`0` → original block runs):

```bash
$ ./symiri twin.sir --main @__twg_tiers_work 9 100 3
Result: 0      # %pa0 != profiled 5 → false
$ ./symiri twin.sir --main @__twg_tiers_work 5 100 5
Result: 0      # %a != profiled 9 → false
```

So the guard fires *exactly* on the profiled state — that is why on input `5`
the twin runs (and returns `14`), and on inputs `3` and `9` the original runs.

### What the guard actually computes

For each leaf it ANDs three checks into `%__acc` (all must pass):

1. **Range gate** — the value is below a prime `P` (as a signed compare, done
   via XOR with the sign mask). For our input: `9 < 239`, `100 < 1048559`,
   `5 < 233`.
2. **The DLP trapdoor** — an unrolled square-and-multiply raising a generator
   `g` to the value: `res = g^value mod P`.
3. **Target compare** — `res == target`, where `target` is `g^value mod P`
   evaluated on the *profiled* value.

Leaf | value | prime P | generator g | target (`g^value mod P`) | gate
-----|-------|---------|-------------|--------------------------|-----
`%a` | 9     | 239     | 7           | `7^9 mod 239   = 130`    | 9 < 239 ✓
`%b` | 100   | 1048559 | 7           | `7^100 mod 1048559 = 1038966` | 100 < 1048559 ✓
`%pa0`| 5    | 233     | 3           | `3^5 mod 233   = 10`     | 5 < 233 ✓

Check these by hand (or with `python3 -c "print(pow(7,9,239))"` → `130`): the
exponent is the *value*, so for leaf `%a` (profiled value `v=9`) the guard
matches an actual value `x` iff `g^x ≡ g^v (mod P)`, which for a prime modulus
and a generator is iff `x ≡ v (mod P-1)` — and the range gate keeps the value
below `P`, so the match is exact. The map is a bijection on the gated domain,
hence zero collisions: no state other than the profiled one can fire the guard,
and the DLP surface (`>>> & ^`, modmul) hides the plain `x == 9` comparison the
original code would have suggested.

You can spot these three `(P, target)` triples directly in `twin.sir`: each leaf
has `let %__P64_<i>: i64 = <P>;` and `%__target_<i> = <target> as <T>;`.

## 6. The same checks, automated

The test suite runs all of the above at scale:

```bash
python3 -m test.unit.run_rytwin_tests ./rytwin ./rysmith ./symiri dlp_bijection   # the two DLP tests
python3 -m test.unit.run_rytwin_tests ./rytwin ./rysmith ./symiri                  # the full rytwin suite
make test-reify                                                                     # differential fuzzing, 100 programs
```

The tier-diversity test (`test_dlp_bijection_tier_diversity`) uses the exact
fixture committed at `test/unit/fixtures/rytwin_handcheck/tiers.sir` (inlined in
the test), looped over 20 seeds: it regenerates the leaf, twines it, and asserts
that the small `i8` leaf's guard reaches a small working type (i16/i32) and
still carries a range gate and a prime. The committed `twin.sir` next to it is
the seed-1 output the walkthrough above reproduces.
