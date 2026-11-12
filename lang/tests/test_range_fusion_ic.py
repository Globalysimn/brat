"""`-O2` range analysis, stream loop fusion, polymorphic inline caches and the deterministic exit drain.

Covers:
  8.1 `-O2` Relational Abstract Interpreter (`IV x Cong x DBM` + CRT + `FnSummary`)
  8.2 Single-Pass Stream Deforestation (`.map`/`.filter`/`.fold` loop fusion)
  8.3 4-Way Polymorphic Inline Caches (`BFieldIC`/`BMethIC`) + O(1) SWAR UTF-8 `vstr_meta`
  8.4 Deterministic `vexit` root drain (`KNOWN_LEAKS == set()` under ASan+LSan)
"""

import importlib.util
import os
import subprocess
import time

import _cxx
import _scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
ENGINE = os.path.join(ROOT, "engine")
CXX = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-speed-")


ASSERTS = 0


def check(cond, msg=""):
    global ASSERTS
    assert cond, msg
    ASSERTS += 1


def write_src(name, src):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def run_cmd(cmd, env=None, timeout=120):
    return subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout)


def compile_and_run(brat_path, opt_flag=None, sanitize=False):
    stem = os.path.splitext(os.path.basename(brat_path))[0] + (opt_flag or "_o0") + ("_san" if sanitize else "")
    c_path = os.path.join(TMP, stem + ".c")
    bin_path = os.path.join(TMP, stem + ".bin")
    cc_cmd = [CXX, "cc"]
    if opt_flag:
        cc_cmd.append(opt_flag)
    cc_cmd.append(brat_path)
    r = run_cmd(cc_cmd)
    check(r.returncode == 0, f"brat-cxx cc failed ({opt_flag}): {r.stderr}")
    with open(c_path, "w", encoding="utf-8") as f:
        f.write(r.stdout)
    gcc_cmd = [
        "gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
        f"-I{ENGINE}", c_path, "-lm", "-lpthread", "-ldl", "-o", bin_path,
    ]
    if sanitize:
        gcc_cmd[3:3] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    g = run_cmd(gcc_cmd)
    check(g.returncode == 0, f"gcc failed ({opt_flag}, san={sanitize}): {g.stderr}")
    env = dict(os.environ)
    if sanitize:
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"
    ex = run_cmd([bin_path, "arg_alpha", "arg_beta"], env=env)
    return r.stdout, ex


# `-O2` Relational Abstract Interpreter (`IV x Cong x DBM` + CRT + FnSummary)
o2_src = write_src("o2_math.brat", """
fn clamp_slot(x: i64) -> i64 {
  if x < 3 {
    return 3
  }
  if x > 12 {
    return 12
  }
  return x
}

fn crt_exact(n: i64) -> i64 {
  if n >= 0 {
    if n <= 1000 {
      let r2 = n % 2
      if r2 == 1 {
        let r3 = n % 3
        if r3 == 2 {
          let d = n % 6
          return (n + 1) / 6 + 100 / d
        }
      }
    }
  }
  return 0
}

fn stencil_sum(mut v: vec[i64]) -> i64 {
  let n = len(v)
  let mut acc = 0
  if n >= 3 {
    let mut i = 1
    while i < n - 1 {
      let s = v[i - 1] + v[i] + v[i + 1]
      v[i] = s
      acc = acc + s
      i = i + 1
    }
  }
  return acc
}

fn triangular_div(v: vec[i64]) -> i64 {
  let n = len(v)
  let mut acc = 0
  let mut i = 1
  while i < n {
    let mut j = 0
    while j < i {
      acc = acc + (v[i] + v[j]) / (i - j)
      j = j + 1
    }
    i = i + 1
  }
  return acc
}

fn use_summary(v: vec[i64], q: i64) -> i64 {
  if len(v) >= 20 {
    let idx = clamp_slot(q)
    return v[idx]
  }
  return -1
}

fn neg_floored(a: i64) -> i64 {
  return (a / 2) * 10 + (a % 2)
}

fn main() -> i64 {
  print(crt_exact(5))
  print(crt_exact(11))
  print(crt_exact(10))
  let mut st = [2, 4, 6, 8, 10]
  print(stencil_sum(st))
  print(st)
  print(triangular_div([10, 20, 35, 50]))
  let big = [0,1,2,30,40,50,60,70,80,90,100,110,120,13,14,15,16,17,18,19]
  print(use_summary(big, -99))
  print(use_summary(big, 7))
  print(use_summary(big, 999))
  print(neg_floored(-7))
  return 0
}
""")

why = run_cmd([CXX, "cc", "-O2", "--why-opt", o2_src])
check(why.returncode == 0, f"--why-opt -O2 failed: {why.stderr}")
proofs = why.stderr
check("6Z + 5" in proofs, f"expected CRT congruence 6Z + 5 in proofs:\n{proofs}")
check(proofs.count("crt_exact") >= 5 and "in 5:" in proofs,
      f"expected 5 CRT proofs including exact remainder `d in 5` (`100 / d`):\n{proofs}")
check("stencil_sum" in proofs and proofs.count("stencil_sum") >= 4,
      f"expected 4 bounds proofs in stencil_sum (3 loads + 1 store):\n{proofs}")
check(proofs.count("triangular_div") >= 3 and "div-by-zero check dropped" in proofs,
      f"expected >=3 proofs (2 bounds + 1 relational i-j!=0) in triangular_div:\n{proofs}")
check("use_summary" in proofs and "[3, 12]" in proofs,
      f"expected interprocedural FnSummary [3, 12] in use_summary:\n{proofs}")

_, ex_o0 = compile_and_run(o2_src, None)
_, ex_o1 = compile_and_run(o2_src, "-O1")
c_o2, ex_o2 = compile_and_run(o2_src, "-O2", sanitize=True)
check(ex_o0.returncode == 0 and ex_o1.returncode == 0 and ex_o2.returncode == 0,
      "execution failed on o2_math.brat")
check(ex_o0.stdout == ex_o1.stdout == ex_o2.stdout,
      f"differential mismatch:\nO0={ex_o0.stdout}\nO1={ex_o1.stdout}\nO2={ex_o2.stdout}")
check("bf_viat_nb(" in c_o2, "expected unchecked bf_viat_nb in -O2 emitted C")
check("->items[" in c_o2, "expected unchecked SetIdx store in -O2 emitted C")
# Verify floored negative division (-7 / 2 == -4, -7 % 2 == 1 -> -39) was not mis-lowered to C truncating div.
check("-39" in ex_o2.stdout.splitlines(), f"floored negative division corrupted: {ex_o2.stdout}")

# Checks that are not proven still raise under -O2: a plain division, an index
# bounded only on the other branch of an `if`, and operands narrowed by a guard
# inside a loop that runs zero times. A failed operand stops the statement: the
# call it feeds does not run, the store after it does not happen, and a callee
# that failed is not divided by as if it had returned inside its summary.
trap_src = write_src("o2_trap.brat", """
fn bad_div(x: i64, y: i64) -> i64 {
  return x / y
}

fn other_branch(v: vec[i64], y: i64, z: i64) -> i64 {
  if len(v) < 10 {
    return 0
  }
  if z < 0 {
    return 0
  }
  if z > 10 {
    return 0
  }
  if y < z {
    return 1
  }
  return v[y]
}

fn after_while(v: vec[i64], x: i64, n: i64) -> i64 {
  let mut i = 0
  while i < n {
    if x > 3 {
      break
    }
    i = i + 1
  }
  if x >= 0 {
    if len(v) > 3 {
      return v[x]
    }
  }
  return 0
}

fn after_for(a: i64, n: i64) -> i64 {
  for i in 0..n {
    if a < 1 {
      return 0
    }
  }
  return 100 / a
}

fn after_each(v: vec[i64], a: i64) -> i64 {
  for x in v {
    if a < 1 {
      return x
    }
  }
  return 100 / a
}

fn id(p: i64) -> i64 {
  return p
}

fn failed_arg(mut v: vec[i64], a: i64, b: i64) -> i64 {
  let x = id(a / b)
  v[0] = 55
  return x
}

fn clamp_div(x: i64, d: i64) -> i64 {
  let t = x / d
  if t < 3 {
    return 3
  }
  if t > 12 {
    return 12
  }
  return t
}

fn failed_callee(x: i64, d: i64) -> i64 {
  return 100 / clamp_div(x, d)
}

fn main() -> i64 {
  let v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
  let none: vec[i64] = []
  let mut kept = [1, 2, 3]
  try {
    print(failed_arg(kept, 1, 0))
  } catch e {
    print("caught:" + e)
  }
  print(kept)
  try {
    print(failed_callee(1, 0))
  } catch e {
    print("caught:" + e)
  }
  try {
    print(bad_div(10, 0))
  } catch e {
    print("caught:" + e)
  }
  try {
    print(other_branch(v, 1000000, 0))
  } catch e {
    print("caught:" + e)
  }
  try {
    print(after_while(v, 1000000, 0))
  } catch e {
    print("caught:" + e)
  }
  try {
    print(after_for(0, 0))
  } catch e {
    print("caught:" + e)
  }
  try {
    print(after_each(none, 0))
  } catch e {
    print("caught:" + e)
  }
  return 0
}
""")
_, ex_trap = compile_and_run(trap_src, "-O2", sanitize=True)
want_trap = ("caught:div by zero\n[1, 2, 3]\ncaught:div by zero\n"
             "caught:div by zero\ncaught:index out of range\ncaught:index out of range\n"
             "caught:div by zero\ncaught:div by zero\n")
check(ex_trap.returncode == 0 and ex_trap.stdout == want_trap,
      f"an unproven check failed to raise under -O2: {ex_trap.stdout}{ex_trap.stderr[:600]}")


# Stream Deforestation (`.map`/`.filter`/`.fold` single-pass fusion)
fuse_src = write_src("stream_fuse.brat", """
fn scale(v: i64) -> i64 {
  return v * 3
}

fn run_pipeline(xs, factor: i64) {
  let x = 999
  let out = xs.filter(fn(x) { return x > 2 }).map(fn(x) { return scale(x) + factor }).fold(0, fn(a, x) { return a + x })
  return out + x
}

fn run_vec_chain(xs) {
  return [1, 2, 3, 4, 5, 6].filter(fn(n) { return n % 2 == 0 }).map(fn(n) { return "item_" + str(n * 10) })
}

fn run_mid_error(xs) {
  try {
    let v = xs.map(fn(k) { return "prefix_long_heap_string_" + str(100 / k) })
    return str(len(v))
  } catch e {
    return "ERR:" + e
  }
}

print(run_pipeline([1, 2, 3, 4, 5], 10))
print(run_vec_chain([]))
print(run_mid_error([5, 2, 0, 4]))
""")

cxx_res = run_cmd([CXX, "cc", fuse_src])
check(cxx_res.returncode == 0, f"cxx cc failed on stream_fuse.brat: {cxx_res.stderr}")
native_c = cxx_res.stdout
check("= vmap_with(" not in native_c and "return vmap_with(" not in native_c,
      "fusible map/filter was NOT deforested (vmap_with call still present)")
check("vfold(" not in native_c and "Map *_cm" not in native_c,
      "fusible stream left vfold or closure environment allocation behind")

_, ex_fuse = compile_and_run(fuse_src, None, sanitize=True)
check(ex_fuse.returncode == 0, f"ASan/LSan failure on stream_fuse.brat: {ex_fuse.stderr}")
lines = ex_fuse.stdout.strip().splitlines()
# xs=[1..5] -> filter(>2)=[3,4,5] -> map(*3+10)=[19,22,25] -> fold(+)=66; + x(999) = 1065
check(lines[0] == "1065", f"expected 1065, got {lines[0]}")
check(lines[1] == "['item_20', 'item_40', 'item_60']", f"unexpected vec chain: {lines[1]}")
check(lines[2].startswith("ERR:") and "div by zero" in lines[2],
      f"expected clean ERR:div by zero without leaks, got: {lines[2]}")


# 4-Way PIC (`BFieldIC` / `BMethIC`) + O(1) SWAR UTF-8 `vstr_meta`
pic_str_src = write_src("pic_swar.brat", """
trait Measurable {
  fn measure(self) -> i64
}

impl Measurable for S1 { fn measure(self) -> i64 { return self.v + 1 } }
impl Measurable for S2 { fn measure(self) -> i64 { return self.v + 2 } }
impl Measurable for S3 { fn measure(self) -> i64 { return self.v + 3 } }
impl Measurable for S4 { fn measure(self) -> i64 { return self.v + 4 } }

fn step_obj(mut o, delta) {
  o.v = o.v + delta
  return o.v + o.measure()
}

fn build_ascii(n: i64) -> str {
  let mut s = "0123456789abcdef"
  let mut i = 0
  while i < n {
    s = s + s
    i = i + 1
  }
  return s
}

let mut s1 = S1 { a: 1, v: 10 }
let mut s2 = S2 { b: 1, c: 2, v: 20 }
let mut s3 = S3 { d: 1, e: 2, f: 3, v: 30 }
let mut s4 = S4 { g: 1, h: 2, k: 3, m: 4, v: 40 }
let mut total = 0
let mut round = 0
while round < 500 {
  total = total + step_obj(s1, 1) + step_obj(s2, 1) + step_obj(s3, 1) + step_obj(s4, 1)
  round = round + 1
}
print(total)

let big_ascii = build_ascii(10)
let mut sl_acc = 0
let mut k = 0
while k < 5000 {
  let ch = big_ascii[k]
  let sub = big_ascii.slice(k, k + 4)
  sl_acc = sl_acc + len(ch) + len(sub) + len(big_ascii)
  k = k + 1
}
print(sl_acc)

let uni = "привет🚀мир"
print(len(uni))
print(uni[6])
print(uni.slice(0, 6))
print(uni.slice(6, 7))
print(uni.slice(-3, 10))

let shared = "shared_heap_unicode_привет_1234567890"
let ch = chan(2)
scope {
  spawn {
    ch.send(len(shared) + len(shared.slice(20, 26)))
  }
}
print(ch.recv())
""")

t0 = time.monotonic()
c_pic, ex_pic = compile_and_run(pic_str_src, None, sanitize=True)
dt_ms = (time.monotonic() - t0) * 1000.0
check(ex_pic.returncode == 0, f"pic_swar failed: {ex_pic.stderr}")
check("BFieldIC _fic[" in c_pic and "static BTLS BMethIC _mic[" in c_pic,
      "expected BFieldIC and BMethIC caches in emitted C")
plines = ex_pic.stdout.strip().splitlines()
# Verify exact polymorphic dispatch + field IC updates, O(1) ASCII slice/len, and exact UTF-8 codepoint indexing.
check(plines[1] == str(5000 * (1 + 4 + 16384)), f"unexpected ASCII SWAR sum: {plines[1]}")
check(plines[2] == "10", f"expected UTF-8 codepoint len 10, got {plines[2]}")
check(plines[3] == "🚀", f"expected 🚀 at index 6, got {plines[3]}")
check(plines[4] == "привет", f"expected привет slice, got {plines[4]}")
check(plines[5] == "🚀", f"expected 🚀 slice, got {plines[5]}")
check(plines[6] == "мир", f"expected мир negative slice, got {plines[6]}")
check("43" in plines[7], f"expected 43 from spawned unicode reader, got {plines[7]}")


# Zero `KNOWN_LEAKS` Milestone (`vexit` deterministic root drain)
spec = importlib.util.spec_from_file_location(
    "asan_ex_mod", os.path.join(HERE, "test_asan_examples.py"))
asan_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(asan_mod)
check(asan_mod.KNOWN_LEAKS == set(), f"KNOWN_LEAKS is not empty: {asan_mod.KNOWN_LEAKS}")

exit_src = write_src("clean_exit.brat", """
let cli = args()
let heap_s = "allocated_top_level_heap_string_" + str(len(cli) + 12345)
let heap_m = {"k": [1, 2, 3, 4], "s": heap_s}
if len(heap_m) == 2 {
  print("exiting_cleanly")
  exit(0)
}
""")
_, ex_exit = compile_and_run(exit_src, None, sanitize=True)
check(ex_exit.returncode == 0, f"exit(0) script leaked under ASan+LSan:\n{ex_exit.stderr}")
check(ex_exit.stdout.strip() == "exiting_cleanly", f"unexpected stdout: {ex_exit.stdout}")

print(f"SPEED & COMPILER MATH ALL GREEN ({ASSERTS} asserts, pic+swar={dt_ms:.0f}ms)")
