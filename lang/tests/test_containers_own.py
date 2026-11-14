"""Containers own their elements, under AddressSanitizer.

Every case fills a container inside a loop and drops it; the -O0 build and the
native binary must print the same thing and the binary must leave nothing
behind. These are the shapes that leaked while containers aliased their
elements: a vector literal, a map literal, a push of a fresh value, a slice, a
key list, concatenation, a struct field, a nested vector, an iterator over a
map, a captured binding, a capture handed to a spawned closure, an error
carried out of a function, and a value a function returns through an enum
constructor, a closure call or an uppercase binding.
"""
import _cxx
import _scratch

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-own-")

CASES = {}

CASES["vec_lit"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let v = ["a {i}", "b {i}"]
    n = n + len(v)
    i = i + 1
  }
  print(n)
  return 0
}
''', "200\n")

CASES["map_lit"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let m = {"k": "v {i}", "j": "w {i}"}
    n = n + len(m.get("k"))
    i = i + 1
  }
  print(n)
  return 0
}
''', "390\n")

CASES["push_fresh"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let mut v = []
    v.push("item {i}")
    n = n + len(v)
    i = i + 1
  }
  print(n)
  return 0
}
''', "100\n")

CASES["push_binding"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let s = "item {i}"
    let mut v = []
    v.push(s)
    n = n + len(v) + len(s)
    i = i + 1
  }
  print(n)
  return 0
}
''', "790\n")

CASES["slice"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let v = ["a {i}", "b {i}", "c {i}"]
    let w = v.slice(1, 3)
    n = n + len(w)
    i = i + 1
  }
  print(n)
  return 0
}
''', "200\n")

CASES["keys_concat"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let m = {"k {i}": "v", "j {i}": "w"}
    let ks = m.keys()
    let both = ks + ["z"]
    n = n + len(both)
    i = i + 1
  }
  print(n)
  return 0
}
''', "300\n")

CASES["struct_field"] = ('''extern struct P { x: f64, y: str }

fn mk(i: i64) -> P {
  let p = P { x: 1.0, y: "p {i}" }
  return p
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let p = mk(i)
    n = n + len(p.y)
    i = i + 1
  }
  print(n)
  return 0
}
''', "390\n")

CASES["nested"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let mut rows = []
    let mut r = []
    r.push("a {i}")
    rows.push(r)
    rows.push(["b {i}", "c {i}"])
    for row in rows {
      n = n + len(row)
    }
    i = i + 1
  }
  print(n)
  return 0
}
''', "300\n")

CASES["map_iter"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let m = {"a {i}": "1", "b {i}": "22"}
    for k in m {
      n = n + len(k)
    }
    let vs = m.vals()
    n = n + len(vs)
    i = i + 1
  }
  print(n)
  return 0
}
''', "980\n")

CASES["closure_capture"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let tag = "t {i}"
    let f = fn(x: i64) { return x + len(tag) }
    n = n + f(1)
    i = i + 1
  }
  print(n)
  return 0
}
''', "490\n")

# The frame hands its reference to the capture map and drops its own at the end
# of the iteration; a second capture inside the spawned closure does the same,
# so a wrong count frees the vector while `v` still names it.
CASES["spawn_capture"] = ("""fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let mut v = []
    let f = fn(k: i64) {
      spawn {
        v.push("x {k}")
      }
      join_all()
      return len(v)
    }
    n = n + f(i)
    i = i + 1
  }
  print(n)
  return 0
}
""", "100\n")

CASES["err_escape"] = ('''fn bad(i: i64) -> i64 {
  if i >= 0 {
    raise("boom {i}")
  }
  return 0
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    match bad(i) {
      Ok(v) => { n = n + v },
      Err(m) => { n = n + len(m) }
    }
    i = i + 1
  }
  print(n)
  return 0
}
''', "690\n")


# A closure literal built at the call site is a value nobody named: v.map(f) and
# v.filter(f) used to leave that value, the argument vector inside the helper and
# the result of the last iteration behind -- 3 448 B under LeakSanitizer.
CASES["map_filter_closure"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let v = [1, 2, 3, 4]
    let a = v.map(fn(x) { return x * 2 })
    let b = v.filter(fn(x) { return x - 2 })
    n = n + a[0] + len(b)
    i = i + 1
  }
  print(n)
  return 0
}
''', "500\n")


CASES["part_of_temp"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let lo = [[3], [1]].min()
    let hi = [[4, 5], [2]].max()
    let head = [7, 8, 9].first()
    let tail = [7, 8, 9].last()
    n = n + len(lo) + len(hi) + head + tail
    i = i + 1
  }
  print(n)
  return 0
}
''', "1900\n")


CASES["index_temp"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    n = n + len([[6], [2]][1])
    n = n + {"a": 5}["a"]
    n = n + len("abc"[0])
    i = i + 1
  }
  print(n)
  return 0
}
''', "700\n")

CASES["return_part"] = ('''fn head() -> i64 {
  let v = [7, 8, 9]
  return v[0]
}

fn pick() -> i64 {
  let m = {"a": 5}
  return m["a"]
}

fn first_word() -> str {
  let words = ["alpha", "beta"]
  return words[1]
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    n = n + head() + pick() + len(first_word())
    i = i + 1
  }
  print(n)
  return 0
}
''', "1600\n")

CASES["get_of_temp"] = ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    n = n + len({"a": [1, 2]}.get("a"))
    n = n + len({"b": "xy"}.get("b").upper())
    i = i + 1
  }
  print(n)
  return 0
}
''', "400\n")

CASES["part_in_literal"] = ('''fn main() -> i64 {
  let mut i = 0
  while i < 100 {
    print(["abc"[0], [[6], [2]][1]])
    i = i + 1
  }
  return 0
}
''', "['a', [2]]\n" * 100)


CASES["find_mutates_receiver"] = ('''fn main() -> i64 {
  let mut values = ["a_long_heap_string_" + str(42), "second_long_heap_str_" + str(7)]
  let found = values.find(fn(item) {
    values.pop()
    return true
  })
  print(found)
  print(len(values))
  return 0
}
''', "a_long_heap_string_42\n1\n")

CASES["filter_mutates_receiver"] = ('''fn main() -> i64 {
  let mut names = ["x" + str(1), "y" + str(2)]
  let picked = names.filter(fn(item) {
    names.pop()
    return true
  })
  print(picked)
  print(len(names))
  return 0
}
''', "['x1']\n1\n")

CASES["fold_mutates_receiver"] = ('''fn main() -> i64 {
  let mut items = ["p" + str(1), "q" + str(2)]
  let total = items.fold(0, fn(acc, item) {
    items.pop()
    return acc + 1
  })
  print(total)
  print(len(items))
  return 0
}
''', "1\n1\n")

CASES["map_mutates_receiver"] = ('''fn main() -> i64 {
  let mut rows = ["r" + str(1), "s" + str(2)]
  let sized = rows.map(fn(item) {
    rows.pop()
    return len(item)
  })
  print(sized)
  print(len(rows))
  return 0
}
''', "[2]\n1\n")


CASES["sort_by_mutates_receiver"] = ('''fn main() -> i64 {
  let mut names = ["alpha_long_heap_name_" + str(1), "beta_long_heap_name_" + str(2)]
  let mut caught = false
  try {
    names.sort_by(fn(item) {
      names.pop()
      return item
    })
  } catch e {
    caught = true
  }
  print(caught)
  print(len(names))
  return 0
}
''', "true\n0\n")

CASES["sort_by_keys_stable"] = ('''fn main() -> i64 {
  let mut words = ["bb" + str(2), "a" + str(1), "cc" + str(3)]
  let ordered = words.sort_by(fn(item) { return len(item) })
  print(ordered)
  print(len(words))
  return 0
}
''', "['a1', 'bb2', 'cc3']\n3\n")

CASES["hof_receiver_reassigned"] = ('''let mut ga = [1, 2, 3]
let mut gb = [1, 2, 3, 4]
let mut gc = [1, 2, 3]
let mut gd = [1, 2, 3]
let mut ge = [2, 4, 6]
let mut gg = ["aa" + str(1), "bb" + str(2), "cc" + str(3)]
let mut gh = ["cc" + str(3), "a" + str(1), "bb" + str(2)]
fn main() -> i64 {
  let m = ga.map(fn(x) { ga = []; return x * 10 })
  print(m, len(ga))
  let f = gb.filter(fn(x) { gb = []; return x % 2 == 0 })
  print(f, len(gb))
  let s = gc.fold(0, fn(acc, x) { gc = []; return acc + x })
  print(s, len(gc))
  print(gd.any(fn(x) { gd = []; return x == 2 }), len(gd))
  print(ge.all(fn(x) { ge = []; return x % 2 == 0 }), len(ge))
  print(gg.find(fn(x) { gg = []; return x == "bb2" }), len(gg))
  let sorted = gh.sort_by(fn(x) { gh = []; return len(x) })
  print(sorted, len(gh))
  let mut caught = false
  try {
    let empty: vec[i64] = [];
    ["bb" + str(1), "a" + str(2)].sort_by(fn(x) { return empty[0] })
  } catch err {
    caught = true
  }
  print(caught)
  return 0
}
''', "[10, 20, 30] 0\n[2, 4] 0\n6 0\ntrue 0\ntrue 0\nbb2 0\n['a1', 'cc3', 'bb2'] 0\ntrue\n")

CASES["user_impl_shadows_hof"] = ('''struct Bag {
  base: i64
}
impl Bag {
  fn map(self, f) -> i64 { return self.base + f(1) }
  fn filter(self, f) -> i64 { return self.base + f(2) }
  fn fold(self, init: i64, f) -> i64 { return self.base + f(init, 3) }
  fn any(self, f) -> i64 { return self.base + f(4) }
  fn all(self, f) -> i64 { return self.base + f(5) }
  fn find(self, f) -> i64 { return self.base + f(6) }
}
fn main() -> i64 {
  let b = Bag { base: 100 }
  let id = fn(x) { return x }
  print(b.map(id), b.filter(id), b.fold(10, fn(a, x) { return a + x }))
  print(b.any(id), b.all(id), b.find(id))
  print([3, 1, 2].map(fn(x) { return x + 1 }))
  print([3, 1, 2].filter(fn(x) { return x > 1 }))
  print([3, 1, 2].fold(0, fn(a, x) { return a + x }))
  print([3, 1, 2].any(fn(x) { return x == 2 }), [3, 1, 2].all(fn(x) { return x > 0 }), [3, 1, 2].find(fn(x) { return x == 1 }))
  print([3, 1, 2].sort_by(fn(x) { return x }))
  return 0
}
''', "101 102 113\n104 105 106\n[4, 2, 3]\n[3, 2]\n6\ntrue true 1\n[1, 2, 3]\n")

CASES["ctor_returned"] = ('''enum Tree {
  Leaf,
  Branch(left, val: i64, right),
}

fn insert(t, v: i64) {
  match t {
    Leaf => { return Branch(Leaf, v, Leaf) },
    Branch(l, x, r) => {
      if v < x {
        return Branch(insert(l, v), x, r)
      }
      return Branch(l, x, insert(r, v))
    },
  }
}

fn size(t) -> i64 {
  match t {
    Leaf => { return 0 },
    Branch(l, x, r) => { return size(l) + 1 + size(r) },
  }
}

fn main() -> i64 {
  let mut t = Leaf
  for v in [5, 3, 8, 1, 4, 7, 9] {
    t = insert(t, v)
  }
  print(size(t))
  print(size(insert(Leaf, 1)))
  return 0
}
''', "7\n1\n")

CASES["closure_call_returned"] = ('''let pair = fn(x) { return ["g {x}", "h {x}"] }

fn apply(g, x) { return g(x) }

fn local(v: i64) {
  let g = fn(x) { return ["item {x}"] }
  return g(v)
}

fn global(v: i64) { return pair(v) }

fn main() -> i64 {
  let mut n = 0
  for i in 0..100 {
    n = n + len(apply(fn(x) { return ["a {x}", "b", "c"] }, i))
    n = n + len(local(i)) + len(global(i))
  }
  print(n)
  return 0
}
''', "600\n")

CASES["upper_binding_returned"] = ('''let TABLE = ["g1", "g2"]

fn build(v: i64) {
  let Items = ["x {v}", "y {v}"]
  return Items
}

fn pass(X) { return X }

fn table() { return TABLE }

fn main() -> i64 {
  let mut n = 0
  for i in 0..100 {
    let xs = ["k {i}"]
    n = n + len(build(i)) + len(pass(xs)) + len(xs) + len(table())
  }
  print(n, TABLE)
  return 0
}
''', "600 ['g1', 'g2']\n")

CASES["vlit_overflow_cached"] = (
    'fn pick() -> str {\n  let mut s = ""\n  for k in 0..3 {\n'
    + "\n".join('    s = "literal_overflow_test_entry_number_%04d"' % i for i in range(540))
    + '\n  }\n  return s\n}\nfn main() -> i64 { print(pick()); return 0 }\n',
    "literal_overflow_test_entry_number_0539\n",
)



def build(src, name):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    c = subprocess.run(_cxx.cli_args("emit-c", "--unsafe-ffi", path), capture_output=True, text=True,
                       timeout=120)
    assert c.returncode == 0, "%s emit-c: %r" % (name, c.stdout + c.stderr)
    cfile = os.path.join(TMP, name + ".c")
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(c.stdout)
    exe = os.path.join(TMP, name + ".exe")
    cc = subprocess.run(["cc", "-fsanitize=address", "-g", "-O1", "-I", ENGINE, cfile,
                         "-o", exe, "-lm"], capture_output=True, text=True, timeout=600)
    assert cc.returncode == 0, "%s cc: %r" % (name, cc.stdout + cc.stderr)
    return path, exe


for name, (src, want) in CASES.items():
    path, exe = build(src, name)
    run = subprocess.run([exe], capture_output=True, text=True, timeout=120,
                         env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=0"))
    out = run.stdout + run.stderr
    assert run.returncode == 0, "%s rc=%d %r" % (name, run.returncode, out[-1200:])
    assert "AddressSanitizer" not in out, "%s broken: %s" % (name, out[-800:])
    assert "LeakSanitizer" not in out, "%s leaked: %s" % (name, out[-800:])
    assert run.stdout == want, "%s stdout=%r want=%r" % (name, run.stdout, want)
    run_o0 = subprocess.run(_cxx.cli_args("run", "-O0", "--unsafe-ffi", path), capture_output=True, text=True,
                            timeout=120)
    assert run_o0.returncode == 0 and run_o0.stdout == want, \
        "%s run_o0=%r" % (name, run_o0.stdout + run_o0.stderr)

print("CONTAINERS OWN ALL GREEN (%d cases)" % len(CASES))
