import _cxx
import _scratch
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

ENV = dict(os.environ)
ENV["PYTHONIOENCODING"] = "utf-8"

n = 0
TMP = _scratch.mkdtemp(prefix="brat-own-")
FAILED = []


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def _run(files):
    paths = []
    for name in sorted(files):
        p = os.path.join(TMP, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(files[name])
        paths.append(p)
    return subprocess.run(_cxx.cli_args("check", *(paths)),
                          capture_output=True, text=True, encoding="utf-8",
                          timeout=120, env=ENV)


REJECT = []

REJECT.append(("a01_vec_push_on_immutable_vec", {"m.brat": """fn main() -> i64 {
  let v = [1, 2]
  let w = v
  v.push(99)
  print(len(w))
  return 0
}
"""}, "immutable"))

REJECT.append(("a02_map_set_on_immutable_map", {"m.brat": """fn main() -> i64 {
  let m = {"a": 1}
  m.set("b", 2)
  print(m["b"])
  return 0
}
"""}, "immutable"))

REJECT.append(("a03_vec_sort_on_immutable_vec", {"m.brat": """fn main() -> i64 {
  let v = [3, 1, 2]
  v.sort()
  print(v[0])
  return 0
}
"""}, "immutable"))

REJECT.append(("a04_vec_push_on_const_vec", {"m.brat": """fn main() -> i64 {
  const vv = [1, 2]
  vv.push(3)
  print(len(vv))
  return 0
}
"""}, "const"))

REJECT.append(("a05_struct_field_index_on_immutable", {"m.brat": """fn main() -> i64 {
  let p = Holder { items: [1, 2] }
  p.items[0] = 9
  print(p.items[0])
  return 0
}
"""}, "immutable"))

REJECT.append(("a06_nested_map_index_on_immutable", {"m.brat": """fn main() -> i64 {
  let m = {"a": {"b": 1}}
  m["a"]["b"] = 2
  print(m["a"]["b"])
  return 0
}
"""}, "immutable"))

REJECT.append(("a07_const_struct_field_index", {"m.brat": """fn main() -> i64 {
  const p = Point { items: [1, 2] }
  p.items[0] = 5
  print(p.items[0])
  return 0
}
"""}, "const"))

REJECT.append(("a08_const_vec_index_uppercase_base", {"m.brat": """fn main() -> i64 {
  const P = [1, 2]
  P[0] = 5
  print(P[0])
  return 0
}
"""}, "const"))


REJECT.append(("a12_spawn_vec_element_borrow", {"m.brat": """fn main() -> i64 {
  let s = "x"
  let r = s
  let v = [r]
  spawn { print(v[0]) }
  join_all()
  return 0
}
"""}, "escapes via spawn"))


REJECT.append(("a15_move_in_while_body", {"m.brat": """fn main() -> i64 {
  let v = "x"
  let mut i = 0
  while i < 3 {
    i = i + 1
    let w = move v
    print(w)
  }
  return 0
}
"""}, "move"))

REJECT.append(("a16_move_in_forrange_body", {"m.brat": """fn main() -> i64 {
  let v = "x"
  for i in 0..3 {
    let w = move v
    print(w)
  }
  return 0
}
"""}, "move"))

REJECT.append(("a17_move_in_foriter_body", {"m.brat": """fn main() -> i64 {
  let v = "x"
  for c in v {
    let w = move v
    print(w)
  }
  return 0
}
"""}, "move"))

REJECT.append(("a18_double_move_in_while_body", {"m.brat": """fn main() -> i64 {
  let mut v = [1, 2]
  let mut i = 0
  while i < 2 {
    i = i + 1
    let w = move v
    print(w)
  }
  return 0
}
"""}, "move"))

REJECT.append(("a19_move_in_loop_branch_read_in_else", {"m.brat": """fn main(flag: bool) -> i64 {
  let mut v = [1, 2]
  let mut i = 0
  while i < 3 {
    i = i + 1
    if flag {
      let r = v
      print(r)
    } else {
      move v
    }
  }
  return 0
}
"""}, "move"))

REJECT.append(("a20_partial_move_then_whole_move", {"m.brat": """fn eat(p: move Point) -> i64 {
  print(p.x)
  return 1
}

fn main() -> i64 {
  let p = Point { x: 1, y: 2 }
  let a = move p.x
  print(a)
  let r = eat(p)
  return r
}
"""}, "move"))

REJECT.append(("a21_loe_projection_twice", {"m.brat": """fn f(a: mut i64, b: mut i64) -> i64 {
  a = a + b
  return a
}

fn main() -> i64 {
  let mut p = Point { x: 1, y: 2 }
  let r = f(p.x, p.x)
  return r
}
"""}, "as mut"))

REJECT.append(("a22_loe_nested_calls", {"m.brat": """fn g(a: mut i64) -> i64 {
  a = a + 1
  return a
}

fn f(a: mut i64, b: mut i64) -> i64 {
  a = a + b
  b = b + 1
  return a
}

fn main() -> i64 {
  let mut x = 1
  let r = f(g(x), g(x))
  return r
}
"""}, "as mut"))

REJECT.append(("a23_mut_param_projection_on_immutable_base", {"m.brat": """fn f(a: mut i64, b: mut i64) -> i64 {
  a = a + b
  return a
}

fn main() -> i64 {
  let p = Point { x: 1, y: 2 }
  let r = f(p.x, p.y)
  return r
}
"""}, "immutable"))

REJECT.append(("a24_element_borrow_then_move_then_use", {"m.brat": """fn main() -> i64 {
  let v = [1, 2, 3]
  let y = v[0]
  let w = move v
  print(y)
  print(w[0])
  return 0
}
"""}, "while borrowed"))

REJECT.append(("a25_mut_self_on_immutable_receiver", {"m.brat": """impl Box {
  fn set(mut self) -> i64 {
    self.v = 99
    return self.v
  }
}

fn main() -> i64 {
  let b = Box { v: 1 }
  print(b.set())
  print(b.v)
  return 0
}
"""}, "immutable"))

REJECT.append(("a26_immutable_laundered_through_mut_alias", {"m.brat": """fn main() -> i64 {
  let C = "s"
  let mut y = C
  y = "t"
  print(y)
  return 0
}
"""}, "immutable"))

REJECT.append(("a27_const_laundered_through_mut_alias", {"m.brat": """fn main() -> i64 {
  const k = "s"
  let mut y = k
  y = "t"
  print(y)
  return 0
}
"""}, "const"))

REJECT.append(("a28_write_through_mut_alias_mutates_source", {"m.brat": """fn main() -> i64 {
  let mut v = [1, 2]
  let mut y = v
  y[0] = 99
  print(y[0])
  print(v[0])
  return 0
}
"""}, "borrow"))

REJECT.append(("a29_spawn_write_mut_capture_short", {"m.brat": """fn main() -> i64 {
  let mut counter = 0
  for i in 0..10 {
    spawn { counter = counter + 1 }
  }
  join_all()
  print(counter)
  return 0
}
"""}, "spawn"))

REJECT.append(("a30_spawn_write_mut_capture_block", {"m.brat": """fn main() -> i64 {
  let mut counter = 0
  spawn {
    let mut local = 1
    local = local + 1
    print(local)
    counter = 5
  }
  join_all()
  print(counter)
  return 0
}
"""}, "spawn"))

REJECT.append(("a31_pub_trait_missing_method", {"m.brat": """pub trait Shout {
  fn loud(self) -> i64
}

impl Shout for Point {
  fn quiet(self) -> i64 {
    return self.x
  }
}

fn main() -> i64 {
  let p = Point { x: 1, y: 2 }
  return p.quiet()
}
"""}, "missing loud"))

REJECT.append(("a32_borrow_live_across_for_then_move", {"m.brat": """fn main() -> i64 {
  let mut v = [1, 2]
  let r = v
  for i in 0..3 {
    let w = move v
    print(w)
  }
  print(r)
  return 0
}
"""}, "while borrowed"))

REJECT.append(("a33_borrow_live_across_while_then_move", {"m.brat": """fn main() -> i64 {
  let mut v = [1, 2]
  let r = v
  let mut i = 0
  while i < 3 {
    i = i + 1
    let w = move v
    print(w)
  }
  print(r)
  return 0
}
"""}, "while borrowed"))

# A fn name is a callback argument, never a value: the backends have no function
# values, so this is a checker error, not a run-time NameError. The module case
# is the same defect with a module short name in the way, which is how it was
# found: `import lib.m` plus `fn m` used to pass `check` and die in `cc`.
REJECT.append(("a34_fn_name_as_value", {"m.brat": """fn double_it(x: i64) -> i64 {
  return x * 2
}

fn main() -> i64 {
  print(double_it)
  return 0
}
"""}, "as a value"))

REJECT.append(("a35_fn_name_beside_module", {"m.brat": """import lib.m

fn m() -> i64 {
  return 1
}

fn main() -> i64 {
  print(m)
  return 0
}
""", "lib/m.brat": """pub fn h() -> i64 {
  return 7
}
"""}, "as a value"))

REJECT.append(("a36_module_name_as_value", {"m.brat": """import lib.m

fn main() -> i64 {
  print(m)
  return 0
}
""", "lib/m.brat": """pub fn h() -> i64 {
  return 7
}
"""}, "use of undeclared var `m`"))

# A method call does not make the name in front of it a value either:
# `helper.push(1)` passed `check` and then failed in the C compiler.
REJECT.append(("a38_fn_name_as_receiver", {"m.brat": """fn helper(x) {
  return x
}

fn main() -> i64 {
  helper.push(1)
  return 0
}
"""}, "as a value"))


ACCEPT = []

ACCEPT.append(("a37_fn_name_in_callback_slot", {"m.brat": """fn dbl(x) {
  return x * 2
}

fn keep(x) {
  return x > 1
}

fn add(a, b) {
  return a + b
}

fn main() -> i64 {
  let v = [1, 2, 3]
  print(v.map(dbl))
  print(v.filter(keep))
  print(v.fold(0, add))
  return 0
}
"""}))

ACCEPT.append(("a11_return_vec_element_borrow_via_main", {"m.brat": """fn wrap(a) -> i64 {
  let v = [a]
  return len(v[0])
}

fn main() -> i64 {
  let s = "hello"
  print(wrap(s))
  return 0
}
"""}))

ACCEPT.append(("a09_return_vec_element_borrow", {"m.brat": """fn f(a) -> i64 {
  let v = [a]
  return v[0]
}

fn main() -> i64 {
  print(f("x"))
  return 0
}
"""}))
ACCEPT.append(("a10_return_struct_field_borrow", {"m.brat": """fn f(a) -> i64 {
  let p = Point { x: a }
  return p.x
}

fn main() -> i64 {
  print(f(1))
  return 0
}
"""}))
ACCEPT.append(("a13_return_param_bare", {"m.brat": """fn f(a) -> i64 {
  return a
}

fn main() -> i64 {
  print(f(1))
  return 0
}
"""}))
ACCEPT.append(("a14_return_param_identity_and_struct", {"m.brat": """fn identity(a) -> i64 {
  return a
}

fn wrapstruct(a) -> i64 {
  let p = Point { x: a }
  return len(p.x)
}

fn main() -> i64 {
  let s = "hello"
  print(identity(s))
  print(wrapstruct(s))
  return 0
}
"""}))

ACCEPT.append(("b01_borrow_in_for_dead_then_assign", {"m.brat": """fn main() -> i64 {
  let mut v = [1, 2]
  for i in 0..3 {
    let r = v
    print(r)
  }
  v[0] = 9
  print(v[0])
  return 0
}
"""}))

ACCEPT.append(("b02_borrow_in_for_dead_then_move", {"m.brat": """fn main() -> i64 {
  let v = "x"
  for i in 0..2 {
    let r = v
    print(r)
  }
  let m = move v
  print(m)
  return 0
}
"""}))

ACCEPT.append(("b03_mut_borrow_in_for_dead_then_use_and_assign", {"m.brat": """fn main() -> i64 {
  let mut v = [1, 2]
  for i in 0..2 {
    let mut r = v
    print(r)
  }
  print(v[0])
  v[0] = 9
  print(v[0])
  return 0
}
"""}))

ACCEPT.append(("b04_unused_borrow_holder_then_move", {"m.brat": """fn main() -> i64 {
  let s = "x"
  let _r = s
  let m = move s
  print(m)
  return 0
}
"""}))

ACCEPT.append(("b05_unused_borrow_holder_then_assign", {"m.brat": """fn main() -> i64 {
  let mut v = [1, 2]
  let _r = v
  v[0] = 9
  print(v[0])
  return 0
}
"""}))

ACCEPT.append(("b06_borrow_in_while_dead_then_assign", {"m.brat": """fn main() -> i64 {
  let mut v = [1, 2]
  let mut i = 0
  while i < 3 {
    let r = v
    print(r)
    i = i + 1
  }
  v[0] = 9
  print(v[0])
  return 0
}
"""}))

ACCEPT.append(("b07_move_in_then_use_in_else", {"m.brat": """fn main(flag: bool) -> i64 {
  let mut v = [1, 2]
  if flag {
    let w = move v
    print(w)
  } else {
    print(v[0])
  }
  return 0
}
"""}))

ACCEPT.append(("b08_defer_move_then_use", {"m.brat": """fn main() -> i64 {
  let v = "x"
  defer move v
  print(v)
  return 0
}
"""}))

ACCEPT.append(("b09_user_fn_named_ref_is_borrow", {"m.brat": """fn ref(x) -> i64 {
  return len(x)
}

fn main() -> i64 {
  let v = [1, 2]
  let mut r = ref(v)
  let mut r2 = ref(v)
  print(r)
  print(r2)
  return 0
}
"""}))

ACCEPT.append(("b10_user_fn_named_ref_shared_twice", {"m.brat": """fn ref(x) -> i64 {
  return len(x)
}

fn main() -> i64 {
  let v = [1, 2]
  print(ref(v))
  print(ref(v))
  return 0
}
"""}))


REJECT.append(("c01_use_after_move", {"m.brat": """fn main() -> i64 {
  let s = "x"
  let m = move s
  print(m)
  print(s)
  return 0
}
"""}, "use of moved"))

REJECT.append(("c02_double_move", {"m.brat": """fn main() -> i64 {
  let a = "x"
  let b = move a
  let c = move a
  print(b)
  print(c)
  return 0
}
"""}, "use of moved"))

REJECT.append(("c03_use_moved_inside_if_branch", {"m.brat": """fn main(flag: bool) -> i64 {
  let v = [1, 2]
  let w = move v
  if flag {
    print(v[0])
  }
  return 0
}
"""}, "use of moved"))

REJECT.append(("c04_use_moved_in_while_and_for", {"m.brat": """fn main() -> i64 {
  let v = [1, 2]
  let w = move v
  while len(v) > 0 {
    print(1)
  }
  for x in v {
    print(x)
  }
  return 0
}
"""}, "use of moved"))

REJECT.append(("c05_assign_immutable_plain", {"m.brat": """fn main() -> i64 {
  let x = 1
  x = 2
  return x
}
"""}, "immutable"))

REJECT.append(("c06_assign_immutable_vec_index", {"m.brat": """fn main() -> i64 {
  let v = [1, 2, 3]
  v[0] = 9
  print(v[0])
  return 0
}
"""}, "immutable"))

REJECT.append(("c07_assign_immutable_map_index", {"m.brat": """fn main() -> i64 {
  let m = {"a": 1}
  m["a"] = 2
  print(m["a"])
  return 0
}
"""}, "immutable"))

REJECT.append(("c08_assign_immutable_field", {"m.brat": """fn main() -> i64 {
  let p = Point { x: 1, y: 2 }
  p.x = 9
  print(p.x)
  return 0
}
"""}, "immutable"))

REJECT.append(("c09_assign_const_plain", {"m.brat": """fn main() -> i64 {
  const X = 5
  X = 6
  return X
}
"""}, "assign to const"))

REJECT.append(("c10_assign_const_vec_index", {"m.brat": """fn main() -> i64 {
  const v = [1, 2]
  v[0] = 5
  print(v[0])
  return 0
}
"""}, "assign to const"))

REJECT.append(("c11_assign_const_map_index", {"m.brat": """fn main() -> i64 {
  const q = {"a": 1}
  q["a"] = 2
  print(q["a"])
  return 0
}
"""}, "assign to const"))

REJECT.append(("c12_assign_const_field", {"m.brat": """fn main() -> i64 {
  const p = Point { x: 1, y: 2 }
  p.x = 5
  return p.x
}
"""}, "assign to const"))

REJECT.append(("c13_double_mut_borrow", {"m.brat": """fn main() -> i64 {
  let mut m = "s"
  let mut a = m
  let mut b = m
  print(a)
  print(b)
  return 0
}
"""}, "mut borrow"))

REJECT.append(("c14_loe_two_mut_params_same_var", {"m.brat": """fn f(a: mut i64, b: mut i64) -> i64 {
  a = a + b
  b = b + a
  return a
}

fn main() -> i64 {
  let mut x = 1
  let r = f(x, x)
  return r
}
"""}, "as mut"))

ACCEPT.append(("c15_ref_escape_via_return", {"m.brat": """fn leak(a) -> i64 {
  let r = a
  return r
}

fn main() -> i64 {
  print(leak("x"))
  return 0
}
"""}))

REJECT.append(("c16_ref_escape_via_spawn", {"m.brat": """fn main() -> i64 {
  let s = "x"
  let r = s
  spawn { print(r) }
  join_all()
  return 0
}
"""}, "escapes via spawn"))

REJECT.append(("c19_local_borrow_cannot_escape_closure", {"m.brat": """fn main() -> i64 {
  let s = "x"
  let r = s
  let f = fn() { return r }
  print(f())
  return 0
}
"""}, "ref `r` escapes via return"))

REJECT.append(("c17_undeclared_call", {"m.brat": """fn main() -> i64 {
  nosuchfn(1)
  return 0
}
"""}, "use of undeclared var"))

REJECT.append(("c18_undeclared_inside_expression", {"m.brat": """fn main() -> i64 {
  let x = [1, 2]
  print(len(x))
  print(len(nope))
  print(nope2[0])
  return 0
}
"""}, "use of undeclared var"))

REJECT.append(("c19_cross_file_pub_move_signature_double_move", {
    "m.brat": """import xmod

fn main() -> i64 {
  let h = "hi"
  let a = pubtake(h)
  let b = pubtake(h)
  print(a)
  print(b)
  return 0
}
""",
    "xmod.brat": """pub fn pubtake(s: move str) -> i64 {
  print(s)
  return 2
}
"""}, "use of moved"))

REJECT.append(("c20_cross_file_private_fn_not_visible", {
    "m.brat": """import xmod

fn main() -> i64 {
  let h = "hi"
  let a = privtake(h)
  return a
}
""",
    "xmod.brat": """fn privtake(s: move str) -> i64 {
  print(s)
  return 1
}

pub fn pubtake(s: move str) -> i64 {
  print(s)
  return 2
}
"""}, "use of undeclared var"))

REJECT.append(("c21_private_trait_missing_method", {"m.brat": """trait Shout {
  fn loud(self) -> i64
}

impl Shout for Point {
  fn quiet(self) -> i64 {
    return self.x
  }
}

fn main() -> i64 {
  let p = Point { x: 1, y: 2 }
  return p.quiet()
}
"""}, "missing loud"))

REJECT.append(("c22_move_param_then_use_caller_side", {"m.brat": """fn eat(s: move str) -> i64 {
  print(s)
  return 1
}

fn main() -> i64 {
  let h = "hi"
  let r = eat(h)
  print(h)
  return r
}
"""}, "use of moved"))

REJECT.append(("c23_move_while_borrowed_used_after", {"m.brat": """fn main() -> i64 {
  let s = "x"
  let r = s
  let m = move s
  print(r)
  print(m)
  return 0
}
"""}, "while borrowed"))

REJECT.append(("c24_move_then_use_as_ref_param", {"m.brat": """fn eat(s: move str) -> i64 {
  print(s)
  return 1
}

fn peek(s: ref str) -> i64 {
  print(s)
  return 0
}

fn main() -> i64 {
  let h = "hi"
  let r = eat(h)
  print(h)
  let q = peek(h)
  return q
}
"""}, "use of moved"))

REJECT.append(("c29_move_out_of_borrowed_parameter", {"m.brat": """fn consume(v: vec[i64]) -> i64 {
  let owned = move v
  return len(owned)
}
"""}, "cannot move borrowed parameter"))

REJECT.append(("c30_move_out_of_borrowed_parameter_alias", {"m.brat": """fn consume(v: vec[i64]) -> i64 {
  let alias = v
  let owned = move alias
  return len(owned)
}
"""}, "cannot move borrowed parameter"))

REJECT.append(("c25_mut_param_on_immutable_source", {"m.brat": """fn g(a: mut str) -> i64 {
  return 1
}

fn main() -> i64 {
  let s = "x"
  let r = s
  let q = g(s)
  print(r)
  print(q)
  return 0
}
"""}, "immutable"))

REJECT.append(("c26_compound_assign_immutable_vec_index", {"m.brat": """fn main() -> i64 {
  let v = [1, 2]
  v[0] += 9
  print(v[0])
  return 0
}
"""}, "immutable"))

REJECT.append(("c27_compound_assign_immutable_scalar", {"m.brat": """fn main() -> i64 {
  let x = 1
  x += 2
  return x
}
"""}, "immutable"))

REJECT.append(("c28_move_struct_then_use_field", {"m.brat": """fn main() -> i64 {
  let p = Point { x: 1, y: 2 }
  let q = move p
  print(p.x)
  return 0
}
"""}, "use of moved"))

REJECT.append(("c29_global_move_then_use", {"m.brat": """let s = "global"

fn f() -> i64 {
  print(s)
  return 1
}

fn g(x: move str) -> i64 {
  print(x)
  return 2
}

fn main() -> i64 {
  print(f())
  print(f())
  let r = g(s)
  print(s)
  return r
}
"""}, "use of moved"))

REJECT.append(("c30_setidx_on_moved_value", {"m.brat": """fn main() -> i64 {
  let mut v = [1, 2, 3]
  let w = move v
  v[0] = 9
  print(w[0])
  return 0
}
"""}, "use of moved"))

REJECT.append(("c31_setfield_on_moved_struct", {"m.brat": """struct Holder {
  s: str,
  n: i64,
}

fn main() -> i64 {
  let mut h = Holder { s: "a", n: 1 }
  let moved_h = move h
  h.s = "b"
  print(moved_h.n)
  return 0
}
"""}, "use of moved"))

REJECT.append(("c32_setfield_under_moved_substruct", {"m.brat": """struct Inner {
  s: str,
}

struct Outer {
  inner: Inner,
}

fn main() -> i64 {
  let mut o = Outer { inner: Inner { s: "a" } }
  let sub = move o.inner
  o.inner.s = "b"
  print(sub.s)
  return 0
}
"""}, "use of moved"))


ACCEPT.append(("d01_copy_scalar_flow_through_annotated_param", {"m.brat": """fn f(n: i64) -> i64 {
  let x = n
  return x
}

fn main() -> i64 {
  return f(7)
}
"""}))

ACCEPT.append(("d02_borrow_dies_before_move", {"m.brat": """fn main() -> i64 {
  let s = "x"
  let r = s
  print(r)
  let m = move s
  print(m)
  return 0
}
"""}))

ACCEPT.append(("d03_nll_borrow_then_assign", {"m.brat": """fn main() -> i64 {
  let mut x = 1
  let y = x
  print(y)
  x = 2
  return x
}
"""}))

ACCEPT.append(("d04_two_readers_then_move", {"m.brat": """fn main() -> i64 {
  let s = "x"
  let r = s
  print(r)
  let t = s
  print(t)
  let m = move s
  print(m)
  return 0
}
"""}))

ACCEPT.append(("d05_borrow_in_while_body", {"m.brat": """fn main() -> i64 {
  let v = "x"
  while true {
    let r = v
    print(r)
  }
  return 0
}
"""}))

ACCEPT.append(("d06_borrows_in_both_if_branches", {"m.brat": """fn main() -> i64 {
  let mut m = "s"
  if true {
    let mut a = m
    print(a)
  } else {
    let mut b = m
    print(b)
  }
  return 0
}
"""}))

ACCEPT.append(("d07_forward_call", {"m.brat": """fn main() -> i64 {
  print(quux())
  return 0
}

fn quux() -> i64 {
  return 1
}
"""}))

ACCEPT.append(("d08_cross_file_pub_move_signature_once", {
    "m.brat": """import xmod

fn main() -> i64 {
  let h = "hi"
  let a = pubtake(h)
  print(a)
  return 0
}
""",
    "xmod.brat": """pub fn pubtake(s: move str) -> i64 {
  print(s)
  return 2
}
"""}))

ACCEPT.append(("d09_assign_through_mut_borrow_of_copy_scalar", {"m.brat": """fn main() -> i64 {
  let mut x = 1
  let mut y = x
  y = y + 1
  x = y
  return x
}
"""}))

ACCEPT.append(("d10_mut_self_on_mut_receiver", {"m.brat": """impl Box {
  fn set(mut self) -> i64 {
    self.v = 99
    return self.v
  }
}

fn main() -> i64 {
  let mut b = Box { v: 1 }
  print(b.set())
  return 0
}
"""}))

ACCEPT.append(("d11_read_only_alias_two_readers", {"m.brat": """fn main() -> i64 {
  let v = [1, 2]
  let w = v
  print(len(w))
  print(w[0])
  return 0
}
"""}))

ACCEPT.append(("d12_defer_read_then_read", {"m.brat": """fn main() -> i64 {
  let s = "x"
  defer print(s)
  print(s)
  return 0
}
"""}))

ACCEPT.append(("d13_module_file_with_pub_move_signature", {"xmod.brat": """fn eat(s: move str) -> i64 {
  print(s)
  return 1
}

pub fn pubtake(s: move str) -> i64 {
  print(s)
  return 2
}
"""}))

ACCEPT.append(("d14_reader_dies_then_write_through_owner", {"m.brat": """fn main() -> i64 {
  let mut v = [1, 2]
  let a = v
  print(a[0])
  print(len(a))
  v[0] = 9
  print(v[0])
  return 0
}
"""}))

ACCEPT.append(("d16_closure_capture_is_a_use", {"m.brat": """fn main() -> i64 {
  let n = 10
  let f = fn(x) { return x + n }
  print(f(5))
  return 0
}
"""}))

ACCEPT.append(("d15_move_param_can_transfer_ownership", {"m.brat": """fn consume(v: move vec[i64]) -> i64 {
  let owned = move v
  return len(owned)
}

fn main() -> i64 {
  let v = [1]
  return consume(v)
}
"""}))

ACCEPT.append(("d17_rebind_mut_var_after_move", {"m.brat": """fn main() -> i64 {
  let mut s = "first"
  let m = move s
  s = "second"
  print(m)
  print(s)
  return 0
}
"""}))

ACCEPT.append(("d18_rebind_parent_struct_after_field_move", {"m.brat": """struct Holder {
  s: str,
  n: i64,
}

fn main() -> i64 {
  let mut h = Holder { s: "first", n: 1 }
  let m = move h.s
  h = Holder { s: "second", n: 2 }
  print(m)
  print(h.s)
  return h.n
}
"""}))

ACCEPT.append(("d19_rebind_nested_substruct_after_leaf_move", {"m.brat": """struct Inner {
  s: str,
  n: i64,
}

struct Outer {
  inner: Inner,
  tag: str,
}

fn main() -> i64 {
  let mut o = Outer { inner: Inner { s: "leaf", n: 1 }, tag: "root" }
  let m = move o.inner.s
  o.inner = Inner { s: "rebound", n: 2 }
  print(m)
  print(o.inner.s)
  print(o.tag)
  return o.inner.n
}
"""}))


SUMMARY = re.compile(r": (\d+) error\(s\),")


def diags(out):
    return [ln.split(": error: ", 1)[1] for ln in out.splitlines() if ": error: " in ln]


def summaries(out):
    return [int(m) for m in SUMMARY.findall(out)]


def closure_capture_both():
    """A name read inside a closure is a use: neither checker warns about it.

    `vars_in` used to stop at a closure node, so a captured binding looked
    unused and `let n = 10` + `fn(x) { return x + n }` warned. A parameter of the
    same spelling is bound by the body, so the outer binding stays unused: the
    two halves of the rule are the same walk.
    """
    capture = """fn main() -> i64 {
  let n = 10
  let f = fn(x) { return x + n }
  print(f(5))
  return 0
}
"""
    shadow = """fn main() -> i64 {
  let n = 10
  let f = fn(n) { return n }
  print(f(5))
  return 0
}
"""
    for label, src, want_warn in (("capture", capture, False), ("shadow", shadow, True)):
        path = os.path.join(TMP, "cap_%s.brat" % label)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(src)
        outs = {}
        for tag, cmd in (("py", _cxx.cli_args("check", path)),
                         ("cxx", _cxx.cli_args("check", path))):
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=ENV)
            outs[tag] = ((r.stdout or "") + (r.stderr or "")).replace(path, "m.brat")
        for tag, out in outs.items():
            check(("unused var `n`" in out) == want_warn,
                  "closure %s %s: want unused-warning=%s, got:\n%s"
                  % (label, tag, want_warn, out[-400:]))
        check(outs["py"] == outs["cxx"],
              "closure %s: both checkers disagree\n--- py ---\n%s\n--- cxx ---\n%s"
              % (label, outs["py"][-400:], outs["cxx"][-400:]))
        print("closure %s: both checkers agree (%s)"
              % (label, "warns" if want_warn else "silent"))


def main():
    try:
        closure_capture_both()
        check(len(REJECT) >= 56, "want >=56 reject cases, have %d" % len(REJECT))
        check(len(ACCEPT) >= 24, "want >=24 accept cases, have %d" % len(ACCEPT))
        names = [c[0] for c in REJECT] + [c[0] for c in ACCEPT]
        check(len(names) == len(set(names)), "duplicate test name in ownership suite")

        for name, files, want in REJECT:
            try:
                p = _run(files)
                out = (p.stdout or "") + (p.stderr or "")
                check(p.returncode != 0,
                      "own reject %s: exit=0, program wrongly ACCEPTED\n%s" % (name, out[-600:]))
                ds = diags(out)
                check(any(want in d for d in ds),
                      "own reject %s: want %r in a diagnostic, got %r\n%s"
                      % (name, want, ds, out[-600:]))
                print("own reject: %s (%s)" % (name, want))
            except AssertionError as e:
                FAILED.append(str(e))

        for name, files in ACCEPT:
            try:
                p = _run(files)
                out = (p.stdout or "") + (p.stderr or "")
                check(p.returncode == 0,
                      "own accept %s: exit=%d\n%s" % (name, p.returncode, out[-600:]))
                cs = summaries(out)
                check(bool(cs) and all(c == 0 for c in cs),
                      "own accept %s: ownership summaries %r, want all 0\n%s"
                      % (name, cs, out[-600:]))
                print("own accept: %s" % name)
            except AssertionError as e:
                FAILED.append(str(e))
    finally:
        shutil.rmtree(TMP, ignore_errors=True)

    if FAILED:
        print("")
        print("OWNERSHIP FAILED (%d of %d)" % (len(FAILED), len(REJECT) + len(ACCEPT)))
        for m in FAILED:
            print("-" * 70)
            print(m)
        raise AssertionError("OWNERSHIP FAILED (%d cases)" % len(FAILED))
    print("OWNERSHIP ALL GREEN (%d asserts)" % n)


main()