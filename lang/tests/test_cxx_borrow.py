"""Native ownership contracts with explicit diagnostics, counts and notes."""

import _cxx
import _scratch

import argparse
import os
import subprocess
import sys
import time

BIN = _cxx.BIN
TMP = _scratch.mkdtemp(prefix="brat-borrow-")

OWNERSHIP = {
    'alias_conflict.brat': 'fn f(v: vec[i64]) -> i64 {\n  let mut w = v\n  w.push(1)\n  return 0\n}\n',
    'alias_const.brat': 'const LIMIT = [1]\nfn f() -> i64 {\n  let mut L = LIMIT\n  L.push(2)\n  return 0\n}\n',
    'assign_while_borrowed.brat': 'fn f(mut v: vec[i64]) -> i64 {\n  let a = v\n  v = [2]\n  return len(a)\n}\n',
    'borrowed_move.brat': 'fn f(v: vec[i64]) -> i64 {\n  let owned = move v\n  return len(owned)\n}\n',
    'const_assign.brat': 'const LIMIT = 10\nfn f() -> i64 {\n  LIMIT = 3\n  return 0\n}\n',
    'consume_close.brat': 'fn f(fd: i64) -> i64 {\n  tcp_close(fd)\n  tcp_close(fd)\n  return 0\n}\n',
    'cut_call.brat': 'fn f(e: map[str, i64]) -> i64 {\n  print("{e["hits"]}")\n  return 0\n}\n',
    'cut_plus.brat': 'fn f() -> str {\n  let n = 1\n  return "{e[" + str(n) + "]}"\n}\n',
    'double_mut.brat': 'fn f(mut v: vec[i64]) -> i64 {\n  let mut a = v\n  let mut b = v\n  print(a)\n  print(b)\n  a.push(1)\n  return 0\n}\n',
    'dup_fn.brat': ('fn h() -> i64 { return 1 }\nfn h() -> i64 { return 2 }\n'
                    'fn f(c: bool) -> i64 {\n  fn g() -> i64 { return 1 }\n'
                    '  fn g() -> i64 { return 2 }\n  let n = g() + h()\n  if c {\n'
                    '    fn side() -> i64 { return 1 }\n    return n + side()\n  } else {\n'
                    '    fn side() -> i64 { return 2 }\n    return n + side()\n  }\n}\n'),
    'dup_method.brat': 'impl Point {\n  fn a(self) -> i64 { return 0 }\n  fn a(self) -> i64 { return 1 }\n}\n',
    'elem_copy.brat': 'fn f(v: vec[i64]) -> i64 {\n  let x = v[0]\n  return x\n}\n',
    'escape_return.brat': 'fn f() -> vec[i64] {\n  let v = [1]\n  let r = v\n  return r\n}\n',
    'closure_escape.brat': 'fn main() -> i64 {\n  let s = \"x\"\n  let r = s\n  let f = fn() { return r }\n  print(f())\n  return 0\n}\n',
    'closure_capture_used.brat': 'fn main() -> i64 {\n  let n = 10\n  let f = fn(x) { return x + n }\n  print(f(5))\n  return 0\n}\n',
    'closure_fn.brat': ('fn main() -> i64 {\n  let g = fn(x) {\n    fn inc(y) { return y + 1 }\n'
                        '    return inc(x)\n  }\n  print(g(0))\n  return 0\n}\n'),
    'closure_shadow.brat': 'fn main() -> i64 {\n  let n = 10\n  let f = fn(n) { return n }\n  print(f(5))\n  return 0\n}\n',
    'if_move.brat': 'fn f(c: bool) -> i64 {\n  let v = [1]\n  if c {\n    move v\n  }\n  return len(v)\n}\n',
    'immutable_set.brat': 'fn f() -> i64 {\n  let s = Point { x: 1 }\n  s.x = 2\n  return 0\n}\n',
    'leak_owned.brat': 'extern "c" fn mk() -> move ptr[i64]\nfn f() -> i64 {\n  let p = mk()\n  return 0\n}\n',
    'iter_push.brat': ('fn f() -> i64 {\n  let mut v = [1, 2]\n  for x in v {\n'
                       '    v.push(x)\n  }\n  return 0\n}\n'),
    'iter_set.brat': ('fn f() -> i64 {\n  let mut m = {"a": 1}\n  for k in m {\n'
                      '    m.set(k + "b", 2)\n  }\n  return 0\n}\n'),
    'iter_index_write.brat': ('fn f() -> i64 {\n  let mut v = [1, 2]\n  for x in v {\n'
                              '    v[0] = x\n  }\n  return 0\n}\n'),
    'iter_mut_param.brat': ('fn add(x: mut vec[i64]) -> i64 {\n  x.push(1)\n  return 0\n}\n'
                            'fn f() -> i64 {\n  let mut v = [1, 2]\n  for x in v {\n'
                            '    add(v)\n  }\n  return 0\n}\n'),
    'iter_nested_outer.brat': ('fn f() -> i64 {\n  let mut v = [1, 2]\n  let mut w = [3]\n'
                               '  for x in v {\n    for y in w {\n      v.push(y)\n    }\n'
                               '  }\n  return 0\n}\n'),
    'iter_ok_range.brat': ('fn f() -> i64 {\n  let mut v = [1, 2]\n  for i in 0..3 {\n'
                           '    v.push(i)\n  }\n  return len(v)\n}\n'),
    'iter_ok_copy.brat': ('fn f() -> i64 {\n  let mut v = [1, 2]\n  for x in v.copy() {\n'
                          '    v.push(x)\n  }\n  return len(v)\n}\n'),
    'iter_ok_collect.brat': ('fn f() -> i64 {\n  let mut v = [1, 2]\n  let mut add = []\n'
                             '  for x in v {\n    add.push(x)\n  }\n  for a in add {\n'
                             '    v.push(a)\n  }\n  return len(v)\n}\n'),
    'loe.brat': 'fn f(mut p: Point, mut q: Point) -> i64 {\n  set2(p, p)\n  return 0\n}\nfn set2(mut a: Point, mut b: Point) -> i64 {\n  return 0\n}\n',
    'loop_move.brat': 'fn f(mut v: vec[i64]) -> i64 {\n  while len(v) > 0 {\n    move v\n  }\n  return 0\n}\n',
    'missing_method.brat': 'trait Shape {\n  fn area(self) -> i64\n  fn name(self) -> str\n}\nimpl Shape for Point {\n  fn area(self) -> i64 {\n    return 0\n  }\n}\n',
    'move_borrowed.brat': 'fn f(mut v: vec[i64]) -> i64 {\n  let a = v\n  move v\n  return len(a)\n}\n',
    'moved_path.brat': 'fn f() -> i64 {\n  let p = Point { x: 1 }\n  let a = move p.x\n  return p.x + a\n}\n',
    'moved_use.brat': 'fn f(v: vec[i64]) -> i64 {\n  move v\n  return len(v)\n}\n',
    'owned_move.brat': 'fn f(v: move vec[i64]) -> i64 {\n  let owned = move v\n  return len(owned)\n}\n',
    'mut_self.brat': 'impl Point {\n  fn bump(mut self) -> i64 {\n    return 0\n  }\n}\nfn f() -> i64 {\n  let p = Point { x: 1 }\n  print(p.bump())\n  return 0\n}\n',
    'mut_while_borrowed.brat': 'fn f(mut v: vec[i64]) -> i64 {\n  let a = v\n  v.push(1)\n  return len(a)\n}\n',
    'own_return.brat': 'extern "c" fn mk() -> move ptr[i64]\nfn f() -> ptr[i64] {\n  let p = mk()\n  return p\n}\n',
    'param_escape.brat': 'fn f(v: vec[i64]) -> vec[i64] {\n  let r = v\n  return r\n}\n',
    'pass_immutable.brat': 'fn g(mut v: vec[i64]) -> i64 {\n  return 0\n}\nfn f(v: vec[i64]) -> i64 {\n  g(v)\n  return 0\n}\n',
    'pass_mut_borrowed.brat': 'fn g(mut v: vec[i64]) -> i64 {\n  return 0\n}\nfn f(mut v: vec[i64]) -> i64 {\n  let a = v\n  g(v)\n  return len(a)\n}\n',
    'reserved_method.brat': 'impl Point {\n  fn push(self, x: i64) -> i64 {\n    return 0\n  }\n}\n',
    'send_move.brat': 'fn f(move v: vec[i64], ch: Chan) -> i64 {\n  ch.send(v)\n  return len(v)\n}\n',
    'send_param.brat': 'fn f(v: vec[i64], ch: Chan) -> i64 {\n  ch.send(v)\n  return 0\n}\n',
    'spawn_capture.brat': 'fn f() -> i64 {\n  let mut x = 1\n  spawn {\n    x = 2\n  }\n  return x\n}\n',
    'spawn_ref.brat': 'fn f(v: vec[i64]) -> i64 {\n  let a = v\n  spawn {\n    print(a)\n  }\n  return 0\n}\n',
    'spawn_fn.brat': ('fn f() -> i64 {\n  spawn {\n    fn one() -> i64 { return 1 }\n'
                      '    print(one())\n  }\n  return 0\n}\n'),
    'spawn_return.brat': 'fn f() -> i64 {\n  let v = [1]\n  spawn {\n    return 1\n  }\n  return v[0]\n}\n',
    'top_fn.brat': ('if true {\n  fn inner() -> i64 { return 1 }\n  print(inner())\n}\n'
                    'let f = fn(k) {\n  fn boxed(n) { return [n] }\n  return boxed(k)\n}\n'
                    'print(f(1))\n'),
    'try_move.brat': 'fn f() -> i64 {\n  let v = [1]\n  try {\n    move v\n  } catch e {\n    print(e)\n  }\n  return len(v)\n}\n',
    'typo.brat': 'fn f() -> i64 {\n  return pritn(1)\n}\n',
    'undeclared.brat': 'fn f() -> i64 {\n  s = 1\n  return 0\n}\n',
    'unknown_trait.brat': 'impl Shape for Point {\n  fn area(self) -> i64 {\n    return 0\n  }\n}\n',
    'unsafe_move.brat': 'fn f(v: vec[i64]) -> i64 {\n  unsafe {\n    move v\n  }\n  return len(v)\n}\n',
    'unused.brat': 'fn f() -> i64 {\n  let x = 1\n  let y = 2\n  return 0\n}\n',
}

# Every line the reference prints for the fixture, path shortened to its name.
# Sources and count lines included: a warning that turns into an error is a diff.
EXPECT = {
    'alias_conflict.brat': [
        'alias_conflict.brat: 2 error(s), 0 warning(s)',
        'alias_conflict.brat:2: error: cannot borrow immutable `v` as mut (`let mut w`)',
        'alias_conflict.brat:3: error: write through `w` mutates immutable `v`',
    ],
    'alias_const.brat': [
        'alias_const.brat: 1 error(s), 0 warning(s)',
        'alias_const.brat:3: error: assign to const `LIMIT` through mut borrow `L`',
    ],
    'assign_while_borrowed.brat': [
        'assign_while_borrowed.brat: 1 error(s), 0 warning(s)',
        'assign_while_borrowed.brat:3: error: cannot assign `v` while borrowed by `a`',
        'assign_while_borrowed.brat:2: note: borrowed by `a` here',
    ],
    'borrowed_move.brat': [
        'borrowed_move.brat: 1 error(s), 0 warning(s)',
        'borrowed_move.brat:2: error: cannot move borrowed parameter `v`: declare it `move` to take ownership',
    ],
    'const_assign.brat': [
        'const_assign.brat: 1 error(s), 0 warning(s)',
        'const_assign.brat:3: error: assign to const `LIMIT`',
    ],
    'consume_close.brat': [
        'consume_close.brat: 1 error(s), 0 warning(s)',
        'consume_close.brat:3: error: use of closed `fd` (hint: the handle was closed; open a new one instead of reusing this)',
        'consume_close.brat:2: note: `fd` closed here',
    ],
    'cut_call.brat': [
        'cut_call.brat: 1 error(s), 1 warning(s)',
        'cut_call.brat:2: error: use of undeclared var `hits`',
        'cut_call.brat:2: warning: string literal looks cut short: a `{...}` placeholder stops at a bare `"`, so `"{e[\\"hits\\"]}"` is not the placeholder it looks like (help: `"{e.hits}"`, or `let hits = e["hits"]` then `"{hits}"`; a quote inside a call is fine: `"{m.get(\\"hits\\")}"`)',
    ],
    'cut_plus.brat': [
        'cut_plus.brat: 0 error(s), 1 warning(s)',
        'cut_plus.brat:3: warning: string literal looks cut short: a `{...}` placeholder stops at a bare `"`, so `"{e[\\"hits\\"]}"` is not the placeholder it looks like (help: `"{e.hits}"`, or `let hits = e["hits"]` then `"{hits}"`; a quote inside a call is fine: `"{m.get(\\"hits\\")}"`)',
    ],
    'double_mut.brat': [
        'double_mut.brat: 3 error(s), 0 warning(s)',
        'double_mut.brat:3: error: cannot use `v` while mutably borrowed by `a` (hint: end the borrow first)',
        'double_mut.brat:2: note: mut borrow here',
        'double_mut.brat:3: error: double mut borrow of `v`: `b` clashes with `a`',
        'double_mut.brat:2: note: first borrow `a` here',
        'double_mut.brat:6: error: double mut borrow of `v`: `a.push` through alias `a` is a second mutable place',
        'double_mut.brat: note: help: `a` is a second mutable place over `v`; keep one writer -- write through the owner (`v[key][...] = ...`) instead of holding a mutating copy of one element',
    ],
    'dup_fn.brat': [
        'dup_fn.brat: 2 error(s), 0 warning(s)',
        'dup_fn.brat:2: error: duplicate fn `h`',
        'dup_fn.brat:1: note: first declared here',
        'dup_fn.brat:5: error: duplicate fn `g`',
        'dup_fn.brat:4: note: first declared here',
    ],
    'dup_method.brat': [
        'dup_method.brat: 1 error(s), 0 warning(s)',
        'dup_method.brat:2: error: duplicate method `Point.a`',
    ],
    'elem_copy.brat': [
        'elem_copy.brat: 0 error(s), 0 warning(s)',
    ],
    'escape_return.brat': [
        'escape_return.brat: 1 error(s), 0 warning(s)',
        'escape_return.brat:4: error: ref `r` escapes via return',
    ],
    'closure_escape.brat': [
        # `r` is read inside the closure on line 4, so "unused var `r`" was a
        # false positive: both checkers now walk a closure body (minus its own
        # parameters) when they build the used set. The escape error stays.
        'closure_escape.brat: 1 error(s), 0 warning(s)',
        'closure_escape.brat:4: error: ref `r` escapes via return',
    ],
    # A name read inside a closure is a use of the binding: no warning. A
    # parameter of the same spelling is bound by the body, so the outer `n`
    # stays unused and the warning stands. Both are the same walk.
    'closure_capture_used.brat': [
        'closure_capture_used.brat: 0 error(s), 0 warning(s)',
    ],
    'closure_fn.brat': [
        'closure_fn.brat: 0 error(s), 0 warning(s)',
    ],
    'closure_shadow.brat': [
        'closure_shadow.brat: 0 error(s), 1 warning(s)',
        'closure_shadow.brat:2: warning: unused var `n`',
    ],
    'if_move.brat': [
        'if_move.brat: 1 error(s), 0 warning(s)',
        'if_move.brat:6: error: use of moved `v` (hint: the value was moved; use a copy or re-bind)',
        'if_move.brat:4: note: `v` moved here',
    ],
    'immutable_set.brat': [
        'immutable_set.brat: 1 error(s), 0 warning(s)',
        'immutable_set.brat:3: error: assign to immutable `s.x` (want `let mut` or mut param)',
        'immutable_set.brat:2: note: `s` declared here without `mut` (help: `let mut s`)',
    ],
    'leak_owned.brat': [
        'leak_owned.brat: 1 error(s), 1 warning(s)',
        'leak_owned.brat:4: error: leaked owned pointer `p`: free it or return it',
        'leak_owned.brat:3: note: `p` allocated here',
        'leak_owned.brat:3: warning: unused var `p`',
    ],
    'loe.brat': [
        'loe.brat: 1 error(s), 0 warning(s)',
        'loe.brat:2: error: law of exclusivity: `p` passed as mut twice in one expression (clashes with `p` used by `a`)',
    ],
    'iter_index_write.brat': [
        'iter_index_write.brat: 1 error(s), 0 warning(s)',
        'iter_index_write.brat:4: error: cannot mutate `v` while iterating it (hint: collect the writes and apply them after the loop)',
        'iter_index_write.brat:3: note: iterated here by this `for`',
    ],
    'iter_mut_param.brat': [
        'iter_mut_param.brat: 1 error(s), 0 warning(s)',
        'iter_mut_param.brat:8: error: cannot mutate `v` while iterating it (hint: collect the writes and apply them after the loop)',
        'iter_mut_param.brat:7: note: iterated here by this `for`',
    ],
    'iter_nested_outer.brat': [
        'iter_nested_outer.brat: 1 error(s), 0 warning(s)',
        'iter_nested_outer.brat:6: error: cannot mutate `v` while iterating it (hint: collect the writes and apply them after the loop)',
        'iter_nested_outer.brat:4: note: iterated here by this `for`',
    ],
    'iter_ok_collect.brat': [
        'iter_ok_collect.brat: 0 error(s), 0 warning(s)',
    ],
    'iter_ok_copy.brat': [
        'iter_ok_copy.brat: 0 error(s), 0 warning(s)',
    ],
    'iter_ok_range.brat': [
        'iter_ok_range.brat: 0 error(s), 0 warning(s)',
    ],
    'iter_push.brat': [
        'iter_push.brat: 1 error(s), 0 warning(s)',
        'iter_push.brat:4: error: cannot mutate `v` while iterating it (hint: collect the writes and apply them after the loop)',
        'iter_push.brat:3: note: iterated here by this `for`',
    ],
    'iter_set.brat': [
        'iter_set.brat: 1 error(s), 0 warning(s)',
        'iter_set.brat:4: error: cannot mutate `m` while iterating it (hint: collect the writes and apply them after the loop)',
        'iter_set.brat:3: note: iterated here by this `for`',
    ],
    'loop_move.brat': [
        'loop_move.brat: 2 error(s), 0 warning(s)',
        'loop_move.brat:3: error: cannot move borrowed parameter `v`: declare it `move` to take ownership',
        'loop_move.brat:3: error: use of moved `v` (hint: the value was moved; use a copy or re-bind)',
        'loop_move.brat:3: note: `v` moved here',
    ],
    'missing_method.brat': [
        'missing_method.brat: 1 error(s), 0 warning(s)',
        'missing_method.brat:5: error: impl Shape for Point missing name',
    ],
    'move_borrowed.brat': [
        'move_borrowed.brat: 2 error(s), 0 warning(s)',
        'move_borrowed.brat:3: error: cannot move borrowed parameter `v`: declare it `move` to take ownership',
        'move_borrowed.brat:3: error: cannot move `v` while borrowed by `a`',
        'move_borrowed.brat:2: note: borrowed by `a` here',
    ],
    'moved_path.brat': [
        'moved_path.brat: 1 error(s), 0 warning(s)',
        'moved_path.brat:4: error: use of moved `p.x`',
    ],
    'moved_use.brat': [
        'moved_use.brat: 2 error(s), 0 warning(s)',
        'moved_use.brat:2: error: cannot move borrowed parameter `v`: declare it `move` to take ownership',
        'moved_use.brat:3: error: use of moved `v` (hint: the value was moved; use a copy or re-bind)',
        'moved_use.brat:2: note: `v` moved here',
    ],
    'mut_self.brat': [
        'mut_self.brat: 1 error(s), 0 warning(s)',
        'mut_self.brat:8: error: cannot mutate immutable `p`: `p.bump` takes mut self',
        'mut_self.brat:7: note: `p` declared here without `mut` (help: `let mut p`)',
    ],
    'mut_while_borrowed.brat': [
        'mut_while_borrowed.brat: 1 error(s), 0 warning(s)',
        'mut_while_borrowed.brat:3: error: cannot mutate immutable `v` while borrowed by `a`',
        'mut_while_borrowed.brat:2: note: borrowed by `a` here',
    ],
    'own_return.brat': [
        'own_return.brat: 0 error(s), 0 warning(s)',
    ],
    'owned_move.brat': [
        'owned_move.brat: 0 error(s), 0 warning(s)',
    ],
    'param_escape.brat': [
        'param_escape.brat: 0 error(s), 0 warning(s)',
    ],
    'pass_immutable.brat': [
        'pass_immutable.brat: 1 error(s), 0 warning(s)',
        'pass_immutable.brat:5: error: pass immutable `v` as mut param `v`',
    ],
    'pass_mut_borrowed.brat': [
        'pass_mut_borrowed.brat: 1 error(s), 0 warning(s)',
        'pass_mut_borrowed.brat:6: error: cannot pass `v` as mut while borrowed by `a`',
        'pass_mut_borrowed.brat:5: note: borrowed by `a` here',
    ],
    'reserved_method.brat': [
        'reserved_method.brat: 1 error(s), 0 warning(s)',
        'reserved_method.brat:1: error: method name push is reserved (built-in mutator)',
    ],
    'send_move.brat': [
        'send_move.brat: 1 error(s), 0 warning(s)',
        'send_move.brat:3: error: use of sent `v` (hint: a channel send hands the value to the receiver; send a copy if you need it back)',
        'send_move.brat:2: note: `v` sent here',
    ],
    'send_param.brat': [
        'send_param.brat: 1 error(s), 0 warning(s)',
        "send_param.brat:2: error: cannot send `v`: a parameter is a view of the caller's storage; declare it `move` or send a copy",
    ],
    'spawn_capture.brat': [
        'spawn_capture.brat: 1 error(s), 0 warning(s)',
        'spawn_capture.brat:4: error: assign to captured `x` in spawn (captured by copy, write is lost)',
    ],
    'spawn_ref.brat': [
        'spawn_ref.brat: 1 error(s), 0 warning(s)',
        'spawn_ref.brat:3: error: ref `a` escapes via spawn',
    ],
    'spawn_fn.brat': [
        'spawn_fn.brat: 0 error(s), 0 warning(s)',
    ],
    'spawn_return.brat': [
        'spawn_return.brat: 1 error(s), 0 warning(s)',
        'spawn_return.brat:4: error: return escapes spawn',
    ],
    'top_fn.brat': [
        'top_fn.brat: 2 error(s), 0 warning(s)',
        'top_fn.brat:2: error: fn `inner` is declared inside a top-level statement: declare it at the top level or inside a fn',
        'top_fn.brat:6: error: fn `boxed` is declared inside a top-level statement: declare it at the top level or inside a fn',
    ],
    'try_move.brat': [
        'try_move.brat: 1 error(s), 0 warning(s)',
        'try_move.brat:8: error: use of moved `v` (hint: the value was moved; use a copy or re-bind)',
        'try_move.brat:4: note: `v` moved here',
    ],
    'typo.brat': [
        'typo.brat: 1 error(s), 0 warning(s)',
        'typo.brat:2: error: use of undeclared var `pritn`',
        'typo.brat: note: help: did you mean `print`?',
    ],
    'undeclared.brat': [
        'undeclared.brat: 1 error(s), 0 warning(s)',
        'undeclared.brat:2: error: use of undeclared var `s`',
        'undeclared.brat: note: help: in scope here are `let` bindings, fn parameters, `for` bindings and `match` arm bindings; functions come from `fn`/`impl`; a name from another file needs `import that.file`',
    ],
    'unknown_trait.brat': [
        'unknown_trait.brat: 1 error(s), 0 warning(s)',
        'unknown_trait.brat:1: error: unknown trait Shape (declare it with `trait Shape { ... }`)',
    ],
    'unsafe_move.brat': [
        'unsafe_move.brat: 1 error(s), 0 warning(s)',
        'unsafe_move.brat:3: error: cannot move borrowed parameter `v`: '
        'declare it `move` to take ownership',
    ],
    'unused.brat': [
        'unused.brat: 0 error(s), 2 warning(s)',
        'unused.brat:2: warning: unused var `x`',
        'unused.brat:3: warning: unused var `y`',
    ],
}

TYPE_ERROR = "type error(s)"


def check(path):
    r = subprocess.run([BIN, "check", path], capture_output=True, text=True, encoding="utf-8", timeout=120)
    assert r.returncode in (0, 1), "brat-cxx check rc=%d %s" % (r.returncode, r.stderr[-300:])
    lines = [l.replace(path, os.path.basename(path)) for l in r.stdout.split("\n")]
    # The type pass has its own fixtures; the line that opens it is not ours.
    return [l for l in lines if l and TYPE_ERROR not in l and not l.startswith("  | ")], r.returncode


def main():
    _cxx.build()
    t0 = time.perf_counter()
    bad = 0

    for name in sorted(OWNERSHIP):
        path = os.path.join(TMP, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(OWNERSHIP[name])
        lines, rc = check(path)
        if lines != EXPECT[name]:
            bad += 1
            print("EXPECT %s\n  want %r\n  got  %r" % (name, EXPECT[name], lines))
        errors = int(EXPECT[name][0].split(":")[1].split()[0])
        if rc != (1 if errors else 0):
            bad += 1
            print("RC %s: rc=%d for %d errors" % (name, rc, errors))

    dt = time.perf_counter() - t0
    assert bad == 0, "%d differences" % bad
    print("CXX BORROW ALL GREEN (%d fixtures, every line pinned, %.2fs)"
          % (len(OWNERSHIP), dt))


if __name__ == "__main__":
    main()
