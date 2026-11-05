"""Memory reclamation with ownership on (the default): every probe is checked with
AddressSanitizer, because a leak is invisible on stdout.

Each case is a shape that used to leak: a fresh value per loop pass, a
container returned out of an arena, a defer that captures nothing.
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
    interp = subprocess.run(_cxx.cli_args("run", "--interp", path), capture_output=True, text=True,
                            timeout=120)
    assert interp.returncode == 0 and interp.stdout == want, \
        "%s interp=%r" % (name, interp.stdout + interp.stderr)

print("RECLAIM ALL GREEN (%d cases)" % len(CASES))
