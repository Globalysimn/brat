"""Loops, scopes and `move`: the checker's two passes must not invent an error.

The capability pass reaches a loop fixpoint by walking the body twice, because a
body runs zero or more times. What it carried back into pass two was the whole
state, including the bindings the body itself declares -- so

    for i in 0..3 {
      let s = "a" + "b"
      let t = move s      // refused: "use of moved `s`"
    }

was an error in a correct program. The fix is the one thing the pass knew but did
not use: a binding declared inside the body is fresh at the top of every
iteration, so the loop head forgets the body's own names before each pass.

The true positives have to keep firing, which is most of what this file checks:

* a value declared *outside* the loop and moved inside it is a use-after-move on
  the second iteration;
* two moves of one value inside the same iteration;
* a `let` inside a loop does not leak its binding out of the loop.
"""
import _cxx
import _scratch

import os
import subprocess
import sys

TMP = _scratch.mkdtemp(prefix="brat-loops")

HEAD = "fn main() -> i64 {\n"

ACCEPTED = {
    "while_local": HEAD + '''  let mut n = 0
  while n < 3 {
    let s = "pay" + "load"
    let t = move s
    print(t)
    n = n + 1
  }
  return 0
}
''',
    "for_local": HEAD + '''  for i in 0..3 {
    let s = "a" + "b"
    let t = move s
    print(t)
  }
  return 0
}
''',
    "for_local_vec": HEAD + '''  for i in 0..2 {
    let v = [1, 2, 3]
    let w = move v
    print(len(w))
  }
  return 0
}
''',
    "outer_read_in_loop": HEAD + '''  let outer = "a" + "b"
  for i in 0..3 {
    let s = "x" + "y"
    let t = move s
    print(t)
    print(outer)
  }
  let last = move outer
  print(last)
  return 0
}
''',
    "nested_loops": HEAD + '''  for i in 0..2 {
    for j in 0..2 {
      let s = "n" + "m"
      let t = move s
      print(t)
    }
  }
  return 0
}
''',
}

REFUSED = {
    # declared outside, moved inside: iteration two reads what iteration one took
    "outer_moved_in_loop": HEAD + '''  let s = "loop-move-payload"
  for i in 0..3 {
    let t = move s
    print(t)
  }
  return 0
}
''',
    # the same value twice in one iteration
    "double_move_in_body": HEAD + '''  for i in 0..3 {
    let s = "a" + "b"
    let t = move s
    let u = move s
    print(t)
    print(u)
  }
  return 0
}
''',
    # a loop-local binding is not visible after the loop
    "leaks_out_of_loop": HEAD + '''  for i in 0..2 {
    let s = "a" + "b"
  }
  print(s)
  return 0
}
''',
}


def write(name, text):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def check(path):
    return subprocess.run(_cxx.cli_args("check", path), capture_output=True,
                          text=True, timeout=300)


def main():
    for name, src in ACCEPTED.items():
        p = check(write(name, src))
        assert p.returncode == 0, "check refused %s:\n%s%s" % (name, p.stdout, p.stderr)

    for name, src in REFUSED.items():
        p = check(write(name, src))
        assert p.returncode != 0, "check accepted %s, which it must not" % name
        assert "error" in p.stdout, (name, p.stdout)

    # A program that runs must also run: the accepted shapes are not just
    # checkable, they execute at -O0 and -O2.
    for name in ("while_local", "for_local", "nested_loops"):
        path = os.path.join(TMP, name + ".brat")
        r = subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True,
                           text=True, timeout=300)
        assert r.returncode == 0, (name, r.stdout + r.stderr)
        exe = os.path.join(TMP, name + ".bin")
        b = subprocess.run(_cxx.cli_args("build", path, "-o", exe),
                           capture_output=True, text=True, timeout=600)
        assert b.returncode == 0, (name, b.stdout + b.stderr)
        n = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        assert n.returncode == 0, (name, n.stdout + n.stderr)
        assert n.stdout == r.stdout, (name, r.stdout, n.stdout)

    print("MOVE LOOPS ALL GREEN (%d accepted, %d refused)"
          % (len(ACCEPTED), len(REFUSED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
