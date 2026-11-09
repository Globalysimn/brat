"""A script-level binding, read and written from a function.

This was the "accepted but unbuildable" shape: the -O0 build ran it, both
checkers reported `0 error(s)`, and the native emitter did not produce a
working binary. Two defects in the emitter caused that, both decisions about
the *fast* path rather than about globals themselves:

* the fast path is raw C over `int64_t`/`double`, while a script-level `let` is
  a `Val` in the file scope. A function that names one was classified fast
  anyway, so the body was emitted as C with no usable symbol for the name:
  forms such as `return ;` and `(long long)()`, which C refuses.
* the slow path declared a *local* of the same name as the global, so a write
  inside a function landed in the local and the file-scope value the top level
  had written stayed `vnil()` -- `type error in +` on the next read.

The native emitter now keeps globals off the fast path (and lets the fixpoint
drop their callers) and leaves globals out of the local declaration list. The
-O0 and the two CLI build paths must agree, which is what the cases
below assert -- the point is not that globals work, it is that they work
*identically*.

Run: python3 test_globals_in_fns.py
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CXX = None
TMP = _scratch.mkdtemp(prefix="brat-globals-")

# name -> (source, expected stdout)
CASES = {
    # Reading a global from a function: the case that produced `return ;`.
    "read_from_fn": ('''let total = 100
fn read_it() -> i64 {
  return total
}
fn main() -> i64 {
  print(read_it())
  return 0
}
''', "100\n"),
    # Writing one: the case that produced the shadowing local and `type error in +`.
    "write_from_fn": ('''let mut total = 100
fn bump(x: i64) -> i64 {
  total = total + x
  return total
}
fn main() -> i64 {
  print(bump(1))
  print(total)
  return 0
}
''', "101\n101\n"),
    # Both, in a loop, so the global is read and written repeatedly and the
    # function is called more than once.
    "loop_accumulate": ('''let mut acc = 0
fn step(i: i64) -> i64 {
  acc = acc + i
  return acc
}
fn main() -> i64 {
  let mut i = 0
  while i < 4 {
    print(step(i))
    i = i + 1
  }
  print(acc)
  return 0
}
''', "0\n1\n3\n6\n6\n"),
    # A mutating method call through a global: `push` is a write, and the object
    # it writes is the one the file scope owns.
    "mutate_through_global": ('''let mut seen = []
fn add(x: i64) -> i64 {
  seen.push(x)
  return len(seen)
}
fn main() -> i64 {
  print(add(1))
  print(add(2))
  print(seen)
  return 0
}
''', "1\n2\n[1, 2]\n"),
    # A function that only calls another function which reads the global: the
    # fixpoint has to drop the caller along with the callee.
    "transitive": ('''let base = 7
fn inner() -> i64 {
  return base
}
fn outer() -> i64 {
  return inner() + 1
}
fn main() -> i64 {
  print(outer())
  return 0
}
''', "8\n"),
    # A closure inside a function captures the global by value, like any other
    # binding in its enclosing frame. This must keep working: the rule is about
    # named functions, not about every function-valued thing.
    "closure_captures_global": ('''let base = 5
fn run() -> i64 {
  let f = fn(x) { return x + base }
  return f(1)
}
fn main() -> i64 {
  print(run())
  return 0
}
''', "6\n"),
    # A global that holds a string, so the value is heap-owned and the release
    # rule sees it: the top level owns it, and a function must not drop it.
    "string_global": ('''let name = "abc"
fn shout() -> i64 {
  return len(name)
}
fn main() -> i64 {
  print(shout())
  print(name)
  return 0
}
''', "3\nabc\n"),
}


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)


def fail(name, msg):
    sys.stdout.write("FAIL %s: %s\n" % (name, msg))
    sys.stdout.flush()
    raise SystemExit(1)


def main():
    global CXX
    CXX = _cxx.build()
    for name, (src, want) in sorted(CASES.items()):
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        for tag, cmd in (("check-py", _cxx.cli_args("check", path)),
                         ("check-cxx", [CXX, "check", path])):
            p = sh(cmd)
            if p.returncode != 0:
                fail(name, "%s refused a legal program: %s"
                     % (tag, (p.stdout + p.stderr)[-400:]))
        got = {}
        # The -O0 build is the oracle for what the program means.
        got["run_o0"] = sh(_cxx.run_args(path)).stdout
        for tag, cmd in (("py", _cxx.cli_args("build", path, "-o", os.path.join(TMP, name + ".py.exe"))),
                         ("cxx", [CXX, "build", path, "-o",
                                  os.path.join(TMP, name + ".cxx.exe")])):
            exe = os.path.join(TMP, name + ("." + tag + ".exe"))
            b = sh(cmd)
            if b.returncode != 0:
                fail(name, "%s build: %s" % (tag, (b.stdout + b.stderr)[-500:]))
            got[tag] = sh([exe]).stdout
            if got[tag] == "":
                fail(name, "%s produced no output (the build wrote no binary?)" % tag)
        for tag, out in sorted(got.items()):
            if out != want:
                fail(name, "%s stdout %r, want %r" % (tag, out, want))
    print("GLOBALS ALL GREEN (%d cases, -O0 + both CLI build paths)" % len(CASES))


if __name__ == "__main__":
    main()
