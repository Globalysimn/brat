"""Strings the runtime builds are values, not raw buffers.

`vjoin`, `vrepeat_n`, `vreplace` and `vcase` (`upper`/`lower`) used to hand back a
plain `malloc` buffer with no `BHead` in front of it, while the emitter treats
every string binding as droppable. `let s = "a,b".split(",").join("-")` therefore
dropped a pointer that had no header: under AddressSanitizer that is a SEGV inside
`vrc_dec`, and without a sanitizer it is a free of a pointer the allocator does not
know - a crash waiting for the right program.

Every case here binds the result to a name (so the scope exit drops it), stores one
in a container (so releasing the container drops it), and returns one from a
function. The -O0 build is the oracle for what the program prints.
"""
import _cxx

import _scratch

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-strown-")

CASES = {
    # one binding per producer, printed and then dropped at scope exit
    "bind": ('''fn main() -> i64 {
  let a = "x,y".split(",").join("-")
  let b = "ab".repeat(3)
  let c = "AbC".upper()
  let d = "MiXeD".lower()
  let e = "  pad  ".trim()
  print(a)
  print(b)
  print(c)
  print(d)
  print(e)
  return 0
}
''', "x-y\nababab\nABC\nmixed\npad\n"),

    # the same values inside a container: releasing the container drops them
    "stored": ('''fn main() -> i64 {
  let mut v = []
  v.push("a,b".split(",").join("+"))
  v.push("z".repeat(2))
  print(v)
  print(len(v.join("/")))
  return 0
}
''', "['a+b', 'zz']\n6\n"),

    # returned across a frame boundary, which is where a drop on both sides shows
    "returned": ('''fn make(n: i64) -> str {
  return "-".repeat(n)
}

fn main() -> i64 {
  let s = make(3)
  print(s)
  print(make(1).upper())
  return 0
}
''', "---\n-\n"),

    # a loop, because a one-shot leak or a one-shot double drop hides in a single pass
    "loop": ('''fn main() -> i64 {
  let mut i = 0
  let mut n = 0
  while i < 200 {
    let s = "k=v".replace("=", ":")
    n = n + len(s.upper())
    i = i + 1
  }
  print(n)
  return 0
}
''', "600\n"),

    # a producer's result fed straight into another producer
    "chained": ('''fn main() -> i64 {
  let s = " a b ".trim().replace(" ", "_").upper()
  print(s)
  print(len(s.repeat(2)))
  return 0
}
''', "A_B\n6\n"),
}


def build(src, name, sanitize):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    c = subprocess.run(_cxx.cli_args("emit-c", path), capture_output=True, text=True,
                       timeout=120)
    assert c.returncode == 0, "%s emit-c: %r" % (name, c.stdout + c.stderr)
    cfile = os.path.join(TMP, name + ".c")
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(c.stdout)
    exe = os.path.join(TMP, name + (".asan" if sanitize else ".exe"))
    cmd = ["cc"]
    if sanitize:
        cmd += ["-fsanitize=address", "-g"]
    cmd += ["-O1", "-I", ENGINE, cfile, "-o", exe, "-lm"]
    cc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    assert cc.returncode == 0, "%s cc: %r" % (name, cc.stdout + cc.stderr)
    return path, exe


for name, (src, want) in CASES.items():
    run_o0 = subprocess.run(_cxx.cli_args("run", "-O0", os.path.join(TMP, name + ".brat")),
                            capture_output=True, text=True, timeout=120) \
        if os.path.exists(os.path.join(TMP, name + ".brat")) else None
    path, exe = build(src, name, sanitize=True)
    run_o0 = subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True, text=True,
                            timeout=120)
    assert run_o0.returncode == 0, "%s run_o0 rc=%d %r" % (name, run_o0.returncode, run_o0.stdout)
    assert run_o0.stdout == want, "%s run_o0 stdout=%r want=%r" % (name, run_o0.stdout, want)

    run = subprocess.run([exe], capture_output=True, text=True, timeout=120,
                         env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=0"))
    out = run.stdout + run.stderr
    assert "AddressSanitizer" not in out, "%s not clean: %s" % (name, out[-900:])
    assert run.returncode == 0, "%s rc=%d %r" % (name, run.returncode, out[-900:])
    assert run.stdout == want, "%s native stdout=%r want=%r" % (name, run.stdout, want)

print("STROWN ALL GREEN (%d cases, -O0 == native == expected, ASan and LSan clean)"
      % len(CASES))
