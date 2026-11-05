"""Containers own their elements, under AddressSanitizer.

Every case fills a container inside a loop and drops it; the interpreter and the
native binary must print the same thing and the binary must leave nothing
behind. These are the shapes that leaked while containers aliased their
elements: a vector literal, a map literal, a push of a fresh value, a slice, a
key list, concatenation, a struct field, a nested vector, an iterator over a
map, a captured binding, a capture handed to a spawned closure, and an error
carried out of a function.
"""
import _cxx
import _scratch

import os
import subprocess
import sys

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
    interp = subprocess.run(_cxx.cli_args("run", "--interp", "--unsafe-ffi", path), capture_output=True, text=True,
                            timeout=120)
    assert interp.returncode == 0 and interp.stdout == want, \
        "%s interp=%r" % (name, interp.stdout + interp.stderr)

print("CONTAINERS OWN ALL GREEN (%d cases)" % len(CASES))
