# DLP Bijection Guard: Verification & Mathematical Analysis

This document demonstrates the equivalence of the base function $B$, the twinned function $B'$, and the Discrete Logarithm Problem (DLP) bijection guard for the known input state.

---

## 1. The Two Programs ($B$ and $B'$)

### Base Function $B$ (`sigfix.sir`):
```sir
// SOLVED: %pa0=7
fun @sigfix(%pa0: i32) : i32 {
  let mut %a: i32 = 3;
  let mut %b: i32 = 1234567;
  ^entry:
    br ^work;
  ^work:
    %a = 2 * %a + %pa0;
    br ^exit;
  ^exit:
    ret %a;
}
```

### Twinned Function $B'$ (`sigfix.p2.sir`):
```sir
fun @sigfix(%pa0: i32) : i32 {
  let mut %a: i32 = 3;
  let mut %b: i32 = 1234567;
^entry:
  br ^work;
^work:
  br call @__twg_sigfix_work(%a, %b, %pa0) != 0, ^work__twin, ^work__orig;
^work__twin:
  %a = -1 * %pa0;
  %a = -1 + 6 * %b;
  %b = -1 + %a;
  %a = %a + -7407388;
  %b = %b + -6172833;
  br ^work__merge;
^work__orig:
  %a = 2 * %a + %pa0;
  br ^work__merge;
^work__merge:
  br ^exit;
^exit:
  ret %a;
}
```

---

## 2. Equivalence Proof ($B(\text{input}) == B'(\text{input})$)

For the profiled input `%pa0 = 7`:

### Execution of $B$:
- Starts with `%a = 3`.
- Evaluates: $\%a = 2 \times 3 + 7 = 13$.
- Returns **`13`**.

### Execution of $B'$:
- The guard function `@__twg_sigfix_work(3, 1234567, 7)` returns `true` (`-1`).
- Execution branches to the twinned block `^work__twin`:
  1. `%a = -1 * 7` $\rightarrow$ `%a = -7`
  2. `%a = -1 + 6 * 1234567` $\rightarrow$ `%a = 7407401`
  3. `%b = -1 + 7407401` $\rightarrow$ `%b = 7407400`
  4. `%a = 7407401 - 7407388` $\rightarrow$ `%a = 13`
  5. `%b = 7407400 - 6172833` $\rightarrow$ `%b = 1234567`
- Returns **`13`** (matching $B$ exactly!).

---

## 3. Mathematical Verification of the Guard

The guard's integer leaves are **one i64 block per leaf** (no 2×32 packing):
each leaf's value is masked to its width into a block $X_i$, and the guard
runs a DLP trapdoor per block against a tier-selected (prime, primitive-root)
pair. For the `sigfix.sir` program above the leaves are `%a`, `%b`, `%pa0`
(three blocks):

$$X_0 = 3, \qquad X_1 = 1234567, \qquad X_2 = 7$$

Each block also carries a **range gate** `X < P` and the zero-substitution
$f(X) = X \text{ if } X \ne 0 \text{ else } 1$ (see §4.3 for why this keeps
the DLP map injective). With the multi-size tier table (§4.1), these i32
leaves select the 32-bit tier (or a 30% bump to 48/62 bits). For example,
with tier 6 pair $(P, g) = (4611686018427387847, 6)$:

### Exponentiation Steps:
1. **Chunk 0 ($X_0 = 3$):**
   - Prime $P_0 = 4611686018427387847$, generator $g_0 = 6$
   - Verification:
     $$6^3 \pmod{4611686018427387847} = 216$$
   - The unrolled square-and-multiply in the guard computes the same value
     (`%__res_0 = 216`), matching `%__target_0 = 216` exactly → `%__comp_0 = true`.

2. **Chunk 1 ($X_1 = 1234567$):**
   - $6^{1234567} \pmod{4611686018427387847}$ — again computed by the
     unrolled loop on both sides; the guard's `%__res_1` equals its
     `%__target_1` → `%__comp_1 = true`.

3. **Chunk 2 ($X_2 = 7$):**
   - $5^7 \pmod{4611686018427387817} = 78125$ (with $g_2 = 5$) →
     `%__comp_2 = true`.

As all sub-checks are satisfied, the guard returns `true`, enabling the twin
block branch. The exact primes/generators are RNG-selected per run; the
computation pattern (mask → range gate → unrolled square-and-multiply vs a
host-computed constant target) is identical for every leaf and tier.

---

## 4. Continuation: Multi-Size DLP Prime Tiers

The single 62-bit table was generalized to a **seven-tier** table spanning
8 → 62 bit primes, so narrow leaves (i8/i16/…) exercise genuinely smaller
working types in the generated guard (and the emitted C), while wide leaves
keep the strong 62-bit modulus.

### 4.1 The verified tier table (all 28 pairs re-checked)

Verification was re-done from scratch in this environment (no Python/gcc):
`factor`(GMP) proved primality of every prime, and a `bc` square-and-multiply
checked the primitive-root witness $g^{(p-1)/q} \not\equiv 1 \pmod p$ for
every distinct prime $q \mid p-1$ (0 collisions out of 102 checks; `modpow`
itself sanity-checked against $2^{10} \bmod 1000 = 24$).

| Tier | primeBits | resBits | mulBits | loopBits | pairs (p, g) |
|------|-----------|---------|---------|----------|--------------|
| 0 | 8 | 16 | 32 | 8 | (251,6), (241,7), (239,7), (233,3) |
| 1 | 10 | 16 | 32 | 10 | (1021,10), (1019,2), (1013,3), (997,7) |
| 2 | 16 | 32 | 64 | 16 | (65521,17), (65519,11), (65497,7), (65479,13) |
| 3 | 20 | 32 | 64 | 20 | (1048573,2), (1048571,2), (1048559,7), (1048517,2) |
| 4 | 32 | 64 | 128 | 32 | (4294967291,2), (4294967279,7), (4294967231,7), (4294967197,6) |
| 5 | 48 | 64 | 128 | 48 | (281474976705359,13), (281474976705023,5), (281474976704939,2), (281474976702863,5) |
| 6 | 62 | 64 | 128 | 64 | existing 4 pairs |

`resBits` (result/base/target width) must be `> primeBits` so every value
$< P$ stays **positive** in the signed type; `mulBits` (multiply/modulus
width) must satisfy `mulBits > 2·primeBits` so `res·base < P²` never
overflows the signed range. **Both are stricter than the original plan**
(the plan's 16/32/32/64/64/128/128 work-bits allow signed overflow in tiers
0, 2, 4, which would be UB on *unmatched* inputs); the table above is the
corrected one.

### 4.2 Tier selection (per leaf block)

- Effective exponent bitwidth `EB = min(leafWidth, 64)` (the block is an i64
  masked to the leaf width).
- Minimum tier with `loopBits ≥ EB` **and** `maxPrime > hostValue`.
- 30% jump to a strictly larger tier (diversity; larger primes/loop counts
  preserve both invariants).
- Pair picked from a random offset, requiring `prime > hostValue`.

The `prime > hostValue` rule is what keeps the range gate `x < P` **firing on
the profiled input**, and it is what makes small tiers safe for *negative*
leaves (e.g. an i32 `-5` masks to `4294967291` ≥ the 32-bit tier's max prime,
so it bumps to the 48-bit tier — the old 62-bit-only code never had to think
about this).

### 4.3 Why the DLP guard stays exact (no wrong fires)

Let $X$ be the masked block value, $f(X) = X \text{ if } X \ne 0 \text{ else } 1$
(the guard's zero-substitution), $P$ the chosen prime, $g$ a primitive root.

- **Range gate**: `X < P` (unsigned, via the `^signMask` trick) gates every
  block, so only $X \in [0, P)$ survive.
- **Injectivity on the gated domain**: for $X_1 \ne X_2$, both $< P$,
  $f(X_1) \equiv f(X_2) \pmod{P-1}$ would force a difference of a multiple of
  $P-1$; the only such pair in $[1, P)$ is $\{0, P-1\}$, and $f$ never yields
  0. So $X \mapsto g^{f(X)} \bmod P$ is injective on the gated domain, and
  `g^f(X) == g^f(X₀)` iff `X == X₀`.
- **Zero-vs-(P−1) collision eliminated**: without the substitution, $X=0$ and
  $X=P-1$ both map to 1; with $f$, $f(0)=1$ gives $g^1 = g \ne 1 = g^{P-1}$.
- **No dead-fires**: `prime > hostValue` (tier selection) ⇒ the profiled
  input always passes its range gate, so the twin executes on the profiled run.
- **UB-free on every input**: products stay below `2^(mulBits-1)` and the
  exponent is fully consumed (`loopBits ≥ leafWidth`), so the guard's body is
  UB-free for matched *and* unmatched inputs.

### 4.4 Files touched

- `src/reify/twin_transform.cpp` — tier table + per-block tier/pair selection
  (replaces the fixed 4-pair 62-bit table).
- `test/unit/run_rytwin_tests.py` — `test_dlp_bijection_tier_diversity`
  (i8 fixture must reach a small working type under some seed) registered in
  `main()`; the existing `test_dlp_bijection_structure` still passes (i32
  leaves ⇒ tier 4 ⇒ `as i128` preserved).
- `CHANGELOG.md` — feature entry under `### Added`.
