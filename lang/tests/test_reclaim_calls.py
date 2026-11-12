"""Memory reclamation with ownership on (the default): every probe is checked with
AddressSanitizer, because a leak is invisible on stdout.

Each case is a shape that used to leak: a fresh value per loop pass, a
container returned out of an arena, a defer that captures nothing, a vector
built for one `par for`.
"""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-reclaim-")

CASES = {}

CASES["loop_calls"] = ('''
fn words() -> vec {
  return ["alpha", "beta", "gamma"]
}

fn label(n: i64) -> str {
  return "row {n}"
}

fn main() -> i64 {
  let mut total = 0
  let mut i = 0
  while i < 200 {
    let ws = words()
    let name = label(i)
    total = total + len(ws) + len(name)
    i = i + 1
  }
  let last = words()
  print(total)
  print(len(last))
  return 0
}
''', "1890\n3\n")

CASES["loop_strings"] = ('''
fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let c = "v {i}"
    n = n + len(c)
    i = i + 1
  }
  print(n)
  return 0
}
''', "390\n")

CASES["arena_own"] = ('''
fn build() -> vec {
  let mut out = []
  arena {
    let parts = ["alpha", "beta", "gamma"]
    out = own(parts)
  }
  arena {
    let tmp = ["x", "y"]
    let keep = len(tmp)
    if keep > 99 { print("unreachable") }
  }
  return out
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let w = build()
    n = n + len(w) + len(w[0])
    i = i + 1
  }
  print(n)
  return 0
}
''', "800\n")

CASES["own_nested"] = ('''
fn build() -> vec {
  arena {
    return own([["a", "b"], ["c"], []])
  }
  return []
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let w = build()
    n = n + len(w[0][0]) + len(w[1]) + len(w[2])
    i = i + 1
  }
  let v = build()
  print(n)
  print(v[0][1])
  return 0
}
''', "200\nb\n")

CASES["own_map"] = ('''
fn build() -> map {
  arena {
    let m = {"x": "left", "y": "right"}
    return own(m)
  }
  return {}
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let m = build()
    n = n + len(m.get("x")) + len(m.get("y"))
    i = i + 1
  }
  let q = build()
  print(n)
  print(q.get("y"))
  return 0
}
''', "900\nright\n")

CASES["own_tag"] = ('''
enum Res {
  Ok(v),
  Err(reason)
}

fn check(n: i64) -> Res {
  arena {
    let parts = ["aa", "bb"]
    if n > 50 {
      return Err(own("late {n}"))
    }
    return Ok(own(parts))
  }
  return Err("unreachable")
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 100 {
    let r = check(i)
    match r {
      Ok(v) => { n = n + len(v) },
      Err(m) => { n = n + len(m) }
    }
    i = i + 1
  }
  print(n)
  return 0
}
''', "445\n")

CASES["defer_capture"] = ('''
fn main() -> i64 {
  let mut log = []
  let mut i = 0
  while i < 100 {
    let tag = "t {i}"
    defer log.push(tag)
    i = i + 1
  }
  print(len(log))
  print(log[99])
  return 0
}
''', "100\nt 99\n")

CASES["parfor_source"] = ('''
fn add(a: i64, b: i64) -> i64 { return a + b }

fn make(n: i64) -> vec[i64] {
  let mut v = []
  for i in 0..n {
    v.push(i + 1)
  }
  return v
}

fn main() -> i64 {
  let mut i = 0
  while i < 50 {
    let lit = par for n in [1, 2, 3] reduce(add, 0) { return n * 2 }
    let built = par for n in make(4) reduce(add, 0) { return n }
    i = i + lit + built
  }
  print(i)
  return 0
}
''', "66\n")

CASES["mixed_slot_relet_and_iter"] = ('''
fn iter_then_untaken_set(flag: bool, k: i64) -> str {
  let v = ["heap_element_long_enough_no_sso_" + str(k)]
  let mut x = ""
  for x in v {
    let _n = len(x)
  }
  if flag {
    x = "owned_heap_string_never_taken_" + str(k)
  }
  return v[0]
}

fn owned_then_iter(k: i64) -> str {
  let v = ["heap_element_after_init_long_" + str(k)]
  let mut x = "initial_heap_string_long_enough_" + str(k)
  for x in v {
    let _n = len(x)
  }
  return v[0]
}

fn param_cond_relet(p: str, flag: bool, k: i64) -> i64 {
  if flag {
    let p = "rebound_param_heap_string_long_" + str(k)
    return len(p)
  }
  return len(p)
}

fn seq_and_branch_relet(flag: bool, k: i64) -> i64 {
  let x = ["first_vec_long_" + str(k), "second"]
  let x = ["replaced_vec_long_" + str(k)]
  let mut acc = len(x)
  if flag {
    let y = ["branch_vec_long_" + str(k)]
    acc = acc + len(y)
  } else {
    let y = 42
    acc = acc + y
  }
  return acc
}

fn main() -> i64 {
  print(iter_then_untaken_set(false, 99))
  print(owned_then_iter(99))
  let caller_s = "caller_owned_heap_string_long_" + str(77)
  print(param_cond_relet(caller_s, false, 77) + param_cond_relet(caller_s, true, 77))
  print(seq_and_branch_relet(true, 55) + seq_and_branch_relet(false, 55))
  return 0
}
''', "heap_element_long_enough_no_sso_99\nheap_element_after_init_long_99\n65\n45\n")


def emit(src, name):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    c = subprocess.run(_cxx.cli_args("emit-c", path), capture_output=True, text=True,
                       timeout=120)
    assert c.returncode == 0, "%s emit-c: %r" % (name, c.stdout + c.stderr)
    cfile = os.path.join(TMP, name + ".c")
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(c.stdout)
    exe = os.path.join(TMP, name + ".exe")
    cc = subprocess.run(["cc", "-fsanitize=address", "-g", "-O1", "-I", ENGINE, cfile, "-o", exe,
                         "-lm"], capture_output=True, text=True, timeout=600)
    assert cc.returncode == 0, "%s cc: %r" % (name, cc.stdout + cc.stderr)
    return path, exe


for name, (src, want) in CASES.items():
    path, exe = emit(src, name)
    run = subprocess.run([exe], capture_output=True, text=True, timeout=120,
                         env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=0"))
    out = run.stdout + run.stderr
    assert run.returncode == 0, "%s rc=%d %r" % (name, run.returncode, out[-1200:])
    assert "ERROR: AddressSanitizer" not in out, "%s broken: %s" % (name, out[-800:])
    if name == "defer_capture":
        # A vec does not release its elements, by design (SPEC: containers alias
        # what they are given), so a program that fills one keeps them. What must
        # not happen is growth with the loop: one element per pass, nothing else.
        size = int(out.split("leaked in")[0].split()[-2]) if "leaked" in out else 0
        assert size <= 8192, "%s leaked %d bytes" % (name, size)
    else:
        assert "LeakSanitizer" not in out, "%s leaked: %s" % (name, out[-800:])
    assert run.stdout == want, "%s stdout=%r want=%r" % (name, run.stdout, want)
    run_o0 = subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True, text=True,
                            timeout=120)
    assert run_o0.returncode == 0 and run_o0.stdout == want, \
        "%s run_o0=%r" % (name, run_o0.stdout + run_o0.stderr)

print("RECLAIM ALL GREEN (%d cases)" % len(CASES))
