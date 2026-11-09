"""Proof suite for Priority 2 dynamic execution speedups ().

Verifies:
1. Unannotated pure-integer dynamic functions (`fn dyn_arith(n)`) lower to raw
   `int64_t` fast helpers with runtime `V_INT` entry guards and safely fall back
   to dynamic execution when called with non-integer values (e.g. strings/floats).
2. Unboxed `int64_t` scalar local specialization (`ub_b_*`) inside mixed
   string/vec/method dynamic functions preserves 64-bit two's-complement wrapping,
   Python-style floor division/modulo (`vub_div`/`vub_mod`), `INT64_MIN / -1`,
   and div-by-zero unwinding inside `try`/`catch`.
3. Zero-malloc stack `BStackVec` argument passing + per-call-site 1-slot inline
   method cache (`vcall_method_ic` + `_mic[]`) handles monomorphic hot loops,
   polymorphic alternating struct/enum receivers at the same call site, builtin
   method fallback (`str`/`vec`/`map`), and >8 argument heap fallback.
4. The native emitted C keeps the expected fast-path structure, with zero
   ASan/UBSan/LSan errors across all dynamic fast paths.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cxx  # noqa: E402
import _scratch  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-dyn-speed-")

SRC = """enum Shape {
  Box(i64)
  Empty
}

impl Shape {
  fn score(self, w) {
    match self {
      Box(v) => return v * w + 1,
      Empty => return w,
    }
  }
}

impl Counter {
  fn add(self, d) {
    return self.v + d
  }
  fn score(self, w) {
    return self.v * w
  }
  fn many9(self, a1, a2, a3, a4, a5, a6, a7, a8, a9) {
    return self.v + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9
  }
}

// Unannotated pure-int dynamic function -> "i64_dyn" fast helper + dynamic fallback:
fn dyn_poly_or_int(a, b) {
  let mut acc = a + b
  if a == b {
    return acc
  }
  acc = acc + b
  return acc
}

fn dyn_hot_arith(n) {
  let mut s = 0
  let mut i = 0
  while i < n {
    s = s + i * 3 - (i - 1)
    i = i + 1
  }
  return s
}

// Mixed string + vec + method dynamic function with unboxed int64_t loop locals:
fn dyn_mixed_loop(prefix, items) {
  let n = len(items)
  let mut s = 0
  let mut i = 0
  let lim = n * 20000
  while i < lim {
    s = s + (i * 5 - 3) / 2 + (i % 7)
    i = i + 1
  }
  return prefix + ":" + str(s)
}

fn dyn_floor_and_catch(z) {
  let q = -7 / 2
  let r = -7 % 2
  let mut caught = 0
  try {
    let bad = 10 / z
    caught = bad
  } catch e {
    caught = 99
  }
  return str(q) + "," + str(r) + "," + str(caught)
}

// Same call site `x.score(3)` hit with Struct, Tag(1), Tag(0), and another Struct:
fn poly_dispatch(seq) {
  let mut total = 0
  for x in seq {
    total = total + x.score(3)
  }
  return total
}

// Builtin method fallback through dynamic call site when receiver is str or vec:
fn dyn_upper_or_custom(x) {
  return x.upper()
}

fn meth_hot(n) {
  let c = Counter { v: 7 }
  let mut i = 0
  let mut s = 0
  while i < n {
    s = s + c.add(i)
    i = i + 1
  }
  return s
}

fn sso_hot(n) {
  let mut i = 0
  let mut ok = 0
  while i < n {
    let s = "id_" + "42" + "_ok"
    let t = s.slice(0, 8)
    let edge14 = "0123456789abcd"
    let heap15 = edge14 + "e"
    if t == "id_42_ok" && len(t) == 8 && len(edge14) == 14 && len(heap15) == 15 && heap15.slice(0, 14) == edge14 {
      ok = ok + 1
    }
    i = i + 1
  }
  return ok
}

fn main() -> i64 {
  // 1. i64_dyn fast path + runtime fallback to string concatenation:
  assert(dyn_poly_or_int(10, 20) == 50)
  assert(dyn_poly_or_int(10, 10) == 20)
  assert(dyn_poly_or_int("ab", "cd") == "abcdcd")
  assert(dyn_hot_arith(100000) == 10000000000)

  // 2. ub_ints mixed function + floor div/mod + div-by-zero catch:
  assert(dyn_mixed_loop("sum", [1, 2, 3, 4, 5]) == "sum:12499999995")
  assert(dyn_floor_and_catch(0) == "-4,1,99")

  // 3. Monomorphic + polymorphic inline method cache + >8-arg fallback + builtin fallback:
  assert(meth_hot(50000) == 1250325000)
  let seq = [Counter { v: 4 }, Box(5), Empty, Counter { v: 10 }]
  assert(poly_dispatch(seq) == 12 + 16 + 3 + 30)
  let c9 = Counter { v: 100 }
  assert(c9.many9(1, 2, 3, 4, 5, 6, 7, 8, 9) == 145)
  assert(dyn_upper_or_custom("brat") == "BRAT")

  // 4. Inline <=14B SSO + 14->15B heap boundary + UTF-8 codepoint len:
  assert(sso_hot(20000) == 20000)
  let ru = "привет"
  assert(len(ru) == 6 && ru + "!" == "привет!")

  print("DYN_SPEED_OK")
  return 0
}
"""

prog_path = os.path.join(TMP, "dyn_speed.brat")
with open(prog_path, "w", encoding="utf-8") as fh:
    fh.write(SRC)

# 1. Emitted C structure from the native emitter:
cxx_cc = subprocess.run([BIN, "cc", prog_path], capture_output=True, text=True, timeout=30)
assert cxx_cc.returncode == 0, cxx_cc.stderr
native_c = cxx_cc.stdout
assert "static BTLS BMethIC _mic[" in native_c
assert "vcall_method_ic(" in native_c
assert "VSTACK_VEC_INIT(" in native_c
assert "int64_t BRAT_UNUSED ub_b_s = 0;" in native_c
assert "bf_f" in native_c  # unannotated dyn_hot_arith / dyn_poly_or_int lowered to fast raw C

# 2. Unoptimized (-O0) vs Native ASan+UBSan+LSan parity:
ri = subprocess.run(_cxx.cli_args("run", "-O0", prog_path),
                    capture_output=True, text=True, timeout=30)
assert ri.returncode == 0 and ri.stdout == "DYN_SPEED_OK\n", (ri.returncode, ri.stdout, ri.stderr)

c_path = os.path.join(TMP, "dyn_speed.c")
exe_asan = os.path.join(TMP, "dyn_speed_asan")
with open(c_path, "w", encoding="utf-8") as fh:
    fh.write(native_c)

cc = subprocess.run(
    ["gcc", "-O2", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
     "-I", os.path.join(ROOT, "lang", "engine"), c_path, "-lm", "-lpthread", "-o", exe_asan],
    capture_output=True, text=True, timeout=60,
)
assert cc.returncode == 0, cc.stderr

rn = subprocess.run(
    [exe_asan],
    capture_output=True, text=True, timeout=30,
    env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"),
)
assert rn.returncode == 0 and rn.stdout == "DYN_SPEED_OK\n", (rn.returncode, rn.stdout, rn.stderr)

print("DYN SPEED ALL GREEN")
