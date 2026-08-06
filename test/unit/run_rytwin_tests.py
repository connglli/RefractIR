#!/usr/bin/env python3
"""Unit tests for rytwin (equivalence-preserving RefractIR transformer).

Scaffold-level coverage: the CLI loads p1 + descriptor + state-profile
sidecar, runs the (currently empty) Pass pipeline, and emits an equivalent
p2. The headline invariant checked here is p1(i) == p2(i): symiri must
return the same value for the emitted program as for the input.

Usage: python3 -m test.unit.run_rytwin_tests <rytwin> <rysmith> <symiri>
"""

import os
import re
import subprocess
import sys
import tempfile

GREEN = "\033[92m"
RED = "\033[91m"
GRAY = "\033[90m"
NC = "\033[0m"

results = []


def run(cmd, **kw):
  print(f"  {GRAY}[RUN>]{NC} " + " ".join(str(c) for c in cmd))
  return subprocess.run(cmd, capture_output=True, text=True, timeout=120, **kw)


def check(name, ok, detail=""):
  results.append((name, ok, detail))
  color = GREEN if ok else RED
  tag = "PASS" if ok else "FAIL"
  print(f"  [{color}{tag}{NC}] {name}" + (f" — {detail}" if detail and not ok else ""))


def gen_p1(rysmith, d, seed="5", nparams="2"):
  """Generate one rysmith leaf with a state sidecar + descriptor. Returns
  (p1_path, desc_path, entry_func, [param_values]) or None on failure."""
  r = run(
    [
      rysmith,
      "--emit-state",
      "pbb",
      "--emit-desc",
      "--n-funcs",
      "1",
      "--seed",
      seed,
      "--n-params",
      nparams,
      "-o",
      d,
    ]
  )
  if r.returncode != 0:
    return None
  sirs = [f for f in os.listdir(d) if f.endswith(".sir") and "_sym" not in f]
  descs = [
    f for f in os.listdir(d) if f.endswith(".json") and not f.endswith(".state.json")
  ]
  if not sirs or not descs:
    return None
  p1 = os.path.join(d, sorted(sirs)[0])
  desc = os.path.join(d, sorted(descs)[0])
  src = open(p1).read()
  fm = re.search(r"fun\s+(@\w+)\s*\(([^)]*)\)", src)
  entry = fm.group(1)
  pnames = [p.split(":")[0].strip() for p in fm.group(2).split(",") if p.strip()]
  hdr = re.search(r"//\s*SOLVED:\s*(.*)", src)
  kv = {}
  if hdr:
    for part in hdr.group(1).split(","):
      if "=" in part:
        k, v = part.strip().split("=", 1)
        kv[k.strip()] = v.strip()
  args = [kv[p] for p in pnames if p in kv]
  return p1, desc, entry, args


def gen_pool(rysmith, d, n="12", seed="202", emit_state=False, extra=None):
  """Generate a pool of pointer-free leaves (guard/vector/aggregate feature
  assertions are cleanest without memory ops; gen_ptr_pool covers those).
  Returns the sorted list of concrete .sir paths, or None on failure."""
  cmd = [rysmith, "--emit-desc", "--n-funcs", n, "--seed", seed]
  cmd += ["--n-params", "2", "--n-stmts", "5", "--max-ptr-depth", "0"]
  if emit_state:
    cmd += ["--emit-state", "pbb"]
  cmd += (extra or []) + ["-o", d]
  r = run(cmd)
  if r.returncode != 0:
    return None
  sirs = [f for f in os.listdir(d) if f.endswith(".sir") and "_sym" not in f]
  return [os.path.join(d, s) for s in sorted(sirs)]


def first_twinned(rytwin, sirs, extra=None):
  """Run rytwin --p-twin 1.0 over the pool; return (p1, p2, stdout+stderr) of
  the first program that grafts, or None if none does."""
  for p1 in sirs:
    p2 = p1[:-4] + ".p2.sir"
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3"] + (extra or []) + ["-o", p2])
    if r.returncode == 0 and os.path.exists(p2):
      return p1, p2, r.stdout + r.stderr
  return None


def symiri_result(symiri, path, entry, args):
  r = run([symiri, "--main", entry, path, "--"] + args)
  out = r.stdout + r.stderr
  m = re.search(r"Result:\s*(\S+)", out)
  return (
    r.returncode,
    m.group(1) if m else None,
    out.strip().splitlines()[-1:] if out else [],
  )


def test_no_twins_errors(rytwin, rysmith):
  """When no twin is grafted (forced here via --p-twin 0), rytwin reports an
  error and writes no output rather than emitting an unchanged copy of p1."""
  with tempfile.TemporaryDirectory() as d:
    g = gen_p1(rysmith, d)
    if not g:
      check("no-twins setup (rysmith gen)", False, "generation failed")
      return
    p1, _, _, _ = g
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "0", "-o", p2])
    check("rytwin errors when no twin grafted", r.returncode != 0, f"rc={r.returncode}")
    check("no output written when no twin", not os.path.exists(p2), "p2 exists")
    check(
      "error explains the missing twin",
      "no twin" in r.stderr or "nothing written" in r.stderr,
      r.stderr[:160],
    )


def test_no_sidecar_needed(rytwin, rysmith):
  """[Stage 1] rytwin profiles p1 in-process: a p1 generated WITHOUT
  --emit-state (no .state.json on disk) still twins successfully."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("no-sidecar setup (rysmith gen)", False, "generation failed")
      return
    assert not any(f.endswith(".state.json") for f in os.listdir(d))
    got = first_twinned(rytwin, sirs)
    check("rytwin twins without a .state.json sidecar", got is not None, "")


def test_corrupt_sidecar_falls_back(rytwin, rysmith):
  """[Stage 1] A stale/corrupt .state.json next to p1 does not kill the run:
  rytwin warns and falls back to in-process profiling."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=True)
    if not sirs:
      check("corrupt-sidecar setup (rysmith gen)", False, "generation failed")
      return
    states = [f for f in os.listdir(d) if f.endswith(".state.json")]
    for f in states:
      open(os.path.join(d, f), "w").write("garbage{not json")
    got = first_twinned(rytwin, sirs)
    check("corrupt sidecar tolerated (twinning still works)", got is not None, "")


def strip_solved_header(path):
  src = open(path).read()
  open(path, "w").write(
    "\n".join(ln for ln in src.splitlines() if not ln.startswith("// SOLVED:")) + "\n"
  )


def test_sidecar_preferred(rytwin, rysmith):
  """[Stage 1] When a valid .state.json is present, rytwin loads it instead
  of interpreting. Discriminator: with the descriptor and SOLVED header
  removed, in-process profiling would run at all-zero args and trap on the
  interest requires — only the sidecar path can succeed."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=True)
    if not sirs:
      check("sidecar-preferred setup (rysmith gen)", False, "generation failed")
      return
    for f in os.listdir(d):
      if f.endswith(".json") and not f.endswith(".state.json"):
        os.remove(os.path.join(d, f))
    for p1 in sirs:
      strip_solved_header(p1)
    rescued = 0
    for p1 in sirs:
      state = p1[:-4] + ".state.json"
      if not os.path.exists(state):
        continue
      # Control: without the sidecar this program must fail (in-process
      # profiling at zero args traps); skip programs that pass at zeros.
      hidden = state + ".hidden"
      os.rename(state, hidden)
      r_ctl = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p1 + ".ctl.sir"])
      os.rename(hidden, state)
      if r_ctl.returncode == 0:
        continue
      r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p1 + ".p2.sir"])
      if r.returncode == 0:
        rescued += 1
        break
    check("valid sidecar loaded in preference to interpreting", rescued > 0, "")


GUARD_FN_RE = re.compile(r"fun\s+(@__twg_\w+)\s*\(([^)]*)\)")
GUARD_CALL_RE = re.compile(r"br\s+call\s+@__twg_\w+\s*\([^)]*\)\s*!=\s*0")


def guard_fun_bodies(src):
  """Return {guard_fn_name: body_text} for every @__twg_ function in src."""
  out = {}
  for m in GUARD_FN_RE.finditer(src):
    start = src.index("{", m.end())
    depth, i = 1, start + 1
    while depth and i < len(src):
      depth += {"{": 1, "}": -1}.get(src[i], 0)
      i += 1
    out[m.group(1)] = src[start:i]
  return out


def test_guard_is_function(rytwin, rysmith):
  """[Stage 2] The guard lives in a dedicated `fun @__twg_... : i1`; the
  twinned block's terminator is `br call @__twg_...(...) != 0, ...` and the
  old in-block scratch locals (%__twg/%__twa/...) are gone."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("guard-fn setup (rysmith gen)", False, "generation failed")
      return
    got = first_twinned(rytwin, sirs)
    if not got:
      check("guard-fn setup (twinnable program)", False, "no twin grafted")
      return
    src = open(got[1]).read()
    check("p2 declares a fun @__twg_", "fun @__twg_" in src, "")
    check("twinned block branches on call @__twg_", bool(GUARD_CALL_RE.search(src)), "")
    check(
      "no in-block guard scratch locals remain",
      "%__twg" not in src and "%__twa" not in src and "%__twfa" not in src,
      "",
    )


def test_guard_covers_full_state(rytwin):
  """[Stage 2] The guard consumes the ENTIRE live-in state, not just the
  block's read set: %b is never read by the twinned block, yet its value
  (1234567) must appear in the guard function."""
  fixture = """// SOLVED: %pa0=7
fun @guardfix(%pa0: i32) : i32 {
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
"""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "guardfix.sir")
    open(p1, "w").write(fixture)
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "--validate", "-o", p2])
    check("fixture twinned", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    bodies = guard_fun_bodies(open(p2).read())
    check(
      "guard consumes the unread variable %b",
      any("1234567" in b for b in bodies.values()),
      f"guards={list(bodies)}",
    )


def test_guard_unique_names(rytwin, rysmith):
  """[Stage 2] One guard function per twin site, names unique; the rewritten
  program re-analyzes (rytwin exits 0)."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("guard-names setup (rysmith gen)", False, "generation failed")
      return
    found = False
    for p1 in sirs:
      p2 = p1[:-4] + ".p2.sir"
      r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
      m = re.search(r"\((\d+) twin", r.stdout)
      if r.returncode != 0 or not m or int(m.group(1)) < 2:
        continue
      found = True
      n = int(m.group(1))
      names = GUARD_FN_RE.findall(open(p2).read())
      check(
        "guard fn per site, all names distinct",
        len(names) == n and len({nm for nm, _ in names}) == n,
        f"twins={n} fns={names}",
      )
      break
    if not found:
      check("guard-names setup (>=2-twin program)", False, "none found")


def test_guard_aggregates_and_vectors(rytwin, rysmith, symiri):
  """[Stage 2] Aggregate state roots cross into the guard by address
  (`ptr [N] T` / `ptr @S` params, `addr %root` args, ptrindex/ptrfield+load
  navigation inside); vector roots cross per-lane (`%v[i]` args). The twins
  stay equivalent on the profiled input."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("agg/vec guard setup (rysmith gen)", False, "generation failed")
      return
    saw_ptr_param = saw_addr_arg = saw_lane_arg = False
    bad = 0
    for p1 in sirs:
      p2 = p1[:-4] + ".p2.sir"
      r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
      if r.returncode != 0:
        continue
      src = open(p2).read()
      for _, params in GUARD_FN_RE.findall(src):
        if "ptr [" in params or "ptr @" in params:
          saw_ptr_param = True
      for m in re.finditer(r"br\s+call\s+@__twg_\w+\s*\(([^)]*)\)", src):
        if "addr %" in m.group(1):
          saw_addr_arg = True
        if re.search(r"%\w+\[\d+\]", m.group(1)):
          saw_lane_arg = True
      fn, pnames, _, iargs = parse_entry(open(p1).read())
      if (
        symiri_result(symiri, p1, fn, iargs)[1:]
        != symiri_result(symiri, p2, fn, iargs)[1:]
      ):
        bad += 1
    check("some guard takes an aggregate by pointer", saw_ptr_param, "")
    check("some caller passes addr %root", saw_addr_arg, "")
    check("some caller passes vector lanes %v[i]", saw_lane_arg, "")
    check("agg/vec twins preserve the profiled result", bad == 0, f"{bad} mismatch(es)")


def test_guard_compiles_c_and_wasm(rytwin, rysmith):
  """[Stage 2] Guard functions survive both backends: p1 and p2 compile via
  --target c --emit-main, the C binaries agree, and --target wasm emits."""
  import shutil

  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False, extra=["--emit-main"])
    if not sirs:
      check("backend setup (rysmith gen)", False, "generation failed")
      return
    got = first_twinned(rytwin, sirs, extra=["--target", "c", "--emit-main"])
    if not got:
      check("backend setup (twinnable program)", False, "no twin grafted")
      return
    p1, p2, _ = got
    check("compiled p2 uses guard functions", "fun @__twg_" in open(p2).read(), "")
    p2c = p2[:-4] + ".c"
    check("p2.c emitted", os.path.exists(p2c), "")
    r = run(
      [
        rytwin,
        p1,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--target",
        "wasm",
        "--emit-main",
        "-o",
        p2[:-4] + ".w.sir",
      ]
    )
    check(
      "p2 compiles to wasm",
      r.returncode == 0 and os.path.exists(p2[:-4] + ".w.wat"),
      r.stderr[:160],
    )
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc or not os.path.exists(p2c):
      return
    # p2's @main asserts the profiled checksum via @check_chksum, so a clean
    # run of the compiled binary proves the twinned program still computes
    # p1's result through the C backend.
    exe = os.path.join(d, "p2.bin")
    rc = run([cc, "-O1", "-o", exe, p2c, "-lm"])
    check("p2.c compiles", rc.returncode == 0, rc.stderr[:200])
    if rc.returncode == 0:
      rr = run([exe])
      check(
        "p2 binary runs clean (checksum assert passes)",
        rr.returncode == 0,
        f"rc={rr.returncode} out={rr.stdout[:120]}",
      )


# --- bijection guard (--twin-guard bijection) ---------------------------
#
# The bijection guard replaces the exact per-leaf `operand == const` with a
# bijective mix of each integer leaf (murmur-style `x ^= x>>>s; x *= odd`)
# compared against the pre-mixed constant. A bijection collides with
# nothing, so discrimination — and hence equivalence — is identical to the
# exact guard; only the surface changes (opaque `*`/`>>>` arithmetic, and
# the raw live-in constant no longer appears verbatim).

BIJ_FIXTURE = """// SOLVED: %pa0=7
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
"""


def test_bijection_guard_option(rytwin, rysmith):
  """--twin-guard bijection is accepted and still grafts; an unknown value
  is rejected with a non-zero exit (mirrors the exact default)."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d)
    if not sirs:
      check("bijection-option setup (rysmith gen)", False, "generation failed")
      return
    got = first_twinned(rytwin, sirs, extra=["--twin-guard", "bijection"])
    check("rytwin accepts --twin-guard bijection", got is not None, "")
    r = run([rytwin, sirs[0], "--twin-guard", "bogus", "-o", sirs[0] + ".x.sir"])
    check("invalid --twin-guard rejected", r.returncode != 0, f"rc={r.returncode}")


def test_bijection_guard_obfuscated(rytwin):
  """The bijection guard mixes each integer leaf: its body carries `>>>`
  (logical-shift) mixing ops and no longer shows the raw live-in constant
  1234567, whereas the exact guard shows 1234567 verbatim and never shifts.
  (`&` is not a discriminator — the exact guard AND-folds with it too.)"""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "sigfix.sir")
    open(p1, "w").write(BIJ_FIXTURE)

    def guard_body(extra):
      p2 = os.path.join(d, "p2.sir")
      r = run(
        [rytwin, p1, "--p-twin", "1.0", "--seed", "3", "--validate"]
        + extra
        + ["-o", p2]
      )
      body = (
        "".join(guard_fun_bodies(open(p2).read()).values()) if r.returncode == 0 else ""
      )
      return r, body

    er, ebody = guard_body([])
    sr, sbody = guard_body(["--twin-guard", "bijection"])
    check("exact fixture twinned", er.returncode == 0, er.stderr[:160])
    check("bijection fixture twinned + validated", sr.returncode == 0, sr.stderr[:160])
    check("exact guard shows the raw live-in constant", "1234567" in ebody, "")
    check("exact guard uses no shift-mixing ops", ">>>" not in ebody, "")
    check("bijection guard hides the raw constant", "1234567" not in sbody, "")
    check("bijection guard mixes with >>>", ">>>" in sbody, "")


def test_bijection_default_is_exact(rytwin):
  """Omitting --twin-guard keeps the exact guard: no mixing ops appear, so
  the default behaviour is unchanged."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "sigfix.sir")
    open(p1, "w").write(BIJ_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
    check("default fixture twinned", r.returncode == 0, r.stderr[:160])
    if r.returncode != 0:
      return
    body = "".join(guard_fun_bodies(open(p2).read()).values())
    check("default guard is exact (no shift-mixing ops)", ">>>" not in body, "")


def test_bijection_guard_equivalence(rytwin, rysmith, symiri):
  """The zero-collision property, empirically: with the bijection guard,
  p1 === p2 on the profiled input AND on random other inputs (where the
  guard must not fire)."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, n="12", seed="202")
    if not sirs:
      check("bijection-equivalence setup (rysmith gen)", False, "generation failed")
      return
    equivalence_over_pool(
      rytwin, symiri, d, sirs, "bijection", extra=["--twin-guard", "bijection"]
    )


def test_bijection_guard_compiles(rytwin, rysmith):
  """The mixer ops (lshr/and/xor) survive the C backend: a bijection-guard
  p2 compiles and its @main checksum-assert binary runs clean."""
  import shutil

  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False, extra=["--emit-main"])
    if not sirs:
      check("bijection-backend setup (rysmith gen)", False, "generation failed")
      return
    got = first_twinned(
      rytwin, sirs, extra=["--twin-guard", "bijection", "--target", "c", "--emit-main"]
    )
    if not got:
      check("bijection-backend setup (twinnable program)", False, "no twin grafted")
      return
    _, p2, _ = got
    body = "".join(guard_fun_bodies(open(p2).read()).values())
    check("bijection guard actually mixed", ">>>" in body, "")
    p2c = p2[:-4] + ".c"
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc or not os.path.exists(p2c):
      check("bijection p2.c emitted", os.path.exists(p2c), "")
      return
    exe = os.path.join(d, "p2.bin")
    rc = run([cc, "-O1", "-o", exe, p2c, "-lm"])
    check("bijection p2.c compiles", rc.returncode == 0, rc.stderr[:200])
    if rc.returncode == 0:
      rr = run([exe])
      check("bijection p2 binary runs clean", rr.returncode == 0, f"rc={rr.returncode}")


def test_bijection_guard_small_width(rytwin):
  """Edge: a narrow (i8) live-in root still validates under the bijection
  guard — the finalizer clamps its shifts to the leaf width and stays a
  bijection on every width."""
  fixture = """// SOLVED: %pa0=5
fun @narrow(%pa0: i8) : i8 {
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
"""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "narrow.sir")
    open(p1, "w").write(fixture)
    p2 = os.path.join(d, "p2.sir")
    r = run(
      [
        rytwin,
        p1,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--twin-guard",
        "bijection",
        "--validate",
        "-o",
        p2,
      ]
    )
    check("narrow (i8) bijection twin validated", r.returncode == 0, r.stderr[:200])
    if r.returncode == 0:
      body = "".join(guard_fun_bodies(open(p2).read()).values())
      check("narrow guard still mixes on i8", ">>>" in body, body[:120])


def test_dlp_bijection_structure(rytwin):
  """Checks that the bijection guard generates the DLP (Discrete Logarithm Problem)
  structure (such as P primes and unrolled square-and-multiply loops) instead of the
  older Murmur-like round structure, and that the multi-size DLP tier table picks
  width-appropriate working types."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "sigfix.sir")
    open(p1, "w").write(BIJ_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run(
      [
        rytwin,
        p1,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--twin-guard",
        "bijection",
        "-o",
        p2,
      ]
    )
    check("dlp bijection twinned successfully", r.returncode == 0, r.stderr[:200])
    if r.returncode == 0:
      body = "".join(guard_fun_bodies(open(p2).read()).values())
      check("dlp bijection contains modular base generator", "%__P_0" in body, body[:200])
      check("dlp bijection utilizes modular multiplication loops", "*" in body, body[:200])
      check("dlp bijection contains i128 castings", "as i128" in body, body[:200])


def test_dlp_bijection_tier_diversity(rytwin):
  """[multi-size DLP] The tier table is actually tiered: an i8 leaf's guard
  uses small working types (i16/i32 casts) at least some of the time — the
  small tiers are reachable — while the (injectivity-preserving) range gate
  and a small prime still appear."""
  fixture = """// SOLVED: %pa0=5
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
"""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "tiers.sir")
    open(p1, "w").write(fixture)
    small = 0
    bodies = []
    for seed in range(1, 21):
      p2 = os.path.join(d, f"p2_{seed}.sir")
      r = run(
        [
          rytwin,
          p1,
          "--p-twin",
          "1.0",
          "--seed",
          str(seed),
          "--twin-guard",
          "bijection",
          "-o",
          p2,
        ]
      )
      if r.returncode != 0:
        continue
      body = "".join(guard_fun_bodies(open(p2).read()).values())
      bodies.append(body)
      if ("as i16" in body or "as i32" in body) and ("%__P_" in body):
        small += 1
    check("dlp tier fixture twinned at least once", len(bodies) > 0, "")
    check(
      "some i8 guard uses a small-tier working type (i16/i32)",
      small > 0,
      f"{small}/{len(bodies)} runs showed a small type",
    )
    if bodies:
      check("small-tier guard still carries a range gate + prime", "%__P64_" in bodies[-1], "")


# --- region twins (--twin-scope region) ---------------------------------
#
# The region scope generalizes the twin unit from one block to the maximal
# dominance region rooted at the entry: the guard fires on the entry state,
# and the twin reproduces the region's net effect and jumps straight to the
# region exit, skipping every block (and every loop iteration) in between.
# Sound by determinism — the full entry state fixes the whole continuation.

# A diamond so ^x has a predecessor outside ^e's region (^entry), making it
# the region exit rather than another dominated block. On %p0!=0 the trace
# is ^entry -> ^e -> ^b1 -> ^x, so the region rooted at ^e covers {^e,^b1}.
SEQ_FIXTURE = """// SOLVED: %p0=3
fun @seqreg(%p0: i32) : i32 {
  let mut %a: i32 = 1;
  let mut %b: i32 = 2;
  ^entry:
    br %p0 != 0, ^e, ^x;
  ^e:
    %a = %a + %p0;
    br ^b1;
  ^b1:
    %a = %a + %b;
    br ^x;
  ^x:
    ret %a;
}
"""

# A counting loop; the whole loop is dominated by ^head and everything runs
# to ^done (ret), so the region collapses all iterations into one twin that
# jumps to ^done.
LOOP_FIXTURE = """// SOLVED: %n=3
fun @loopreg(%n: i32) : i32 {
  let mut %i: i32 = 0;
  let mut %s: i32 = 0;
  ^entry:
    br ^head;
  ^head:
    br %i < %n, ^body, ^done;
  ^body:
    %s = %s + %i;
    %i = %i + 1;
    br ^head;
  ^done:
    ret %s;
}
"""


def test_scope_flag(rytwin):
  """--twin-scope region is accepted; an unknown value exits non-zero."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "seqreg.sir")
    open(p1, "w").write(SEQ_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run(
      [rytwin, p1, "--p-twin", "1.0", "--seed", "3", "--twin-scope", "region", "-o", p2]
    )
    check("rytwin accepts --twin-scope region", r.returncode == 0, r.stderr[:160])
    rb = run([rytwin, p1, "--twin-scope", "bogus", "-o", p2 + ".x"])
    check("invalid --twin-scope rejected", rb.returncode != 0, f"rc={rb.returncode}")


def test_scope_region_sequence(rytwin, symiri):
  """Region scope collapses the ^e->^b1 chain: the ^e twin branches straight
  to ^x, ^b1 stays put (for the orig path), and p1 === p2."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "seqreg.sir")
    open(p1, "w").write(SEQ_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run(
      [
        rytwin,
        p1,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--twin-scope",
        "region",
        "--validate",
        "-o",
        p2,
      ]
    )
    check("sequence region twinned + validated", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    src = open(p2).read()
    bodies = twin_block_bodies(src)
    check(
      "a twin jumps straight to ^x (skips ^b1)",
      any("br ^x" in b for b in bodies),
      str(bodies),
    )
    check("skipped block ^b1 is preserved for the orig path", "^b1:" in src, "")
    check(
      "sequence p1 === p2",
      symiri_result(symiri, p1, "@seqreg", ["3"])[1:]
      == symiri_result(symiri, p2, "@seqreg", ["3"])[1:],
      "",
    )


def test_scope_region_loop(rytwin, symiri):
  """Region scope collapses an entire loop: a twin branches straight to
  ^done, the loop body ^body is preserved, the twin actually executes (so the
  loop was skipped on the profiled run), and p1 === p2."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "loopreg.sir")
    open(p1, "w").write(LOOP_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run(
      [
        rytwin,
        p1,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--twin-scope",
        "region",
        "--validate",
        "-o",
        p2,
      ]
    )
    check("loop region twinned + validated", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    src = open(p2).read()
    bodies = twin_block_bodies(src)
    check(
      "a twin jumps straight to ^done (loop collapsed)",
      any("br ^done" in b for b in bodies),
      str(bodies),
    )
    check("loop body ^body is preserved for the orig path", "^body:" in src, "")
    check(
      "loop twin executes on profiled run",
      "__twin" in dump_trace(symiri, p2, "@loopreg", ["3"]),
      "",
    )
    check(
      "loop p1 === p2",
      symiri_result(symiri, p1, "@loopreg", ["3"])[1:]
      == symiri_result(symiri, p2, "@loopreg", ["3"])[1:],
      "",
    )


def test_scope_default_is_block(rytwin):
  """Without --twin-scope the default is single-block: the ^e twin branches
  to ^e's immediate successor ^b1, never straight to ^x."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "seqreg.sir")
    open(p1, "w").write(SEQ_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
    check("default (block) fixture twinned", r.returncode == 0, r.stderr[:160])
    if r.returncode != 0:
      return
    bodies = twin_block_bodies(open(p2).read())
    check(
      "no twin collapses ^e->^x under block scope",
      not any("br ^x" in b for b in bodies),
      str(bodies),
    )


def test_scope_region_equivalence(rytwin, rysmith, symiri):
  """Region scope preserves p1 === p2 on the profiled input AND other inputs
  across a random pool — the guard still fires only on the entry state."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, n="12", seed="202")
    if not sirs:
      check("region-equivalence setup (rysmith gen)", False, "generation failed")
      return
    equivalence_over_pool(
      rytwin, symiri, d, sirs, "region", extra=["--twin-scope", "region"]
    )


def test_scope_region_with_bijection(rytwin, symiri):
  """Region scope composes with the bijection guard: the loop collapses and
  the guard is bijection-mixed, and p1 === p2."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "loopreg.sir")
    open(p1, "w").write(LOOP_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run(
      [
        rytwin,
        p1,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--twin-scope",
        "region",
        "--twin-guard",
        "bijection",
        "--validate",
        "-o",
        p2,
      ]
    )
    check("region + bijection twinned + validated", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    src = open(p2).read()
    gbody = "".join(guard_fun_bodies(src).values())
    tbodies = twin_block_bodies(src)
    check(
      "region collapses under bijection guard",
      any("br ^done" in b for b in tbodies),
      "",
    )
    check("guard is bijection-mixed", ">>>" in gbody, "")


# --- interesting-region selection (--twin-select) -----------------------
#
# `interesting` scores every eligible region (loop iterations collapsed
# dominate, then size / state-diff / fan-in) and grafts the highest-scoring
# non-overlapping ones. Two sequential loops under one dominating entry: the
# entry's whole-function region subsumes both and outscores every inner
# candidate, so it wins the overlap.
TWO_LOOP_FIXTURE = """// SOLVED: %p=1
fun @twoloop(%p: i32) : i32 {
  let mut %i: i32 = 0;
  let mut %j: i32 = 0;
  let mut %s: i32 = 0;
  ^entry:
    br %p != 0, ^a_head, ^mid;
  ^a_head:
    br %i < 4, ^a_body, ^mid;
  ^a_body:
    %s = %s + %i;
    %i = %i + 1;
    br ^a_head;
  ^mid:
    br ^b_head;
  ^b_head:
    br %j < 2, ^b_body, ^done;
  ^b_body:
    %s = %s + %j;
    %j = %j + 1;
    br ^b_head;
  ^done:
    ret %s;
}
"""


def test_select_flag(rytwin):
  """--twin-select interesting is accepted; an unknown value exits non-zero."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "twoloop.sir")
    open(p1, "w").write(TWO_LOOP_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run(
      [
        rytwin,
        p1,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--twin-scope",
        "region",
        "--twin-select",
        "interesting",
        "-o",
        p2,
      ]
    )
    check("rytwin accepts --twin-select interesting", r.returncode == 0, r.stderr[:160])
    rb = run([rytwin, p1, "--twin-select", "bogus", "-o", p2 + ".x"])
    check("invalid --twin-select rejected", rb.returncode != 0, f"rc={rb.returncode}")


def test_select_interesting_wins_overlap(rytwin, symiri):
  """The highest-scored region wins the overlap: the entry's whole-function
  region (both loops) is grafted and jumps straight to ^done, the losing
  overlapping candidates are dropped, and p1 === p2."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "twoloop.sir")
    open(p1, "w").write(TWO_LOOP_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run(
      [
        rytwin,
        p1,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--twin-scope",
        "region",
        "--twin-select",
        "interesting",
        "--verbose",
        "--validate",
        "-o",
        p2,
      ]
    )
    check(
      "interesting selection twinned + validated", r.returncode == 0, r.stderr[:200]
    )
    if r.returncode != 0:
      return
    log = r.stdout + r.stderr
    check(
      "losing candidates report the overlap",
      "overlaps a selected region" in log,
      log[-300:],
    )
    bodies = twin_block_bodies(open(p2).read())
    check(
      "winning twin collapses both loops to ^done",
      any("br ^done" in b for b in bodies),
      str(bodies),
    )
    check(
      "interesting p1 === p2",
      symiri_result(symiri, p1, "@twoloop", ["1"])[1:]
      == symiri_result(symiri, p2, "@twoloop", ["1"])[1:],
      "",
    )


def test_select_interesting_equivalence(rytwin, rysmith, symiri):
  """Interesting selection preserves p1 === p2 on the profiled input AND
  other inputs across a random pool."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, n="12", seed="202")
    if not sirs:
      check("interesting-equivalence setup (rysmith gen)", False, "generation failed")
      return
    equivalence_over_pool(
      rytwin,
      symiri,
      d,
      sirs,
      "interesting",
      extra=["--twin-scope", "region", "--twin-select", "interesting"],
    )


def test_select_softmax_probability(rytwin):
  """The per-region twin probability rises with interestingness: at a low
  --p-twin the whole-function region (highest score) gets a strictly higher
  twin probability than a plain one-block region."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "twoloop.sir")
    open(p1, "w").write(TWO_LOOP_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    # The probability is deterministic given score/p-twin; the seed only
    # drives the draw. Scan seeds for a run where both regions declined (so
    # both probabilities are printed) and compare them.
    entry_p = body_p = None
    for s in range(1, 60):
      r = run(
        [
          rytwin,
          p1,
          "--p-twin",
          "0.15",
          "--seed",
          str(s),
          "--twin-scope",
          "region",
          "--twin-select",
          "interesting",
          "--verbose",
          "-o",
          p2,
        ]
      )
      log = r.stdout + r.stderr
      me = re.search(r"\^entry: skipped \(twin p=([\d.]+)\)", log)
      mb = re.search(r"\^a_body: skipped \(twin p=([\d.]+)\)", log)
      if me and mb:
        entry_p, body_p = float(me.group(1)), float(mb.group(1))
        break
    check("captured per-region twin probabilities", entry_p is not None, "")
    if entry_p is not None:
      check(
        "harder region gets a higher twin probability",
        entry_p > body_p,
        f"entry={entry_p} body={body_p}",
      )


def twin_block_bodies(src):
  """Return the instruction text of every ^..__twin block in src."""
  bodies = []
  cur = None
  for line in src.splitlines():
    stripped = line.strip()
    if stripped.startswith("^") and stripped.endswith(":"):
      if cur is not None:
        bodies.append("\n".join(cur))
        cur = None
      if stripped.rstrip(":").endswith("__twin"):
        cur = []
      continue
    if cur is not None:
      if stripped.startswith("}"):
        bodies.append("\n".join(cur))
        cur = None
      else:
        cur.append(stripped)
  if cur is not None:
    bodies.append("\n".join(cur))
  return bodies


def has_computed_rhs(body):
  """True if some assignment's RHS references a local — i.e. the twin is a
  computation, not a constant reconstruction. `addr`/`null` RHSs are the
  positional pointer reconstruction both twin kinds emit; they don't count."""
  for line in body.splitlines():
    if " = " in line and not line.startswith("require") and not line.startswith("br"):
      rhs = line.split(" = ", 1)[1]
      if rhs.startswith("addr ") or rhs.startswith("null"):
        continue
      if "%" in rhs:
        return True
  return False


def test_twin_is_generated(rytwin, rysmith):
  """[Stage 3] By default twin blocks are
  solver-generated computations, not bare constant reconstructions: some
  twin RHS references a variable."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("twin-gen setup (rysmith gen)", False, "generation failed")
      return
    computed = total = 0
    for p1 in sirs:
      p2 = p1[:-4] + ".p2.sir"
      r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
      if r.returncode != 0:
        continue
      for b in twin_block_bodies(open(p2).read()):
        total += 1
        if has_computed_rhs(b):
          computed += 1
    check(
      "some twin blocks are generated computations",
      computed > 0,
      f"{computed}/{total} twin blocks computed",
    )


def test_twin_fully_concrete(rytwin, rysmith):
  """[Stage 3] Generated twins are concretized before grafting: no %?
  symbol survives into p2."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("twin-concrete setup (rysmith gen)", False, "generation failed")
      return
    got = first_twinned(rytwin, sirs, extra=None)
    if not got:
      check("twin-concrete setup (twinnable program)", False, "no twin grafted")
      return
    check("no %? symbol left in p2", "%?" not in open(got[1]).read(), "")


def test_twin_gen_equivalence(rytwin, rysmith, symiri):
  """[Stage 3] The full equivalence sweep holds with generated twins."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, seed="303", emit_state=False)
    if sirs is None:
      check("twin-gen equivalence setup", False, "generation failed")
      return
    equivalence_over_pool(rytwin, symiri, d, sirs, "twin-gen", extra=None)


def test_twin_gen_fallback(rytwin, rysmith):
  """[Stage 3] --no-twin-smith twins via constant reconstruction, and
  a generation budget of zero falls back instead of crashing."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("twin-gen fallback setup", False, "generation failed")
      return
    got = first_twinned(rytwin, sirs, extra=["--no-twin-smith"])
    check("--no-twin-smith twins via constants", got is not None, "")
    if got:
      bodies = twin_block_bodies(open(got[1]).read())
      check(
        "constant twins have no computed RHS",
        bodies and not any(has_computed_rhs(b) for b in bodies),
        "",
      )
    got = first_twinned(rytwin, sirs, extra=["--twin-retries", "0"])
    check("zero retries falls back cleanly", got is not None, "")


def test_twin_requires_stripped(rytwin, rysmith):
  """[Stage 3] The equality requires that pin the generated twin to s' are
  solver-side scaffolding; they are stripped from the graft. UB-safety
  requires may remain and must survive --keep-require compilation."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False, extra=["--emit-main"])
    if not sirs:
      check("requires-stripped setup", False, "generation failed")
      return
    got = first_twinned(
      rytwin,
      sirs,
      extra=["--keep-require", "--target", "c", "--emit-main"],
    )
    if not got:
      check("requires-stripped setup (twinnable program)", False, "no twin")
      return
    bodies = twin_block_bodies(open(got[1]).read())
    eq_requires = [
      ln
      for b in bodies
      for ln in b.splitlines()
      if ln.startswith("require") and "==" in ln
    ]
    check(
      "no equality require left in twin blocks", not eq_requires, str(eq_requires[:3])
    )
    check("--keep-require --target c compiles", os.path.exists(got[1][:-4] + ".c"), "")


def gen_ptr_pool(rysmith, d, n="12", seed="404", emit_state=False):
  """[Stage 4] A pool generated WITH pointers and memory ops (default
  --max-ptr-depth)."""
  cmd = [rysmith, "--emit-desc", "--n-funcs", n, "--seed", seed]
  cmd += ["--n-params", "2", "--n-stmts", "5"]
  if emit_state:
    cmd += ["--emit-state", "pbb"]
  cmd += ["-o", d]
  r = run(cmd)
  if r.returncode != 0:
    return None
  sirs = [f for f in os.listdir(d) if f.endswith(".sir") and "_sym" not in f]
  return [os.path.join(d, s) for s in sorted(sirs)]


def test_ptr_state_json_provenance(rysmith):
  """[Stage 4] The state sidecar records pointer provenance (root + path)
  instead of an opaque ptr marker, and round-trips through the reader."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_ptr_pool(rysmith, d, n="4", emit_state=True)
    if not sirs:
      check("ptr-provenance setup (rysmith gen)", False, "generation failed")
      return
    states = [f for f in os.listdir(d) if f.endswith(".state.json")]
    joined = ""
    for f in states:
      joined += open(os.path.join(d, f)).read()
    check("sidecar has ptr entries", '"k":"ptr"' in joined, "")
    check('ptr entries carry provenance ("root")', '"root"' in joined, "")


STORE_FIXTURE = """// SOLVED: %pa0=9
fun @stfix(%pa0: i32) : i32 {
  let mut %a: [3] i32 = {1, 2, 3};
  let mut %p: ptr i32 = undef;
^entry:
  %p = addr %a[1];
  br ^work;
^work:
  store %p, 7 * %pa0;
  %p = addr %a[2];
  br ^exit;
^exit:
  ret %a[1] + %a[0];
}

fun @main() : i32 {
  let mut %r: i32 = undef;
^entry:
  %r = call @stfix(9);
  ret %r;
}
"""


def test_store_block_twinnable(rytwin, symiri):
  """[Stage 4] A block containing a store (and a pointer reassignment) is a
  twin candidate: its effect is the state diff, pointers reconstruct via
  addr, and the twin executes."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "stfix.sir")
    open(p1, "w").write(STORE_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "--validate", "-o", p2])
    check("store-bearing fixture twinned", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    src = open(p2).read()
    check("store block grafted", "^work__twin" in src, "")
    check(
      "store twin validated + fired",
      "validated: OK" in r.stdout and "0 twin exec" not in r.stdout,
      r.stdout[:160],
    )


MEM_OP_RE = re.compile(r"\b(load|store|addr|ptrindex|ptrfield)\b")


def orig_block_bodies(src):
  """Instruction text of every ^..__orig block in src."""
  bodies = []
  cur = None
  for line in src.splitlines():
    stripped = line.strip()
    if stripped.startswith("^") and stripped.endswith(":"):
      if cur is not None:
        bodies.append("\n".join(cur))
        cur = None
      if stripped.rstrip(":").endswith("__orig"):
        cur = []
      continue
    if cur is not None:
      if stripped.startswith("}"):
        bodies.append("\n".join(cur))
        cur = None
      else:
        cur.append(stripped)
  if cur is not None:
    bodies.append("\n".join(cur))
  return bodies


def test_equivalence_with_pointers(rytwin, rysmith, symiri):
  """[Stage 4] The full equivalence sweep holds on programs generated WITH
  pointers and memory operations, and memory-op blocks are actually being
  twinned (not just the pointer-free remnant)."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_ptr_pool(rysmith, d)
    if sirs is None:
      check("ptr equivalence setup", False, "generation failed")
      return
    equivalence_over_pool(rytwin, symiri, d, sirs, "ptr")
    mem_twins = 0
    for p1 in sirs:
      p2 = os.path.join(d, os.path.basename(p1)[:-4] + ".p2.sir")
      if not os.path.exists(p2):
        continue
      for b in orig_block_bodies(open(p2).read()):
        if MEM_OP_RE.search(b):
          mem_twins += 1
    check("memory-op blocks get twinned", mem_twins > 0, f"mem twins={mem_twins}")


def test_guard_covers_ptr_leaves(rytwin, rysmith):
  """[Stage 4] Pointer-valued state crosses into the guard: the caller
  passes the pointer and its addr-reconstructed expected value, compared
  with `==` (defined cross-object)."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_ptr_pool(rysmith, d)
    if not sirs:
      check("ptr-guard setup (rysmith gen)", False, "generation failed")
      return
    saw_expected_ptr = False
    for p1 in sirs:
      p2 = p1[:-4] + ".p2.sir"
      r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
      if r.returncode != 0:
        continue
      # Expected-pointer params are named %__e<k> by the guard builder.
      for _, params in GUARD_FN_RE.findall(open(p2).read()):
        if "%__e" in params:
          saw_expected_ptr = True
    check("guard takes addr-reconstructed expected-ptr params", saw_expected_ptr, "")


def test_ptr_program_targets(rytwin, symiri):
  """[Stage 4] The store-bearing fixture survives the backends: its twin
  compiles to C and wasm, and the C binary computes p1's result."""
  import shutil

  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "stfix.sir")
    open(p1, "w").write(STORE_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run(
      [
        rytwin,
        p1,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--target",
        "c",
        "--emit-main",
        "-o",
        p2,
      ]
    )
    check(
      "store fixture compiles to C",
      r.returncode == 0 and os.path.exists(p2[:-4] + ".c"),
      r.stderr[:200],
    )
    r = run(
      [
        rytwin,
        p1,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--target",
        "wasm",
        "--emit-main",
        "-o",
        p2[:-4] + ".w.sir",
      ]
    )
    check(
      "store fixture compiles to wasm",
      r.returncode == 0 and os.path.exists(p2[:-4] + ".w.wat"),
      r.stderr[:160],
    )
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc or not os.path.exists(p2[:-4] + ".c"):
      return
    exe = os.path.join(d, "p2.bin")
    rc = run([cc, "-O1", "-o", exe, p2[:-4] + ".c", "-lm"])
    check("store fixture p2.c compiles", rc.returncode == 0, rc.stderr[:200])
    if rc.returncode == 0:
      rr = run([exe])
      want = symiri_result(symiri, p1, "@stfix", ["9"])[1]
      # @main returns the result; the process exit code carries its low
      # 8 bits.
      ok = want is not None and rr.returncode == int(want) % 256
      check(
        "store fixture binary matches symiri",
        ok,
        f"rc={rr.returncode} want={want}",
      )


def test_ptr_state_twin_generated(rytwin, symiri):
  """[Stage 4b] Pointer-bearing state no longer disables twin generation:
  the store fixture's twin is a computed body (with its final ptr
  reconstruction), not a constant replication."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "stfix.sir")
    open(p1, "w").write(STORE_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "--validate", "-o", p2])
    check("gen store fixture twinned + validated", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    bodies = twin_block_bodies(open(p2).read())
    check(
      "store-fixture twin is a generated computation",
      any(has_computed_rhs(b) for b in bodies),
      "\n---\n".join(bodies)[:300],
    )


def test_rylink_generated_twins(rytwin, symiri):
  """[Stage 4b] The real rylink program gets generated (computed) twin
  bodies, not 100% constant replication."""
  fixture = os.path.join(os.path.dirname(__file__), "fixtures", "rylink_twin_p1.sir")
  with tempfile.TemporaryDirectory() as d:
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, fixture, "--p-twin", "1.0", "--seed", "3", "--validate", "-o", p2])
    check("rylink fixture twinned + validated (gen)", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    bodies = twin_block_bodies(open(p2).read())
    computed = sum(1 for b in bodies if has_computed_rhs(b))
    check(
      "some rylink twins are generated computations",
      computed > 0,
      f"{computed}/{len(bodies)} computed",
    )
    r1 = symiri_result(symiri, fixture, "@main", [])
    r2 = symiri_result(symiri, p2, "@main", [])
    check("rylink generated twins stay equivalent", r1[1:] == r2[1:], f"{r1} vs {r2}")


def test_ptr_pool_generated(rytwin, rysmith):
  """[Stage 4b] Across a pointered pool, generation produces computed twin
  bodies and every one is fully concretized (no %? survives)."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_ptr_pool(rysmith, d)
    if not sirs:
      check("ptr-pool gen setup", False, "generation failed")
      return
    computed = total = 0
    leaked = False
    for p1 in sirs:
      p2 = p1[:-4] + ".p2.sir"
      r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
      if r.returncode != 0:
        continue
      src = open(p2).read()
      if "%?" in src:
        leaked = True
      for b in twin_block_bodies(src):
        total += 1
        if has_computed_rhs(b):
          computed += 1
    check(
      "pointered pool has generated twins",
      computed > 0,
      f"{computed}/{total} computed",
    )
    check("no %? leaks into pointered p2s", not leaked, "")


WHOLE_PROG_FIXTURE = """fun @leaf(%pa0: i32) : i32 {
  let mut %a: i32 = 3;
^entry:
  br ^work;
^work:
  %a = 2 * %a + %pa0;
  br ^exit;
^exit:
  ret %a;
}

fun @main() : i32 {
  let mut %r: i32 = undef;
^entry:
  %r = call @leaf(41);
  ret %r;
}
"""


def dump_trace(symiri, path, entry="@main", args=None):
  r = run([symiri, "--dump-trace", "--main", entry, path, "--"] + (args or []))
  return r.stdout + r.stderr


def test_whole_program_twin_fires(rytwin, symiri):
  """[whole-program] The program's entry is @main; the twin lives in a
  callee. The guard must be keyed on the state the callee actually sees at
  runtime (args from @main's call site), so the twin executes."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "prog.sir")
    open(p1, "w").write(WHOLE_PROG_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
    check("whole-program p1 twinned", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    check(
      "twin block executes on the real input",
      "__twin" in dump_trace(symiri, p2),
      "",
    )


def test_validate_asserts_twin_fires(rytwin, symiri):
  """[whole-program] --validate interprets p2 on the profiled input and
  confirms at least one twin actually executed — a dead twin is a failure,
  not a silent success."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "prog.sir")
    open(p1, "w").write(WHOLE_PROG_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "--validate", "-o", p2])
    check("--validate exits 0 on whole program", r.returncode == 0, r.stderr[:200])
    check(
      "--validate reports twin execution",
      re.search(r"twin exec", r.stdout) is not None,
      r.stdout[:200],
    )


LABEL_COLLIDE_FIXTURE = """fun @leafa(%pa0: i32) : i32 {
  let mut %a: i32 = 3;
^entry:
  br ^work;
^work:
  %a = 2 * %a + %pa0;
  br ^exit;
^exit:
  ret %a;
}

fun @leafb(%pa0: i32) : i32 {
  let mut %b: i32 = 5;
^entry:
  br ^work;
^work:
  %b = 3 * %b + %pa0;
  br ^exit;
^exit:
  ret %b;
}

fun @main() : i32 {
  let mut %r: i32 = undef;
  let mut %s: i32 = undef;
^entry:
  %r = call @leafa(41);
  %s = call @leafb(17);
  %r = %r + %s;
  ret %r;
}
"""


def test_label_collision_across_functions(rytwin, symiri):
  """[whole-program] Two callees share the label ^work. Each must get its
  own guard keyed on its own frame state, and both twins must execute."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "prog.sir")
    open(p1, "w").write(LABEL_COLLIDE_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
    check("collision p1 twinned", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    src = open(p2).read()
    names = {nm for nm, _ in GUARD_FN_RE.findall(src)}
    check(
      "one guard per function, frame-scoped names",
      "@__twg_leafa_work" in names and "@__twg_leafb_work" in names,
      str(names),
    )
    check(
      "both twins execute",
      dump_trace(symiri, p2).count("__twin") >= 2,
      "",
    )
    r1 = symiri_result(symiri, p1, "@main", [])
    r2 = symiri_result(symiri, p2, "@main", [])
    check("collision program equivalent", r1[1:] == r2[1:], f"{r1} vs {r2}")


TWICE_CALLED_FIXTURE = """fun @leaf(%pa0: i32) : i32 {
  let mut %a: i32 = 3;
^entry:
  br ^work;
^work:
  %a = 2 * %a + %pa0;
  br ^exit;
^exit:
  ret %a;
}

fun @main() : i32 {
  let mut %r: i32 = undef;
  let mut %s: i32 = undef;
^entry:
  %r = call @leaf(41);
  %s = call @leaf(7);
  %r = %r + %s;
  ret %r;
}
"""


def test_first_visit_across_activations(rytwin, symiri):
  """[whole-program] A callee invoked twice with different args: the twin is
  planned from the first activation's state, fires there and only there, and
  the program stays equivalent."""
  with tempfile.TemporaryDirectory() as d:
    p1 = os.path.join(d, "prog.sir")
    open(p1, "w").write(TWICE_CALLED_FIXTURE)
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
    check("twice-called p1 twinned", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    trace = dump_trace(symiri, p2)
    check(
      "twin fires exactly once",
      trace.count("__twin:") == 1,
      f"count={trace.count('__twin:')}",
    )
    r1 = symiri_result(symiri, p1, "@main", [])
    r2 = symiri_result(symiri, p2, "@main", [])
    check("twice-called program equivalent", r1[1:] == r2[1:], f"{r1} vs {r2}")


def test_real_rylink_regression(rytwin, symiri):
  """[whole-program] The checked-in rylink program (structs, vectors, odd
  widths, 8 callees + @main) twins and the twin executes at runtime."""
  fixture = os.path.join(os.path.dirname(__file__), "fixtures", "rylink_twin_p1.sir")
  with tempfile.TemporaryDirectory() as d:
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, fixture, "--p-twin", "1.0", "--seed", "3", "-o", p2])
    check("rylink fixture twinned", r.returncode == 0, r.stderr[:200])
    if r.returncode != 0:
      return
    check("rylink twin executes", "__twin" in dump_trace(symiri, p2), "")
    r1 = symiri_result(symiri, fixture, "@main", [])
    r2 = symiri_result(symiri, p2, "@main", [])
    check("rylink program equivalent", r1[1:] == r2[1:], f"{r1} vs {r2}")


def test_solved_header_fallback(rytwin, rysmith, symiri):
  """[Stage 1] With no descriptor AND no sidecar, rytwin recovers the
  profiled input from p1's `// SOLVED:` header (a wrong input would trap on
  the interest requires during in-process profiling)."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("solved-header setup (rysmith gen)", False, "generation failed")
      return
    for f in os.listdir(d):
      if f.endswith(".json"):
        os.remove(os.path.join(d, f))
    got = first_twinned(rytwin, sirs, extra=["--validate"])
    check("rytwin twins from the SOLVED header alone", got is not None, "")
    if got:
      check("validated: OK without descriptor", "validated: OK" in got[2], got[2][:160])


def test_validate_without_sidecar(rytwin, rysmith):
  """[Stage 1] --validate works on a sidecar-free p1 (profiled input comes
  from the descriptor realization)."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if not sirs:
      check("validate-no-sidecar setup (rysmith gen)", False, "generation failed")
      return
    got = first_twinned(rytwin, sirs, extra=["--validate"])
    check("--validate succeeds without a sidecar", got is not None, "")
    if got:
      check("validated: OK without sidecar", "validated: OK" in got[2], got[2][:160])


def test_bad_guard_rejected(rytwin, rysmith):
  """[Stage 2] The --guard flag is gone (the full-state guard function is
  the only guard); passing it is an unknown-option error."""
  with tempfile.TemporaryDirectory() as d:
    g = gen_p1(rysmith, d)
    if not g:
      check("rytwin bad-guard setup", False, "generation failed")
      return
    p1, desc, _, _ = g
    p2 = os.path.join(d, "p2.sir")
    r = run([rytwin, p1, "--guard", "bogus", "-o", p2])
    check("removed --guard rejected (rc != 0)", r.returncode != 0, f"rc={r.returncode}")


def test_missing_args_usage(rytwin):
  """No input / no output prints usage with a non-zero exit."""
  r = run([rytwin])
  check("missing input/output → non-zero exit", r.returncode != 0, f"rc={r.returncode}")


def parse_entry(src):
  fm = re.search(r"fun\s+(@\w+)\s*\(([^)]*)\)", src)
  params = [p.split(":") for p in fm.group(2).split(",") if p.strip()]
  pnames = [p[0].strip() for p in params]
  ptypes = [p[1].strip() for p in params]
  hdr = re.search(r"//\s*SOLVED:\s*(.*)", src)
  kv = {}
  if hdr:
    for part in hdr.group(1).split(","):
      if "=" in part:
        k, v = part.strip().split("=", 1)
        kv[k.strip()] = v.strip()
  return fm.group(1), pnames, ptypes, [kv.get(p, "0") for p in pnames]


def equivalence_over_pool(rytwin, symiri, d, sirs, tag, extra=None):
  """Twin every program in the pool with --p-twin 1.0 and assert p1 === p2 on
  the profiled input AND on other inputs (where the guard does not fire)."""
  import random

  rng = random.Random(1)
  twinned = prof_bad = other_bad = other_checks = 0
  for p1 in sirs:
    stem = os.path.basename(p1)[:-4]
    desc = os.path.join(d, re.sub(r"[a-z]$", "", stem) + ".json")
    if not os.path.exists(desc):
      continue
    fn, pnames, ptypes, iargs = parse_entry(open(p1).read())
    p2 = os.path.join(d, stem + ".p2.sir")
    rr = run(
      [rytwin, p1, "--p-twin", "1.0", "--seed", "3"] + (extra or []) + ["-o", p2]
    )
    if rr.returncode != 0:
      # A block-free-of-eligible-scalars program now exits non-zero with a
      # "no twin" message — expected, skip it. Anything else is a failure.
      if "no twin" in rr.stderr or "nothing written" in rr.stderr:
        continue
      check(f"rytwin ran on {stem} [{tag}]", False, rr.stderr[:160])
      return
    twinned += 1
    if (
      symiri_result(symiri, p1, fn, iargs)[1:]
      != symiri_result(symiri, p2, fn, iargs)[1:]
    ):
      prof_bad += 1
    # Other inputs (only when all params are integer) — the exact guard
    # must keep the equivalence even where the guard does not fire.
    if all("f" not in t for t in ptypes):
      for _ in range(4):
        a = [str(rng.randint(-1_000_000, 1_000_000)) for _ in pnames]
        other_checks += 1
        if symiri_result(symiri, p1, fn, a)[1:] != symiri_result(symiri, p2, fn, a)[1:]:
          other_bad += 1
  check(
    f"TwinTransform grafted at least one twin [{tag}]",
    twinned > 0,
    f"twinned={twinned}",
  )
  check(
    f"every twin preserves the profiled result [{tag}]",
    prof_bad == 0,
    f"{prof_bad} mismatch(es)",
  )
  check(
    f"every twin preserves results on other inputs [{tag}]",
    other_bad == 0,
    f"{other_bad}/{other_checks} mismatch(es)",
  )


def test_twinpass_grafts_and_preserves_equivalence(rytwin, rysmith, symiri):
  """On pointer-free programs (scalars, structs, arrays, vectors and pure
  intrinsic calls — everything eligibility now covers), TwinTransform grafts twins
  whose exact guard keeps p1 === p2: identical results on the profiled input
  AND on other inputs."""
  with tempfile.TemporaryDirectory() as d:
    # Pointers/memory aren't twin candidates yet, so disable them; keep
    # aggregates, vectors and intrinsic calls to exercise the lifted
    # eligibility.
    sirs = gen_pool(rysmith, d, emit_state=True)
    if sirs is None:
      check("twinpass setup (scalar-only gen)", False, "generation failed")
      return
    equivalence_over_pool(rytwin, symiri, d, sirs, "sidecar")


def test_equivalence_without_sidecar(rytwin, rysmith, symiri):
  """[Stage 1] The full equivalence suite holds when p1 is generated without
  --emit-state — the profile rytwin keys its guards on is computed
  in-process."""
  with tempfile.TemporaryDirectory() as d:
    sirs = gen_pool(rysmith, d, emit_state=False)
    if sirs is None:
      check("no-sidecar equivalence setup", False, "generation failed")
      return
    equivalence_over_pool(rytwin, symiri, d, sirs, "no-sidecar")


def test_validate_and_target(rytwin, rysmith, symiri):
  """--validate asserts p1 === p2 in-process, and --target c compiles p2."""
  with tempfile.TemporaryDirectory() as d:
    r = run(
      [
        rysmith,
        "--emit-state",
        "pbb",
        "--emit-desc",
        "--emit-main",
        "--n-funcs",
        "12",
        "--seed",
        "100",
        "--n-params",
        "2",
        "--n-stmts",
        "4",
        "--max-ptr-depth",
        "0",
        "--no-vec",
        "--no-agg-ptr",
        "--no-intrinsics",
        "-o",
        d,
      ]
    )
    if r.returncode != 0:
      check("validate/target setup", False, r.stderr[:200])
      return
    # Find a program that actually grafts a twin.
    picked = None
    for s in sorted(f for f in os.listdir(d) if f.endswith(".sir") and "_sym" not in f):
      p1 = os.path.join(d, s)
      p2 = os.path.join(d, s[:-4] + ".p2.sir")
      rr = run([rytwin, p1, "--p-twin", "1.0", "--seed", "3", "-o", p2])
      m = re.search(r"\((\d+) twin", rr.stdout)
      if rr.returncode == 0 and m and int(m.group(1)) > 0:
        picked = p1
        break
    check("found a twinnable program", picked is not None, "")
    if not picked:
      return
    p2 = os.path.join(d, "pv.sir")
    r = run(
      [
        rytwin,
        picked,
        "--p-twin",
        "1.0",
        "--seed",
        "3",
        "--validate",
        "--target",
        "c",
        "--emit-main",
        "-o",
        p2,
      ]
    )
    check("rytwin --validate --target c exits 0", r.returncode == 0, r.stderr[:200])
    check("rytwin reports validated: OK", "validated: OK" in r.stdout, r.stdout[:200])
    check("rytwin emitted p2.c", os.path.exists(os.path.join(d, "pv.c")), "")


def main():
  if len(sys.argv) not in (4, 5):
    print(
      "Usage: python3 -m test.unit.run_rytwin_tests <rytwin> <rysmith> <symiri>"
      " [test-name-substring]"
    )
    sys.exit(2)
  rytwin, rysmith, symiri = sys.argv[1:4]
  only = sys.argv[4] if len(sys.argv) == 5 else ""

  tests = [
    (
      "rytwin: no twin grafted -> error, no output",
      lambda: test_no_twins_errors(rytwin, rysmith),
    ),
    (
      "rytwin: no sidecar needed (in-process profiling)",
      lambda: test_no_sidecar_needed(rytwin, rysmith),
    ),
    (
      "rytwin: corrupt sidecar falls back to in-process profiling",
      lambda: test_corrupt_sidecar_falls_back(rytwin, rysmith),
    ),
    (
      "rytwin: valid sidecar preferred over interpreting",
      lambda: test_sidecar_preferred(rytwin, rysmith),
    ),
    (
      "rytwin: SOLVED-header fallback",
      lambda: test_solved_header_fallback(rytwin, rysmith, symiri),
    ),
    (
      "rytwin: --validate without sidecar",
      lambda: test_validate_without_sidecar(rytwin, rysmith),
    ),
    (
      "rytwin: invalid --guard rejected",
      lambda: test_bad_guard_rejected(rytwin, rysmith),
    ),
    (
      "TwinTransform: guard is a function",
      lambda: test_guard_is_function(rytwin, rysmith),
    ),
    (
      "TwinTransform: guard covers the full state",
      lambda: test_guard_covers_full_state(rytwin),
    ),
    (
      "TwinTransform: guard function names unique per site",
      lambda: test_guard_unique_names(rytwin, rysmith),
    ),
    (
      "TwinTransform: aggregates by pointer, vectors per-lane",
      lambda: test_guard_aggregates_and_vectors(rytwin, rysmith, symiri),
    ),
    (
      "TwinTransform: guard functions compile (C binary + wasm)",
      lambda: test_guard_compiles_c_and_wasm(rytwin, rysmith),
    ),
    (
      "bijection guard: --twin-guard option accepted/validated",
      lambda: test_bijection_guard_option(rytwin, rysmith),
    ),
    (
      "bijection guard: mixes leaves, hides raw constants",
      lambda: test_bijection_guard_obfuscated(rytwin),
    ),
    (
      "bijection guard: default stays exact",
      lambda: test_bijection_default_is_exact(rytwin),
    ),
    (
      "bijection guard: zero-collision equivalence (profiled + other)",
      lambda: test_bijection_guard_equivalence(rytwin, rysmith, symiri),
    ),
    (
      "bijection guard: mixer survives the C backend",
      lambda: test_bijection_guard_compiles(rytwin, rysmith),
    ),
    (
      "bijection guard: narrow (i8) width validates",
      lambda: test_bijection_guard_small_width(rytwin),
    ),
    (
      "bijection guard: DLP structure checks",
      lambda: test_dlp_bijection_structure(rytwin),
    ),
    (
      "bijection guard: DLP multi-size tier diversity",
      lambda: test_dlp_bijection_tier_diversity(rytwin),
    ),
    (
      "region scope: --twin-scope option accepted/rejected",
      lambda: test_scope_flag(rytwin),
    ),
    (
      "region scope: collapses a block sequence to its exit",
      lambda: test_scope_region_sequence(rytwin, symiri),
    ),
    (
      "region scope: collapses a whole loop to its exit",
      lambda: test_scope_region_loop(rytwin, symiri),
    ),
    (
      "region scope: default stays single-block",
      lambda: test_scope_default_is_block(rytwin),
    ),
    (
      "region scope: equivalence sweep (profiled + other)",
      lambda: test_scope_region_equivalence(rytwin, rysmith, symiri),
    ),
    (
      "region scope: composes with the bijection guard",
      lambda: test_scope_region_with_bijection(rytwin, symiri),
    ),
    (
      "select: --twin-select option accepted/rejected",
      lambda: test_select_flag(rytwin),
    ),
    (
      "select: interesting wins the overlap (whole-function region)",
      lambda: test_select_interesting_wins_overlap(rytwin, symiri),
    ),
    (
      "select: interesting-selection equivalence sweep",
      lambda: test_select_interesting_equivalence(rytwin, rysmith, symiri),
    ),
    (
      "select: softmax per-region probability",
      lambda: test_select_softmax_probability(rytwin),
    ),
    (
      "twin_gen: twin blocks are generated computations",
      lambda: test_twin_is_generated(rytwin, rysmith),
    ),
    (
      "twin_gen: twins fully concrete",
      lambda: test_twin_fully_concrete(rytwin, rysmith),
    ),
    (
      "twin_gen: equivalence sweep",
      lambda: test_twin_gen_equivalence(rytwin, rysmith, symiri),
    ),
    (
      "twin_gen: fallback paths",
      lambda: test_twin_gen_fallback(rytwin, rysmith),
    ),
    (
      "twin_gen: equality requires stripped",
      lambda: test_twin_requires_stripped(rytwin, rysmith),
    ),
    (
      "whole-program: twin fires via @main",
      lambda: test_whole_program_twin_fires(rytwin, symiri),
    ),
    (
      "whole-program: --validate asserts twin execution",
      lambda: test_validate_asserts_twin_fires(rytwin, symiri),
    ),
    (
      "whole-program: label collision across functions",
      lambda: test_label_collision_across_functions(rytwin, symiri),
    ),
    (
      "whole-program: first visit across activations",
      lambda: test_first_visit_across_activations(rytwin, symiri),
    ),
    (
      "whole-program: real rylink regression",
      lambda: test_real_rylink_regression(rytwin, symiri),
    ),
    (
      "pointers: sidecar records provenance",
      lambda: test_ptr_state_json_provenance(rysmith),
    ),
    (
      "pointers: store-bearing block twinnable",
      lambda: test_store_block_twinnable(rytwin, symiri),
    ),
    (
      "pointers: equivalence with memory ops",
      lambda: test_equivalence_with_pointers(rytwin, rysmith, symiri),
    ),
    (
      "pointers: guard covers ptr leaves",
      lambda: test_guard_covers_ptr_leaves(rytwin, rysmith),
    ),
    (
      "pointers: backends on pointered twins",
      lambda: test_ptr_program_targets(rytwin, symiri),
    ),
    (
      "twin_gen+ptr: store fixture twin is generated",
      lambda: test_ptr_state_twin_generated(rytwin, symiri),
    ),
    (
      "twin_gen+ptr: rylink program gets generated twins",
      lambda: test_rylink_generated_twins(rytwin, symiri),
    ),
    (
      "twin_gen+ptr: pointered pool generates concretized twins",
      lambda: test_ptr_pool_generated(rytwin, rysmith),
    ),
    ("rytwin: missing args usage", lambda: test_missing_args_usage(rytwin)),
    (
      "TwinTransform: grafts twins, preserves equivalence (profiled + other inputs)",
      lambda: test_twinpass_grafts_and_preserves_equivalence(rytwin, rysmith, symiri),
    ),
    (
      "TwinTransform: equivalence without sidecar",
      lambda: test_equivalence_without_sidecar(rytwin, rysmith, symiri),
    ),
    (
      "rytwin: --validate and --target c",
      lambda: test_validate_and_target(rytwin, rysmith, symiri),
    ),
  ]
  for title, fn in tests:
    if only and only not in title and only not in fn.__code__.co_names[0]:
      continue
    print(f"=== {title} ===")
    fn()

  passed = sum(1 for _, ok, _ in results if ok)
  total = len(results)
  print(f"\nSummary (rytwin_tests): {passed}/{total} passed.\n")
  sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
  main()
