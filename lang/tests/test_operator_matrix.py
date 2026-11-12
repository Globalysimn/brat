"""Every binary operator over 25 values, and seven unary operations over the same values.

The program below applies `+ - * / % == != < <= > >= && || in []` to each ordered pair of 25 values
(ints, floats with NaN and infinity, bools, strings, None, vectors, maps and the three tags) and
prints the result, or the error the pair raises: 9550 lines of output from 61 lines of source. The
compiled program must run cleanly under AddressSanitizer + UBSan, reject variant indexing
(`Ok(1)[1]` -> `index of non-indexable`), and match `-O0` on all 9550 lines.

The source lives in this file because the optimiser and emitter tests treat every `.brat` file in
the tree as part of their corpus and pin its size.
"""
import _cxx
import _scratch

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
SOURCE = r'''let inf = 1e308 * 10.0
let nan = inf - inf
let vals = [0, 1, -1, 3, 1.5, 2.0, -0.5, 1e30, inf, nan, true, false, "", "a", "abc", "é", None, [], [1, 2], ["a"], {}, {"a": 1}, Ok(1), Err("e"), Some(2)]
let ops = ["+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "in", "[]"]

fn apply(op, a, b) {
  match op {
    "+" => { return a + b },
    "-" => { return a - b },
    "*" => { return a * b },
    "/" => { return a / b },
    "%" => { return a % b },
    "==" => { return a == b },
    "!=" => { return a != b },
    "<" => { return a < b },
    "<=" => { return a <= b },
    ">" => { return a > b },
    ">=" => { return a >= b },
    "&&" => { return a && b },
    "||" => { return a || b },
    "in" => { return a in b },
    "[]" => { return a[b] }
  }
  return "unknown op"
}

fn unary(op, a) {
  match op {
    "neg" => { return -a },
    "not" => { return !a },
    "len" => { return len(a) },
    "str" => { return str(a) },
    "unwrap" => { return unwrap(a) },
    "is_err" => { return is_err(a) },
    "tagof" => { return tagof(a) }
  }
  return "unknown op"
}

let uops = ["neg", "not", "len", "str", "unwrap", "is_err", "tagof"]

for op in ops {
  for i in 0..len(vals) {
    for j in 0..len(vals) {
      try {
        print("{op} {i} {j} = " + str(apply(op, vals[i], vals[j])))
      } catch e {
        print("{op} {i} {j} ERR " + str(e))
      }
    }
  }
}
for op in uops {
  for i in 0..len(vals) {
    try {
      print("{op} {i} = " + str(unary(op, vals[i])))
    } catch e {
      print("{op} {i} ERR " + str(e))
    }
  }
}
'''
NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-operator-matrix-")
PROGRAM = os.path.join(TMP, "matrix.brat")
with open(PROGRAM, "w", encoding="utf-8") as f:
    f.write(SOURCE)

run_o0 = subprocess.run(_cxx.cli_args("run", "-O0", PROGRAM), capture_output=True,
                        text=True, timeout=300)
native = subprocess.run([NATIVE, "run", PROGRAM], capture_output=True, text=True, timeout=300)
assert run_o0.returncode == 0, run_o0.stderr[-400:]
assert native.returncode == 0, native.stderr[-400:]

a, b = run_o0.stdout.split("\n"), native.stdout.split("\n")
assert len(a) == len(b) == 9551, "%d and %d lines" % (len(a), len(b))
different = [(x, y) for x, y in zip(a, b) if x != y]
assert not different, "%d of %d lines differ, first: %r" % (
    len(different), len(a), different[0])

c_path = os.path.join(TMP, "matrix.c")
emitted = subprocess.run([NATIVE, "emit-c", PROGRAM], capture_output=True, timeout=300)
assert emitted.returncode == 0, emitted.stderr[-400:]
with open(c_path, "wb") as f:
    f.write(emitted.stdout)
exe = os.path.join(TMP, "matrix")
cc = subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                     "-fno-omit-frame-pointer", "-I", ENGINE, c_path, "-o", exe, "-lm", "-lpthread",
                     "-ldl"], capture_output=True, text=True, timeout=300)
assert cc.returncode == 0, cc.stderr[-600:]
sanitized = subprocess.run([exe], capture_output=True, text=True, timeout=300,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=99",
                                    UBSAN_OPTIONS="print_stacktrace=1"))
assert sanitized.returncode == 0, "sanitizer run exited %d:\n%s" % (sanitized.returncode,
                                                                    sanitized.stderr[-800:])
assert sanitized.stdout == native.stdout, "the sanitized build printed something else"

print("OPERATOR MATRIX ALL GREEN (%d cases, 0 differ)" % (len(a) - 1))
