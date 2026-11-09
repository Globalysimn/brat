"""Canonical fixed-slot struct layout, direct slot access, escape analysis, and SRA tests."""
import os
import subprocess
import sys

import _cxx
import _scratch

NATIVE = _cxx.build()
CXX = NATIVE
TMP = _scratch.mkdtemp(prefix="brat-struct-sra-")
ENV = dict(os.environ, BRAT_FILES="1")

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
  let mut promoted_field = ""
  let mut promoted_whole = Packet { tag: "", items: [], weight: 0 }
  arena {
    let mut j = 0
    while j < 100 {
      let p = Point { x: j * 2, y: j + 3 }
      arena_sum = arena_sum + p.x + p.y
      j = j + 1
    }
    // SRA-decomposed struct inside arena whose heap field is promoted via own(h1.tag)
    let h1 = Packet {
      tag: "long_arena_header_payload_" + str(123456),
      items: [10, 20, 30, 40],
      weight: 42,
    }
    promoted_field = own(h1.tag)
    arena {
      let mut k = 0
      while k < 30 {
        let inner = Packet {
          tag: "inner_nested_arena_chunk_" + str(k),
          items: [k, k + 1],
          weight: k * 2,
        }
        arena_sum = arena_sum + inner.weight + len(inner.tag) + len(inner.items)
        k = k + 1
      }
      // Whole struct escaping into own(whole) inside nested arena (must NOT be broken by SRA)
      let whole = Packet { tag: "whole_" + str(99), items: [7, 8], weight: 99 }
      promoted_whole = own(whole)
    }
  }
  // Loop with `continue` out of `arena` + immediate pooled BChunk overwrite on next iteration
  let mut c = 0
  while c < 4 {
    arena {
      if c == 1 {
        let overwrite = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]
        arena_sum = arena_sum + len(overwrite)
      }
      let transient = Packet {
        tag: "transient_arena_payload_" + str(c),
        items: [c, c + 1],
        weight: c + 10,
      }
      if c == 0 {
        c = c + 1
        continue
      }
      arena_sum = arena_sum + transient.weight + len(transient.tag)
    }
    c = c + 1
  }
  return "{sum}|{last}|{arena_sum}|{promoted_field}|{promoted_whole.tag}:{promoted_whole.weight}:{len(promoted_whole.items)}"
}

fn pure_dot(ax: i64, ay: i64, bx: i64, by: i64) -> i64 {
  let a = Point { x: ax, y: ay }
  let b = Point { x: bx, y: by }
  return a.x * b.x + a.y * b.y
}

struct Envelope {
  inner: Packet,
  label: str,
}

fn cf_take_str(s: move str) -> i64 {
  return len(s)
}

fn cf_maybe_err(fail: bool, v: i64) -> Result[i64, str] {
  if fail {
    return Err("cf_err_" + str(v))
  }
  return Ok(v + 10)
}

fn cf_sra_rebind(flag: bool, k: i64) -> i64 {
  let mut p = Packet { tag: "hello_world_" + str(k), items: [k, k + 1], weight: k }
  if flag {
    let _ = cf_take_str(move p.tag)
  }
  p = Packet { tag: "rebound_" + str(k), items: [k + 2], weight: k + 1 }
  return len(p.tag) + len(p.items) + p.weight
}

fn cf_nested_rebind(flag: bool, k: i64) -> i64 {
  let mut env = Envelope {
    inner: Packet { tag: "sub_" + str(k), items: [k], weight: k },
    label: "env_" + str(k),
  }
  if flag {
    let _ = cf_take_str(move env.inner.tag)
  }
  env.inner = Packet { tag: "fresh_sub_" + str(k), items: [k, k + 1], weight: k + 2 }
  return len(env.inner.tag) + len(env.label) + env.inner.weight + len(env.inner.items)
}

fn cf_sra_early_ret(flag: bool, k: i64) -> i64 {
  let mut p = Packet { tag: "sra_ret_" + str(k), items: [k], weight: k }
  if flag {
    let m = cf_take_str(move p.tag)
    return m + p.weight
  }
  return len(p.tag) + p.weight
}

fn cf_early_ret_before_move(early: bool, k: i64) -> i64 {
  let x = "allocated_early_" + str(k)
  if early {
    return k
  }
  return cf_take_str(move x)
}

fn cf_quest_before_move(fail: bool, k: i64) -> Result[i64, str] {
  let x = "allocated_quest_" + str(k)
  let v = cf_maybe_err(fail, k)?
  let n = cf_take_str(move x)
  return Ok(v + n)
}

fn cf_loop_rebind_after_move(iters: i64) -> i64 {
  let mut s = "initial_" + str(iters)
  let mut acc = cf_take_str(move s)
  let mut i = 0
  while i < iters {
    s = "iter_payload_" + str(i)
    acc = acc + len(s)
    i = i + 1
  }
  return acc
}

fn cf_loop_break_after_rebind(flag: bool, k: i64) -> i64 {
  let mut z = "pre_" + str(k)
  let mut acc = cf_take_str(move z)
  let mut i = 0
  while i < 2 {
    i = i + 1
    z = "fresh_in_loop_" + str(k + i)
    if flag {
      acc = acc + cf_take_str(move z)
    } else {
      break
    }
  }
  return acc
}

fn cf_part_return_fallthrough(flag: bool, k: i64) -> str {
  let mut v = ["first_" + str(k), "second_" + str(k)]
  if flag {
    return v[0]
  }
  v = ["replaced_" + str(k)]
  return v[0]
}

fn cf_boom_str(fail: bool, k: i64) -> str {
  if fail {
    let a = [1]
    return str(a[5])
  }
  return "ok_" + str(k)
}

fn cf_try_catch_rebind(fail: bool, k: i64) -> i64 {
  let mut s = "init_try_" + str(k)
  let _ = cf_take_str(move s)
  let mut total = 0
  try {
    s = "rebound_before_throw_" + str(k)
    let r = cf_boom_str(fail, k)
    total = total + len(r) + cf_take_str(move s)
  } catch _e {
    s = "recovered_" + str(k)
    total = total + len(s)
  }
  return total
}

fn cf_move_and_rebind() -> i64 {
  let mut sum = 0
  sum = sum + cf_sra_rebind(true, 10) + cf_sra_rebind(false, 10)
  sum = sum + cf_nested_rebind(true, 20) + cf_nested_rebind(false, 20)
  sum = sum + cf_sra_early_ret(true, 30) + cf_sra_early_ret(false, 30)
  sum = sum + cf_early_ret_before_move(true, 40) + cf_early_ret_before_move(false, 40)
  let r1 = cf_quest_before_move(true, 50)
  let r2 = cf_quest_before_move(false, 50)
  if r1.is_err() { sum = sum + 1 }
  sum = sum + r2.unwrap_or(0)
  sum = sum + cf_loop_rebind_after_move(0) + cf_loop_rebind_after_move(5)
  sum = sum + cf_loop_break_after_rebind(true, 60) + cf_loop_break_after_rebind(false, 60)
  sum = sum + len(cf_part_return_fallthrough(true, 70)) + len(cf_part_return_fallthrough(false, 70))
  sum = sum + cf_try_catch_rebind(true, 80) + cf_try_catch_rebind(false, 80)
  return sum
}

enum BoxPair {
  PairBox(str, str),
}

fn quest_struct_unwind(k: i64) -> Result[Packet, str] {
  let pkt = Packet {
    tag: "quest_struct_payload_" + str(k),
    items: [k, cf_maybe_err(true, k)?],
    weight: k + 5,
  }
  return Ok(pkt)
}

fn quest_cmp_unwind(k: i64) -> Result[bool, str] {
  let eq = [k, k + 1, k + 2] == [cf_maybe_err(true, k)?]
  return Ok(eq)
}

fn unwind_partial_containers(k: i64) -> str {
  let mut caught = 0
  let mut tail_ran = 0
  let bump_tail = fn() {
    tail_ran = tail_ran + 1
    return "tail"
  }
  try {
    let v = ["vec_head_long_payload_" + str(k), cf_boom_str(true, k), bump_tail()]
    caught = caught + len(v)
  } catch _e {
    caught = caught + 1
  }
  try {
    let m1 = {"a": "shaped_map_long_payload_" + str(k), "b": cf_boom_str(true, k)}
    caught = caught + len(m1)
  } catch _e {
    caught = caught + 1
  }
  try {
    let m2 = {"dyn_k1_" + str(k): "dyn_v1_long_" + str(k), "dyn_k2_" + str(k): cf_boom_str(true, k)}
    caught = caught + len(m2)
  } catch _e {
    caught = caught + 1
  }
  try {
    let bx = PairBox("enum_long_payload_" + str(k), cf_boom_str(true, k))
    match bx {
      PairBox(a, b) => { caught = caught + len(a) + len(b) }
    }
  } catch _e {
    caught = caught + 1
  }
  try {
    let s = ("lhs_long_heap_prefix_exceeding_sso_" + str(k)) + cf_boom_str(true, k)
    caught = caught + len(s)
  } catch _e {
    caught = caught + 1
  }
  if quest_struct_unwind(k).is_err() { caught = caught + 1 }
  if quest_cmp_unwind(k).is_err() { caught = caught + 1 }
  return "{caught}:{tail_ran}"
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
  print(cf_move_and_rebind())
  print(unwind_partial_containers(99))
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
check(lines[-3] == "536", f"cf_move_and_rebind broken: {lines[-3]!r}")
check(lines[-2] == "7:0", f"unwind_partial_containers broken: {lines[-2]!r}")
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


# 4. ASan + UBSan + LSan verification on emitted `-O0` and `-O2` C (and `-Wunused` cleanliness!)
if sys.platform.startswith("linux"):
    cc_bin = os.environ.get("CC", "cc")
    eng_inc = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "engine"))
    asan_env = dict(ENV, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    for tag, emitted in (("o0", em_o0.stdout), ("o2", em_o2.stdout)):
        c_file = write_src(f"e2e_{tag}.c", emitted)
        bin_out = os.path.join(TMP, f"e2e_asan_{tag}")
        comp = subprocess.run(
            [cc_bin, "-std=c11", "-O1", "-g1", "-Wall", "-Wno-unused-function",
             "-Werror=unused-const-variable", "-fsanitize=address,undefined",
             "-I", eng_inc, c_file, "-lm", "-lpthread", "-o", bin_out],
            capture_output=True, text=True, timeout=120,
        )
        check(comp.returncode == 0, f"ASan/Wall compile ({tag}) failed:\n{comp.stdout}\n{comp.stderr}")
        ar = subprocess.run([bin_out], capture_output=True, text=True, timeout=60, env=asan_env)
        check(ar.returncode == 0 and ar.stdout == outs["-O2"],
              f"ASan/LSan ({tag}) run failed: rc={ar.returncode}\nout={ar.stdout}\nerr={ar.stderr}")

    # 5. C FFI (`extern struct` by-value + contiguous pointer memory with `--header` verification) alongside SRA
    ffi_h = write_src("ffi_vec.h", """#include <stdint.h>
typedef struct CVec2 {
    int64_t x;
    int64_t y;
} CVec2;
CVec2 c_vec2_step(CVec2 v, int64_t scale);
CVec2 *c_vec2_alloc(void);
void c_vec2_free(CVec2 *p);
int64_t c_vec2_ptr_sum(const CVec2 *vp);
""")
    ffi_c = write_src("ffi_vec.c", """#include "ffi_vec.h"
#include <stdlib.h>
CVec2 c_vec2_step(CVec2 v, int64_t scale) {
    CVec2 out = { v.x * scale, v.y * scale + 1 };
    return out;
}
CVec2 *c_vec2_alloc(void) { return (CVec2 *)calloc(1, sizeof(CVec2)); }
void c_vec2_free(CVec2 *p) { free(p); }
int64_t c_vec2_ptr_sum(const CVec2 *vp) {
    return vp->x * 100 + vp->y;
}
""")
    ffi_o = os.path.join(TMP, "ffi_vec.o")
    comp_o = subprocess.run([cc_bin, "-std=c11", "-O2", "-c", ffi_c, "-o", ffi_o],
                            capture_output=True, text=True, timeout=60)
    check(comp_o.returncode == 0, f"ffi_vec.c compile failed: {comp_o.stderr}")

    ffi_brat = write_src("ffi_sra.brat", """extern struct CVec2 {
  x: i64,
  y: i64,
}
extern fn c_vec2_step(v: CVec2, scale: i64) -> CVec2
extern fn c_vec2_alloc() -> ptr[CVec2]
extern fn c_vec2_free(mut p: ptr[CVec2])
extern fn c_vec2_ptr_sum(vp: ptr[CVec2]) -> i64

struct LocalPair {
  a: i64,
  b: i64,
}

fn main() -> i64 {
  let lp = LocalPair { a: 3, b: 4 }
  let cv = CVec2 { x: lp.a, y: lp.b }
  unsafe {
    let stepped = c_vec2_step(cv, 10)
    let mut buf = c_vec2_alloc()
    ptr_write_struct(buf, stepped, "CVec2")
    let psum = c_vec2_ptr_sum(buf)
    let round = ptr_read_struct(buf, "CVec2")
    c_vec2_free(buf)
    print("{stepped.x},{stepped.y}|{psum}|{round.x},{round.y}")
  }
  return 0
}
""")
    for fl in ("-O0", "-O1", "-O2"):
        ffi_exe = os.path.join(TMP, f"ffi_sra_{fl[1:]}")
        b_res = run_cxx("build", fl, "--header", ffi_h, "--object", ffi_o, ffi_brat, "-o", ffi_exe)
        check(b_res.returncode == 0, f"FFI build {fl} failed:\n{b_res.stdout}\n{b_res.stderr}")
        r_res = subprocess.run([ffi_exe], capture_output=True, text=True, timeout=30, env=ENV)
        check(r_res.returncode == 0 and r_res.stdout.strip() == "30,41|3041|30,41",
              f"FFI run {fl} mismatch: rc={r_res.returncode} out={r_res.stdout!r} err={r_res.stderr!r}")

copy_sra_src = write_src("copy_sra.brat", """struct Pair {
  x: i64,
  y: i64,
}
fn copy_loop(n: i64) -> i64 {
  let mut p = Pair { x: 1, y: 2 }
  let mut acc = 0
  for i in 0..n {
    let mut q = p.copy()
    q.x = q.x + i
    p = q.copy()
    acc = acc + p.x + p.y
  }
  return acc
}
fn main() -> i64 {
  print(copy_loop(10))
  return 0
}
""")
em_copy = run_cxx("cc", "-O2", copy_sra_src)
check(em_copy.returncode == 0, f"copy_sra emit failed: {em_copy.stderr}")
cb_copy = fn_body(em_copy.stdout, "copy_loop")
check("bmap_alloc" not in cb_copy and "vmap" not in cb_copy,
      f"copy_loop failed to scalarize .copy() structs:\n{cb_copy}")
r_copy_o0 = run_brat("run", "-O0", copy_sra_src)
r_copy_o2 = run_brat("run", "-O2", copy_sra_src)
check(r_copy_o0.returncode == 0 and r_copy_o0.stdout == r_copy_o2.stdout,
      f"copy_sra -O0/-O2 mismatch: {r_copy_o0.stdout!r} vs {r_copy_o2.stdout!r}")

shadow_src = write_src("lexical_shadow.brat", """struct Boxed {
  tag: str,
}
fn run_shadows(flag: bool) -> i64 {
  let x = [10, 20, 30]
  let mut acc = 0
  if flag {
    let x = Boxed { tag: "inner_struct_" + str(len(x)) }
    acc = acc + len(x.tag)
  } else {
    let x = 99
    acc = acc + x
  }
  acc = acc + len(x)
  for elem in [4, 5] {
    let x = elem
    acc = acc + x
  }
  acc = acc + len(x)
  let opt: Option[i64] = Some(7)
  match opt {
    Some(x) => { acc = acc + x },
    None => {},
  }
  return acc + len(x)
}
fn main() -> i64 {
  print(run_shadows(true))
  print(run_shadows(false))
  return 0
}
""")
r_sh_o0 = run_brat("run", "-O0", shadow_src)
r_sh_o2 = run_brat("run", "-O2", shadow_src)
check(r_sh_o0.returncode == 0 and r_sh_o0.stdout == "39\n124\n",
      f"lexical_shadow -O0 unexpected output: {r_sh_o0.stdout!r} {r_sh_o0.stderr!r}")
check(r_sh_o2.returncode == 0 and r_sh_o2.stdout == r_sh_o0.stdout,
      f"lexical_shadow -O0/-O2 mismatch: {r_sh_o0.stdout!r} vs {r_sh_o2.stdout!r}")

shadow_bad_ty = write_src("shadow_bad_ty.brat", """struct Secret {
  token: str,
}
fn leak_int(flag: bool) -> Secret {
  let x = 999
  if flag {
    let x = Secret { token: "ok" }
  }
  return x
}
fn main() -> i64 {
  return 0
}
""")
chk_bad = run_cxx("check", shadow_bad_ty)
check(chk_bad.returncode != 0 and "return: want Secret, got i64" in (chk_bad.stdout + chk_bad.stderr),
      f"branch shadow leaked type into outer scope: {chk_bad.stdout!r} {chk_bad.stderr!r}")

print(f"ok: {n} struct layout & SRA checks passed")
