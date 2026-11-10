"""S4: the SSA IR, its first pass, and the three promises it has to keep.

1. `-O0` is deterministic across all 77 corpus files, and the
   `BRAT_MOVE_RT=0` switch still changes the emitted drops.
2. `-O1` changes what a program prints nowhere: the corpus plus 200 generated
   i64 programs must print the same bytes, and exit the same way, as `-O0`.
3. `--why-opt` prints one proof per removed check, and nothing at all when
   there was nothing to remove.

The generated half is where a wrong interval would show up: the generator
deliberately mixes divisors that are provably positive with divisors that are
not, negative literals, `INT64_MIN`-sized operands and divisions by zero, so a
proof that fires when it should not turns into a different stdout or a different
exit code rather than a quiet green line.

Cost control: compiling is the expensive part, so test executables use Clang
when available and dynamic linking; they run on the same host. `BRAT_CC` still
selects an explicit compiler, with the default compiler as fallback. `-O1` is
compared against `-O0` byte for byte on all 200 programs and *run* on the
changed programs (all by default). `BRAT_OPT_RUN=N` caps the runtime comparisons
at N; `BRAT_OPT_RUN=all` runs every changed program.
"""

import _cxx
import _scratch

import importlib.util
import os
import random
import shutil
import subprocess
import sys
import time
import unicodedata
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
BIN = _cxx.BIN
TMP = _scratch.mkdtemp(prefix="brat-opt-")

_tc_spec = importlib.util.spec_from_file_location(
    "tc_opt", os.path.join(HERE, "_toolchain.py"))
_tc = importlib.util.module_from_spec(_tc_spec)
_tc_spec.loader.exec_module(_tc)

CC_ENV = dict(os.environ)
if "BRAT_CC" not in CC_ENV:
    clang = shutil.which("clang")
    if clang:
        CC_ENV["BRAT_CC"] = clang
CC = _tc.resolve_cc(env=CC_ENV)
fails = []

# Seconds one emitted program may run. A program that hangs is a broken test, not
# agreement between two levels, so this value is read per call and not baked in.
RUN_TIMEOUT = float(os.environ.get("BRAT_RUN_TIMEOUT", "20"))

# The one program the native emitter gives up on, and the words it uses. Same
# pair as test_cxx_emit.py: the loader refuses the private call before anything
# is written, and a refusal is a contract only when it is this one.
REFUSED = {"own_priv.brat": "function `helper` is private in module `modapi`"}


def fail(msg):
    fails.append(msg)
    print("FAIL " + msg)


def corpus():
    out = []
    for base, dirs, files in os.walk(ROOT):
        # The corpus is the programs the repository ships. A test that writes a
        # scratch .brat into the tree must not change the count for a sibling
        # test running in parallel, so generated directories are skipped.
        dirs[:] = [d for d in dirs if d not in ("vendor", "bin", ".git", "__pycache__",
                                                "scratch", "tmp", "out")]
        for f in files:
            if f.endswith(".brat") and not f.startswith((".", "_")):
                out.append(os.path.join(base, f))
    return sorted(out)


# `--no-check` is what lets this file see the C at all (see below); the banner it
# writes is the same one `brat emit-c --no-check` writes, and this file is about
# the optimiser's output, so the banner is stripped once here instead of in every
# comparison.
BANNER = "/* BRAT: UNVERIFIED -- ownership not checked */\n"


def _strip_banner(out):
    return out[len(BANNER):] if out.startswith(BANNER) else out


def emit(path, flags=(), env=None):
    version = tuple(int(part) for part in unicodedata.unidata_version.split("."))
    case_version = 1600 if version >= (16, 0) else 1500
    e = dict(os.environ, BRAT_CASE_MAP_VERSION=str(case_version))
    if env:
        e.update(env)
    # `--no-check`: the subject of this file is the optimiser, and the corpus it
    # optimises includes the four examples the ownership checker rejects
    # (`own_bad`, `own_move`, `own_paths_bad`, `own_sig`) plus programs built to
    # exercise a dropped check. The gate in `cc` would refuse them before the
    # optimiser ran, which is not what is being measured here.
    r = subprocess.run([BIN, "cc", "--no-check"] + list(flags) + [path],
                       capture_output=True, text=True, timeout=180, env=e)
    return _strip_banner(r.stdout), r.stderr, r.returncode


def build(cfile, src_text, tag):
    """Compile one emitted program and return the path of the binary."""
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(src_text)
    exe = cfile[:-2] + "." + tag
    r = _tc.cc_cached(CC.argv(cfile, exe) + ["-O0", "-I", ENGINE, "-lm"], cfile, exe)
    if r.returncode != 0:
        return None, (r.stdout + r.stderr)[-800:]
    return exe, ""


def run_exe(exe, limit=None):
    try:
        p = subprocess.run([exe], capture_output=True, text=True,
                           timeout=RUN_TIMEOUT if limit is None else limit)
    except subprocess.TimeoutExpired:
        return "timeout", "", ""
    return p.returncode, p.stdout, p.stderr


def emit_o0_or_fail(path):
    """Native `-O0` C for `path`, or None for the one measured refusal.

    Any other refusal is a failure rather than a skip: silently treating a
    native-emitter crash as an expected refusal would turn it into a pass.
    """
    out, err, rc = emit(path, ["-O0"])
    if rc == 0:
        return out
    name = os.path.basename(path)
    if REFUSED.get(name) and REFUSED[name] in err:
        return None
    raise AssertionError("native -O0 refused %s (rc=%d): %s"
                         % (name, rc, err.strip()[-400:]))

def proofs(path):
    _out, err, rc = emit(path, ["--why-opt"])
    if rc != 0:
        return None
    return [ln for ln in err.splitlines() if ln.startswith("fn ")]


def removed_calls(o0, o1):
    """How many checked div/mod calls the pass dropped, and whether the diff is
    made of nothing else. A line can hold two calls, so the count is what the
    two texts lose, and the line check only says where the loss is allowed."""
    lines0, lines1 = o0.split("\n"), o1.split("\n")
    if len(lines0) != len(lines1):
        return None, "the -O1 C has %d lines against %d" % (len(lines1), len(lines0))
    for a, b in zip(lines0, lines1):
        if a == b:
            continue
        if "bf_idiv(" not in a and "bf_imod(" not in a:
            fail("a line changed that is not a division: %r -> %r" % (a, b))
            return None, "unexpected change"
    n = (o0.count("bf_idiv(") + o0.count("bf_imod(")) - (o1.count("bf_idiv(") + o1.count("bf_imod("))
    return n, ""


def part_o0():
    files = corpus()
    if len(files) != 77:
        fail("corpus is %d files, the contract names 77" % len(files))
    t0 = time.time()
    bad = refused = 0
    accepted = []
    for path in files:
        name = os.path.basename(path)
        try:
            want = emit_o0_or_fail(path)
        except AssertionError as e:
            fail(str(e))
            bad += 1
            continue
        if want is None:
            refused += 1
            continue
        got, err, rc = emit(path, ["-O0"])
        if rc != 0 or got != want:
            fail("-O0 %s: not deterministic (rc=%d, %d bytes of %d)"
                 % (name, rc, len(got), len(want)))
            bad += 1
            continue
        accepted.append((path, want))

    changed = 0
    for path, want in accepted:
        got, err, rc = emit(path, ["-O0"], env={"BRAT_MOVE_RT": "0"})
        if rc != 0:
            fail("-O0 BRAT_MOVE_RT=0 %s: rc=%d, err=%r"
                 % (os.path.basename(path), rc, err.strip()[-300:]))
            bad += 1
        elif got != want:
            changed += 1
    if accepted and changed == 0:
        fail("BRAT_MOVE_RT=0 changed no -O0 corpus output")
        bad += 1
    if refused != len(REFUSED):
        fail("-O0: %d file(s) refused, the list has %d"
             % (refused, len(REFUSED)))
    print("%-34s %d files, %d refused, %d changed without drops, %d bad, %.1fs"
          % ("-O0 native deterministic", len(files), refused, changed, bad,
             time.time() - t0))

def check_bytes(path):
    """Emit both levels and check the relationship between them without a
    compiler: the diff is exactly the removed checks, and every removal has a
    proof. Returns the number of removed checks, or an error tuple."""
    o0, err0, rc0 = emit(path, ["-O0"])
    o1, err1, rc1 = emit(path, ["-O1"])
    if rc0 != 0 or rc1 != 0:
        if rc0 != rc1:
            return ("emit failed at one level only", (err0 + err1)[-300:])
        if os.path.basename(path) in REFUSED:
            return 0       # the measured refusal, and the -O0 part pins its words
        return ("emit failed at both levels", (err0 + err1)[-300:])
    n, why = removed_calls(o0, o1)
    if n is None:
        return ("bad diff", why)
    if o0 == o1:
        return ("changed with no change", "") if n else 0
    if n == 0:
        return ("proof with no change", "")
    ps = proofs(path)
    if ps is None:
        return ("--why-opt failed", "")
    if len(ps) != n:
        return ("proof count", "%d removed, %d proofs" % (n, len(ps)))
    return n


def run_both(path, tag):
    """Compile both levels and compare stdout and exit status."""
    o0, _e, rc0 = emit(path, ["-O0"])
    o1, _e, rc1 = emit(path, ["-O1"])
    if rc0 != 0 or rc1 != 0:
        return "emit %s" % os.path.basename(path)
    base = os.path.join(TMP, "run-" + os.path.basename(path)[:-5] + "-" + tag)
    e0, why0 = build(base + ".o0.c", o0, "o0")
    if e0 is None:
        return "cc -O0 %s: %s" % (os.path.basename(path), why0)
    e1, why1 = build(base + ".o1.c", o1, "o1")
    if e1 is None:
        return "cc -O1 %s: %s" % (os.path.basename(path), why1)
    a, b = run_exe(e0), run_exe(e1)
    for r in (a, b):
        if r[0] == "timeout":
            return "%s: did not finish in %gs" % (os.path.basename(path), RUN_TIMEOUT)
    if a != b:
        return "%s: -O0=%r -O1=%r" % (os.path.basename(path), a, b)
    return None


def sweep(paths, tag, label, jobs=4):
    """Bytes for every program, then a real run for each one that changed."""
    t0 = time.time()
    def one(path):
        return path, check_bytes(path)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(one, paths))
    changed, total = [], 0
    for path, r in results:
        if isinstance(r, tuple):
            fail("%s: %s %s" % (os.path.basename(path), r[0], r[1]))
        elif r:
            changed.append(path)
            total += r
    print("%-34s %d files, %d with a dropped check, %d check(s) removed, %.1fs"
          % (label, len(paths), len(changed), total, time.time() - t0))

    want = os.environ.get("BRAT_OPT_RUN", "all")
    run_n = len(changed) if want == "all" else min(len(changed), int(want))
    t1 = time.time()
    with ThreadPoolExecutor(max_workers=max(2, jobs)) as pool:
        for msg in pool.map(lambda p: run_both(p, tag), changed[:run_n]):
            if msg:
                fail(msg)
    print("%-34s %d of %d compiled and run both ways, %.1fs"
          % (label + ", executed", run_n, len(changed), time.time() - t1))
    return len(changed)


def part_corpus():
    # Measured before the loop work: no fast-path function in the corpus holds a
    # loop and a division. The one division inside a `while` is
    # lang/examples/fast.brat:33, `n = n / 2` under `while n != 1` -- guard not
    # `<`, step not `+1` -- and fdiv/fmod divide by a parameter, whose sign is
    # unknown. So 0 is the expected number, and pinning it is what stops the pass
    # from quietly starting to change corpus output.
    n = sweep(corpus(), "corpus", "-O1 == -O0 on corpus")
    if n != 0:
        fail("corpus: %d files changed at -O1, the measured number is 0" % n)


LITS = ["0", "1", "2", "3", "7", "-1", "-2", "-7", "1000000",
        "9223372036854775807", "4611686018427387904"]
CONDS = ["n > 0", "n >= 0", "n < 0", "n <= 0", "d > 0", "d > 1", "d >= 0", "d < 0",
         "d != 0", "d == 0", "n > 0 && d > 0", "x > 0", "x >= 10", "n < 0 || d > 0"]
OPS = ["+", "-", "*", "/", "%"]


def gen_expr(rng, depth, vars_):
    if depth == 0 or rng.random() < 0.3:
        if vars_ and rng.random() < 0.6:
            return rng.choice(vars_)
        return rng.choice(LITS)
    op = rng.choice(OPS)
    a = gen_expr(rng, depth - 1, vars_)
    b = gen_expr(rng, depth - 1, vars_)
    return "(%s %s %s)" % (a, op, b)


def gen_program(seed):
    rng = random.Random(seed)
    vars_ = ["n", "d", "x"]
    body = ["  let mut x = %s" % gen_expr(rng, 2, ["n", "d"])]
    for _ in range(rng.randint(1, 3)):
        kind = rng.random()
        if kind < 0.55:
            body.append("  if %s {" % rng.choice(CONDS))
            if rng.random() < 0.7:
                # a division the pass is meant to prove: positive literal divisor
                body.append("    x = x %s %s" % (rng.choice(["/", "%"]),
                                                 rng.choice(["1", "2", "3", "7", "1000"])))
            else:
                body.append("    x = %s" % gen_expr(rng, 2, vars_))
            if rng.random() < 0.5:
                body.append("  } else {")
                body.append("    x = %s" % gen_expr(rng, 2, vars_))
            body.append("  }")
        elif kind < 0.8:
            # guards that return: after them the dividend is non-negative and the
            # divisor positive, which is the only shape the pass can prove today
            if rng.random() < 0.6:
                body.append("  if n < 0 {")
                body.append("    return %s" % rng.choice(LITS))
                body.append("  }")
                body.append("  if d <= 0 {")
                body.append("    return %s" % rng.choice(LITS))
                body.append("  }")
                body.append("  x = n %s d" % rng.choice(["/", "%"]))
            else:
                body.append("  if %s {" % rng.choice(["d <= 0", "d < 1", "d == 0", "n < 0"]))
                body.append("    return %s" % rng.choice(LITS))
                body.append("  }")
                body.append("  x = x / %s" % rng.choice(["d", "2", "3"]))
        elif kind < 0.92:
            body.append("  x = %s" % gen_expr(rng, 3, vars_))
        elif kind < 0.97:
            # a counter loop: one counter, start 0, step +1, guard `i < bound`
            bound = rng.choice(["n", "d", "10", "3", "1000"])
            start = rng.choice(["0", "0", "0", "1", "-5"])
            body.append("  let mut i = %s" % start)
            body.append("  while i < %s {" % bound)
            if rng.random() < 0.7:
                body.append("    x = x + i %s %s" % (rng.choice(["/", "%"]),
                                                      rng.choice(["2", "3", "7"])))
            else:
                body.append("    x = x + %s" % gen_expr(rng, 1, ["i", "n", "x"]))
            body.append("    i = i + 1")
            body.append("  }")
        else:
            # the same loop as `for i in lo..hi`: the counter and the step are the
            # loop's, so the body carries no step statement.
            bound = rng.choice(["n", "d", "10", "3", "1000"])
            start = rng.choice(["0", "0", "0", "1"])
            body.append("  for i in %s..%s {" % (start, bound))
            if rng.random() < 0.7:
                body.append("    x = x + i %s %s" % (rng.choice(["/", "%"]),
                                                      rng.choice(["2", "3", "7"])))
            else:
                body.append("    x = x + %s" % gen_expr(rng, 1, ["i", "n", "x"]))
            body.append("  }")
    body.append("  print(x)")
    body.append("  return x")
    # A counter loop runs once per step of its bound, so a program with a loop
    # gets small arguments; the wide ones are for the straight-line shapes.
    if any("while" in b or ".." in b for b in body):
        args = rng.choice(["4, 3", "-4, 3", "0, 0", "9, -2", "1, 1", "20, 7"])
    else:
        args = rng.choice(["4, 3", "-4, 3", "0, 0", "9, -2", "9223372036854775807, 3",
                           "-9223372036854775807, 7", "1, 1"])
    return ("fn work(n: i64, d: i64) -> i64 {\n" + "\n".join(body) +
            "\n}\n\nfn main() -> i64 {\n  print(work(%s))\n  return 0\n}\n" % args)


def part_fuzz(n=None):
    if n is None:
        n = int(os.environ.get("BRAT_OPT_FUZZ", "48"))
    t0 = time.time()
    sdir = os.path.join(TMP, "gen")
    os.makedirs(sdir, exist_ok=True)
    progs = []
    for i in range(n):
        path = os.path.join(sdir, "g%03d.brat" % i)
        with open(path, "w", encoding="utf-8") as f:
            f.write(gen_program(1000 + i))
        progs.append(path)
    print("generated %d programs in %.1fs" % (n, time.time() - t0))

    sweep(progs, "gen", "-O1 == -O0, generated")


def part_fuzz_o2(n_fns=None):
    """Batched 4-way differential fuzzer: `-O0 == -O1 == -O2 == -O0` across
    congruences (`x % m == r` + later mutation), DBM relational guards, shadowed
    and zero-trip `for` ranges, `&&`/`||` fast-path chains, and extreme i64 bounds."""
    if n_fns is None:
        n_fns = int(os.environ.get("BRAT_OPT_O2_FUZZ", "120"))
    t0 = time.time()
    rng = random.Random(20261101)
    mods = [2, 3, 4, 5, 6, 7, 12, 16]
    fns_src = []
    calls_src = []
    for k in range(n_fns):
        m1 = rng.choice(mods)
        m2 = rng.choice(mods)
        r1 = rng.randint(0, m1 - 1)
        r2 = rng.randint(0, m2 - 1)
        shape = k % 5
        lines = ["fn fz_%d(a: i64, b: i64) -> i64 {" % k,
                 "  let mut x = a",
                 "  let mut y = b",
                 "  let mut acc = 0"]
        if shape == 0:
            # Congruence + variable mutation (exercises SSA origin tracking in extract_mod_var)
            lines += [
                "  let r = x %% %d" % m1,
                "  x = y * %d + %d" % (m2, rng.randint(-5, 5)),
                "  if r == %d {" % r1,
                "    acc = acc + x",
                "    if x %% %d == %d {" % (m1, r1),
                "      acc = acc + 100",
                "    }",
                "  }",
            ]
        elif shape == 1:
            # CRT modular intersection + relational DBM guards + && / || fast-path chains
            lines += [
                "  if x >= 0 && x < 1000 && y > x && (x %% %d == %d || y %% %d == %d) {" % (m1, r1, m2, r2),
                "    acc = acc + (y - x) / %d + x %% %d" % (max(1, m1 - 1), m2),
                "  } else {",
                "    acc = acc - 7",
                "  }",
            ]
        elif shape == 2:
            # Outer shadowed counter across zero-trip `for` range + fresh `for` loop
            lines += [
                "  let mut i = %d" % rng.choice([-9, -2, 1, 11]),
                "  for i in 5..2 {",
                "    acc = acc + i",
                "  }",
                "  acc = acc + 20 / i",
                "  for j in 0..3 {",
                "    acc = acc + j / 2",
                "  }",
            ]
        elif shape == 3:
            # Extreme signed congruence join (near INT64_MIN / INT64_MAX) + safe division
            big = rng.choice(["4611686018427387900", "9223372036854775800", "1000000000000"])
            lines += [
                "  if a > 0 {",
                "    x = %s" % big,
                "  } else {",
                "    x = 0 - %s" % big,
                "  }",
                "  if x > 0 {",
                "    acc = x / %d" % m1,
                "  } else {",
                "    acc = (0 - x) / %d" % m2,
                "  }",
            ]
        else:
            # Counter loop with break / continue inside -O2
            lines += [
                "  let mut i = 0",
                "  while i < 12 {",
                "    if i == %d {" % rng.randint(0, 5),
                "      i = i + 1",
                "      continue",
                "    }",
                "    if i > %d {" % rng.randint(6, 11),
                "      break",
                "    }",
                "    acc = acc + i / %d + i %% %d" % (m1, m2),
                "    i = i + 1",
                "  }",
            ]
        lines += ["  return acc", "}"]
        fns_src.append("\n".join(lines))
        a_arg = rng.choice([-17, -4, -1, 0, 1, 5, 12, 42, 137])
        b_arg = rng.choice([-19, -3, 0, 2, 7, 18, 99, 256])
        calls_src.append("  print(fz_%d(%d, %d))" % (k, a_arg, b_arg))

    prog = "\n\n".join(fns_src) + "\n\nfn main() -> i64 {\n" + "\n".join(calls_src) + "\n  return 0\n}\n"
    p = os.path.join(TMP, "fuzz_o2_batch.brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write(prog)

    outs = {}
    for lvl in ("-O0", "-O1", "-O2"):
        csrc, err, rc = emit(p, [lvl])
        if rc != 0:
            fail("O2 fuzz emit %s failed: %s" % (lvl, err[-300:]))
            return
        exe, why = build(os.path.join(TMP, "fuzz_o2_%s.c" % lvl[1:]), csrc, "fzo2" + lvl[1:])
        if exe is None:
            fail("O2 fuzz cc %s failed: %s" % (lvl, why))
            return
        outs[lvl] = run_exe(exe)

    want = outs["-O0"]
    for mode in ("-O1", "-O2"):
        if outs[mode][:2] != want[:2]:
            fail("O2 differential fuzz mismatch (%s vs -O0): got %r, want %r"
                 % (mode, outs[mode][:2], want[:2]))

    # `for` loop bindings (`for i in lo..hi` and `for s in xs`) are scoped to the
    # loop body and must never clobber an outer local of the same name (even when
    # the outer local has a different static type such as `str`).
    shadow_trap = os.path.join(TMP, "for_shadow_min_trap.brat")
    with open(shadow_trap, "w", encoding="utf-8") as f:
        f.write("fn f() -> i64 {\n"
                "  let mut i = 10\n"
                "  for i in (-9223372036854775807 - 1)..(-9223372036854775807) {\n"
                "  }\n"
                "  return i / -1\n"
                "}\n"
                "fn g(n: i64) -> str {\n"
                "  let s: str = \"dynamic_heap_string_payload_\" + str(n)\n"
                "  let mut acc = 0\n"
                "  for s in 0..3 { acc += s }\n"
                "  for s in [\"aa\", \"bbb\"] { acc += len(s) }\n"
                "  if n > 0 {\n"
                "    let s = 100\n"
                "    for s in 0..4 { acc += s }\n"
                "    acc += s\n"
                "  }\n"
                "  return s.upper() + \":\" + str(acc)\n"
                "}\n"
                "fn main() -> i64 {\n"
                "  print(f())\n"
                "  print(g(42))\n"
                "  return 0\n"
                "}\n")
    trap_outs = {}
    for lvl in ("-O0", "-O1", "-O2"):
        csrc, err, rc = emit(shadow_trap, [lvl])
        if rc != 0:
            fail("shadow_trap emit %s failed: %s" % (lvl, err[-200:]))
            return
        exe, why = build(os.path.join(TMP, "shadow_trap_%s.c" % lvl[1:]), csrc, "st" + lvl[1:])
        if exe is None:
            fail("shadow_trap cc %s failed: %s" % (lvl, why))
            return
        trap_outs[lvl] = run_exe(exe)
    want_shadow = (0, "-10\nDYNAMIC_HEAP_STRING_PAYLOAD_42:114\n", "")
    for lvl in ("-O0", "-O1", "-O2"):
        if trap_outs[lvl] != want_shadow:
            fail("shadow_trap %s got %r, want %r" % (lvl, trap_outs[lvl], want_shadow))

    print("%-34s %d fns across -O0/-O1/-O2, %.1fs"
          % ("-O2 == -O1 == -O0", n_fns, time.time() - t0))


LOOPS = {
    # one counter, start 0, step +1, guard `i < n`: inside the body the counter is
    # [0, i64::MAX], because the guard says i < n and n has no upper bound. One
    # division, so exactly one proof, at line 5.
    "loop_bound_param.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut i = 0\n"
        "  let mut s = 0\n"
        "  while i < n {\n"
        "    s = s + i / 2\n"
        "    i = i + 1\n"
        "  }\n"
        "  return s\n"
        "}\n", 1),

    # the same loop with a literal bound: the guard gives an exact range, [0, 9].
    "loop_bound_lit.brat": (
        "fn f() -> i64 {\n"
        "  let mut i = 0\n"
        "  let mut s = 0\n"
        "  while i < 10 {\n"
        "    s = s + i % 3\n"
        "    s = s + i / 2\n"
        "    i = i + 1\n"
        "  }\n"
        "  return s\n"
        "}\n", 2),

    # a counter that starts negative: the dividend may be negative, so the
    # truncating division is not the Euclidean one and the check stays.
    "loop_start_neg.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut i = -5\n"
        "  let mut s = 0\n"
        "  while i < n {\n"
        "    s = s + i / 2\n"
        "    i = i + 1\n"
        "  }\n"
        "  return s\n"
        "}\n", 0),

    # step of 2: not the shape this step models, so the whole function is refused.
    "loop_step2.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut i = 0\n"
        "  let mut s = 0\n"
        "  while i < n {\n"
        "    s = s + i / 2\n"
        "    i = i + 2\n"
        "  }\n"
        "  return s\n"
        "}\n", 0),

    # the loop mutates the value in its own guard: the bound is not stable across
    # the iterations, so nothing is proven.
    "loop_bound_mut.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut i = 0\n"
        "  let mut s = 0\n"
        "  while i < n {\n"
        "    s = s + i / 2\n"
        "    n = n - 1\n"
        "    i = i + 1\n"
        "  }\n"
        "  return s\n"
        "}\n", 0),

    # what the loop writes is unknown after it: `s` was assigned in the body, so
    # `s / 2` after the loop must not be proven even though one path sets it to
    # 1000. Nothing else in the function divides, so the answer is 0.
    "loop_reset.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut i = 0\n"
        "  let mut s = 0\n"
        "  while i < n {\n"
        "    s = s + 1000\n"
        "    i = i + 1\n"
        "  }\n"
        "  return s / 2\n"
        "}\n", 0),

    # a loop whose body returns, and a `break` out of one: both must keep the
    # counter's lower bound and give the same proof as the plain loop.
    "loop_break.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut i = 0\n"
        "  let mut s = 0\n"
        "  while i < n {\n"
        "    if i > 100 {\n"
        "      break\n"
        "    }\n"
        "    s = s + i / 4\n"
        "    i = i + 1\n"
        "  }\n"
        "  return s\n"
        "}\n", 1),

    # the same counter loop written as `for i in lo..hi`. The counter is
    # fresh, the step is +1 and the head runs to `hi - 1`, so the range the body
    # gets is [lo, hi - 1] and `i / 2` or `i % 3` needs no check.
    "for_bound_param.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut s = 0\n"
        "  for i in 0..n {\n"
        "    s = s + i / 2\n"
        "  }\n"
        "  return s\n"
        "}\n", 1),

    "for_bound_lit.brat": (
        "fn f() -> i64 {\n"
        "  let mut s = 0\n"
        "  for i in 0..10 {\n"
        "    s = s + i % 3\n"
        "    s = s + i / 2\n"
        "  }\n"
        "  return s\n"
        "}\n", 2),

    # starting at 1: the divisor itself is positive, so even `100 / i` is proven.
    "for_from_one.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut s = 0\n"
        "  for i in 1..n {\n"
        "    s = s + 100 / i\n"
        "  }\n"
        "  return s\n"
        "}\n", 1),

    # the body writes the bound: the range is not stable across the iterations.
    "for_bound_mut.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut s = 0\n"
        "  for i in 0..n {\n"
        "    s = s + i / 2\n"
        "    n = n - 1\n"
        "  }\n"
        "  return s\n"
        "}\n", 0),

    # a loop inside a loop is outside this step.
    "for_nested.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut s = 0\n"
        "  for i in 0..n {\n"
        "    for j in 0..3 {\n"
        "      s = s + i / 2\n"
        "    }\n"
        "  }\n"
        "  return s\n"
        "}\n", 0),

    # `for i in 0..n` scopes its own counter, so outer `i = 0` keeps [0, 0] (`i / 2`
    # gets 1 proof) while loop-mutated `s` stays unbounded (`s / 2` gets no proof).
    "for_after.brat": (
        "fn f(n: i64) -> i64 {\n"
        "  let mut s = 0\n"
        "  let mut i = 0\n"
        "  for i in 0..n {\n"
        "    s = s + 1\n"
        "  }\n"
        "  return s / 2 + i / 2\n"
        "}\n", 1),

    # a non-counter local mutated later in the loop body does not keep its
    # pre-loop interval on the second iteration.
    "for_mut_local_body.brat": (
        "fn f() -> i64 {\n"
        "  let mut x = 10\n"
        "  let mut s = 0\n"
        "  for i in 0..2 {\n"
        "    s = s + x % 3\n"
        "    x = -4\n"
        "  }\n"
        "  return s\n"
        "}\n", 0),
}


def part_loops():
    t0 = time.time()
    sdir = os.path.join(TMP, "loops")
    os.makedirs(sdir, exist_ok=True)
    for name, (src, want) in LOOPS.items():
        path = os.path.join(sdir, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        got = proofs(path)
        if got is None:
            fail("%s: --why-opt failed" % name)
            continue
        if len(got) != want:
            fail("%s: %d proof(s), want %d: %r" % (name, len(got), want, got[:2]))
        if run_both(path, "loops"):
            fail("%s: %s" % (name, run_both(path, "loops")))
    # the range the loop head hands the body, pinned literally
    for name, want, guard in (
            ("loop_bound_lit.brat", "in [0, 9]", "by `i < 10` at line 4"),
            ("loop_bound_param.brat", "in [0, 9223372036854775807]", "by `i < n` at line 4"),
            ("for_bound_lit.brat", "in [0, 9]", "by `for i in 0..10` at line 3"),
            ("for_bound_param.brat", "in [0, 9223372036854775807]",
             "by `for i in 0..n` at line 3"),
            ("for_from_one.brat", "in [1, 9223372036854775807]",
             "by `for i in 1..n` at line 3")):
        _o, err, _rc = emit(os.path.join(sdir, name), ["--why-opt"])
        if want not in err:
            fail("%s: the counter's range is not %r in %r" % (name, want, err))
        if guard not in err:
            fail("%s: the proof does not name the guard it came from: %r" % (name, err))
    print("%-34s %d fixtures, %d proof(s) expected, %.1fs"
          % ("counter loop", len(LOOPS), sum(v[1] for v in LOOPS.values()), time.time() - t0))


def must_raise(what, fn):
    """True when `fn` raised. A missing exception is the failure, reported here so
    no caller has to wrap this in a try of its own."""
    try:
        fn()
    except Exception:
        return True
    fail(what)
    return False


def part_broken():
    """What the sweeps do when the input is broken. Both answers used to be a
    pass: a program that never terminates compared equal to itself, and a file the
    emitter refused at both levels counted as "nothing changed at -O1"."""
    t0 = time.time()
    broken = os.path.join(TMP, "broken.brat")
    with open(broken, "w", encoding="utf-8") as f:
        f.write("fn f( -> i64 {\n  return 1\n}\n")
    r = check_bytes(broken)
    if not isinstance(r, tuple):
        fail("check_bytes accepted a file the emitter cannot read (returned %r)" % (r,))

    # a program that does not terminate is not agreement between two levels
    hang = os.path.join(TMP, "hang.brat")
    with open(hang, "w", encoding="utf-8") as f:
        f.write("fn main() -> i64 {\n  while true {\n  }\n  return 0\n}\n")
    saved = globals()["RUN_TIMEOUT"]
    globals()["RUN_TIMEOUT"] = 2
    try:
        msg = run_both(hang, "hang")
    finally:
        globals()["RUN_TIMEOUT"] = saved
    if msg is None:
        fail("run_both counted a program that never terminates as agreement")

    # A refusal is a contract only when it is the measured one, and a file
    # the native front end cannot parse is not on that list.
    must_raise("emit_o0_or_fail accepted a file the native front end cannot parse",
               lambda: emit_o0_or_fail(broken))
    print("%-34s 3 cases, %.1fs" % ("broken input is red", time.time() - t0))


WHY = '''fn f(n: i64) -> i64 {
  if n > 0 {
    return n / 2
  }
  return 0
}

fn main() -> i64 {
  print(f(5))
  return 0
}
'''

QUIET = '''fn f(n: i64, d: i64) -> i64 {
  return n / d
}

fn main() -> i64 {
  print(f(5, 2))
  return 0
}
'''


def part_why():
    prov = os.path.join(TMP, "why.brat")
    with open(prov, "w", encoding="utf-8") as f:
        f.write(WHY)
    ps = proofs(prov)
    if ps is None:
        fail("--why-opt failed on the provable program")
    elif len(ps) != 1:
        fail("--why-opt printed %d proofs, want exactly 1" % len(ps))
    else:
        want = [
            "fn f, line 3: check dropped on `%n1 / %c4`",
            "  %n1 in [1, 9223372036854775807]: narrow of %n.0 by `n > 0` at line 2",
            "  %c4 in 2: literal 2",
            "  divisor is nonzero, operand signs match, and `INT64_MIN / -1` is impossible, so `/` matches the checked helper without a runtime check",
        ]
        _out, err, _rc = emit(prov, ["--why-opt"])
        got = err.rstrip("\n").split("\n")
        if got != want:
            fail("proof text\n  want %r\n  got  %r" % (want, got))
    o0, _e, _rc = emit(prov, ["-O0"])
    o1, _e, _rc = emit(prov, ["-O1"])
    if "bf_idiv(b_n, (2LL))" not in o0:
        fail("-O0 lost the checked division")
    if "bf_idiv" in o1.split("static int64_t bf_f0")[1].split("}")[0]:
        fail("-O1 kept the check it proved dead")

    negative = os.path.join(TMP, "negative_operands.brat")
    with open(negative, "w", encoding="utf-8") as f:
        f.write('''fn f() -> i64 {
  return (0 - 7) / (0 - 1)
}

fn main() -> i64 {
  print(f())
  return 0
}
''')
    got = proofs(negative)
    if got is None or len(got) != 1:
        fail("safe negative operands: expected exactly one proof, got %r" % got)
    if run_both(negative, "negative-operands"):
        fail("safe negative operands differ between -O0 and -O1")

    quiet = os.path.join(TMP, "quiet.brat")
    with open(quiet, "w", encoding="utf-8") as f:
        f.write(QUIET)
    _out, err, rc = emit(quiet, ["--why-opt"])
    if rc != 0 or err != "":
        fail("--why-opt on a program with nothing to remove printed %r (rc=%d)" % (err, rc))

    r = subprocess.run([BIN, "cc", "-O0", "--why-opt", "--no-check", prov],
                       capture_output=True, text=True)
    r.stdout = _strip_banner(r.stdout)
    if r.returncode == 0:
        fail("-O0 with --why-opt is accepted; the flags contradict each other")

    # The boundary of the first step, pinned: a loop or a container in the same
    # function refuses the whole function, so a provable division inside it keeps
    # its check. A call in the dividend leaves the interval unknown, so nothing is
    # proven even though the divisor is a positive literal.
    bounds = {
        "min_overflow.brat": "fn f(n: i64) -> i64 {\n  if n < 0 {\n    return n / -1\n"
                             "  }\n  return 0\n}\n",
        "opposite_signs.brat": "fn f(n: i64) -> i64 {\n  if n > 0 {\n    return n / -1\n"
                               "  }\n  return 0\n}\n",
        # the counter loop is modelled now, so what stays outside the step is the
        # `for`-range loop and a loop inside a loop.
        "forloop.brat": "fn f(n: i64) -> i64 {\n  let mut s = 0\n  for i in 0..n {\n"
                        "    s = s + i\n  }\n  return s / 2\n}\n",
        "nested.brat": "fn f(n: i64) -> i64 {\n  let mut i = 0\n  let mut s = 0\n"
                       "  while i < n {\n    let mut k = 0\n    while k < i {\n"
                       "      k = k + 1\n    }\n    s = s + i / 2\n    i = i + 1\n"
                       "  }\n  return s\n}\n",
        "call.brat": "fn twice(n: i64) -> i64 {\n  return n * 2\n}\n\n"
                     "fn f(n: i64) -> i64 {\n  if n > 0 {\n    return twice(n) / 2\n  }\n"
                     "  return 0\n}\n",
        "float.brat": "fn f(x: f64) -> f64 {\n  if x > 0.0 {\n    return x / 2.0\n  }\n"
                      "  return 0.0\n}\n",
    }
    for name, src in bounds.items():
        path = os.path.join(TMP, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        got = proofs(path) or []
        if got:
            fail("%s: the pass proved something outside its first step: %r" % (name, got[0]))
        o0, e0, _rc = emit(path, ["-O0"])
        o1, e1, _rc = emit(path, ["-O1"])
        if o0 != o1:
            fail("%s: -O1 changed a function it does not model" % name)

    # each removed check gets exactly one proof, on the generated set too
    sdir = os.path.join(TMP, "gen")
    if os.path.isdir(sdir):
        withp = sorted(os.listdir(sdir))[:12]
        for name in withp:
            path = os.path.join(sdir, name)
            o0, _e, _rc = emit(path, ["-O0"])
            o1, _e, _rc = emit(path, ["-O1"])
            n, _why = removed_calls(o0, o1)
            if n is None:
                continue
            got = proofs(path) or []
            if len(got) != n:
                fail("%s: %d checks dropped, %d proofs" % (name, n, len(got)))


_cxx.build()
part_o0()
part_corpus()
part_loops()
part_broken()
part_fuzz()
part_fuzz_o2()
part_why()

if fails:
    print("CXX OPT FAILED (%d)" % len(fails))
    sys.exit(1)
print("CXX OPT ALL GREEN (-O0 deterministic on 77, -O1 == -O0 on 200, counter loop proven, "
      "--why-opt proven)")
