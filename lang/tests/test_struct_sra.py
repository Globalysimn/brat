"""Canonical fixed-slot struct layout, direct slot access, escape analysis, and SRA tests."""
import os
import subprocess
import sys

import _cxx
import _scratch

NATIVE = _cxx.build()
CXX = os.path.join(os.path.dirname(NATIVE), "brat-cxx")
TMP = _scratch.mkdtemp(prefix="brat-struct-sra-")
ENV = dict(os.environ, BRAT_FILES="1", BRAT_NO_NATIVE_CLI="1")

n = 0


def check(cond, msg):
    global n
    if not cond:
        raise AssertionError(msg)
    n += 1


def write_src(rel, content):
    p = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    return p


def run_brat(*args):
    return subprocess.run([NATIVE, *args], capture_output=True, text=True, timeout=120, env=ENV)


def run_cxx(*args):
    return subprocess.run([CXX, *args], capture_output=True, text=True, timeout=120, env=ENV)


def fn_body(c_src, fn_name):
    marker = f"/* fn {fn_name} */"
    idx = c_src.find(marker)
    check(idx >= 0, f"missing {marker!r} in emitted C")
    rest = c_src[idx + len(marker):]
    nxt = rest.find("/* fn ")
    nxt_m = rest.find("/* meth ")
    ends = [p for p in (nxt, nxt_m) if p >= 0]
    return rest[:min(ends)] if ends else rest


# 1. End-to-end differential conformance (-O0 vs -O1 vs -O2) + ASan/UBSan/LSan:
#    - Canonical field layout with out-of-order initializers preserving source eval order
#    - Single-level & nested 2-level SRA in hot loops
#    - Self-referential struct swap (p = Point { x: p.y, y: p.x })
#    - Exception atomicity on throwing struct reassignment inside try/catch
#    - Pattern matching (closed + open) on SRA-decomposed structs
#    - String interpolation: "{p.x},{p.y}" (scalarized) vs "{p}" (escaping)
#    - Escape soundness: aliasing, mutating callee, closure capture, vec storage, duplicate let
#    - Gradual typing: unshaped Map (std.json.parse) + mistyped Dyn passed to `p: Point`
#    - Heap fields (`str`, `vec[i64]`), move-out, and `arena { ... }` under LSan
e2e_src = """import std.json

struct Point {
  x: i64,
  y: i64,
}

struct Vec3 {
  x: i64,
  y: i64,
  z: i64,
}

struct Ray {
  orig: Vec3,
  dir: Vec3,
}

struct Packet {
  tag: str,
  items: vec[i64],
  weight: i64,
}

impl Point {
  fn shifted(self, dx: i64, dy: i64) -> Point {
    return Point { x: self.x + dx, y: self.y + dy }
  }
  fn bump(mut self, d: i64) -> i64 {
    self.x = self.x + d
    self.y = self.y - d
    return self.x * 100 + self.y
  }
}

fn eval_order() -> str {
  let mut log: vec[str] = []
  let push_ret = fn(lbl, v) {
    log.push(lbl)
    return v
  }
  // Out-of-order literal on non-escaping SRA struct
  let a = Point { y: push_ret("a.y", 20), x: push_ret("a.x", 10) }
  // Out-of-order literal on escaping heap struct (must share canonical shape with in-order)
  let b = Point { y: push_ret("b.y", 20), x: push_ret("b.x", 10) }
  let c = Point { x: 10, y: 20 }
  let eq_bc = b == c
  return "{log[0]},{log[1]},{log[2]},{log[3]}|{a.x},{a.y}|{b.x},{b.y}|{eq_bc}"
}

fn nested_ray_loop(n: i64) -> i64 {
  let mut acc = 0
  let mut i = 0
  while i < n {
    let mut r = Ray {
      orig: Vec3 { x: i, y: i + 1, z: i + 2 },
      dir: Vec3 { z: 3, x: 1, y: 2 },
    }
    r.orig.x = r.orig.x + r.dir.x
    r.orig.y = r.orig.y + r.dir.y
    acc = acc + r.orig.x * 3 + r.orig.y * 5 + r.orig.z * r.dir.z
    i = i + 1
  }
  return acc
}

fn swap_and_atomic(boom_at: i64) -> str {
  let mut p = Point { x: 3, y: 7 }
  let mut i = 0
  while i < 5 {
    p = Point { x: p.y + 1, y: p.x * 2 }
    i = i + 1
  }
  let before = "{p.x}:{p.y}"
  let explode = fn(k) {
    if k == 1 { return 1 / (k - 1) }
    return 999
  }
  try {
    p = Point { x: 888, y: explode(boom_at) }
  } catch _e {
    // p.x must remain untouched when field 2 throws!
    return "{before}|caught:{p.x}:{p.y}"
  }
  return "{before}|ok:{p.x}:{p.y}"
}

fn match_and_interp(v: i64) -> str {
  let p = Point { x: v, y: v + 4 }
  let mut m1 = 0
  match p {
    Point { x: 0, y } => { m1 = y }
    Point { x, y } => { m1 = x * 10 + y }
  }
  let mut m2 = 0
  match p {
    Point { x, .. } => { m2 = x + 100 }
  }
  let s_non_esc = "{p.x}-{p.y}"
  let q = Point { x: 1, y: 2 }
  let s_esc = "{q}"
  return "{m1}|{m2}|{s_non_esc}|{len(s_esc) > 0}"
}

fn bump_ext(mut p: Point) {
  p.x = p.x + 50
}

fn escape_semantics() -> str {
  // 1. Local alias must observe in-place mutation
  let mut a = Point { x: 10, y: 20 }
  let mut alias = a
  alias.x = 77
  let r1 = a.x

  // 2. Passing to mutating function must mutate caller's struct in place
  let mut b = Point { x: 5, y: 6 }
  bump_ext(b)
  let r2 = b.x

  // 3. Closure capture
  let c = Point { x: 9, y: 11 }
  let rd = fn() { return c.x + c.y }
  let r3 = rd()

  // 4. Duplicate let in sibling blocks
  let mut r4 = 0
  if r1 > 0 {
    let d = Point { x: 4, y: 5 }
    r4 = r4 + d.x
  }
  if r2 > 0 {
    let d = Point { x: 6, y: 7 }
    r4 = r4 + d.y
  }
  return "{r1}|{r2}|{r3}|{r4}"
}

fn dyn_passthrough(x) {
  return x
}

fn read_point_typed(mut p: Point) -> str {
  let s = "{p.x},{p.y}"
  p.x = p.x + 100
  return "{s}->{p.x}"
}

fn gradual_safety() -> str {
  let parsed = std.json.parse("{\\"y\\": 42, \\"x\\": 17}").unwrap()
  let ok_res = read_point_typed(dyn_passthrough(parsed))
  let mut bad_res = "missed"
  try {
    let _bad = read_point_typed(dyn_passthrough(12345))
  } catch e {
    bad_res = "caught"
  }
  return "{ok_res}|{bad_res}"
}

fn heap_and_arena_sra() -> str {
  let mut sum = 0
  let mut last = ""
  let mut i = 0
  while i < 200 {
    let mut pkt = Packet {
      tag: "pkt_" + str(i),
      items: [i, i + 1, i + 2],
      weight: i * 3,
    }
    pkt.tag = pkt.tag + "_ok"
    let moved_tag = pkt.tag
    sum = sum + pkt.weight + len(pkt.items) + len(moved_tag)
    if i == 199 {
      last = moved_tag
    }
    i = i + 1
  }
  let mut arena_sum = 0
  arena {
    let mut j = 0
    while j < 100 {
      let p = Point { x: j * 2, y: j + 3 }
      arena_sum = arena_sum + p.x + p.y
      j = j + 1
    }
  }
  return "{sum}|{last}|{arena_sum}"
}

fn pure_dot(ax: i64, ay: i64, bx: i64, by: i64) -> i64 {
  let a = Point { x: ax, y: ay }
  let b = Point { x: bx, y: by }
  return a.x * b.x + a.y * b.y
}

fn main() -> i64 {
  print(eval_order())
  print(nested_ray_loop(1000))
  print(swap_and_atomic(1))
  print(swap_and_atomic(0))
  print(match_and_interp(5))
  print(escape_semantics())
  print(gradual_safety())
  print(heap_and_arena_sra())
  let mut p = Point { x: 4, y: 9 }.shifted(2, 3)
  print("{p.bump(5)}|{pure_dot(3, 4, 5, 6)}")
  print("struct-sra-all-ok")
  return 0
}
"""

prog_path = write_src("e2e.brat", e2e_src)
chk = run_brat("check", prog_path)
check(chk.returncode == 0, f"check failed:\n{chk.stdout}\n{chk.stderr}")

outs = {}
for opt_flag in ("-O0", "-O1", "-O2"):
    r = run_brat("run", opt_flag, prog_path)
    check(r.returncode == 0, f"run {opt_flag} failed:\n{r.stdout}\n{r.stderr}")
    outs[opt_flag] = r.stdout

check(outs["-O0"] == outs["-O1"] == outs["-O2"],
      f"-O0/-O1/-O2 mismatch:\nO0={outs['-O0']!r}\nO1={outs['-O1']!r}\nO2={outs['-O2']!r}")

lines = outs["-O2"].strip().splitlines()
check(lines[0] == "a.y,a.x,b.y,b.x|10,20|10,20|true", f"unexpected eval_order: {lines[0]!r}")
check(lines[2] == "35:30|caught:35:30", f"exception atomicity broken: {lines[2]!r}")
check(lines[3] == "35:30|ok:888:999", f"normal Set broken: {lines[3]!r}")
check(lines[4] == "59|105|5-9|true", f"match_and_interp broken: {lines[4]!r}")
check(lines[5] == "77|55|20|11", f"escape_semantics broken: {lines[5]!r}")
check(lines[6] == "17,42->117|caught", f"gradual_safety broken: {lines[6]!r}")
check(lines[-1] == "struct-sra-all-ok", f"missing final sentinel: {lines[-1]!r}")


# 2. Verify emitted C properties:
#    - `-O0` uses direct slot helpers (`vstruct_slot_ptr` / `vstruct_slot_set` / `vstruct_slot_val`)
#    - `-O2` completely eliminates `vmap_shaped` / `vmap(` in `nested_ray_loop` and `pure_dot`,
#      and unlocks `_fast` scalar C specialization for `pure_dot`!
em_o0 = run_brat("emit-c", "-O0", prog_path)
check(em_o0.returncode == 0, f"emit-c -O0 failed: {em_o0.stderr}")
read_pt_o0 = fn_body(em_o0.stdout, "read_point_typed")
check("vstruct_slot_ptr(" in read_pt_o0, "missing vstruct_slot_ptr in -O0 C output")
check("vstruct_slot_set(" in read_pt_o0, "missing vstruct_slot_set in -O0 C output")
check("_fic[" not in read_pt_o0, f"read_point_typed unexpectedly used _fic inline cache:\n{read_pt_o0}")

em_o2 = run_brat("emit-c", "-O2", prog_path)
check(em_o2.returncode == 0, f"emit-c -O2 failed: {em_o2.stderr}")
ray_c = fn_body(em_o2.stdout, "nested_ray_loop")
check("vmap_shaped" not in ray_c and "vmap(" not in ray_c,
      f"nested_ray_loop still allocates Map at -O2:\n{ray_c}")
dot_c = fn_body(em_o2.stdout, "pure_dot")
check("vmap_shaped" not in dot_c and "bf_f" in dot_c,
      f"pure_dot missing zero-alloc scalar bf_f specialization at -O2:\n{dot_c}")


# 3. Verify `--why-opt` contract:
#    - `-O1 --why-opt` does NOT emit `sra:` lines (preserves existing -O1 proof counts)
#    - `-O2 --why-opt` reports `sra: scalarized non-escaping struct` with clean user names
why_src = write_src("why_sra.brat", """struct Pair {
  a: i64,
  b: i64,
}
fn sum_pair(x: i64) -> i64 {
  let p = Pair { a: x, b: x + 1 }
  return p.a + p.b
}
fn main() -> i64 {
  print(sum_pair(10))
  return 0
}
""")
w_o1 = run_cxx("cc", "-O1", "--why-opt", why_src)
check(w_o1.returncode == 0 and "sra:" not in w_o1.stderr,
      f"-O1 --why-opt unexpectedly emitted sra proof: {w_o1.stderr!r}")
w_o2 = run_cxx("cc", "-O2", "--why-opt", why_src)
check(w_o2.returncode == 0 and "sra: scalarized non-escaping struct `Pair` local `p` (2 fields)" in w_o2.stderr,
      f"-O2 --why-opt missing expected sra proof: {w_o2.stderr!r}")
check("_sra_" not in w_o2.stderr, f"--why-opt leaked synthetic _sra_ identifier: {w_o2.stderr!r}")


# 4. ASan + UBSan + LSan verification on emitted `-O2` C (and `-Wunused` cleanliness!)
if sys.platform.startswith("linux"):
    c_file = write_src("e2e_o2.c", em_o2.stdout)
    bin_out = os.path.join(TMP, "e2e_asan")
    cc_bin = os.environ.get("CC", "cc")
    eng_inc = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "engine"))
    comp = subprocess.run(
        [cc_bin, "-std=c11", "-O1", "-g1", "-Wall", "-Wno-unused-function",
         "-Werror=unused-const-variable", "-fsanitize=address,undefined",
         "-I", eng_inc, c_file, "-lm", "-lpthread", "-o", bin_out],
        capture_output=True, text=True, timeout=120,
    )
    check(comp.returncode == 0, f"ASan/Wall compile failed:\n{comp.stdout}\n{comp.stderr}")
    asan_env = dict(ENV, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    ar = subprocess.run([bin_out], capture_output=True, text=True, timeout=60, env=asan_env)
    check(ar.returncode == 0 and ar.stdout == outs["-O2"],
          f"ASan/LSan run failed: rc={ar.returncode}\nout={ar.stdout}\nerr={ar.stderr}")

print(f"ok: {n} struct layout & SRA checks passed")
