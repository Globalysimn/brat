"""`brat run` keeps stdout for the program.

A program that draws checker warnings still runs, and a script that pipes it
must read only what the program printed. The C++ front end used to print its
"N error(s), M warning(s)" report on stdout ahead of the program's own output;
the -O0 build never did. Checker diagnostics belong on stderr in every path.
"""
import _cxx
import _scratch

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-run-stdout-")

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def write(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


WARNS = write("warns.brat", 'fn main() -> i64 {\n  let unused = 5\n  print("hi")\n  return 0\n}\n')
MOVED = write("moved.brat", 'fn main() -> i64 {\n  let h = "hi"\n  let m = move h\n  print(h)\n  return 0\n}\n')

PATHS = {
    "brat-cxx run": [NATIVE, "run"],
    "brat-cxx run -O0": _cxx.cli_args("run", "-O0"),
}

for label, command in PATHS.items():
    ran = subprocess.run(command + [WARNS], capture_output=True, text=True, timeout=300)
    check(ran.returncode == 0, "%s on a warning: exit %d: %r" % (label, ran.returncode, ran.stderr))
    check(ran.stdout == "hi\n", "%s: stdout is %r, the program printed only `hi`" % (label, ran.stdout))
    check("warning: unused var `unused`" in ran.stderr,
          "%s: the warning is missing from stderr: %r" % (label, ran.stderr))

    refused = subprocess.run(command + [MOVED], capture_output=True, text=True, timeout=300)
    check(refused.returncode != 0, "%s ran a program with a checker error" % label)
    check(refused.stdout == "", "%s: stdout is %r when the checker refuses" % (label, refused.stdout))
    check("use of moved `h`" in refused.stderr, "%s: stderr lacks the error: %r" % (label, refused.stderr))

print("RUN STDOUT ALL GREEN (%d asserts)" % n)
