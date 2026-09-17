"""End-to-end tests for rysmith's v0.2.2 generation surface:

  (1) Every run prints a 6-hex-char generation ID derived from --seed
      and prefixes every generated function and struct with it.
  (2) `--n-params N` adds N scalar parameters to each generated
      function. Parameters appear in the function signature and
      are usable as RValues throughout the body.
  (3) Every successful generation writes a `func_<id>_<i>.json`
      descriptor with the canonical schema.
  (4) The concrete .sir output carries a `// SOLVED:` header naming
      the solved parameter values, which `symiri ... -- <vals>`
      replays bit-exact.
  (5) R3 assignment-shape invariants: the generator never emits
      `%x = %x;` (self-assigns), plain `%x = %y;` copies for
      scalar/vec LHS, or any pattern where the LHS token reappears
      on the RHS. These shapes fold flat under SCCP / IPA-CP and
      were the bulk of pre-R3's optimization surface.

Run as:

  python3 -m test.lib.run_rysmith_tests <rysmith> <symiri>

Each test prints PASS or FAIL; exit code reflects the worst result.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

GREEN = "\033[32m"
RED = "\033[31m"
GRAY = "\033[90m"
NC = "\033[0m"

results = []

# rysmith emits a `generation id = <hex>` banner on stdout — every test
# below relies on this line because --id was removed. Same regex is
# duplicated in run_rylink_tests.py to keep each module standalone.
_ID_RE = re.compile(r"generation id\s*=\s*([0-9a-f]{6})")


def run(cmd, **kw):
  print(f"  {GRAY}[RUN>]{NC} " + " ".join(cmd))
  return subprocess.run(cmd, capture_output=True, text=True, timeout=60, **kw)


def check(name, ok, detail=""):
  results.append((name, ok, detail))
  color = GREEN if ok else RED
  tag = "PASS" if ok else "FAIL"
  print(f"  [{color}{tag}{NC}] {name}" + (f" — {detail}" if detail and not ok else ""))


def extract_id(stdout):
  m = _ID_RE.search(stdout or "")
  return m.group(1) if m else None


def test_id_banner(rysmith):
  """rysmith prints a 6-hex-char generation ID and uses it for every
  generated function and struct."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "2",
        "--seed",
        "42",
        "--emit-desc",
        "-o",
        d,
      ]
    )
    check(
      "rysmith default-id run exits 0",
      r.returncode == 0,
      f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
    )
    gid = extract_id(r.stdout)
    check(
      "stdout reports a 6-hex generation id",
      gid is not None,
      f"stdout={r.stdout[:200]!r}",
    )
    if gid is None:
      return
    files = sorted(os.listdir(d))
    expected_sirs = {f"func_{gid}_0", f"func_{gid}_1"}
    sir_stems = {f.rsplit(".", 1)[0] for f in files if f.endswith(".sir")}
    # Single-init produces `func_<id>_<i>.sir`; multi-init produces
    # `func_<id>_<i><a..z>.sir`. Trim a trailing init-letter to
    # recover the base name in both cases.
    base_stems = set()
    for s in sir_stems:
      m = re.match(rf"(func_{gid}_\d+)[a-z]?$", s)
      if m:
        base_stems.add(m.group(1))
    check(
      f"both func_{gid}_<i> base names present",
      expected_sirs.issubset(base_stems),
      f"base={base_stems}",
    )
    json_names = sorted(f for f in files if f.endswith(".json"))
    check(
      f"descriptors func_{gid}_0.json and func_{gid}_1.json emitted",
      json_names == [f"func_{gid}_0.json", f"func_{gid}_1.json"],
      str(json_names),
    )


def test_seed_id_determinism(rysmith):
  """Two runs with the same --seed produce the same generation ID."""
  with tempfile.TemporaryDirectory() as d1, tempfile.TemporaryDirectory() as d2:
    r1 = run([rysmith, "--n-funcs", "1", "--seed", "777", "-o", d1])
    r2 = run([rysmith, "--n-funcs", "1", "--seed", "777", "-o", d2])
    g1 = extract_id(r1.stdout)
    g2 = extract_id(r2.stdout)
    check(
      "same --seed yields the same generation id across runs",
      g1 is not None and g1 == g2,
      f"g1={g1!r} g2={g2!r}",
    )


def test_n_params(rysmith):
  """--n-params 3 puts three scalar parameters into every generated function."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--emit-desc",
        "--seed",
        "7",
        "--n-params",
        "3",
        "-o",
        d,
      ]
    )
    check(
      "rysmith --n-params=3 exits 0", r.returncode == 0, f"stderr={r.stderr[:200]!r}"
    )
    gid = extract_id(r.stdout)
    if gid is None:
      check("rysmith id discovery for --n-params", False, "no id in stdout")
      return
    desc_path = os.path.join(d, f"func_{gid}_0.json")
    check("descriptor exists", os.path.isfile(desc_path))
    if not os.path.isfile(desc_path):
      return
    desc = json.load(open(desc_path))
    check(
      "descriptor lists 3 params",
      len(desc.get("params", [])) == 3,
      f"params={desc.get('params')}",
    )
    names = [p["name"] for p in desc.get("params", [])]
    check("param names are %pa0..%pa2", names == ["%pa0", "%pa1", "%pa2"], str(names))
    types = [p["type"] for p in desc.get("params", [])]
    scalar_pat = re.compile(r"^(i\d+|f32|f64)$")
    check(
      "all params are scalar types", all(scalar_pat.match(t) for t in types), str(types)
    )
    # Body actually uses each param somewhere (or at least references them
    # — rysmith may not always pick them when many other vars are around)
    sir_path = next(
      (
        os.path.join(d, f)
        for f in os.listdir(d)
        if f.startswith(f"func_{gid}_0") and f.endswith(".sir")
      ),
      None,
    )
    check("at least one concrete .sir emitted", sir_path is not None)
    if sir_path:
      body = open(sir_path).read()
      check(
        "signature uses %pa0..%pa2",
        all(p in body for p in ["%pa0", "%pa1", "%pa2"]),
        body[:200],
      )


def test_descriptor_schema(rysmith):
  """Descriptor JSON parses and carries every documented field."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--emit-desc",
        "--seed",
        "11",
        "--n-params",
        "2",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith descriptor-schema setup", False, r.stderr[:200])
      return
    gid = extract_id(r.stdout)
    if gid is None:
      check("rysmith id discovery for descriptor-schema", False, "no id in stdout")
      return
    desc = json.load(open(os.path.join(d, f"func_{gid}_0.json")))
    for k in (
      "id",
      "name",
      "ret_type",
      "params",
      "path",
      "structs",
      "realizations",
    ):
      check(f"descriptor field `{k}` present", k in desc, str(list(desc.keys())))
    # Top-level `syms` was dropped (each realization has its own
    # potentially-different sym set; the old top-level field reflected
    # only the last init and was misleading).
    check("descriptor.syms removed", "syms" not in desc, str(list(desc.keys())))
    check("descriptor.id matches stdout id", desc.get("id") == gid, desc.get("id"))
    check(
      f"name is @func_{gid}_0",
      desc.get("name") == f"@func_{gid}_0",
      desc.get("name"),
    )
    rzs = desc.get("realizations")
    check(
      "at least one realization listed",
      isinstance(rzs, list) and rzs,
      str(rzs),
    )
    if rzs:
      rz = rzs[0]
      check("realization has file", "file" in rz, str(rz))
      check("realization has params dict", isinstance(rz.get("params"), dict), str(rz))
      check("realization has syms dict", isinstance(rz.get("syms"), dict), str(rz))
      check("realization has ret", "ret" in rz, str(rz))
      # All declared params have a value
      param_names = [p["name"] for p in desc.get("params", [])]
      check(
        "every declared param has a solved value",
        all(n in rz.get("params", {}) for n in param_names),
        f"params={rz.get('params')}",
      )


def test_descriptor_outcome_field(rysmith):
  """--emit-desc records the runtime `outcome`: `return` by default, `trap`
  under --require-ub, `diverge` under --require-nonterm."""
  for flags, expected in (
    ([], "return"),
    (["--require-ub"], "trap"),
    (["--require-nonterm"], "diverge"),
  ):
    with tempfile.TemporaryDirectory() as d:
      r = run(
        [
          rysmith,
          "--emit-desc",
          "--seed",
          "5",
          "--n-funcs",
          "1",
          "--n-concretes",
          "1",
          "-o",
          d,
        ]
        + flags
      )
      jsons = [f for f in os.listdir(d) if f.endswith(".json")]
      if r.returncode != 0 or not jsons:
        check(f"outcome={expected}: descriptor emitted", False, r.stderr[:200])
        continue
      desc = json.load(open(os.path.join(d, jsons[0])))
      check(
        f"descriptor outcome == {expected!r}",
        desc.get("outcome") == expected,
        f"got {desc.get('outcome')!r}",
      )
      # The legacy has_ub bool is gone (generalized into outcome).
      check(
        f"outcome={expected}: no legacy has_ub field",
        "has_ub" not in desc,
        str(desc.keys()),
      )


# Single-function generation is inherently seed-fragile: the exact seed
# whose lone function solves drifts whenever the generator's RNG draw
# sequence changes (a new expression shape, a reordered candidate pool).
# Rather than pin one seed and chase it on every change, try a handful and
# use the first that yields a concrete (solved) `func_<gid>_0*.sir`.
_REPLAY_SEEDS = (25, 42, 22, 7, 101, 13, 314, 2718)


def _first_solved_run(rysmith, extra_args):
  """Generate a single 2-param function, trying _REPLAY_SEEDS until one
  solves. Returns (gid, sir_path, tmpdir); the caller removes tmpdir. On
  total failure returns (None, None, None)."""
  for seed in _REPLAY_SEEDS:
    d = tempfile.mkdtemp(prefix="replay_")
    r = run(
      [rysmith, "--n-funcs", "1", "--seed", str(seed), "--n-params", "2", "-o", d]
      + extra_args
    )
    gid = extract_id(r.stdout) if r.returncode == 0 else None
    if gid:
      sirs = sorted(
        f for f in os.listdir(d) if f.startswith(f"func_{gid}_0") and f.endswith(".sir")
      )
      if sirs:
        return gid, os.path.join(d, sirs[0]), d
    shutil.rmtree(d, ignore_errors=True)
  return None, None, None


def test_solved_replay(rysmith, symiri):
  """SOLVED header round-trips through symiri positional args."""
  gid, sir_path, d = _first_solved_run(rysmith, ["--emit-desc"])
  if gid is None:
    check("at least one concrete .sir for replay", False, "no solving seed found")
    return
  try:
    header = open(sir_path).readline().strip()
    check("first line is // SOLVED:", header.startswith("// SOLVED:"), header)
    if not header.startswith("// SOLVED:"):
      return
    kv = dict(re.findall(r"(%\w+|ret)=(-?\d+(?:\.\d+(?:[eE][-+]?\d+)?)?)", header))
    pa0 = kv.get("%pa0")
    pa1 = kv.get("%pa1")
    ret = kv.get("ret")
    check(
      "SOLVED has %pa0, %pa1, ret",
      pa0 is not None and pa1 is not None and ret is not None,
      str(kv),
    )
    if pa0 is None or pa1 is None:
      return
    # Replay through symiri — must produce the same return value.
    r2 = run([symiri, "--main", f"@func_{gid}_0", sir_path, "--", pa0, pa1])
    expected = f"Result: {ret.rstrip('.0')}" if ret.endswith(".0") else f"Result: {ret}"
    # symiri prints int returns as bare decimal; tolerate equality on the int.
    ok = r2.returncode == 0 and (f"Result: {ret}" in r2.stdout or expected in r2.stdout)
    check(
      f"symiri replay @func_{gid}_0({pa0},{pa1}) -> {ret}",
      ok,
      f"rc={r2.returncode}, stdout={r2.stdout[:120]!r}",
    )
  finally:
    shutil.rmtree(d, ignore_errors=True)


def test_rysmith_main(rysmith, symiri):
  """rysmith --emit-main generates a @main wrapper that we can execute directly via symiri."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--seed",
        "42",
        "--n-params",
        "2",
        "--emit-main",
        "-o",
        d,
      ]
    )
    check(
      "rysmith --emit-main run exits 0",
      r.returncode == 0,
      f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
    )
    gid = extract_id(r.stdout)
    if gid is None:
      return
    sirs = [f for f in os.listdir(d) if f.endswith(".sir")]
    if not sirs:
      check("at least one concrete .sir for main test", False, "no sirs")
      return
    sir_path = os.path.join(d, sirs[0])

    # Read file and verify @main is present
    with open(sir_path) as f:
      content = f.read()
    check(
      "generated program contains fun @main",
      "fun @main(" in content,
      "fun @main not found in generated SIR",
    )

    # Execute symiri on it directly without entry or parameters!
    r2 = run([symiri, sir_path])
    check(
      "executing @main directly via symiri exits 0 and returns Result: 0",
      r2.returncode == 0 and "Result: 0" in r2.stdout,
      f"rc={r2.returncode}, stdout={r2.stdout[:200]!r}, stderr={r2.stderr[:200]!r}",
    )


# ---------------------------------------------------------------------------
# R3 assignment-shape invariants
# ---------------------------------------------------------------------------
#
# rysmith's RHS generator (src/reify/expr_gen.cpp) filters the LHS local
# name out of every variable-pool pick so reductive patterns can't appear
# in the solved output. The plan + rationale lives in tmp/reify_effectiveness_plan.md.
# These tests sample a band of seeds, walk every emitted body assignment,
# and assert the four invariants below.

# Seeds chosen to exercise a variety of programs without overlapping the
# seeds used by the v0.2.2-surface tests above.
_R3_SEEDS = [1001, 1002, 1003, 1004, 1005, 1006]

# `%v0`, `%pa1`, `%pp2`, `%t0`, `%a0`, etc. — any local sigil.
_R3_LOCAL = r"%[A-Za-z_][A-Za-z0-9_]*"
# A simple LValue at the start of a line: `LHS = RHS;` with no field/index
# accesses on LHS. Aggregate LHS (`%a[i] = …`) is excluded because the
# root may legitimately appear in RHS at a different access path.
_R3_SIMPLE_LHS = re.compile(rf"^\s*({_R3_LOCAL})\s*=\s*(.+);\s*$")
_R3_BARE_LOCAL_RHS = re.compile(rf"^({_R3_LOCAL})$")
# rysmith naming convention (see src/reify/var_catalogue.cpp): %v scalar,
# %vec vector, %a array, %t struct, %p ptr, %pp ptr-to-ptr, %ap aggregate
# ptr, %pa param scalar. The plain-copy test exempts pointer-typed LHS:
# `%p0 = %p1;` is a meaningful aliasing operation and the alternative
# (e.g. `%p0 = %p1 + 1;`) would push %p0 past the single-element object
# it points to and cause UB on any subsequent `load %p0`.
_R3_PTR_LHS = re.compile(r"^%(p|pp|ap)\d")


def _r3_collect_sirs(rysmith, seeds, n_funcs=4):
  """Generate programs with the given seeds and return a list of .sir paths."""
  sirs = []
  for s in seeds:
    d = tempfile.mkdtemp(prefix="r3_")
    r = run([rysmith, "--n-funcs", str(n_funcs), "--seed", str(s), "-o", d])
    if r.returncode != 0:
      print(f"  {RED}rysmith failed for seed {s}: {r.stderr[:200]!r}{NC}")
      continue
    for f in os.listdir(d):
      if f.endswith(".sir"):
        sirs.append(os.path.join(d, f))
  return sirs


def _r3_body_assign_lines(sir_path):
  """Yield body assignment lines from a .sir file, skipping the synthesized
  CRC32 exit-block chain and the @main wrapper (both produced by reify
  rather than the user-code generator)."""
  with open(sir_path) as f:
    text = f.read()
  in_main = False
  for line in text.splitlines():
    stripped = line.strip()
    if not stripped or stripped.startswith("//"):
      continue
    if stripped.startswith("fun @main"):
      in_main = True
      continue
    if stripped.startswith("fun @") and not stripped.startswith("fun @main"):
      in_main = False
      continue
    if in_main:
      continue
    # Skip the CRC32 exit-block chain (all lines touch %_chk only).
    if "%_chk" in stripped:
      continue
    if "=" not in stripped or not stripped.endswith(";"):
      continue
    if stripped.startswith("let "):
      continue
    yield stripped


def _r3_split_lhs_rhs(line):
  m = _R3_SIMPLE_LHS.match(line)
  if not m:
    return None, None
  return m.group(1), m.group(2).strip()


def test_r3_no_self_assign(rysmith):
  """R3: `%x = %x;` should never appear."""
  sirs = _r3_collect_sirs(rysmith, _R3_SEEDS)
  bad = []
  for sir in sirs:
    for line in _r3_body_assign_lines(sir):
      lhs, rhs = _r3_split_lhs_rhs(line)
      if lhs is None:
        continue
      m = _R3_BARE_LOCAL_RHS.match(rhs)
      if m and m.group(1) == lhs:
        bad.append((sir, line))
  check(
    f"R3: 0 scalar/ptr/vec self-assigns across {len(sirs)} .sir files",
    not bad,
    f"first violation: {bad[0]}" if bad else "",
  )


def test_r3_no_plain_copy(rysmith):
  """R3 + P6: `%x = %y;` (plain copy, both bare locals, x != y) is allowed
  for scalar/vec LHS only at the low kPAllowPlainCopy probability — copy
  propagation is a distinct dataflow shape worth keeping in the mix, but a
  high share would re-open the SCCP collapse R3 closed. Pointer LHS is
  exempted — `%p0 = %p1;` is a meaningful aliasing op and the obvious
  "fix" (`%p1 + 1`) would step a pointer past its single-element object
  and trip load UB."""
  sirs = _r3_collect_sirs(rysmith, _R3_SEEDS)
  copies = 0
  total = 0
  for sir in sirs:
    for line in _r3_body_assign_lines(sir):
      lhs, rhs = _r3_split_lhs_rhs(line)
      if lhs is None or _R3_PTR_LHS.match(lhs):
        continue
      total += 1
      m = _R3_BARE_LOCAL_RHS.match(rhs)
      if m and m.group(1) != lhs:
        copies += 1
  share = copies / total if total else 0.0
  check(
    f"plain-copy share <= 10% of scalar/vec assigns ({copies}/{total} across "
    f"{len(sirs)} .sir files)",
    total > 0 and share <= 0.10,
    f"share={share:.1%}",
  )


def test_r3_no_lhs_in_rhs(rysmith):
  """R3: the LHS variable token must not appear anywhere in the RHS of a
  simple-LHS assignment. Catches `%v3 = %v3 - 4 * %v3 + 4 * %v3;` and
  similar reductive patterns. `addr`/`load` forms are exempted because
  they index different pools that R3 intentionally leaves untouched."""
  sirs = _r3_collect_sirs(rysmith, _R3_SEEDS)
  bad = []
  for sir in sirs:
    for line in _r3_body_assign_lines(sir):
      lhs, rhs = _r3_split_lhs_rhs(line)
      if lhs is None:
        continue
      if rhs.startswith("addr ") or rhs.startswith("load "):
        continue
      pattern = re.compile(rf"(?<![A-Za-z0-9_]){re.escape(lhs)}(?![A-Za-z0-9_])")
      if pattern.search(rhs):
        bad.append((sir, line))
  check(
    f"R3: 0 LHS-appears-in-RHS violations across {len(sirs)} .sir files",
    not bad,
    f"first violation: {bad[0]}" if bad else "",
  )


def test_r3_baseline_assignment_volume(rysmith):
  """R3 sanity guard: the harness actually saw a meaningful number of
  body assignments. Without this, the "0 violations" tests pass
  trivially if rysmith stops emitting code."""
  sirs = _r3_collect_sirs(rysmith, _R3_SEEDS)
  n = 0
  for sir in sirs:
    for _ in _r3_body_assign_lines(sir):
      n += 1
  check(
    f"R3: >= 200 body assignments observed across {len(sirs)} .sir files",
    n >= 200,
    f"only {n} body assignments",
  )


def test_r3_pa_param_uses_present(rysmith):
  """R3 spirit-check: plain-copy elimination must not starve out param
  usage. After R3 we still expect non-trivial expressions involving
  `%paN` (e.g. `%v0 = %pa1 - 4;` or `%v0 = sym * %pa1`) — otherwise
  the generator may have degenerated into literal-only RHSs."""
  sirs = _r3_collect_sirs(rysmith, _R3_SEEDS)
  param_uses = 0
  for sir in sirs:
    for line in _r3_body_assign_lines(sir):
      lhs, rhs = _r3_split_lhs_rhs(line)
      if lhs is None:
        continue
      if re.search(r"%pa\d", rhs) and re.search(r"[+\-*/%&|^<>]", rhs):
        param_uses += 1
  check(
    f"R3: >= 1 non-trivial RHS uses of a %paN param across {len(sirs)} files",
    param_uses >= 1,
    f"only {param_uses} non-trivial param uses observed",
  )


# ---------------------------------------------------------------------------
# R4–R8 effectiveness invariants
# ---------------------------------------------------------------------------
#
# Each R-step closes a specific optimization-fold pathway that PLDI-style
# reify exposed. The tests below sample a band of seeds, parse the output,
# and assert the headline outcome — not the implementation. Detailed
# rationale: tmp/reify_effectiveness_plan.md.

_R48_SEEDS = [2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008]


def _collect_sirs_with(rysmith, seeds, extra_flags=None, n_funcs=4, n_params=2):
  """Generate programs with optional extra rysmith flags."""
  sirs = []
  for s in seeds:
    d = tempfile.mkdtemp(prefix="r48_")
    cmd = [
      rysmith,
      "--n-funcs",
      str(n_funcs),
      "--seed",
      str(s),
      "--n-params",
      str(n_params),
      "-o",
      d,
    ]
    if extra_flags:
      cmd.extend(extra_flags)
    r = run(cmd)
    if r.returncode != 0:
      print(f"  {RED}rysmith failed for seed {s}: {r.stderr[:200]!r}{NC}")
      continue
    for f in os.listdir(d):
      if f.endswith(".sir"):
        sirs.append(os.path.join(d, f))
  return sirs


def test_r4_default_loops_iterate(rysmith):
  """R4: the default `--max-loop-iter` was raised so the path sampler is
  allowed to take back edges. Verified directly via `--help`: pre-R4 the
  default was 1 (no iteration possible); post-R4 it is 3. The actual
  iteration rate depends on the random walk and the CFG's back-edge
  density; forcing iteration via `--min-loop-iter > 0` stresses pre-
  existing typechecker fragility in the generation pipeline, so the
  default leaves min at 0 and lets the bump act as a ceiling raise."""
  r = run([rysmith, "--help"])
  has_default_three = "max-loop-iter" in r.stdout and "(default: 3)" in r.stdout
  check(
    "R4: --help reports --max-loop-iter default 3",
    has_default_three,
    f"help missing 'max-loop-iter ... (default: 3)'; head={r.stdout[:300]!r}",
  )


_LIT_INT_RE = re.compile(r"(?<![\w.])(-?\d+)(?![\w.])")
# Look for integer literals appearing as a Mul/Add coefficient in body
# code. Excludes literal indices like `[0]` (covered by `?<!.` and `?!.`
# rules above) and floats like `1.5` (the trailing `.` would match).


def _body_int_literals(sir_path):
  """Yield all integer literals from body statements (excluding indices
  inside `[...]`, the SOLVED/PATH header, the @main wrapper, and the
  %_chk exit-block helper)."""
  with open(sir_path) as f:
    text = f.read()
  in_main = False
  for line in text.splitlines():
    stripped = line.strip()
    if not stripped or stripped.startswith("//"):
      continue
    if stripped.startswith("fun @main"):
      in_main = True
      continue
    if stripped.startswith("fun @") and not stripped.startswith("fun @main"):
      in_main = False
      continue
    if in_main:
      continue
    if "%_chk" in stripped:
      continue
    if stripped.startswith("let "):
      continue
    # Strip out array/struct index brackets so `[3]` and `[0]` don't
    # pollute the literal pool.
    cleaned = re.sub(r"\[\s*-?\d+\s*\]", "", stripped)
    for m in _LIT_INT_RE.finditer(cleaned):
      try:
        yield int(m.group(1))
      except ValueError:
        pass


def test_r5_large_coef_share(rysmith):
  """R5: the tiered interest-coef require lets ~30% of coefs land outside
  the small-value pool. Proxy: ≥18% of body integer literals must have
  `|v| > 2**20`. Pre-R5 the unconditional `c != 0,1,-1` triple clusters
  the solver at ±2, leaving ≤10% of literals in this range."""
  sirs = _collect_sirs_with(rysmith, _R48_SEEDS)
  large = 0
  total = 0
  for sir in sirs:
    for v in _body_int_literals(sir):
      total += 1
      if abs(v) > (1 << 20):
        large += 1
  share = large / total if total else 0.0
  check(
    f"R5: >= 18% of body int literals have |v| > 2^20 across {len(sirs)} files",
    share >= 0.18,
    f"share={share:.3%} ({large}/{total})",
  )


def test_r6_offpath_mul_widened(rysmith):
  """R6: off-path `<lit> * %v` patterns now draw from the per-width int
  pool, not the [-8, 8] kOffPathCoef_* range. Proxy: ≥20 distinct
  `<lit> * %var` patterns where |lit| falls in (8, 2^20]. Pre-R6 only
  on-path solver values (rare, clustered at ±2) reach this range, so
  baseline is ≤10."""
  sirs = _collect_sirs_with(rysmith, _R48_SEEDS)
  mul_lit_var = re.compile(rf"(?<![\w.])(-?\d+)\s*\*\s*({_R3_LOCAL})")
  count = 0
  for sir in sirs:
    with open(sir) as f:
      text = f.read()
    for m in mul_lit_var.finditer(text):
      lit = abs(int(m.group(1)))
      if 8 < lit <= (1 << 20):
        count += 1
  check(
    f"R6: >= 80 `<lit> * %var` patterns with 8 < |lit| <= 2^20 across {len(sirs)} files",
    count >= 80,
    f"only {count} mid-range mul patterns observed",
  )


def test_r7_pickselectval_literal_diversity(rysmith):
  """R7: the `select cond, a, b` literal slot must draw from the per-width
  pool, not the hardcoded `intCoef(1)` / `floatCoef(1.0)` constants.
  Pre-R7 every `select <cond>, <lit>, <lit>` (both literal-slot picks)
  has both arms set to 1 because the literal slot is constant. After R7
  the arms come from the int / float concrete pool, so most such pairs
  have at least one arm != 1."""
  sirs = _collect_sirs_with(rysmith, _R48_SEEDS)
  both_literal = 0
  both_one = 0
  pat = re.compile(r"select\s+[^,]+,\s*(-?\d+(?:\.\d+)?),\s*(-?\d+(?:\.\d+)?)\s*[,;]")
  for sir in sirs:
    with open(sir) as f:
      text = f.read()
    for m in pat.finditer(text):
      both_literal += 1
      a, b = m.group(1), m.group(2)
      if a in ("1", "1.0") and b in ("1", "1.0"):
        both_one += 1
  if both_literal == 0:
    check("R7: at least 1 dual-literal select observed", False, "no patterns")
    return
  share_one = both_one / both_literal
  check(
    f"R7: < 50% of `select …, lit, lit` patterns are `lit, lit = 1, 1` "
    f"({both_one}/{both_literal} both-one)",
    share_one < 0.5,
    f"share_one={share_one:.2%}",
  )


def test_r8_dyadic_fp_pool_extension(rysmith):
  """R8: kFloatLitPool / kFloatMulCoefPool extended with non-power-of-2
  dyadic values {±1.5, ±2.5, ±3.75, ±1.125, ±0.375, ±1.75, ±0.625}. At
  least one of these new values must appear in a generated FP literal."""
  sirs = _collect_sirs_with(rysmith, _R48_SEEDS, n_funcs=6)
  new_values = {
    "1.5",
    "-1.5",
    "2.5",
    "-2.5",
    "3.75",
    "-3.75",
    "1.125",
    "-1.125",
    "0.375",
    "-0.375",
    "1.75",
    "-1.75",
    "0.625",
    "-0.625",
  }
  found = set()
  for sir in sirs:
    with open(sir) as f:
      text = f.read()
    for m in re.finditer(r"(-?\d+\.\d+)", text):
      v = m.group(1)
      # Drop trailing zeros but keep the canonical form
      if v in new_values:
        found.add(v)
  check(
    f"R8: >= 1 non-power-of-2 dyadic FP literal observed across {len(sirs)} files",
    len(found) >= 1,
    f"found={sorted(found)}",
  )


def _count_assigns_per_block(sir_path):
  """Return {block_label: assign_count} for body blocks of the entry
  function. Excludes @main, the %_chk exit-block chain, store / require
  lines and the terminator. Counts every `LHS = RHS;` body line —
  including `%p = addr %v;`, since the ptr-reassign branch of
  `genBlockStmts` is allowed to pick an `addr` atom on the RHS and that
  is a real body assignment (the entry block's synthesized addr inits
  are filtered separately by the caller). The returned counts are
  comparable to the `--n-stmts` budget."""
  text = open(sir_path).read()
  in_main = False
  blocks = {}
  cur = None
  for line in text.splitlines():
    s = line.strip()
    if s.startswith("fun @main"):
      in_main = True
      continue
    if s.startswith("fun @") and not s.startswith("fun @main"):
      in_main = False
      continue
    if in_main:
      continue
    m = re.match(r"\^(\w+):", s)
    if m:
      cur = m.group(1)
      blocks.setdefault(cur, 0)
      continue
    if cur is None or "%_chk" in s:
      continue
    if (
      s.startswith("store ")
      or s.startswith("require ")
      or s.startswith("br ")
      or s.startswith("ret ")
      or s.startswith("unreachable")
    ):
      continue
    if re.match(r"%\w[^=]*=", s):
      blocks[cur] += 1
  return blocks


def test_n_stmts_counts_assignments_only(rysmith):
  """`--n-stmts N` is the per-block AssignInstr budget. Stores are
  spliced before assignments (Bernoulli per slot, controlled by the
  internal `kPStoreBeforeAssign` HP) and do NOT consume `nStmts`.
  Before this contract was tightened, `nStmts` was the combined
  store-or-assign budget, so the mean assign count per intermediate
  block fell short of `nStmts` by the store density."""
  with tempfile.TemporaryDirectory() as d:
    nStmts = 3
    r = run(
      [
        rysmith,
        "--n-funcs",
        "6",
        "--seed",
        "1001",
        "--n-params",
        "2",
        "--n-stmts",
        str(nStmts),
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("n-stmts: rysmith run exits 0", False, r.stderr[:200])
      return
    # Aggregate over every intermediate (non-entry, non-exit) block we see.
    counts = []
    for f in os.listdir(d):
      if not f.endswith(".sir"):
        continue
      for label, n in _count_assigns_per_block(os.path.join(d, f)).items():
        if label in ("entry", "exit"):
          continue
        counts.append(n)
    if not counts:
      check(
        "n-stmts: observed at least one intermediate block",
        False,
        "no intermediate blocks across generated files",
      )
      return
    mean = sum(counts) / len(counts)
    # With the restructured loop and nStmts=3 every intermediate block
    # has exactly 3 assignments unless a target-build skip fires (rare
    # with the default var pool). Mean ≥ 0.9 * nStmts (= 2.7) holds
    # comfortably after the change; pre-change the mean is ≈ nStmts *
    # (1 - kPStoreBeforeAssign) ≈ 2.25.
    check(
      f"n-stmts: mean intermediate-block assign count ≥ {0.9 * nStmts:.1f} "
      f"(observed {mean:.2f} across {len(counts)} blocks)",
      mean >= 0.9 * nStmts,
      f"mean={mean:.3f}, nStmts={nStmts}",
    )


def _classify_body_assigns(rysmith_dir):
  """Walk every body assignment across the .sir files in `rysmith_dir`
  and bucket them by RHS shape. Returns (total, trivial, single_atom_lit)
  where:
    - total: every `LHS = RHS;` body line (excludes store / require /
      `addr` inits / @main / %_chk).
    - trivial: RHS contains no `%` — the entire RHS is a literal-only
      expression that the compiler can fold without any runtime data
      flow. Covers `%x = 3;`, `%x = 3 + 5 - 2;`, `%x = 3 as i64;`,
      `%x = 3 as i64 - 2147483647;`, etc.
    - single_atom_lit: RHS is a single bare literal (`3`, `-1.5`, `null`),
      a strict subset of `trivial`.
  """
  total = trivial = single_atom_lit = 0
  for f in os.listdir(rysmith_dir):
    if not f.endswith(".sir"):
      continue
    text = open(os.path.join(rysmith_dir, f)).read()
    in_main = False
    for line in text.splitlines():
      s = line.strip()
      if s.startswith("fun @main"):
        in_main = True
        continue
      if s.startswith("fun @") and not s.startswith("fun @main"):
        in_main = False
        continue
      if in_main or "%_chk" in s:
        continue
      if (
        s.startswith("store ")
        or s.startswith("require ")
        or s.startswith("br ")
        or s.startswith("ret ")
        or s.startswith("unreachable")
      ):
        continue
      m = re.match(r"%\w[^=]*=\s*(.+);\s*$", s)
      if not m:
        continue
      rhs = m.group(1).strip()
      # Skip ptr-init `addr %v` shapes that look like assignments in
      # entry blocks but are synthesized by reify, not by the body
      # generator.
      if rhs.startswith("addr "):
        total += 1
        continue
      total += 1
      if "%" in rhs:
        continue
      trivial += 1
      if re.fullmatch(r"-?\d+(?:\.\d+(?:[eE][-+]?\d+)?)?|null", rhs):
        single_atom_lit += 1
  return total, trivial, single_atom_lit


def test_no_all_literal_assignment(rysmith):
  """The bulk of body RHS expressions must reference a runtime LValue.
  Pure-literal RHS (`%x = 3 + 5 - 2;`, `%x = 3 as i64;`, etc.) folds
  flat under SCCP and contributes nothing to compiler stress. The
  generator allows a small `kPAllowAllLiteral` slice (default 5%) so
  the solver retains headroom on tight paths."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "8",
        "--seed",
        "1001",
        "--n-params",
        "2",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("trivial-shape: rysmith run exits 0", False, r.stderr[:200])
      return
    total, trivial, _ = _classify_body_assigns(d)
    if total == 0:
      check("trivial-shape: observed any assignments", False, "no assigns")
      return
    share = trivial / total
    check(
      f"trivial-shape: all-literal RHS share <= 10% "
      f"(observed {share:.1%}, {trivial}/{total})",
      share <= 0.10,
      f"share={share:.3%}",
    )


def test_no_single_atom_literal_assignment(rysmith):
  """`%x = 3;` is the worst case: SCCP folds the assignment, then DCE
  removes the def, and `%x` becomes a constant. The generator should
  almost never emit this shape — `kPAllowAllLiteral`-fraction allowance
  applies."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "8",
        "--seed",
        "1001",
        "--n-params",
        "2",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("trivial-shape: rysmith run exits 0", False, r.stderr[:200])
      return
    total, _, single_lit = _classify_body_assigns(d)
    if total == 0:
      check("trivial-shape: observed any assignments", False, "no assigns")
      return
    share = single_lit / total
    check(
      f"trivial-shape: single-atom literal RHS share <= 10% "
      f"(observed {share:.1%}, {single_lit}/{total})",
      share <= 0.10,
      f"share={share:.3%}",
    )


def test_cfg_header_dump(rysmith):
  """CFG dump is present in the solved .sir header beside PATH."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--seed",
        "42",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("cfg-header-dump: rysmith run exits 0", False, r.stderr[:200])
      return

    header = ""
    for f in os.listdir(d):
      if f.endswith(".sir") and not f.endswith(".oracle.tmp"):
        with open(os.path.join(d, f)) as file:
          header = file.read(2048)
        break

    check("cfg-header-dump: header contains '// CFG:\\n'", "// CFG:\n" in header)
    check("cfg-header-dump: header contains '// PATH:'", "// PATH:" in header)
    check("cfg-header-dump: header contains entry block", "//   entry" in header)
    check(
      "cfg-header-dump: header contains block line formatting",
      bool(re.search(r"//\s+entry(\s+->)?", header)),
    )

    cfg_pos = header.find("// CFG:\n")
    path_pos = header.find("// PATH:")
    check(
      "cfg-header-dump: // CFG: appears before // PATH:",
      cfg_pos != -1 and path_pos != -1 and cfg_pos < path_pos,
    )


def test_min_atoms_negative_validation(rysmith):
  """--min-atoms < 1 should fail validation and exit 2."""
  r = run([rysmith, "--min-atoms", "0"])
  check(
    "validation: --min-atoms 0 exits 2",
    r.returncode == 2,
    f"rc={r.returncode}, stderr={r.stderr[:200]!r}",
  )
  check(
    "validation: --min-atoms 0 prints error",
    "error: --min-atoms must be >= 1" in r.stderr,
    f"stderr={r.stderr[:200]!r}",
  )


def test_max_atoms_less_than_min_validation(rysmith):
  """--max-atoms < --min-atoms should fail validation and exit 2."""
  r = run([rysmith, "--min-atoms", "3", "--max-atoms", "2"])
  check(
    "validation: --min-atoms 3 --max-atoms 2 exits 2",
    r.returncode == 2,
    f"rc={r.returncode}, stderr={r.stderr[:200]!r}",
  )
  check(
    "validation: --min-atoms 3 --max-atoms 2 prints error",
    "error: --max-atoms must be >= --min-atoms" in r.stderr,
    f"stderr={r.stderr[:200]!r}",
  )


def _get_integer_scalar_body_assigns(sir_path):
  """Returns a list of RHS expressions for scalar integer variables in the body of sir_path."""
  var_types = {}
  with open(sir_path) as f:
    text = f.read()
  for m in re.finditer(r"\blet\s+mut\s+(%v\d+)\s*:\s*(i\d+|f32|f64)\b", text):
    var_types[m.group(1)] = m.group(2)
  in_main = False
  assigns = []
  for line in text.splitlines():
    s = line.strip()
    if s.startswith("fun @main"):
      in_main = True
      continue
    if s.startswith("fun @") and not s.startswith("fun @main"):
      in_main = False
      continue
    if in_main or "%_chk" in s:
      continue
    if (
      s.startswith("store ")
      or s.startswith("require ")
      or s.startswith("br ")
      or s.startswith("ret ")
      or s.startswith("unreachable")
    ):
      continue
    m = re.match(r"(%v\d+)\s*=\s*(.+);\s*$", s)
    if not m:
      continue
    lhs, rhs = m.group(1), m.group(2).strip()
    if rhs.startswith("addr ") or rhs.startswith("load "):
      continue
    if var_types.get(lhs, "").startswith("i"):
      assigns.append(rhs)
  return assigns


def _count_atoms(rhs):
  # Count only top-level " + " and " - " (nesting level of parentheses/braces/brackets is 0).
  level = 0
  operators = 0
  i = 0
  n = len(rhs)
  while i < n:
    c = rhs[i]
    if c in "([{":
      level += 1
      i += 1
    elif c in ")]}":
      level -= 1
      i += 1
    elif level == 0 and i + 3 <= n and rhs[i : i + 3] in (" + ", " - "):
      operators += 1
      i += 3
    else:
      i += 1
  return 1 + operators


def test_min_max_atoms_exact_one(rysmith):
  """Verify that --min-atoms 1 --max-atoms 1 produces only 1-atom integer scalar expressions."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "4",
        "--seed",
        "123",
        "--min-atoms",
        "1",
        "--max-atoms",
        "1",
        # Atom knobs are on-path-only; pin the off-path multiplier so the
        # file-wide exactness scan below holds for every block.
        "--off-path-multiplier",
        "1.0",
        "-o",
        d,
      ]
    )
    check(
      "rysmith --min-atoms 1 --max-atoms 1 exits 0",
      r.returncode == 0,
      f"stderr={r.stderr[:200]!r}",
    )
    if r.returncode != 0:
      return
    all_assigns = []
    for f in os.listdir(d):
      if f.endswith(".sir"):
        all_assigns.extend(_get_integer_scalar_body_assigns(os.path.join(d, f)))
    check("observed integer scalar assignments for 1-atom test", len(all_assigns) > 0)
    non_conforming = [rhs for rhs in all_assigns if _count_atoms(rhs) != 1]
    check(
      "all integer scalar assignments have exactly 1 atom",
      not non_conforming,
      f"violations: {non_conforming[:5]}",
    )


def test_min_max_atoms_exact_two(rysmith):
  """Verify that --min-atoms 2 --max-atoms 2 produces only 2-atom integer scalar expressions."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "4",
        "--seed",
        "123",
        "--min-atoms",
        "2",
        "--max-atoms",
        "2",
        "--off-path-multiplier",
        "1.0",
        "-o",
        d,
      ]
    )
    check(
      "rysmith --min-atoms 2 --max-atoms 2 exits 0",
      r.returncode == 0,
      f"stderr={r.stderr[:200]!r}",
    )
    if r.returncode != 0:
      return
    all_assigns = []
    for f in os.listdir(d):
      if f.endswith(".sir"):
        all_assigns.extend(_get_integer_scalar_body_assigns(os.path.join(d, f)))
    check("observed integer scalar assignments for 2-atom test", len(all_assigns) > 0)
    non_conforming = [rhs for rhs in all_assigns if _count_atoms(rhs) != 2]
    check(
      "all integer scalar assignments have exactly 2 atoms",
      not non_conforming,
      f"violations: {non_conforming[:5]}",
    )


def test_min_max_atoms_exact_three(rysmith):
  """Verify that --min-atoms 3 --max-atoms 3 produces only 3-atom integer scalar expressions."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "4",
        "--seed",
        "123",
        "--min-atoms",
        "3",
        "--max-atoms",
        "3",
        "--off-path-multiplier",
        "1.0",
        "-o",
        d,
      ]
    )
    check(
      "rysmith --min-atoms 3 --max-atoms 3 exits 0",
      r.returncode == 0,
      f"stderr={r.stderr[:200]!r}",
    )
    if r.returncode != 0:
      return
    all_assigns = []
    for f in os.listdir(d):
      if f.endswith(".sir"):
        all_assigns.extend(_get_integer_scalar_body_assigns(os.path.join(d, f)))
    check("observed integer scalar assignments for 3-atom test", len(all_assigns) > 0)
    non_conforming = [rhs for rhs in all_assigns if _count_atoms(rhs) != 3]
    check(
      "all integer scalar assignments have exactly 3 atoms",
      not non_conforming,
      f"violations: {non_conforming[:5]}",
    )


def test_large_coef_help_default(rysmith):
  """The interest-coef magnitude threshold is exposed as --large-coef and
  defaults to 1048576 (2^20), pairing with the --p-large-coef fraction."""
  r = run([rysmith, "--help"])
  check(
    "--help lists --large-coef with default 1048576",
    "large-coef" in r.stdout and "(default: 1048576)" in r.stdout,
    f"help missing 'large-coef ... (default: 1048576)'; stdout={r.stdout[:400]!r}",
  )


def test_large_coef_negative_validation(rysmith):
  """--large-coef must be >= 0 (a magnitude threshold); negative is rejected."""
  r = run([rysmith, "--large-coef", "-1"])
  check(
    "validation: --large-coef -1 exits 2",
    r.returncode == 2,
    f"rc={r.returncode}, stderr={r.stderr[:200]!r}",
  )
  check(
    "validation: --large-coef -1 prints error",
    "error: --large-coef must be >= 0" in r.stderr,
    f"stderr={r.stderr[:200]!r}",
  )


def test_large_coef_narrow_domain_sat(rysmith):
  """Regression: a --coef-domain narrower than the large-coef threshold must
  NOT cause mass UNSAT. Pre-fix the threshold was a hardcoded 2^20 ignoring
  the coef domain, so `--coef-domain [-1000,1000] --p-large-coef 1.0`
  emitted `c > 2^20` for every on-path coef → unsatisfiable against the
  domain `c <= 1000` → every function failed. After the per-coef clamp the
  require degrades to the largest in-domain magnitude and stays SAT."""
  sirs = _collect_sirs_with(
    rysmith,
    _R48_SEEDS,
    extra_flags=["--coef-domain", "[-1000,1000]", "--p-large-coef", "1.0"],
  )
  check(
    "narrow --coef-domain + --p-large-coef 1.0 still produces output",
    len(sirs) >= 8,
    f"only {len(sirs)} concretes produced (pre-fix: ~0 from mass UNSAT)",
  )


def test_large_coef_custom_threshold_respected(rysmith):
  """A custom --large-coef raises the magnitude floor: with a wide coef
  domain and --large-coef 8000000 --p-large-coef 1.0, a meaningful share of
  body integer literals must exceed 8e6. With the old hardcoded 2^20 floor
  (≈1.05e6) almost none would."""
  sirs = _collect_sirs_with(
    rysmith,
    _R48_SEEDS,
    extra_flags=["--large-coef", "8000000", "--p-large-coef", "1.0"],
  )
  big = 0
  total = 0
  for sir in sirs:
    for v in _body_int_literals(sir):
      total += 1
      if abs(v) > 8000000:
        big += 1
  share = big / total if total else 0.0
  check(
    f"--large-coef 8000000 pushes >= 8% of body int literals past 8e6 "
    f"across {len(sirs)} files",
    share >= 0.08,
    f"share={share:.3%} ({big}/{total})",
  )


def test_large_coef_zero_accepted(rysmith):
  """Edge: --large-coef 0 is accepted and stays SAT. With threshold 0 the
  require degenerates to `c > 0` / `c < 0` (a nonzero floor), which fits any
  non-degenerate domain."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "4",
        "--seed",
        "2001",
        "--large-coef",
        "0",
        "--p-large-coef",
        "1.0",
        "-o",
        d,
      ]
    )
    produced = [f for f in os.listdir(d) if f.endswith(".sir")]
    check(
      "--large-coef 0 exits 0 and produces output",
      r.returncode == 0 and len(produced) >= 1,
      f"rc={r.returncode}, produced={len(produced)}, stderr={r.stderr[:200]!r}",
    )


def _split_blocks(sir_path):
  """Parse a concrete .sir into (path_label_set, {label: [stmt lines]}) for
  the entry function, using the `// PATH:` header. Labels are without `^`.
  Only the first `fun @...` is scanned (no --emit-main in these tests)."""
  path_labels = set()
  blocks = {}
  cur = None
  in_fun = False
  with open(sir_path) as f:
    for line in f:
      stripped = line.strip()
      m = re.match(r"//\s*PATH:\s*(.+)", stripped)
      if m:
        path_labels = {p.strip() for p in m.group(1).split("->")}
        continue
      if stripped.startswith("fun @"):
        if in_fun:
          break  # second function — stop
        in_fun = True
        continue
      if not in_fun:
        continue
      bm = re.match(r"\^(\w+):", stripped)
      if bm:
        cur = bm.group(1)
        blocks[cur] = []
        continue
      if cur is not None and stripped and not stripped.startswith("//"):
        blocks[cur].append(stripped)
  return path_labels, blocks


def _assigns_in(stmts):
  """Count AssignInstr lines: `%lhs ... = expr;` (excludes require / store /
  br / let / ret, which never start with `%`)."""
  return sum(1 for s in stmts if s.startswith("%") and " = " in s)


def _block_density(sirs):
  """Mean assignments per on-path and off-path intermediate block across
  files. Entry and exit blocks are excluded (entry carries the ptr-init
  preamble, exit the checksum)."""
  on_counts, off_counts = [], []
  for sir in sirs:
    path_labels, blocks = _split_blocks(sir)
    if not path_labels:
      continue
    for label, stmts in blocks.items():
      if label in ("entry", "exit"):
        continue
      n = _assigns_in(stmts)
      if label in path_labels:
        on_counts.append(n)
      else:
        off_counts.append(n)

  def mean(v):
    return sum(v) / len(v) if v else 0.0

  return mean(on_counts), mean(off_counts), len(on_counts), len(off_counts)


def test_off_path_multiplier_help_default(rysmith):
  """--off-path-multiplier is exposed and defaults to 2.0."""
  r = run([rysmith, "--help"])
  check(
    "--help lists --off-path-multiplier with default 2.0",
    "off-path-multiplier" in r.stdout and "(default: 2.0)" in r.stdout,
    f"stdout={r.stdout[:400]!r}",
  )


def test_off_path_multiplier_negative_validation(rysmith):
  """A negative multiplier is rejected."""
  r = run([rysmith, "--off-path-multiplier", "-1"])
  check(
    "validation: --off-path-multiplier -1 exits 2",
    r.returncode == 2 and "--off-path-multiplier must be >= 0" in r.stderr,
    f"rc={r.returncode}, stderr={r.stderr[:200]!r}",
  )


def test_off_path_density_default(rysmith):
  """P1: --n-stmts describes ON-path blocks; off-path blocks default to 2x.
  With --n-stmts 3, on-path intermediate blocks carry ~3 assignments and
  off-path intermediate blocks ~6."""
  sirs = _collect_sirs_with(rysmith, [3001, 3002])
  on_mean, off_mean, n_on, n_off = _block_density(sirs)
  check(
    f"on-path mean ~3 (got {on_mean:.2f} over {n_on} blocks)",
    n_on > 0 and 2.7 <= on_mean <= 3.3,
    f"on_mean={on_mean}",
  )
  check(
    f"off-path mean ~6 (got {off_mean:.2f} over {n_off} blocks)",
    n_off > 0 and 5.4 <= off_mean <= 6.6,
    f"off_mean={off_mean}",
  )


def test_off_path_multiplier_one_uniform(rysmith):
  """--off-path-multiplier 1.0 restores uniform density."""
  sirs = _collect_sirs_with(
    rysmith, [3001, 3002], extra_flags=["--off-path-multiplier", "1.0"]
  )
  on_mean, off_mean, n_on, n_off = _block_density(sirs)
  check(
    f"multiplier 1.0: off-path mean ~3 (got {off_mean:.2f} over {n_off} blocks)",
    n_off > 0 and 2.7 <= off_mean <= 3.3,
    f"on={on_mean}, off={off_mean}",
  )


def test_off_path_atoms_scaled(rysmith):
  """--min/max-atoms describe ON-path expressions; off-path scales them.
  With --min-atoms 2 --max-atoms 2 and the default 2x multiplier, off-path
  integer scalar assignments have exactly 4 atoms."""
  sirs = _collect_sirs_with(
    rysmith, [3001, 3002], extra_flags=["--min-atoms", "2", "--max-atoms", "2"]
  )
  bad = []
  total = 0
  for sir in sirs:
    path_labels, blocks = _split_blocks(sir)
    var_types = {}
    with open(sir) as f:
      for m in re.finditer(r"\blet\s+(?:mut\s+)?(%v\d+)\s*:\s*(i\d+)\b", f.read()):
        var_types[m.group(1)] = m.group(2)
    for label, stmts in blocks.items():
      if label in ("entry", "exit") or label in path_labels:
        continue
      for s in stmts:
        m = re.match(r"^(%v\d+) = (.+);$", s)
        if not m or m.group(1) not in var_types:
          continue
        total += 1
        if _count_atoms(m.group(2)) != 4:
          bad.append(s)
  check(
    f"off-path int scalar assigns have exactly 4 atoms ({total} checked)",
    total > 0 and not bad,
    f"violations: {bad[:5]}",
  )


def test_cheap_rewrite_sym_share(rysmith):
  """P3: when the trivial-shape rewrite fires ON-path it must produce cheap
  linear atoms (concrete-coef mul, plain read, cast, load) instead of
  rerolling the sym-heavy slot table. In single-atom mode (--min/max-atoms
  1) the bare-sym slot (~40%) always triggers the rewrite, so the share of
  on-path assignments containing a `%?s` sym is a direct probe: pre-P3 the
  rerolled replacements were mostly `sym * var`, giving ~29%; post-P3 only
  the organically non-trivial sym slots remain (~8%)."""
  tot = symy = 0
  for seed in (5001, 5002, 5003):
    d = tempfile.mkdtemp(prefix="p3_")
    r = run(
      [
        rysmith,
        "--n-funcs",
        "4",
        "--seed",
        str(seed),
        "--min-atoms",
        "1",
        "--max-atoms",
        "1",
        "--keep-symbolic",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      continue
    for f in os.listdir(d):
      if "_sym" not in f or not f.endswith(".sir"):
        continue
      path_labels, blocks = _split_blocks(os.path.join(d, f))
      for label, stmts in blocks.items():
        if label in ("entry", "exit") or label not in path_labels:
          continue
        for s in stmts:
          if s.startswith("%") and " = " in s and not s.startswith("%?"):
            tot += 1
            if "%?s" in s:
              symy += 1
  share = symy / tot if tot else 1.0
  check(
    f"on-path single-atom sym share <= 15% (got {share:.1%} of {tot})",
    tot > 0 and share <= 0.15,
    f"share={share:.1%}",
  )


def _collect_sym_text(rysmith, seeds, extra_flags=None):
  """Concatenated symbolic-dump text (comments stripped) across seeds."""
  chunks = []
  for seed in seeds:
    d = tempfile.mkdtemp(prefix="p7_")
    cmd = [rysmith, "--n-funcs", "4", "--seed", str(seed), "--keep-symbolic", "-o", d]
    if extra_flags:
      cmd.extend(extra_flags)
    r = run(cmd)
    if r.returncode != 0:
      continue
    for f in os.listdir(d):
      if "_sym" in f and f.endswith(".sir"):
        chunks.append(open(os.path.join(d, f)).read())
  return re.sub(r"//[^\n]*", "", "\n".join(chunks))


_P7_SEEDS = (8101, 8102, 8103)


def test_offpath_shift_atoms(rysmith):
  """P7: off-path blocks generate shift atoms (all three ops, lit- or
  var-shifted). Pre-change the only shifts were on-path `%?sN << %v`
  (i32-only), so a shift whose shiftee is NOT a sym is a pure off-path
  signature."""
  text = _collect_sym_text(rysmith, _P7_SEEDS)
  # The patterns are mutually exclusive: `coef >> %v` cannot match inside
  # `coef >>> %v` because the `%` anchor after the operator would land on
  # the third `>`.
  found = {
    op: len(re.findall(rf"(?:\b-?\d+|%(?!\?)\w+) {re.escape(op)} %", text))
    for op in ("<<", ">>", ">>>")
  }
  check(
    f"off-path shifts present for all ops ({found})",
    all(n >= 3 for n in found.values()),
    f"found={found}",
  )


def test_offpath_var_op_var_atoms(rysmith):
  """P7: `Coef ::= LocalId` — `%a * %b`, `%a & %b`, ... are grammar-legal
  and must appear off-path (runtime values on both sides; the compiler
  cannot fold them, the solver never sees them)."""
  text = _collect_sym_text(rysmith, _P7_SEEDS)
  n = len(re.findall(r"%(?!\?)\w+ (?:\*|/|&|\||\^) %(?!\?)\w+", text))
  check(
    f"var-op-var atoms present off-path ({n} found)",
    n >= 10,
    f"found={n}",
  )


def test_offpath_fp_divmod_atoms(rysmith):
  """P7: FP `/` and `%` (fmod) are typecheck-legal (Mul/Div/Mod for floats)
  and must appear off-path. A float-literal coefficient (always printed
  with a `.`) dividing a var is the signature."""
  text = _collect_sym_text(rysmith, _P7_SEEDS)
  n = len(re.findall(r"-?\d+\.\d+(?:e[+-]?\d+)? [/%] %", text))
  check(
    f"FP div/mod atoms present off-path ({n} found)",
    n >= 3,
    f"found={n}",
  )


def test_offpath_select_cond_nonzero_rhs(rysmith):
  """P7: off-path select conditions compare against a drawn literal, not
  the hardcoded 0 — `select %v >= -123, ...` instead of always
  `select %v >= 0, ...`."""
  text = _collect_sym_text(rysmith, _P7_SEEDS)
  n = len(
    re.findall(r"select %\w+ (?:==|!=|<=|>=|<|>) -?(?:[1-9]\d*)(?:\.\d+)?,", text)
  )
  check(
    f"off-path select conds with nonzero literal RHS ({n} found)",
    n >= 3,
    f"found={n}",
  )


def test_sublvalue_addr_generated(rysmith):
  """P7: `addr` of a sub-lvalue (`addr %t.f0`, `addr %a[1]`) is grammar-legal
  (SPEC: any sub-lvalue rooted at a let-mut local) and exercises field /
  element provenance — SROA/TBAA territory. Pre-change only whole-var
  `addr %v` was generated."""
  text = _collect_sym_text(rysmith, _P7_SEEDS)
  n = len(re.findall(r"addr %\w+[.\[]", text))
  check(
    f"sub-lvalue addr present ({n} found)",
    n >= 5,
    f"found={n}",
  )


def test_pointer_store_generated(rysmith):
  """P7: storing a pointer value through a `ptr ptr T` (`store %pp, addr %x`)
  — alias-analysis stress. Pre-change `tryEmitStore` restricted the pointee
  to int/fp, so a store whose VALUE is `addr ...` never appeared."""
  text = _collect_sym_text(rysmith, _P7_SEEDS)
  n = len(re.findall(r"store %\w+, addr ", text))
  check(
    f"pointer-valued store present ({n} found)",
    n >= 3,
    f"found={n}",
  )


# Pointer arithmetic. A `ptr T_scalar` reassign can emit two in-bounds
# pointer-arithmetic shapes:
#   - array:  `ptrindex %ap, b ± d` off `%ap : ptr [N] T_scalar`, b and
#             b±d in [0, N-1] (real, loadable elements);
#   - struct: `ptrfield %sp, f ± d` off `%sp : ptr @S`, stepping across a
#             run of consecutive same-type fields so the result lands on a
#             same-typed (loadable) field cell.
# Both need an aggregate source and a matching scalar-ptr to coexist in one
# function, which is sparse at the default sizing; the collector widens
# vars/statements/agg-elems so the precondition is reliably met across the
# band rather than concentrating in one lucky seed.
_PTR_ARITH_SEEDS = (9001, 9002, 9003, 9004, 9005, 9006, 9007, 9008)
# Array element step, e.g. `ptrindex %ap, 2 + 1` or `ptrindex %ap, 2 - 1`.
_PTR_ARITH_RE = re.compile(r"ptrindex %\w+,\s*-?\d+\s*[+-]\s*\d+")
# The `ptr - iN` (negative-offset) form specifically — never emitted before
# the offset was generalized away from a constant `+ 1`.
_PTR_ARITH_NEG_RE = re.compile(r"ptrindex %\w+,\s*-?\d+\s*-\s*\d+")
# Struct field step, e.g. `ptrfield %sp, f1 + 1` or `ptrfield %sp, f2 - 1`.
_PTR_FIELD_ARITH_RE = re.compile(r"ptrfield %\w+,\s*\w+\s*[+-]\s*\d+")
# Off-path arbitrary arithmetic on a bare `addr`, e.g. `= addr %v6 + 51;`.
# Unique to unexecuted blocks: the in-bounds on-path forms (ptrindex/ptrfield
# on the left, or `%var ± d`) never append an arithmetic tail to a raw addr.
_OFFPATH_ADDR_ARITH_RE = re.compile(r"=\s*addr %[\w.\[\]]+\s*[+-]\s*\d+")
# Direct pointer-variable arithmetic `%p2 = %p1 ± d;` (both ptr-typed).
_PTR_DECL_RE = re.compile(r"let\s+mut\s+(%\w+)\s*:\s*ptr\b")
_VAR_ARITH_RE = re.compile(r"(?m)^\s*(%\w+)\s*=\s*(%\w+)\s*[+-]\s*\d+\s*;")


def _count_ptr_var_arith(text):
  """Count `%p2 = %p1 ± d;` lines where both sides are ptr-typed locals —
  the (3) direct pointer-variable arithmetic shape. A ptr-typed LHS assigned
  `%var ± int` can only be pointer arithmetic (the typechecker forbids
  `ptr = scalar`); requiring a ptr RHS base too excludes scalar
  `%v = %w ± n`."""
  ptrs = set(_PTR_DECL_RE.findall(text))
  return sum(
    1 for m in _VAR_ARITH_RE.finditer(text) if m.group(1) in ptrs and m.group(2) in ptrs
  )


# Pointer-to-pointer (`ptr ptr T`) arithmetic — proves the slot guard was
# relaxed from scalar-only to any LOADABLE pointee.
_PTR_PTR_DECL_RE = re.compile(r"let\s+mut\s+(%\w+)\s*:\s*ptr\s+ptr\b")
_PTR_PTR_ARITH_RE = re.compile(
  r"(?m)^\s*(%\w+)\s*=\s*(?:ptrindex|ptrfield|addr|%\w+)\b.*?[+-]\s*\d+\s*;"
)


def _count_ptr_ptr_arith(text):
  """Count pointer arithmetic whose result is a `ptr ptr T` local. A ptr-ptr
  LHS with an arithmetic RHS (`ptrindex`/`ptrfield`/`addr`/`%q` ± k) can only
  be pointer arithmetic; before the guard relaxation no `ptr ptr T` could
  reach the ptr-arith slot at all."""
  pp = set(_PTR_PTR_DECL_RE.findall(text))
  return sum(1 for m in _PTR_PTR_ARITH_RE.finditer(text) if m.group(1) in pp)


_NAV_RE = re.compile(r"(?m)^\s*(%\w+)\s*=\s*(ptrindex|ptrfield)\s+(%\w+)")


def _count_nav_chains(text):
  """Count uniform aggregate-navigation *chains* and how many involve a
  `ptrfield` step. A chain is `%x = ptrindex|ptrfield %y, ...` where `%y` was
  itself produced by a ptrindex/ptrfield earlier in the SAME function — i.e.
  multi-step navigation of a nested array / array-of-struct / struct-of-array
  to a loadable leaf. Reset per function so reused names can't fake a chain.
  Returns (chains, chains_touching_a_struct_field)."""
  total = 0
  with_field = 0
  for fn in re.split(r"(?m)^fun ", text):
    nav_kind = {}  # var -> step kind that produced it (or chain-carried field flag)
    field_seen = {}  # var -> whether its producing chain touched a ptrfield
    for m in _NAV_RE.finditer(fn):
      lhs, kind, src = m.group(1), m.group(2), m.group(3)
      if src in nav_kind:
        total += 1
        touched = field_seen.get(src, False) or kind == "ptrfield"
        if touched:
          with_field += 1
        field_seen[lhs] = touched
      else:
        field_seen[lhs] = kind == "ptrfield"
      nav_kind[lhs] = kind
  return total, with_field


def _collect_ptr_arith_text(
  rysmith, seeds, extra_flags=None, n_funcs="6", n_vars="20", n_stmts="6"
):
  """Concatenated symbolic-dump text across seeds, generated dense enough
  (more vars + statements, and wider structs/arrays via --max-agg-elems)
  that aggregate sources and a matching scalar-ptr reliably coexist — the
  precondition for a ptr-arithmetic reassign, including the struct shape
  which additionally needs a same-type field run. The direct ptr-var shape
  additionally needs an array source in scope, which is denser at higher
  --n-vars — hence the override knobs."""
  chunks = []
  for seed in seeds:
    d = tempfile.mkdtemp(prefix="ptrarith_")
    cmd = [
      rysmith,
      "--n-funcs",
      n_funcs,
      "--n-vars",
      n_vars,
      "--n-stmts",
      n_stmts,
      "--max-agg-elems",
      "6",
      "--seed",
      str(seed),
      "--keep-symbolic",
      "-o",
      d,
    ]
    if extra_flags:
      cmd.extend(extra_flags)
    r = run(cmd)
    if r.returncode != 0:
      continue
    for f in os.listdir(d):
      if "_sym" in f and f.endswith(".sir"):
        chunks.append(open(os.path.join(d, f)).read())
  return re.sub(r"//[^\n]*", "", "\n".join(chunks))


def test_no_ptrarith_flag(rysmith):
  """`--no-ptrarith` disables every pointer-arithmetic shape — array
  `ptrindex %ap, b ± d` and struct `ptrfield %sp, f ± d` (and, once added,
  direct `%p = %q ± d`). The same dense config that emits many such shapes
  by default must emit zero with the flag set, and generation must still
  succeed. The explicit exit-0 check guards against a rejected flag
  silently passing the zero-count assertion."""
  d = tempfile.mkdtemp(prefix="noptrarith_")
  r = run([rysmith, "--n-funcs", "2", "--no-ptrarith", "--seed", "9001", "-o", d])
  check(
    "--no-ptrarith is accepted (exit 0)",
    r.returncode == 0,
    f"rc={r.returncode}, stderr={r.stderr[:200]!r}",
  )
  text = _collect_ptr_arith_text(
    rysmith, _PTR_ARITH_SEEDS, extra_flags=["--no-ptrarith"]
  )
  n_arr = len(_PTR_ARITH_RE.findall(text))
  n_field = len(_PTR_FIELD_ARITH_RE.findall(text))
  n_var = _count_ptr_var_arith(text)
  n_off = len(_OFFPATH_ADDR_ARITH_RE.findall(text))
  n_pp = _count_ptr_ptr_arith(text)
  check(
    f"--no-ptrarith emits zero ptr-arith (array={n_arr}, struct={n_field}, "
    f"var={n_var}, offpath={n_off}, ptrptr={n_pp})",
    n_arr == 0 and n_field == 0 and n_var == 0 and n_off == 0 and n_pp == 0,
    f"array={n_arr} struct={n_field} var={n_var} offpath={n_off} ptrptr={n_pp}",
  )


def test_ptr_arith_reassign_generated(rysmith):
  """A `ptr T_scalar` reassignment can emit in-bounds pointer arithmetic.
  Three behaviours are asserted:

    1. array element step `%p = ptrindex %ap, b ± d;` (b, b±d ∈ [0, N-1]);
    2. the negative-offset (`ptr - iN`) form — impossible before the offset
       was generalized away from the original constant `+ 1`;
    3. struct field step `%p = ptrfield %sp, f ± d;` across a run of
       consecutive same-type fields (lands on a same-typed, loadable cell
       per SPEC §7.5 rule 15b);
    4. arbitrary off-path arithmetic on a bare addr (`= addr %v + k;`) —
       unexecuted blocks are never run or UB-checked, so they carry any
       pointer ± any (unbounded) stride for pure codegen/alias stress.

  Pre-feature ptr reassigns were single-atom only (`genPtrAtom`), so no
  arithmetic tail could appear at all."""
  text = _collect_ptr_arith_text(rysmith, _PTR_ARITH_SEEDS)
  n_arr = len(_PTR_ARITH_RE.findall(text))
  n_neg = len(_PTR_ARITH_NEG_RE.findall(text))
  n_field = len(_PTR_FIELD_ARITH_RE.findall(text))
  n_off = len(_OFFPATH_ADDR_ARITH_RE.findall(text))
  n_pp = _count_ptr_ptr_arith(text)
  check(
    f"array ptr-arith reassignment present ({n_arr} found)",
    n_arr >= 3,
    f"found={n_arr}",
  )
  check(
    # On-path array arith is sparse at this sizing and drifts with RNG; the
    # signed-offset path itself is covered by the differential, so a low
    # presence bar suffices here.
    f"negative-offset (ptr - iN) array arith present ({n_neg} found)",
    n_neg >= 2,
    f"found={n_neg}",
  )
  check(
    f"struct field ptr-arith reassignment present ({n_field} found)",
    n_field >= 3,
    f"found={n_field}",
  )
  check(
    f"off-path arbitrary addr arithmetic present ({n_off} found)",
    n_off >= 3,
    f"found={n_off}",
  )
  check(
    f"ptr-ptr-T (loadable-pointee) arithmetic present ({n_pp} found)",
    n_pp >= 3,
    f"found={n_pp}",
  )


# The direct ptr-var shape (3) additionally needs an array source live in the
# same function as the reassigned scalar-ptr; that coexistence is much denser
# at higher --n-vars, so this collection widens vars/statements beyond the
# array/struct band above. Fewer seeds keep the (heavier) generation bounded.
_PTR_VAR_ARITH_SEEDS = (9001, 9002, 9003, 9004, 9005)


def test_ptr_var_arith_generated(rysmith):
  """(3) direct pointer-variable arithmetic: a coupled

      %p1 = ptrindex %ap, i;
      %p2 = %p1 ± d;          (i ± d ∈ [0, N-1])

  where the step's left operand is a pointer READ FROM A VARIABLE — the only
  shape exercising the `ptr_var ± iN` lowering path (the array/struct shapes
  put a fresh navigation expression on the left). The anchor pins %p1's
  provenance and literal index in the same block, so the bound holds by local
  def-use."""
  text = _collect_ptr_arith_text(
    rysmith, _PTR_VAR_ARITH_SEEDS, n_funcs="8", n_vars="30", n_stmts="10"
  )
  n = _count_ptr_var_arith(text)
  check(
    f"direct ptr-var arithmetic present ({n} found)",
    n >= 3,
    f"found={n}",
  )
  # Uniform aggregate navigation reuses this dense collection: chained
  # ptrindex/ptrfield steps (staged through injected `%nav` locals) reach a
  # loadable leaf of a nested array / array-of-struct / struct-of-array. We
  # assert chains exist AND that some involve a `ptrfield` step (the struct
  # case the array-only special form never covered).
  n_chain, n_field = _count_nav_chains(text)
  check(
    f"aggregate navigation chains present ({n_chain} found)",
    n_chain >= 3,
    f"found={n_chain}",
  )
  check(
    f"navigation chains through a struct field present ({n_field} found)",
    n_field >= 1,
    f"found={n_field}",
  )


def test_custom_int_widths_generated(rysmith):
  """P7: the SPEC admits any `iN` (`IntType := "i" Nat`) and the whole
  toolchain implements iN via widen-and-mask — a surface with zero
  generator coverage until now (the crc32 sub-byte bug lived exactly
  there). Custom widths {12, 20, 24, 40, 48} must appear in generated
  programs, and generation must stay healthy (concrete outputs exist)."""
  text = _collect_sym_text(rysmith, _P7_SEEDS)
  widths = {w: len(re.findall(rf"\bi{w}\b", text)) for w in (12, 20, 24, 40, 48)}
  total = sum(widths.values())
  check(
    f"custom iN widths present ({widths})",
    total >= 10 and sum(1 for n in widths.values() if n > 0) >= 3,
    f"widths={widths}",
  )


# ---------------------------------------------------------------------------
# --no-crc32 tests
# ---------------------------------------------------------------------------


def test_no_crc32_flag_accepted(rysmith):
  """rysmith --no-crc32 is accepted as a valid CLI flag and runs successfully."""
  with tempfile.TemporaryDirectory() as d:
    r = run([rysmith, "--n-funcs", "1", "--no-crc32", "--seed", "7", "-o", d])
    check(
      "rysmith --no-crc32 run exits 0",
      r.returncode == 0,
      f"rc={r.returncode}, stderr={r.stderr[:300]!r}",
    )


def test_no_crc32_no_crc32_update_in_sir(rysmith):
  """When --no-crc32 is specified, the generated SIR file does not contain crc32_update."""
  with tempfile.TemporaryDirectory() as d:
    r = run([rysmith, "--n-funcs", "1", "--no-crc32", "--seed", "7", "-o", d])
    if r.returncode != 0:
      check("no-crc32 presence check setup", False, r.stderr[:200])
      return
    sirs = [f for f in os.listdir(d) if f.endswith(".sir")]
    check(
      "at least one concrete .sir emitted for no-crc32 check", len(sirs) > 0, str(sirs)
    )
    if not sirs:
      return
    with open(os.path.join(d, sirs[0])) as f:
      content = f.read()
    check(
      "generated SIR does not contain crc32_update when --no-crc32 is specified",
      "crc32_update" not in content,
      "crc32_update found in SIR",
    )


def test_no_crc32_emit_main_runs_correctly(rysmith, symiri):
  """When --no-crc32 and --emit-main are specified, the program runs and validates with symiri."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--seed",
        "42",
        "--n-params",
        "2",
        "--emit-main",
        "--no-crc32",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("no-crc32 main validation setup", False, r.stderr[:200])
      return
    sirs = [f for f in os.listdir(d) if f.endswith(".sir")]
    if not sirs:
      check("at least one concrete .sir for no-crc32 main validation", False, "no sirs")
      return
    sir_path = os.path.join(d, sirs[0])

    # Execute symiri on it directly without entry or parameters!
    r2 = run([symiri, sir_path])
    check(
      "executing no-crc32 @main directly via symiri exits 0 and returns Result: 0",
      r2.returncode == 0 and "Result: 0" in r2.stdout,
      f"rc={r2.returncode}, stdout={r2.stdout[:200]!r}, stderr={r2.stderr[:200]!r}",
    )


def test_no_crc32_solved_header_matches_sum(rysmith, symiri):
  """When --no-crc32 is specified, the SOLVED header matches the return value of symiri."""
  gid, sir_path, d = _first_solved_run(rysmith, ["--no-crc32"])
  if gid is None:
    check(
      "at least one concrete .sir for no-crc32 replay", False, "no solving seed found"
    )
    return
  try:
    header = open(sir_path).readline().strip()
    check("no-crc32 first line is // SOLVED:", header.startswith("// SOLVED:"), header)
    if not header.startswith("// SOLVED:"):
      return
    kv = dict(re.findall(r"(%\w+|ret)=(-?\d+(?:\.\d+(?:[eE][-+]?\d+)?)?)", header))
    pa0 = kv.get("%pa0")
    pa1 = kv.get("%pa1")
    ret = kv.get("ret")
    check(
      "no-crc32 SOLVED has %pa0, %pa1, ret",
      pa0 is not None and pa1 is not None and ret is not None,
      str(kv),
    )
    if pa0 is None or pa1 is None:
      return
    # Replay through symiri — must produce the same return value.
    r2 = run([symiri, "--main", f"@func_{gid}_0", sir_path, "--", pa0, pa1])
    expected = f"Result: {ret.rstrip('.0')}" if ret.endswith(".0") else f"Result: {ret}"
    ok = r2.returncode == 0 and (f"Result: {ret}" in r2.stdout or expected in r2.stdout)
    check(
      f"no-crc32 symiri replay @func_{gid}_0({pa0},{pa1}) -> {ret}",
      ok,
      f"rc={r2.returncode}, stdout={r2.stdout[:120]!r}",
    )
  finally:
    shutil.rmtree(d, ignore_errors=True)


def test_no_crc32_descriptor_matches_solved_header(rysmith):
  """When --no-crc32 is specified, the descriptor json ret matches the SOLVED header ret."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--emit-desc",
        "--n-funcs",
        "1",
        "--seed",
        "11",
        "--n-params",
        "2",
        "--no-crc32",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("no-crc32 descriptor schema setup", False, r.stderr[:200])
      return
    gid = extract_id(r.stdout)
    if gid is None:
      check(
        "rysmith id discovery for no-crc32 descriptor schema", False, "no id in stdout"
      )
      return
    desc = json.load(open(os.path.join(d, f"func_{gid}_0.json")))
    check(
      "descriptor realization exists", "realizations" in desc, str(list(desc.keys()))
    )
    rzs = desc.get("realizations")
    if not rzs:
      return
    desc_ret = rzs[0].get("ret")
    sirs = sorted(
      f for f in os.listdir(d) if f.startswith(f"func_{gid}_0") and f.endswith(".sir")
    )
    if not sirs:
      check(
        "at least one concrete .sir for no-crc32 descriptor check", False, "no sirs"
      )
      return
    sir_path = os.path.join(d, sirs[0])
    header = open(sir_path).readline().strip()
    kv = dict(re.findall(r"(%\w+|ret)=(-?\d+(?:\.\d+(?:[eE][-+]?\d+)?)?)", header))
    solved_ret = kv.get("ret")
    check(
      "descriptor ret matches solved header ret",
      desc_ret == solved_ret,
      f"desc={desc_ret} solved={solved_ret}",
    )


# ---------------------------------------------------------------------------
# --require-ub tests
# ---------------------------------------------------------------------------


def test_require_ub_programs_actually_trap(rysmith, symiri):
  """Every concrete .sir produced under --require-ub must actually trigger
  UB when replayed through symiri with its SOLVED parameter values.

  Regression guard for the crc32-vs-require-ub interaction: the solver
  sees the sum-form `%_chk = %_chk + <leaf>` checksum and may satisfy the
  "at least one UB on the path" obligation by overflowing that
  accumulator. The crc32 rewrite would then replace the `+` with a total
  @crc32_update call, deleting the very UB the solver proved — so the
  emitted program came out UB-free. --require-ub now implies --no-crc32
  so the program we emit is byte-identical to the one we solved. This
  test fails if that guarantee regresses.
  """
  with tempfile.TemporaryDirectory() as d:
    r = run([rysmith, "--require-ub", "--n-funcs", "6", "--seed", "1234", "-o", d])
    if r.returncode != 0:
      check("require-ub run setup", False, r.stderr[:300])
      return
    sirs = sorted(f for f in os.listdir(d) if f.endswith(".sir") and "_sym" not in f)
    check("require-ub emitted at least one .sir", len(sirs) > 0, str(sirs))
    if not sirs:
      return
    # A require-ub program that traps never reaches a clean `ret`, so the
    # crc32 return-value oracle is vestigial: --require-ub implies
    # --no-crc32 and the emitted checksum must stay sum-form.
    with open(os.path.join(d, sirs[0])) as f:
      first_body = f.read()
    check(
      "require-ub emits no crc32_update (implies --no-crc32)",
      "crc32_update" not in first_body,
      "crc32_update found in a --require-ub program",
    )
    ub_free = []
    for name in sirs:
      path = os.path.join(d, name)
      src = open(path).read()
      m = re.search(r"//\s*SOLVED:\s*(.*)", src)
      kv = {}
      if m:
        for part in m.group(1).split(","):
          part = part.strip()
          if "=" in part:
            k, v = part.split("=", 1)
            kv[k.strip()] = v.strip()
      fm = re.search(r"fun\s+(@\w+)\s*\(([^)]*)\)", src)
      if not fm:
        continue
      fname = fm.group(1)
      pnames = [p.split(":")[0].strip() for p in fm.group(2).split(",") if p.strip()]
      args = [kv[p] for p in pnames if p in kv]
      rr = run([symiri, "--main", fname, path, "--"] + args)
      out = rr.stdout + rr.stderr
      if "UB" not in out and "ndefined" not in out:
        ub_free.append((name, out.strip().splitlines()[-1:] if out.strip() else []))
    check(
      "every --require-ub program traps UB in symiri",
      len(ub_free) == 0,
      f"{len(ub_free)} UB-free program(s): {ub_free}",
    )


def _solved_call(src):
  """Extract (fname, positional-args) for a rysmith .sir from its `fun`
  signature (declaration order) and `// SOLVED:` header. Shared by the
  replay-style tests."""
  m = re.search(r"//\s*SOLVED:\s*(.*)", src)
  kv = {}
  if m:
    for part in m.group(1).split(","):
      part = part.strip()
      if "=" in part:
        k, v = part.split("=", 1)
        kv[k.strip()] = v.strip()
  fm = re.search(r"fun\s+(@\w+)\s*\(([^)]*)\)", src)
  if not fm:
    return None, []
  fname = fm.group(1)
  pnames = [p.split(":")[0].strip() for p in fm.group(2).split(",") if p.strip()]
  return fname, [kv[p] for p in pnames if p in kv]


def test_require_nonterm_programs_actually_diverge(rysmith, symiri):
  """Every concrete .sir produced under --require-nonterm must actually
  diverge (⇑) when replayed through symiri with its SOLVED parameters: the
  lasso's header state recurs after one lap, so the loop runs forever. A
  bounded replay confirms the program neither returns (a `Result:` line) nor
  traps (UB) within the window — either would mean the fixed point the solver
  proved did not hold at runtime. --require-nonterm also implies --no-crc32
  (the checksum is dead and its oracle would hang)."""

  def runs_forever(fname, path, args, timeout=1.5):
    try:
      rr = subprocess.run(
        [symiri, "--main", fname, path, "--"] + args,
        capture_output=True,
        text=True,
        timeout=timeout,
      )
      return False, (rr.stdout + rr.stderr)  # returned => terminated or trapped
    except subprocess.TimeoutExpired:
      return True, ""  # still running => diverges

  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--require-nonterm",
        "--n-funcs",
        "6",
        "--seed",
        "7",
        "--n-concretes",
        "1",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("require-nonterm run setup", False, r.stderr[:300])
      return
    sirs = sorted(f for f in os.listdir(d) if f.endswith(".sir") and "_sym" not in f)
    check("require-nonterm emitted at least one .sir", len(sirs) > 0, str(sirs))
    if not sirs:
      return
    first_body = open(os.path.join(d, sirs[0])).read()
    check(
      "require-nonterm emits no crc32_update (implies --no-crc32)",
      "crc32_update" not in first_body,
      "crc32_update found in a --require-nonterm program",
    )
    not_diverging = []
    for name in sirs:
      path = os.path.join(d, name)
      fname, args = _solved_call(open(path).read())
      if fname is None:
        continue
      diverges, out = runs_forever(fname, path, args)
      if not diverges:
        last = out.strip().splitlines()[-1:] if out.strip() else []
        not_diverging.append((name, last))
    check(
      f"every --require-nonterm program diverges under symiri "
      f"({len(sirs) - len(not_diverging)}/{len(sirs)} bounded runs)",
      len(not_diverging) == 0,
      f"{len(not_diverging)} terminated/trapped: {not_diverging}",
    )


def test_require_nonterm_validate_flag(rysmith):
  """rysmith --require-nonterm --validate runs the in-process bounded-replay
  check (fuel-capped interpreter + bit-exact header-state recurrence) and
  reports OK for every solved program without hanging -- confirming the fixed
  point the solver proved actually holds at runtime."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--require-nonterm",
        "--validate",
        "--seed",
        "7",
        "--n-funcs",
        "6",
        "--n-concretes",
        "1",
        "-o",
        d,
      ]
    )
    check("require-nonterm --validate exits 0", r.returncode == 0, r.stderr[:300])
    oks = r.stdout.count("validated: OK(")
    fails = r.stdout.count("validated: FAIL(")
    check(
      "require-nonterm --validate: >=1 OK and 0 FAIL",
      oks >= 1 and fails == 0,
      f"oks={oks} fails={fails}",
    )


def test_require_nonterm_emit_main(rysmith, symiri, symirc):
  """--require-nonterm --emit-main appends a @main that calls the diverging
  entry and checks its (unreachable) return against a random checksum, so the
  whole program stays compilable for differential testing: the compiler cannot
  prove the loop is infinite, so it must preserve the computation against
  @check_chksum's abort(). The wrapper must be well-formed and lower to C."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--require-nonterm",
        "--emit-main",
        "--seed",
        "42",
        "--n-funcs",
        "3",
        "--n-concretes",
        "1",
        "-o",
        d,
      ]
    )
    check("require-nonterm --emit-main exits 0", r.returncode == 0, r.stderr[:300])
    sirs = sorted(f for f in os.listdir(d) if f.endswith(".sir") and "_sym" not in f)
    check(
      "require-nonterm --emit-main emitted at least one .sir", len(sirs) > 0, str(sirs)
    )
    for name in sirs:
      p = os.path.join(d, name)
      body = open(p).read()
      check(
        f"{name}: @main calls the entry and @check_chksum",
        "fun @main(" in body and "call @check_chksum(" in body,
        body[:200],
      )
      cr = run([symiri, "--check", p])
      check(f"{name}: symiri --check well-formed", cr.returncode == 0, cr.stderr[:200])
      cc = run([symirc, "--target", "c", p, "-o", p + ".c"])
      check(f"{name}: symirc --target c lowers", cc.returncode == 0, cc.stderr[:200])


def test_require_nonterm_c_stays_infinite(rysmith):
  """A diverging program compiled to C keeps looping at -O2 under both gcc and
  clang. A side-effect-free infinite loop is deletable under C11 6.8.5p6 /
  mustprogress, and gcc/clang exploit that non-deterministically at -O2 — so
  rysmith plants an `@observe` beacon (a volatile write) in the cycle to keep
  the loop observably side-effecting. This test uses the default goto lowering
  and seed 33, a config that terminated under clang -O2 *before* the beacon; a
  build that terminates now is a regression (beacon dropped or lowered wrong)."""
  import shutil

  ccs = [c for c in ("gcc", "clang") if shutil.which(c)]
  if not ccs:
    check("nonterm-C: a C compiler is available", False, "neither gcc nor clang found")
    return
  with tempfile.TemporaryDirectory() as d:
    run(
      [
        rysmith,
        "--require-nonterm",
        "--emit-main",
        "--target",
        "c",
        "--seed",
        "33",
        "--n-funcs",
        "4",
        "--n-concretes",
        "1",
        "-o",
        d,
      ]
    )
    cs = [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".c")]
    check("nonterm-C: emitted at least one .c", len(cs) >= 1, str(cs))
    for cf in cs:
      for cc in ccs:
        binp = cf + "." + cc
        comp = subprocess.run(
          [cc, "-O2", cf, "-lm", "-o", binp], capture_output=True, text=True
        )
        if comp.returncode != 0:
          check(f"{os.path.basename(cf)}: {cc} -O2 compiles", False, comp.stderr[:200])
          continue
        try:
          subprocess.run([binp], capture_output=True, timeout=2.0)
          diverges = False  # returned within the window => terminated
        except subprocess.TimeoutExpired:
          diverges = True  # still running => diverges, as intended
        check(
          f"{os.path.basename(cf)}: {cc} -O2 build stays infinite",
          diverges,
          "compiled program terminated — the diverging loop was deleted",
        )


def _nonterm_batch_state_kinds(rysmith, seed, n_funcs, extra=()):
  """Generate a --require-nonterm batch and report which type kinds appear as
  mutable state, plus the concrete .sir paths. Shared by the coverage tests."""
  d = tempfile.mkdtemp()
  r = run(
    [
      rysmith,
      "--require-nonterm",
      "--seed",
      str(seed),
      "--n-funcs",
      str(n_funcs),
      "--n-concretes",
      "1",
      "-o",
      d,
    ]
    + list(extra)
  )
  sirs = (
    [
      os.path.join(d, f)
      for f in sorted(os.listdir(d))
      if f.endswith(".sir") and "_sym" not in f
    ]
    if r.returncode == 0
    else []
  )
  bodies = "".join(open(p).read() for p in sirs)
  kinds = {
    "fp": bool(re.search(r"let mut [^:]+:\s*(f32|f64)\b", bodies)),
    "ptr": bool(re.search(r"let mut [^:]+:\s*ptr\b", bodies)),
    "int": bool(re.search(r"let mut [^:]+:\s*i\d+\b", bodies)),
  }
  return d, r, sirs, kinds


def _all_diverge(symiri, sirs, timeout=1.5):
  """Bounded replay: return the subset of `sirs` that terminated or trapped."""
  bad = []
  for path in sirs:
    fname, args = _solved_call(open(path).read())
    if fname is None:
      continue
    try:
      rr = subprocess.run(
        [symiri, "--main", fname, path, "--"] + args,
        capture_output=True,
        text=True,
        timeout=timeout,
      )
      bad.append((os.path.basename(path), (rr.stdout + rr.stderr).strip()[-160:]))
    except subprocess.TimeoutExpired:
      pass  # still running => diverges, as intended
  return bad


def test_require_nonterm_covers_fp_state(rysmith, symiri):
  """--require-nonterm admits floating-point state. A float leaf is closed by
  the same additive correction as an integer one (`%v = %v + %?ntK`), and the
  header recurrence is bit-exact equality, so a lap that only permutes signed
  zeros is not mistaken for a fixed point. Requires that float locals actually
  appear in a batch and that every program still diverges."""
  d, r, sirs, kinds = _nonterm_batch_state_kinds(
    rysmith, seed=7, n_funcs=10, extra=("--no-vec", "--no-agg-ptr")
  )
  check("nonterm-fp: generation exits 0", r.returncode == 0, r.stderr[:300])
  check(
    "nonterm-fp: float locals appear as state",
    kinds["fp"],
    f"no f32/f64 `let mut` across {len(sirs)} programs (FP still disabled?)",
  )
  bad = _all_diverge(symiri, sirs)
  check(f"nonterm-fp: all {len(sirs)} programs diverge", not bad, f"terminated: {bad}")


def test_require_nonterm_covers_pointer_state(rysmith, symiri):
  """--require-nonterm admits pointer state. A pointer leaf is closed by
  pointer arithmetic against a fresh i64 correction symbol (`%p = %p + %?ntK`),
  which the asserted in-bounds guard keeps within the pointee's object.
  Requires that pointer locals actually appear and every program diverges."""
  d, r, sirs, kinds = _nonterm_batch_state_kinds(
    rysmith, seed=11, n_funcs=10, extra=("--no-fp",)
  )
  check("nonterm-ptr: generation exits 0", r.returncode == 0, r.stderr[:300])
  check(
    "nonterm-ptr: pointer locals appear as state",
    kinds["ptr"],
    f"no `ptr` `let mut` across {len(sirs)} programs (pointers still disabled?)",
  )
  bad = _all_diverge(symiri, sirs)
  check(f"nonterm-ptr: all {len(sirs)} programs diverge", not bad, f"terminated: {bad}")


def test_require_nonterm_validate_full_lattice(rysmith):
  """With the type lattice unrestricted, `--validate`'s bounded replay (which
  compares header states *bit-exactly*, unlike the solver's FP reasoning) must
  still confirm every solved program: 0 FAIL. This is the check that would
  catch a float fixed point the solver believed but that does not hold
  bit-for-bit at runtime (the ±0 hazard)."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--require-nonterm",
        "--validate",
        "--seed",
        "23",
        "--n-funcs",
        "10",
        "--n-concretes",
        "1",
        "-o",
        d,
      ]
    )
    check("nonterm-lattice: --validate exits 0", r.returncode == 0, r.stderr[:300])
    oks, fails = r.stdout.count("validated: OK("), r.stdout.count("validated: FAIL(")
    check(
      "nonterm-lattice: >=1 OK and 0 FAIL",
      oks >= 1 and fails == 0,
      f"oks={oks} fails={fails}",
    )


def test_emit_main_checksum_oracle_agrees(rysmith, symiri):
  """The --emit-main checksum is an independent oracle: it is built from the
  *solver's* model of the end-of-path state, then checked against what the
  *interpreter* actually computes. Running @main must therefore exit 0 on
  every generated program — a mismatch means the two disagree about the final
  state.

  The delicate part is pointers. The solver reports where each pointer ended
  up as (targetLocal, targetOffset), and the oracle builder walks that offset
  back down into a leaf. Both sides must measure with the same size model; a
  decoder counting one unit per scalar leaf while the solver reports packed
  bytes lands on the wrong leaf, and only when a struct mixes scalar widths
  (e.g. an `i24` beside an `[2] i64`), which is why one fixed seed is not
  enough coverage here."""
  bad = []
  with tempfile.TemporaryDirectory() as d:
    for seed in ("42", "7", "11", "23", "33", "101"):
      sub = os.path.join(d, "s" + seed)
      os.makedirs(sub, exist_ok=True)
      r = run(
        [
          rysmith,
          "--n-funcs",
          "4",
          "--seed",
          seed,
          "--n-params",
          "2",
          "--emit-main",
          "-o",
          sub,
        ]
      )
      if r.returncode != 0:
        bad.append((seed, "generation failed", r.stderr[:120]))
        continue
      for name in sorted(f for f in os.listdir(sub) if f.endswith(".sir")):
        rr = run([symiri, os.path.join(sub, name)])
        if rr.returncode != 0:
          bad.append((seed, name, (rr.stdout + rr.stderr).strip()[:120]))
  check(
    "emit-main: checksum oracle agrees with the interpreter on every program",
    not bad,
    f"{len(bad)} mismatched: {bad[:4]}",
  )


def _lasso_period(sir_text):
  """Read the orbit's period off the `// LASSO:` header: the header block is
  the last label, and k+1 arrivals at it means k laps."""
  for line in sir_text.splitlines():
    if "LASSO:" in line:
      labels = line.split("LASSO:")[1].replace("->", " ").split()
      return labels.count(labels[-1]) - 1
  return None


def test_max_lasso_period_generates_multi_lap_orbits(rysmith):
  """--max-lasso-period N draws the orbit period k from [1, N], so a batch
  contains genuinely multi-lap lassos, not just the classic single-lap fixed
  point. Each program's spliced modulus local must equal its own period: the
  counter is what makes the k-cycle realizable, since the additive corrections
  alone can only ever close a period-1 orbit (RefractIR's `+` traps on signed
  overflow, so the solver is forced to a zero correction)."""
  with tempfile.TemporaryDirectory() as d:
    for seed in ("1", "2", "3"):
      run(
        [
          rysmith,
          "--require-nonterm",
          "--max-lasso-period",
          "4",
          "--seed",
          seed,
          "--n-funcs",
          "10",
          "--n-concretes",
          "1",
          "-o",
          d,
        ]
      )
    sirs = [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(".sir")]
    periods, mismatched = [], []
    for p in sirs:
      body = open(p).read()
      k = _lasso_period(body)
      if k is None:
        continue
      periods.append(k)
      m = re.search(r"let mut %_ntmod: i32 = (\d+);", body)
      declared = int(m.group(1)) if m else 1
      if declared != max(k, 1) and not (k == 1 and m is None):
        mismatched.append((os.path.basename(p), k, declared))
    check("max-lasso-period: emitted programs", len(periods) > 0, "no lassos found")
    check(
      "max-lasso-period: some orbits have k > 1",
      any(k > 1 for k in periods),
      f"all periods were 1: {sorted(set(periods))}",
    )
    check(
      "max-lasso-period: odd periods reachable too",
      any(k % 2 == 1 and k > 1 for k in periods),
      f"no odd k>1 orbit; periods seen: {sorted(set(periods))}",
    )
    check(
      "max-lasso-period: modulus local matches each orbit's period",
      not mismatched,
      f"mismatches (file, k, declared): {mismatched}",
    )


def test_max_lasso_period_requires_nonterm(rysmith):
  """--max-lasso-period only means something for lasso sampling, so asking for
  it without --require-nonterm is a usage error rather than a silent no-op."""
  with tempfile.TemporaryDirectory() as d:
    r = run([rysmith, "--max-lasso-period", "3", "--n-funcs", "1", "-o", d])
    check(
      "max-lasso-period without --require-nonterm exits non-zero",
      r.returncode != 0,
      f"rc={r.returncode}",
    )
    check(
      "max-lasso-period: error names the missing flag",
      "require-nonterm" in r.stderr,
      r.stderr[:200],
    )


def test_max_lasso_period_validates(rysmith):
  """The bounded-replay check must understand periods: for a period-k orbit
  *consecutive* header arrivals deliberately differ, so validation compares
  arrivals k laps apart. A validator that compared neighbours would fail every
  k > 1 program. Seed pinned so the assertion is deterministic."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--require-nonterm",
        "--max-lasso-period",
        "4",
        "--validate",
        "--seed",
        "3",
        "--n-funcs",
        "10",
        "--n-concretes",
        "1",
        "-o",
        d,
      ]
    )
    check("max-lasso-period --validate exits 0", r.returncode == 0, r.stderr[:300])
    oks, fails = r.stdout.count("validated: OK("), r.stdout.count("validated: FAIL(")
    check(
      "max-lasso-period: multi-lap orbits validate as diverging",
      oks >= 5,
      f"oks={oks} fails={fails}",
    )


def test_require_ub_emit_main_reparses(rysmith, symiri):
  """Every --require-ub --emit-main whole-program .sir must re-parse.

  The trap check in test_require_ub_programs_actually_trap replays SOLVED
  values as CLI positional args, where symiri's parser routes through strtod
  and silently accepts `inf`/`nan`. That masks a distinct failure mode: when an
  FP overflow's UB is satisfied by a non-finite *input*, the @main wrapper
  concretizes the leaf call to `call @f(inf, ...)` (and any FP `sym` to a bare
  `nan` literal in the body) — tokens the RefractIR lexer cannot start (a
  number must begin with a digit), so the emitted program no longer parses.

  Inputs are RefractIR values and thus finite (SPEC v0.2.2 §5), so a correct
  --require-ub program always re-parses and reaches real UB, never a frontend
  "Expected atom" parse error. Run the whole @main program through symiri and
  require it to reach UB rather than fail to parse.
  """
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--require-ub",
        "--emit-main",
        "--n-funcs",
        "6",
        "--seed",
        "1234",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("require-ub --emit-main run setup", False, r.stderr[:300])
      return
    sirs = sorted(
      f
      for f in os.listdir(d)
      if f.endswith(".sir") and "@main" in open(os.path.join(d, f)).read()
    )
    check("require-ub --emit-main emitted @main programs", len(sirs) > 0, str(sirs))
    unparseable = []
    for name in sirs:
      path = os.path.join(d, name)
      rr = run([symiri, "--main", "@main", path])
      out = rr.stdout + rr.stderr
      if "UB" not in out and "ndefined" not in out:
        unparseable.append((name, out.strip().splitlines()[-2:]))
    check(
      "every --require-ub --emit-main program re-parses (no inf/nan literal leak)",
      len(unparseable) == 0,
      f"{len(unparseable)} unparseable program(s): {unparseable}",
    )


# ---------------------------------------------------------------------------
# --emit-state tests
# ---------------------------------------------------------------------------


def test_emit_state_pbb_sidecar(rysmith):
  """rysmith --emit-state pbb writes a well-formed func_<id>_<i>.state.json
  sidecar: a per-block-entry trace of the concrete state under the solved
  input."""
  with tempfile.TemporaryDirectory() as d:
    r = run([rysmith, "--emit-state", "pbb", "--n-funcs", "1", "--seed", "5", "-o", d])
    if r.returncode != 0:
      check("emit-state pbb run setup", False, r.stderr[:300])
      return
    states = [f for f in os.listdir(d) if f.endswith(".state.json")]
    check(
      "emit-state pbb wrote a .state.json sidecar", len(states) > 0, str(os.listdir(d))
    )
    if not states:
      return
    prof = json.load(open(os.path.join(d, states[0])))
    check(
      "state profile func is canonical",
      prof.get("func", "").startswith("@"),
      str(prof.get("func")),
    )
    check(
      "state profile granularity is pbb",
      prof.get("granularity") == "pbb",
      str(prof.get("granularity")),
    )
    trace = prof.get("trace") or []
    check("state profile trace is non-empty", len(trace) > 0, str(len(trace)))
    if not trace:
      return
    p0 = trace[0]
    check(
      "pbb points are block-entry (instr == -1)",
      all(p.get("instr") == -1 for p in trace),
      str([p.get("instr") for p in trace[:4]]),
    )
    check(
      "first point carries variable state",
      isinstance(p0.get("vars"), dict) and len(p0["vars"]) > 0,
      str(p0.get("vars"))[:120],
    )
    # Spot-check the tagged value shape on some scalar var.
    tagged = [
      v for v in p0.get("vars", {}).values() if isinstance(v, dict) and "k" in v
    ]
    check(
      "values use the tagged {k,...} shape", len(tagged) > 0, str(p0.get("vars"))[:120]
    )


def test_emit_state_ppp_and_validation(rysmith):
  """--emit-state ppp adds per-instruction points; an invalid granularity
  is rejected."""
  with tempfile.TemporaryDirectory() as d:
    r = run([rysmith, "--emit-state", "ppp", "--n-funcs", "1", "--seed", "3", "-o", d])
    states = (
      [f for f in os.listdir(d) if f.endswith(".state.json")]
      if r.returncode == 0
      else []
    )
    if states:
      prof = json.load(open(os.path.join(d, states[0])))
      check(
        "ppp granularity recorded",
        prof.get("granularity") == "ppp",
        str(prof.get("granularity")),
      )
      instrs = [p.get("instr") for p in prof.get("trace", [])]
      check(
        "ppp trace includes per-instruction points",
        any(i is not None and i >= 0 for i in instrs),
        str(instrs[:8]),
      )
    else:
      check("emit-state ppp produced a sidecar", False, f"rc={r.returncode}")
  # Invalid granularity must be rejected with a non-zero exit.
  with tempfile.TemporaryDirectory() as d:
    r = run([rysmith, "--emit-state", "xyz", "--n-funcs", "1", "-o", d])
    check(
      "invalid --emit-state value rejected", r.returncode != 0, f"rc={r.returncode}"
    )


def test_require_reducible_all_reducible(rysmith, symirc):
  """--require-reducible: every generated .sir passes symirc --require-reducible."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "4",
        "--seed",
        "77",
        "--p-backedge",
        "0.9",
        "--n-bbls",
        "18",
        "--require-reducible",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith --require-reducible accepted", False, r.stderr[:300])
      return
    sirs = [f for f in os.listdir(d) if f.endswith(".sir")]
    if not sirs:
      check("--require-reducible produced programs", False, "no .sir emitted")
      return
    bad = []
    for f in sirs:
      rc = run([symirc, os.path.join(d, f), "--require-reducible", "-o", os.devnull])
      if rc.returncode != 0:
        bad.append(f)
    check(
      f"all {len(sirs)} --require-reducible programs are reducible",
      not bad,
      f"irreducible: {bad}",
    )


def test_descriptor_reducible_field(rysmith):
  """Descriptor JSON records per-function reducibility as a bool."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--emit-desc",
        "--seed",
        "11",
        "--require-reducible",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith reducible-descriptor setup", False, r.stderr[:200])
      return
    gid = extract_id(r.stdout)
    if gid is None:
      check("rysmith id discovery for reducible-descriptor", False, "no id in stdout")
      return
    desc = json.load(open(os.path.join(d, f"func_{gid}_0.json")))
    check("descriptor `reducible` present", "reducible" in desc, str(list(desc.keys())))
    check(
      "descriptor `reducible` is true under --require-reducible",
      desc.get("reducible") is True,
      str(desc.get("reducible")),
    )


def test_structured_lowering_true_goto_free(rysmith):
  """--structured-lowering true: emitted C is goto-free."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "2",
        "--seed",
        "5150",
        "--p-backedge",
        "0.8",
        "--target",
        "c",
        "--structured-lowering",
        "true",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith --structured-lowering true accepted", False, r.stderr[:300])
      return
    cs = [f for f in os.listdir(d) if f.endswith(".c")]
    if not cs:
      check("structured-lowering true produced .c files", False, "no .c emitted")
      return
    with_goto = [f for f in cs if "goto" in open(os.path.join(d, f)).read()]
    check(
      f"all {len(cs)} structured .c files are goto-free",
      not with_goto,
      f"goto in: {with_goto}",
    )


def test_structured_lowering_implies_reducible(rysmith, symirc):
  """--structured-lowering true implies --require-reducible for the run."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "3",
        "--seed",
        "77",
        "--p-backedge",
        "0.9",
        "--n-bbls",
        "18",
        "--target",
        "c",
        "--structured-lowering",
        "true",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("structured-lowering implies-reducible setup", False, r.stderr[:300])
      return
    sirs = [f for f in os.listdir(d) if f.endswith(".sir")]
    bad = []
    for f in sirs:
      rc = run([symirc, os.path.join(d, f), "--require-reducible", "-o", os.devnull])
      if rc.returncode != 0:
        bad.append(f)
    check(
      f"structured-lowering true made all {len(sirs)} programs reducible",
      bool(sirs) and not bad,
      f"irreducible: {bad}" if sirs else "no .sir emitted",
    )


def test_structured_lowering_value_validation(rysmith):
  """random is accepted; unknown values are rejected."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "2",
        "--seed",
        "31",
        "--target",
        "c",
        "--structured-lowering",
        "random",
        "-o",
        d,
      ]
    )
    cs = [f for f in os.listdir(d) if f.endswith(".c")]
    check(
      "--structured-lowering random runs and compiles",
      r.returncode == 0 and bool(cs),
      f"rc={r.returncode}, .c={len(cs)}",
    )
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--target",
        "c",
        "--structured-lowering",
        "maybe",
        "-o",
        d,
      ]
    )
    check(
      "--structured-lowering maybe rejected",
      r.returncode != 0,
      f"rc={r.returncode}",
    )


def test_structured_lowering_wasm(rysmith):
  """structured lowering on the wasm target emits dispatch-free WASM."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "6",
        "--target",
        "wasm",
        "--structured-lowering",
        "true",
        "--emit-main",
        "--seed",
        "7",
        "-o",
        d,
      ]
    )
    wats = [f for f in os.listdir(d) if f.endswith(".wat")]
    # Structured emission reconstructs block/loop/if; the $__pc +
    # br_table dispatch loop must be gone.
    dispatchy = [
      f
      for f in wats
      if "br_table" in open(os.path.join(d, f)).read()
      or "dispatch_loop" in open(os.path.join(d, f)).read()
    ]
    check(
      f"--structured-lowering + wasm emits {len(wats)} dispatch-free .wat",
      r.returncode == 0 and bool(wats) and not dispatchy,
      f"rc={r.returncode}, wats={len(wats)}, dispatch in={dispatchy}, stderr={r.stderr[:150]!r}",
    )


def test_python_vec_lowering_flag(rysmith):
  """--target python honors an explicit --vec-lowering strategy."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "2",
        "--seed",
        "11",
        "--target",
        "python",
        "--vec-lowering",
        "structarray",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith python --vec-lowering structarray accepted", False, r.stderr[:300])
      return
    pys = [f for f in os.listdir(d) if f.endswith(".py")]
    bad = [
      f
      for f in pys
      if "# vec-lowering: structarray" not in open(os.path.join(d, f)).read()
    ]
    check(
      f"all {len(pys)} python outputs stamped structarray",
      bool(pys) and not bad,
      f"unstamped: {bad}" if pys else "no .py emitted",
    )


def test_python_vec_lowering_random(rysmith):
  """--vec-lowering random on python sweeps the python set (no vecext)."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "4",
        "--seed",
        "1234",
        "--target",
        "python",
        "--vec-lowering",
        "random",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith python --vec-lowering random accepted", False, r.stderr[:300])
      return
    pys = [f for f in os.listdir(d) if f.endswith(".py")]
    allowed = {"array", "scalars", "structarray", "structscalars"}
    bad = []
    for f in pys:
      m = re.search(r"# vec-lowering: (\S+)", open(os.path.join(d, f)).read())
      if not m or m.group(1) not in allowed:
        bad.append(f"{f}: {m.group(1) if m else 'no stamp'}")
    check(
      f"all {len(pys)} random python outputs use python strategies",
      bool(pys) and not bad,
      "; ".join(bad) if pys else "no .py emitted",
    )


def test_python_vec_lowering_vecext_rejected(rysmith):
  """vecext has no python analogue and is rejected up-front."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--target",
        "python",
        "--vec-lowering",
        "vecext",
        "-o",
        d,
      ]
    )
    check(
      "rysmith python + vecext rejected",
      r.returncode != 0 and "vec-lowering" in (r.stderr or ""),
      f"rc={r.returncode}, stderr={r.stderr[:150]!r}",
    )


def test_target_python_emits_py(rysmith):
  """--target python writes a syntactically valid .py per program."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--n-funcs",
        "2",
        "--seed",
        "11",
        "--p-backedge",
        "0.8",
        "--target",
        "python",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("rysmith --target python accepted", False, r.stderr[:300])
      return
    pys = [f for f in os.listdir(d) if f.endswith(".py")]
    if not pys:
      check("--target python produced .py files", False, "no .py emitted")
      return
    # The .sir companions must survive the end-of-run orphan sweep
    # (each .py is the .sir's compiled twin, like .c / .wat).
    sirs = {f[:-4] for f in os.listdir(d) if f.endswith(".sir")}
    missing = [f for f in pys if f[:-3] not in sirs]
    check(
      "python outputs keep their .sir companions",
      not missing,
      f".sir missing for: {missing}",
    )
    bad = []
    for f in pys:
      p = os.path.join(d, f)
      try:
        compile(open(p).read(), p, "exec")
      except SyntaxError as e:
        bad.append(f"{f}: {e}")
    check(
      f"all {len(pys)} python outputs compile",
      not bad,
      "; ".join(bad),
    )


# ---------------------------------------------------------------------------
# --n-examples: multiple input/output examples per .sir
# ---------------------------------------------------------------------------
#
# Every example is one `// SOLVED:` line in the banner; under --emit-main
# they are additionally replayed as one @entry call + @check_chksum pair
# each in @main. Extra examples re-solve the same template with fresh
# solver seeds, so examples differ in their parameter values.

_EX_SOLVED_RE = re.compile(r"(%\w+|ret)=(-?\d+(?:\.\d+(?:[eE][-+]?\d+)?)?)")


def _ex_solved_lines(text):
  """Every `// SOLVED:` line of a .sir, stripped of the marker."""
  return [
    ln[len("// SOLVED:") :].strip()
    for ln in text.splitlines()
    if ln.startswith("// SOLVED:")
  ]


def _ex_parse_sig(text):
  """(entry name, [param names in declaration order]) from the first `fun`."""
  m = re.search(r"fun\s+(@\w+)\s*\(([^)]*)\)", text)
  if not m:
    return None, []
  fname = m.group(1)
  pnames = [p.split(":")[0].strip() for p in m.group(2).split(",") if p.strip()]
  return fname, pnames


def _ex_first_multi_example_run(rysmith, extra_args=None, wanted=2):
  """Generate a 2-param function with --n-examples 3 over _REPLAY_SEEDS until
  one run lands >= `wanted` banner examples. Returns (gid, sir_path, tmpdir)
  or (None, None, None); the caller removes tmpdir."""
  for seed in _REPLAY_SEEDS:
    d = tempfile.mkdtemp(prefix="nex_")
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--seed",
        str(seed),
        "--n-params",
        "2",
        "--n-examples",
        "3",
        "-o",
        d,
      ]
      + (extra_args or [])
    )
    gid = extract_id(r.stdout) if r.returncode == 0 else None
    if gid:
      sirs = sorted(
        f for f in os.listdir(d) if f.startswith(f"func_{gid}_0") and f.endswith(".sir")
      )
      for s in sirs:
        body = open(os.path.join(d, s)).read()
        if len(set(_ex_solved_lines(body))) >= wanted:
          return gid, os.path.join(d, s), d
    shutil.rmtree(d, ignore_errors=True)
  return None, None, None


def test_n_examples_banner_lines(rysmith):
  """--n-examples 3 puts every example in the banner: one `// SOLVED:`
  line each, all declared params and a ret valued."""
  gid, sir_path, d = _ex_first_multi_example_run(rysmith)
  if gid is None:
    check(
      "at least one multi-example .sir in banner mode", False, "no solving seed found"
    )
    return
  try:
    text = open(sir_path).read()
    lines = _ex_solved_lines(text)
    check(
      "banner carries >= 2 SOLVED lines under --n-examples 3",
      len(lines) >= 2,
      f"{len(lines)} lines: {lines}",
    )
    if len(lines) < 2:
      return
    _, pnames = _ex_parse_sig(text)
    bad = [
      ln for ln in lines if any(f"{n}=" not in ln for n in pnames) or "ret=" not in ln
    ]
    check(
      "every SOLVED line values all params and a ret",
      not bad,
      f"missing keys in {bad}",
    )
  finally:
    shutil.rmtree(d, ignore_errors=True)


def test_n_examples_banner_distinct(rysmith):
  """Two SOLVED lines never name the same parameter-value tuple."""
  gid, sir_path, d = _ex_first_multi_example_run(rysmith)
  if gid is None:
    check(
      "at least one multi-example .sir for distinctness", False, "no solving seed found"
    )
    return
  try:
    lines = _ex_solved_lines(open(sir_path).read())
    tuples = {ln for ln in lines}
    check(
      "SOLVED lines name distinct param tuples",
      len(tuples) == len(lines),
      f"{len(lines)} lines collapse to {len(tuples)}: {lines}",
    )
  finally:
    shutil.rmtree(d, ignore_errors=True)


def test_n_examples_examples_replay(rysmith, symiri):
  """Every banner example replays through symiri to its own ret."""
  gid, sir_path, d = _ex_first_multi_example_run(rysmith)
  if gid is None:
    check("at least one multi-example .sir for replay", False, "no solving seed found")
    return
  try:
    text = open(sir_path).read()
    fname, pnames = _ex_parse_sig(text)
    lines = _ex_solved_lines(text)
    if fname is None or len(lines) < 2:
      check("multi-example .sir parses", False, text[:200])
      return
    bad = []
    for ln in lines:
      kv = dict(_EX_SOLVED_RE.findall(ln))
      args = [kv[p] for p in pnames if p in kv]
      r2 = run([symiri, "--main", fname, sir_path, "--"] + args)
      if r2.returncode != 0 or f"Result: {kv['ret']}" not in r2.stdout:
        bad.append((ln, (r2.stdout + r2.stderr).strip()[:120]))
    check(
      "every banner example replays to its own ret",
      not bad,
      f"{len(bad)} replay mismatches: {bad[:3]}",
    )
  finally:
    shutil.rmtree(d, ignore_errors=True)


def test_n_examples_main_multi_calls(rysmith, symiri):
  """With --emit-main, --n-examples 3 appends a @main with one entry call
  plus @check_chksum per example, and running it exits 0."""
  gid, sir_path, d = _ex_first_multi_example_run(
    rysmith, extra_args=["--emit-main"], wanted=1
  )
  if gid is None:
    check(
      "at least one multi-example .sir in main mode", False, "no solving seed found"
    )
    return
  try:
    # `@main` is the last `fun` in the file; join the trailing body.
    body = open(sir_path).read()
    main_text = body[body.index("fun @main(") :]
    n_calls = len(re.findall(r"call @" + re.escape(f"func_{gid}_0") + r"\(", main_text))
    n_checks = len(re.findall(r"call @check_chksum\(", main_text))
    check(
      "@main carries one entry call per example (>= 2)",
      n_calls >= 2,
      f"{n_calls} entry calls in @main",
    )
    check(
      "@main carries one @check_chksum per example (>= 2)",
      n_checks >= 2,
      f"{n_checks} checks in @main",
    )
    r2 = run([symiri, sir_path])
    check(
      "executing the multi-call @main via symiri exits 0",
      r2.returncode == 0 and "Result: 0" in r2.stdout,
      f"rc={r2.returncode}, stdout={r2.stdout[:200]!r}, stderr={r2.stderr[:200]!r}",
    )
  finally:
    shutil.rmtree(d, ignore_errors=True)


def test_n_examples_main_banner_lines(rysmith):
  """--emit-main does not change the banner: every example is still one
  SOLVED line, and the line count matches @main's entry calls."""
  gid, sir_path, d = _ex_first_multi_example_run(
    rysmith, extra_args=["--emit-main"], wanted=2
  )
  if gid is None:
    check(
      "at least one multi-example .sir for the banner rule",
      False,
      "no solving seed found",
    )
    return
  try:
    body = open(sir_path).read()
    lines = _ex_solved_lines(body)
    n_calls = len(
      re.findall(
        r"call @" + re.escape(f"func_{gid}_0") + r"\(",
        body[body.index("fun @main(") :],
      )
    )
    check(
      "banner carries one SOLVED line per example under --emit-main",
      len(lines) >= 2,
      f"{len(lines)} lines: {lines}",
    )
    check(
      "banner line count matches the @main entry calls",
      len(lines) == n_calls,
      f"{len(lines)} lines vs {n_calls} entry calls",
    )
  finally:
    shutil.rmtree(d, ignore_errors=True)


def test_n_examples_descriptor_examples(rysmith):
  """--emit-desc --n-examples 3: the realization's `extra_examples` array
  carries the banner examples beyond the primary, and the primary is the
  top-level `params`/`ret`."""
  for seed in _REPLAY_SEEDS:
    d = tempfile.mkdtemp(prefix="nexd_")
    r = run(
      [
        rysmith,
        "--n-funcs",
        "1",
        "--seed",
        str(seed),
        "--n-params",
        "2",
        "--n-examples",
        "3",
        "--emit-desc",
        "-o",
        d,
      ]
    )
    gid = extract_id(r.stdout) if r.returncode == 0 else None
    ok = False
    if gid:
      for f in os.listdir(d):
        if f.endswith(".json"):
          desc = json.load(open(os.path.join(d, f)))
          rzs = desc.get("realizations") or []
          if not rzs:
            continue
          rz = rzs[0]
          exs = rz.get("extra_examples") or []
          if len(exs) < 2:
            continue
          check(
            "descriptor realization carries >= 2 extra examples",
            len(exs) >= 2,
            f"{len(exs)} extra examples",
          )
          check(
            "primary example is the top-level params/ret",
            rz.get("params") is not None and rz.get("ret") is not None,
            f"top=({rz.get('params')}, {rz.get('ret')})",
          )
          decl = [p["name"] for p in desc.get("params", [])]
          covered = all(all(n in ex.get("params", {}) for n in decl) for ex in exs)
          check("every extra example values all declared params", covered, str(exs))
          ok = True
          break
    shutil.rmtree(d, ignore_errors=True)
    if ok:
      return
  check(
    "descriptor examples array emitted under --n-examples 3",
    False,
    "no solving seed found",
  )


def test_n_examples_requires_default_mode(rysmith):
  """--n-examples > 1 is a usage error under --require-ub and
  --require-nonterm (trap/diverge programs have one certifiable input and
  no output), and a sub-1 value clamps with a warning."""
  for extra in (["--require-ub"], ["--require-nonterm"]):
    d = tempfile.mkdtemp(prefix="nexmode_")
    r = run([rysmith, "--n-examples", "3", "--n-funcs", "1", "-o", d] + extra)
    shutil.rmtree(d, ignore_errors=True)
    check(
      f"--n-examples 3 {' '.join(extra)} exits non-zero",
      r.returncode != 0 and "n-examples" in r.stderr,
      f"rc={r.returncode}, stderr={r.stderr[:200]!r}",
    )
  clamps = False
  ok = False
  # Single-function runs are seed-fragile (see _REPLAY_SEEDS); the clamp
  # warns and the run must still produce a concrete .sir.
  for seed in _REPLAY_SEEDS:
    d = tempfile.mkdtemp(prefix="nexclamp_")
    r = run(
      [rysmith, "--n-examples", "0", "--n-funcs", "1", "--seed", str(seed), "-o", d]
    )
    clamps = "warning: --n-examples clamped" in r.stderr
    ok = r.returncode == 0 and any(f.endswith(".sir") for f in os.listdir(d))
    shutil.rmtree(d, ignore_errors=True)
    if clamps and ok:
      break
  check("--n-examples 0 clamps to 1 and runs", clamps and ok)


def test_n_examples_default_one(rysmith):
  """The knob defaults to 1: with --emit-main the @main calls the entry
  exactly once and the banner carries one SOLVED line."""
  gid, sir_path, d = _first_solved_run(rysmith, ["--emit-main"])
  if gid is None:
    check("at least one concrete .sir for default-one", False, "no solving seed found")
    return
  try:
    body = open(sir_path).read()
    lines = _ex_solved_lines(body)
    check(
      "banner carries exactly one SOLVED line by default", len(lines) == 1, str(lines)
    )
    main_text = body[body.index("fun @main(") :]
    n_calls = len(re.findall(r"call @" + re.escape(f"func_{gid}_0") + r"\(", main_text))
    check(
      "@main carries exactly one entry call by default",
      n_calls == 1,
      f"{n_calls} calls",
    )
  finally:
    shutil.rmtree(d, ignore_errors=True)


# ---------------------------------------------------------------------------
# Horizontal-reduction intrinsics (@reduce_*) — vector-in / scalar-out
# ---------------------------------------------------------------------------

_REDUCE_SEEDS = (5501, 5502, 5503, 5504, 5505, 5506)


def test_reduce_intrinsics_generated(rysmith):
  """The reduction family (`@reduce_add/min/max/and/or/xor`) is the only
  intrinsic shape whose operand is a vector `<N> T` and whose result is
  the scalar `T`. rysmith emits them wherever a matching vector var is in
  scope, folding both integer and FP lanes. Across a batch, several
  distinct reductions must appear as actual call sites (loose thresholds
  keep this robust against RNG drift)."""
  text = _collect_sym_text(rysmith, _REDUCE_SEEDS)
  calls = re.findall(r"call @(reduce_[a-z]+)\(", text)
  kinds = set(calls)
  check(
    f"@reduce_* call sites generated ({len(calls)} calls, kinds={sorted(kinds)})",
    len(calls) >= 3 and len(kinds) >= 2,
    f"calls={len(calls)} kinds={sorted(kinds)}",
  )


def test_reduce_needs_vectors(rysmith):
  """A reduction can only fold a vector operand, so with `--no-vec` (no
  `<N> T` vars are ever generated) not a single `@reduce_*` — declaration
  or call — may appear. This is the invariant direction of the feature
  and is not subject to RNG drift."""
  text = _collect_sym_text(rysmith, _REDUCE_SEEDS, extra_flags=["--no-vec"])
  leaked = re.findall(r"@reduce_[a-z]+", text)
  check(
    "no @reduce_* under --no-vec",
    not leaked,
    f"leaked {len(leaked)}: {sorted(set(leaked))}",
  )


def main():
  if len(sys.argv) != 4:
    print("Usage: python3 -m test.lib.run_rysmith_tests <rysmith> <symiri> <symirc>")
    sys.exit(2)
  rysmith, symiri, symirc = sys.argv[1:4]
  print("=== rysmith generation-id banner ===")
  test_id_banner(rysmith)
  print("=== rysmith --seed determinism ===")
  test_seed_id_determinism(rysmith)
  print("=== rysmith --n-params ===")
  test_n_params(rysmith)
  print("=== rysmith descriptor schema ===")
  test_descriptor_schema(rysmith)
  print("=== descriptor outcome field (return / trap / diverge) ===")
  test_descriptor_outcome_field(rysmith)
  print("=== rysmith SOLVED header replay via symiri ===")
  test_solved_replay(rysmith, symiri)
  print("=== rysmith --emit-main wrapper ===")
  test_rysmith_main(rysmith, symiri)
  print("=== R3: scalar/ptr/vec self-assign elimination ===")
  test_r3_no_self_assign(rysmith)
  print("=== R3+P6: plain-copy share low ===")
  test_r3_no_plain_copy(rysmith)
  print("=== R3: LHS does not appear in RHS ===")
  test_r3_no_lhs_in_rhs(rysmith)
  print("=== R3: baseline assignment volume sanity ===")
  test_r3_baseline_assignment_volume(rysmith)
  print("=== R3: parameter uses survive in complex RHS ===")
  test_r3_pa_param_uses_present(rysmith)
  print("=== R4: default loop-iter actually iterates ===")
  test_r4_default_loops_iterate(rysmith)
  print("=== R5: large-coef share above floor ===")
  test_r5_large_coef_share(rysmith)
  print("=== R6: off-path mul-coef widened ===")
  test_r6_offpath_mul_widened(rysmith)
  print("=== R7: pickSelectVal literal diversity ===")
  test_r7_pickselectval_literal_diversity(rysmith)
  print("=== R8: dyadic FP pool extension ===")
  test_r8_dyadic_fp_pool_extension(rysmith)
  print("=== n-stmts counts only assignments ===")
  test_n_stmts_counts_assignments_only(rysmith)
  print("=== trivial-shape: all-literal RHS ===")
  test_no_all_literal_assignment(rysmith)
  print("=== trivial-shape: single-atom literal RHS ===")
  test_no_single_atom_literal_assignment(rysmith)
  print("=== CFG header dump ===")
  test_cfg_header_dump(rysmith)
  print("=== rysmith --min-atoms validation ===")
  test_min_atoms_negative_validation(rysmith)
  print("=== rysmith --max-atoms validation ===")
  test_max_atoms_less_than_min_validation(rysmith)
  print("=== rysmith --min-atoms 1 --max-atoms 1 ===")
  test_min_max_atoms_exact_one(rysmith)
  print("=== rysmith --min-atoms 2 --max-atoms 2 ===")
  test_min_max_atoms_exact_two(rysmith)
  print("=== rysmith --min-atoms 3 --max-atoms 3 ===")
  test_min_max_atoms_exact_three(rysmith)
  print("=== --large-coef: help/default ===")
  test_large_coef_help_default(rysmith)
  print("=== --large-coef: negative validation ===")
  test_large_coef_negative_validation(rysmith)
  print("=== --large-coef: narrow domain stays SAT ===")
  test_large_coef_narrow_domain_sat(rysmith)
  print("=== --large-coef: custom threshold respected ===")
  test_large_coef_custom_threshold_respected(rysmith)
  print("=== --large-coef: zero accepted ===")
  test_large_coef_zero_accepted(rysmith)
  print("=== --off-path-multiplier: help/default ===")
  test_off_path_multiplier_help_default(rysmith)
  print("=== --off-path-multiplier: negative validation ===")
  test_off_path_multiplier_negative_validation(rysmith)
  print("=== --off-path-multiplier: default 2x density ===")
  test_off_path_density_default(rysmith)
  print("=== --off-path-multiplier: 1.0 uniform ===")
  test_off_path_multiplier_one_uniform(rysmith)
  print("=== --off-path-multiplier: atoms scaled ===")
  test_off_path_atoms_scaled(rysmith)
  print("=== P3: cheap on-path rewrite sym share ===")
  test_cheap_rewrite_sym_share(rysmith)
  print("=== P7: off-path shift atoms ===")
  test_offpath_shift_atoms(rysmith)
  print("=== P7: off-path var-op-var atoms ===")
  test_offpath_var_op_var_atoms(rysmith)
  print("=== P7: off-path FP div/mod atoms ===")
  test_offpath_fp_divmod_atoms(rysmith)
  print("=== P7: off-path select cond literal RHS ===")
  test_offpath_select_cond_nonzero_rhs(rysmith)
  print("=== P7: custom iN widths ===")
  test_custom_int_widths_generated(rysmith)
  print("=== P7: sub-lvalue addr ===")
  test_sublvalue_addr_generated(rysmith)
  print("=== P7: pointer-valued store ===")
  test_pointer_store_generated(rysmith)
  print("=== ptr-arith: in-bounds ptrindex reassignment ===")
  test_ptr_arith_reassign_generated(rysmith)
  print("=== ptr-arith: direct ptr-var arithmetic ===")
  test_ptr_var_arith_generated(rysmith)
  print("=== ptr-arith: --no-ptrarith disables it ===")
  test_no_ptrarith_flag(rysmith)
  print("=== --no-crc32: CLI flag accepted ===")
  test_no_crc32_flag_accepted(rysmith)
  print("=== --no-crc32: no crc32_update in SIR ===")
  test_no_crc32_no_crc32_update_in_sir(rysmith)
  print("=== --no-crc32: emit-main runs correctly ===")
  test_no_crc32_emit_main_runs_correctly(rysmith, symiri)
  print("=== --no-crc32: solved header matches sum ===")
  test_no_crc32_solved_header_matches_sum(rysmith, symiri)
  print("=== --no-crc32: descriptor matches solved header ===")
  test_no_crc32_descriptor_matches_solved_header(rysmith)
  print("=== --require-ub: emitted programs actually trap UB ===")
  test_require_ub_programs_actually_trap(rysmith, symiri)
  print("=== --require-nonterm: emitted programs actually diverge ===")
  test_require_nonterm_programs_actually_diverge(rysmith, symiri)
  print("=== --require-nonterm --validate: in-process bounded-replay check ===")
  test_require_nonterm_validate_flag(rysmith)
  print("=== --require-nonterm --emit-main: compilable diverging whole program ===")
  test_require_nonterm_emit_main(rysmith, symiri, symirc)
  print("=== --require-nonterm: compiled C stays infinite at -O2 (gcc/clang) ===")
  test_require_nonterm_c_stays_infinite(rysmith)
  print("=== --require-nonterm: floating-point state ===")
  test_require_nonterm_covers_fp_state(rysmith, symiri)
  print("=== --require-nonterm: pointer state ===")
  test_require_nonterm_covers_pointer_state(rysmith, symiri)
  print("=== --require-nonterm: --validate over the full type lattice ===")
  test_require_nonterm_validate_full_lattice(rysmith)
  print("=== --emit-main: checksum oracle agrees across seeds ===")
  test_emit_main_checksum_oracle_agrees(rysmith, symiri)
  print("=== --max-lasso-period: multi-lap orbits ===")
  test_max_lasso_period_generates_multi_lap_orbits(rysmith)
  print("=== --max-lasso-period: requires --require-nonterm ===")
  test_max_lasso_period_requires_nonterm(rysmith)
  print("=== --max-lasso-period: period-aware bounded replay ===")
  test_max_lasso_period_validates(rysmith)
  print("=== --require-ub --emit-main: whole programs re-parse (no inf/nan leak) ===")
  test_require_ub_emit_main_reparses(rysmith, symiri)
  print("=== --emit-state: pbb sidecar shape ===")
  test_emit_state_pbb_sidecar(rysmith)
  print("=== --emit-state: ppp granularity + validation ===")
  test_emit_state_ppp_and_validation(rysmith)
  print("=== --require-reducible: reducible CFG generation ===")
  test_require_reducible_all_reducible(rysmith, symirc)
  print("=== descriptor `reducible` field ===")
  test_descriptor_reducible_field(rysmith)
  print("=== --structured-lowering true: goto-free C ===")
  test_structured_lowering_true_goto_free(rysmith)
  print("=== --structured-lowering implies --require-reducible ===")
  test_structured_lowering_implies_reducible(rysmith, symirc)
  print("=== --structured-lowering value validation ===")
  test_structured_lowering_value_validation(rysmith)
  print("=== --structured-lowering: dispatch-free wasm ===")
  test_structured_lowering_wasm(rysmith)
  print("=== --n-examples: multiple input/output examples per .sir ===")
  print("=== --n-examples: banner lines ===")
  test_n_examples_banner_lines(rysmith)
  print("=== --n-examples: distinct inputs ===")
  test_n_examples_banner_distinct(rysmith)
  print("=== --n-examples: every example replays ===")
  test_n_examples_examples_replay(rysmith, symiri)
  print("=== --n-examples: --emit-main multi-call main ===")
  test_n_examples_main_multi_calls(rysmith, symiri)
  print("=== --n-examples: --emit-main banner keeps one line per example ===")
  test_n_examples_main_banner_lines(rysmith)
  print("=== --n-examples: descriptor examples array ===")
  test_n_examples_descriptor_examples(rysmith)
  print("=== --n-examples: mode validation ===")
  test_n_examples_requires_default_mode(rysmith)
  print("=== --n-examples: default one ===")
  test_n_examples_default_one(rysmith)
  print("=== @reduce_*: reduction intrinsics generated ===")
  test_reduce_intrinsics_generated(rysmith)
  print("=== @reduce_*: suppressed under --no-vec ===")
  test_reduce_needs_vectors(rysmith)
  print("=== --target python ===")
  test_target_python_emits_py(rysmith)
  print("=== python --vec-lowering: explicit / random / vecext ===")
  test_python_vec_lowering_flag(rysmith)
  test_python_vec_lowering_random(rysmith)
  test_python_vec_lowering_vecext_rejected(rysmith)

  passed = sum(1 for _, ok, _ in results if ok)
  total = len(results)
  print(f"\nSummary (rysmith_tests): {passed}/{total} passed.\n")
  sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
  main()
