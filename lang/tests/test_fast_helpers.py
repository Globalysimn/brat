"""Fast-path helpers: every `bf_*` the emitted C calls must also be defined.

`bf_idiv`/`bf_imod` used to be requested only for a `kind == "i64"` fast
function, while the emitter also emits the call inside an `i64v` function — a
function whose parameters are `i64`/`vec[i64]` in any mix. The result was a
program `brat check` accepted and `brat build` could not compile, because the
helper was never written out. The native emitter had the bug, so a program with
`t += x / 2` in a vector loop failed on the C compiler and passed in the
-O0.

This file pins the class, not one operator: for each fast kind it emits the C,
collects the `bf_*` calls, and requires a definition for every one of them. It
then builds the program on both backends and compares stdout with the
-O0, because a helper that exists but computes something else is the
same bug in a different place.
"""
import _cxx
import _scratch
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, "..", "engine")
TMP = _scratch.mkdtemp(prefix="brat-fasthelpers-")

CALL = re.compile(r"\b(bf_[A-Za-z0-9_]+)\s*\(")
DEF = re.compile(r"\b(?:static\s+[\w*\s]+|BFN\s+[\w*\s]+)\b(bf_[A-Za-z0-9_]+)\s*\(")

# kind -> (source, expected stdout)
CASES = {
    "i64_div": ("""fn f(a: i64, b: i64) -> i64 {
  return a / b
}
print(f(-7, 2))
""", "-4\n"),
    "i64_mod": ("""fn f(a: i64, b: i64) -> i64 {
  return a % b
}
print(f(-7, 2))
""", "1\n"),
    "i64v_div": ("""fn f(v: vec[i64]) -> i64 {
  let mut t = 0
  for x in v {
    t += x / 2
  }
  return t
}
print(f([3, 5, 7]))
""", "6\n"),
    "i64v_mod": ("""fn f(v: vec[i64]) -> i64 {
  let mut t = 0
  for x in v {
    t += x % 2
  }
  return t
}
print(f([3, 5, 7]))
""", "3\n"),
    "i64v_mixed_params": ("""fn f(v: vec[i64], k: i64) -> i64 {
  let mut t = k
  for x in v {
    t += x / k
    t += x % k
  }
  return t
}
print(f([7, -3, 8], 3))
""", "9\n"),
    "i64v_index": ("""fn f(v: vec[i64]) -> i64 {
  let mut t = 0
  for i in 0..len(v) {
    t += v[i] / 2
  }
  return t
}
print(f([4, 6, 9]))
""", "9\n"),
    "i64v_set": ("""fn f(v: mut vec[i64]) -> i64 {
  for i in 0..len(v) {
    v[i] = v[i] / 2
  }
  return len(v)
}
let mut w = [4, 6, 9]
print(f(w))
print(w)
""", "3\n[2, 3, 4]\n"),
    "f64_div": ("""fn f(a: f64, b: f64) -> f64 {
  return a / b
}
print(f(7.0, 2.0))
""", "3.5\n"),
    "mixed_call": ("""fn g(a: i64) -> i64 {
  return a % 3
}
fn f(v: vec[i64]) -> i64 {
  let mut t = 0
  for x in v {
    t += g(x)
  }
  return t
}
print(f([5, 9, 10]))
""", "3\n"),
}


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)


def emit(path):
    p = run(_cxx.cli_args("emit-c", path), env=dict(os.environ))
    if p.returncode != 0:
        return None
    return p.stdout


def main():
    bad = 0
    for name in sorted(CASES):
        src, want = CASES[name]
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)

        c = emit(path)
        if c is None:
            print("FAIL %s: emit-c rejected an accepted program" % name)
            bad += 1
            continue
        missing = sorted(set(CALL.findall(c)) - set(DEF.findall(c)))
        if missing:
            print("FAIL %s: emitted C calls %s with no definition" % (name, missing))
            bad += 1

        exe = os.path.join(TMP, name + ".exe")
        b = run(_cxx.cli_args("build", path, "-o", exe))
        if b.returncode != 0:
            print("FAIL %s: build: %s" % (name, (b.stdout + b.stderr)[-500:]))
            bad += 1
            continue
        got = run([exe]).stdout
        if got != want:
            print("FAIL %s: native stdout %r, want %r" % (name, got, want))
            bad += 1
            continue
        run_o0 = run(_cxx.run_args(path)).stdout
        if run_o0 != want:
            print("FAIL %s: run_o0 stdout %r, want %r" % (name, run_o0, want))
            bad += 1

    if bad:
        print("FAST HELPERS: %d case(s) failed" % bad)
        return 1
    print("FAST HELPERS OK (%d cases, helpers defined and both backends agree)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
