"""An uncaught runtime error is printed, not freed on the way out.

A frame that dies of a runtime error drops its locals before returning, and one of
those locals can be the error value itself: `n = n + boom()` stores the failing
call's result into `n`, the frame drops `n`, and the error payload -- the message
the top level is about to print -- is freed. The program then printed
`unhandled: ('err'` followed by freed memory, and AddressSanitizer called it a
heap-use-after-free in `vprint_f`.

`vrc_dec` now leaves the pending error alone: it belongs to the error channel until
`verr_take`. What this file pins: the message, the exit code, no sanitizer report,
and the same words from the -O0 build. The caught case is here too, because a
`catch` must still be able to release the error it bound.
"""
import _cxx

import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-errrep-")

CASES = {
    # the failing call's result lands in a local the frame then drops
    "index": ('''fn boom() -> i64 {
  let v = [1, 2, 3]
  return v[9]
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 3 {
    n = n + boom()
    i = i + 1
  }
  print(n)
  return 0
}
''', "index out of range"),

    # the same through a method call on a missing key
    "key": ('''fn main() -> i64 {
  let m = {"a": 1}
  let mut n = 0
  n = n + m["b"]
  print(n)
  return 0
}
''', "no key b"),

}

CAUGHT = '''fn boom() -> i64 {
  let v = [1]
  return v[5]
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 50 {
    try {
      n = n + boom()
    } catch e {
      n = n + 1
    }
    i = i + 1
  }
  print(n)
  return 0
}
'''


def build(src, name, sanitize=True):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    c = subprocess.run(_cxx.cli_args("emit-c", path), capture_output=True, text=True,
                       timeout=120)
    assert c.returncode == 0, "%s emit-c: %r" % (name, (c.stdout + c.stderr)[-600:])
    cfile = os.path.join(TMP, name + ".c")
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(c.stdout)
    exe = os.path.join(TMP, name + (".asan" if sanitize else ".exe"))
    cmd = ["cc"] + (["-fsanitize=address", "-g"] if sanitize else ["-O2"]) + \
          ["-I", ENGINE, cfile, "-o", exe, "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, "%s cc: %r" % (name, (r.stdout + r.stderr)[-600:])
    return path, exe


for name, (src, message) in CASES.items():
    path, exe = build(src, name)
    want_err = "unhandled: Err('%s')\n" % message
    run = subprocess.run([exe], capture_output=True, text=True, timeout=120,
                         env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:exitcode=0"))
    out = run.stdout + run.stderr
    assert "AddressSanitizer" not in out, "%s not clean: %s" % (name, out[-1000:])
    assert run.returncode == 1, "%s rc=%d want 1 (%r)" % (name, run.returncode, out[-400:])
    assert run.stdout == "", "%s printed a value after a fatal error: %r" % (name, run.stdout)
    assert run.stderr == want_err, "%s stderr=%r want=%r" % (name, run.stderr, want_err)

    run_o0 = subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True, text=True,
                            timeout=120)
    assert run_o0.returncode == 1, "%s run_o0 rc=%d" % (name, run_o0.returncode)
    assert run_o0.stdout == "", "%s run_o0 printed a value: %r" % (name, run_o0.stdout)
    # The report format differs between backends (Python names the exception class
    # and line, native prints the `unhandled: ('err', ...)` tag), so the message
    # and the exit code are what this test holds both to.
    assert message in run_o0.stderr, "%s run_o0 stderr=%r" % (name, run_o0.stderr)

# A store whose RHS raised must not land: the target keeps its old value and the
# handler runs, which is what the -O0 build does with the same source.
ASSIGNED = {
    # a runtime error inside the call, caught once per round
    "assign_index": '''fn boom() -> i64 {
  let v = [1]
  return v[5]
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 2 {
    try {
      n = n + boom()
    } catch e {
      n = n + 1
    }
    i = i + 1
  }
  print(n)
  return 0
}
''',
    # a `raise` in the callee: it comes back as an err value, which `+` refuses
    "assign_raise": '''fn boom() -> i64 {
  raise("no")
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 2 {
    try {
      n = n + boom()
    } catch e {
      n = n + 1
    }
    i = i + 1
  }
  print(n)
  return 0
}
''',
    # the same with an operator other than `+`
    "assign_sub": '''fn boom() -> i64 {
  raise("no")
}

fn main() -> i64 {
  let mut i = 0
  let mut n = 10
  while i < 3 {
    try {
      n = n - boom()
    } catch e {
      n = n + 1
    }
    i = i + 1
  }
  print(n)
  return 0
}
''',
    # the append shape: `s = s + f()` must not read the value `f` left behind
    "assign_append": '''fn boom() -> i64 {
  return [1][4]
}

fn main() -> i64 {
  let mut s = "a"
  let mut n = 0
  try {
    s = s + "b"
    n = n + boom()
    s = s + "c"
  } catch e {
    n = 1
  }
  print(s)
  print(n)
  return 0
}
''',
    # an operator that refuses its operand, written straight into a store: the
    # container took the error, the catch freed it, and `print` read freed memory
    "store_operator": '''struct Box { v: dyn }

fn id(x) {
  return x
}

fn main() -> i64 {
  let s = id("a string long enough to live on the heap")
  let mut m = {"k": [0]}
  let mut out = [[1]]
  let mut b = Box { v: [2] }
  let mut n = 0
  let mut i = 0
  while i < 2 {
    try { m["k"] = s - 1 } catch e { n = n + 1 }
    try { out[0] = [s - 1] } catch e { n = n + 1 }
    try { b.v = -s } catch e { n = n + 1 }
    try { m.set("z", s < 1) } catch e { n = n + 1 }
    i = i + 1
  }
  print(n, m, out, b.v)
  return 0
}
''',
    # a literal built around the failed operand has no owner on the way to the catch
    "literal_operand": '''fn id(x) {
  return x
}

fn main() -> i64 {
  let s = id("a string long enough to live on the heap")
  let mut out = [[1]]
  let mut w = id([7])
  let mut n = 0
  let mut i = 0
  while i < 2 {
    try { w = [s - 1] } catch e { n = n + 1 }
    try { out = out + [s - 1] } catch e { n = n + 1 }
    try { w = {"a": ~s} } catch e { n = n + 1 }
    i = i + 1
  }
  print(n, out, w)
  return 0
}
''',
    # a failed method call bound by `let`: the binding counted a reference to the
    # error that no release gave back
    "bind_method": '''fn id(x) {
  return x
}

fn main() -> i64 {
  let s = id("a string long enough to live on the heap")
  let mut out = []
  let mut n = 0
  let mut i = 0
  while i < 2 {
    try {
      let q = s.nope()
      print(q)
    } catch e {
      n = n + 1
    }
    try { out.push(s.nope()) } catch e { n = n + 1 }
    i = i + 1
  }
  print(n, out)
  return 0
}
''',
}

for name, src in ASSIGNED.items():
    path, exe = build(src, name)
    # Leaks on: an err value that `raise` returned is refused by the operator, and
    # the operand temp that held it has no owner once the frame leaves for the
    # catch -- 144 B per caught raise before `err_check_line` learned to drop it.
    run = subprocess.run([exe], capture_output=True, text=True, timeout=120,
                         env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=0"))
    out = run.stdout + run.stderr
    assert "AddressSanitizer" not in out, "%s not clean: %s" % (name, out[-800:])
    run_o0 = subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True, text=True,
                            timeout=120)
    assert (run.returncode, run.stdout) == (run_o0.returncode, run_o0.stdout), \
        "%s native=%r run_o0=%r" % (name, (run.returncode, run.stdout, run.stderr),
                                     (run_o0.returncode, run_o0.stdout, run_o0.stderr))
    assert run.returncode == 0, "%s rc=%d (%r)" % (name, run.returncode, out[-400:])

# a caught error is released normally: fifty raises, no leak
path, exe = build(CAUGHT, "caught")
run = subprocess.run([exe], capture_output=True, text=True, timeout=120,
                     env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=0"))
out = run.stdout + run.stderr
assert "AddressSanitizer" not in out, "caught case not clean: %s" % out[-1000:]
assert run.returncode == 0 and run.stdout == "50\n", \
    "caught case rc=%d out=%r" % (run.returncode, run.stdout)
run_o0 = subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True, text=True,
                        timeout=120)
assert run_o0.returncode == 0 and run_o0.stdout == "50\n", \
    "caught run_o0 rc=%d out=%r" % (run_o0.returncode, run_o0.stdout + run_o0.stderr)

# `fn main() -> Result[i64, str]` exits with the Ok(code) status or prints unhandled Err(...) without leaks
MAIN_OK = '''fn step(x: i64) -> Result[i64, str] {
  if x < 0 {
    return Err("neg")
  }
  return Ok(x)
}

fn main() -> Result[i64, str] {
  let v = step(0)?
  return Ok(v)
}
'''
_, exe_ok = build(MAIN_OK, "main_result_ok")
run_ok = subprocess.run([exe_ok], capture_output=True, text=True, timeout=120,
                        env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=42"))
assert run_ok.returncode == 0 and run_ok.stderr == "", \
    "main_result_ok rc=%d stderr=%r" % (run_ok.returncode, run_ok.stderr)

MAIN_BAD_RET = '''fn main() {
  return ["not_an_exit_code"]
}
'''
_, exe_bad = build(MAIN_BAD_RET, "main_bad_ret")
run_bad = subprocess.run([exe_bad], capture_output=True, text=True, timeout=120,
                         env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=42"))
assert run_bad.returncode == 1 and "TypeError: main must return i64" in run_bad.stderr, \
    "main_bad_ret rc=%d stderr=%r" % (run_bad.returncode, run_bad.stderr)

print("ERR REPORT ALL GREEN (2 uncaught shapes print the message and exit 1 under ASan, "
      "a caught error still frees)")
