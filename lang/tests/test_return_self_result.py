"""`return r.sort()` and `return r.first()` hand back what the function's own exit then frees.

`sort` answers with its receiver and `first`, `last`, `min`, `max` and `get` with an element of it. The
native emitter took such a call for a fresh value, so a function that returned it released `r` on the way out
and the caller printed freed memory: `[, 2, 3]` for `[3, 1, 2]` in a release build, a heap-use-after-free
under ASan. Returning `v.first()` of a parameter handed out the caller's own reference. The compiled
program must print what the interpreter prints, and must be clean under AddressSanitizer when the C
compiler has it.
"""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-self-result-")

PROGRAM = """fn words() {
  return ["b".repeat(30), "a".repeat(30)]
}

fn sorted_local() {
  let mut r = words()
  return r.sort()
}

fn sorted_copy(v) {
  let mut w = v.copy()
  return w.sort()
}

fn sorted_keys() {
  let m = {"k": "v".repeat(30)}
  return m.keys().sort()
}

fn first_local() {
  let r = words()
  return r.first()
}

fn last_local() {
  let r = words()
  return r.last()
}

fn max_local() {
  let r = words()
  return r.max()
}

fn min_local() {
  let r = words()
  return r.min()
}

fn get_local() {
  let m = {"k": "v".repeat(30)}
  return m.get("k")
}

fn first_param(v) {
  return v.first()
}

fn main() -> i64 {
  print(sorted_local())
  print(sorted_copy(["z".repeat(30), "y".repeat(30)]))
  print(sorted_keys())
  print(first_local())
  print(last_local())
  print(max_local())
  print(min_local())
  print(get_local())
  let z = words()
  print(first_param(z))
  print(z)
  let mut q = [3, 1, 2]
  print(q.sort())
  return 0
}
"""
path = os.path.join(TMP, "self_result.brat")
with open(path, "w") as f:
    f.write(PROGRAM)

want = subprocess.run(_cxx.cli_args("run", "--interp", path), capture_output=True, text=True,
                      timeout=120)
assert want.returncode == 0, want.stderr[-300:]
got = subprocess.run([NATIVE, "run", path], capture_output=True, text=True, timeout=120)
assert got.returncode == 0, got.stderr[-300:]
assert got.stdout == want.stdout, "compiled %r, interpreter %r" % (got.stdout[:120], want.stdout[:120])

wrapper = os.path.join(TMP, "cc-asan.sh")
with open(wrapper, "w") as f:
    f.write('#!/bin/sh\nexec cc "$@" -fsanitize=address -fno-omit-frame-pointer -g\n')
os.chmod(wrapper, 0o755)
env = dict(os.environ, BRAT_CC=wrapper, BRAT_CACHE=os.path.join(TMP, "cache"), ASAN_OPTIONS="detect_leaks=0")
asan = subprocess.run([NATIVE, "run", path], capture_output=True, text=True, timeout=120, env=env)
if "libasan" in asan.stderr or "cannot find" in asan.stderr or "unrecognized" in asan.stderr:
    print("SELF RESULT green without ASan: this C compiler has none (2 asserts)")
    sys.exit(0)
assert "AddressSanitizer" not in asan.stderr, asan.stderr[:600]
assert asan.returncode == 0 and asan.stdout == want.stdout, (asan.returncode, asan.stderr[-300:])

print("SELF RESULT ALL GREEN (5 asserts, ASan included, 11 returns)")
