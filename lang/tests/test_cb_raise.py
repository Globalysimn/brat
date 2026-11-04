"""A raise inside a callback stops the program instead of handing C a garbage value.

`raise` inside a function called through C cannot unwind: the C frame between the
callback and the BRAT caller knows nothing about BRAT errors, so the wrapper has to
decide. It used to return whatever the abandoned function left in its result slot,
which reached the caller as a number invented by the compiler ("94700151231784"
from a program that raised `bad 2`), and the process exited 0.

Now the wrapper reports the error exactly as an uncaught one at the top level and
exits 1. Both backends must agree on exit code, stdout, and the runtime error. The CLI's
FFI proof notice is checked separately; a clean call proves the callback check
does not fire on success.
"""

import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")
TMP = _scratch.mkdtemp(prefix="brat-cbraise-")
LIB = os.path.join(TMP, "libcbraise.so")

with open(os.path.join(TMP, "libcbraise.c"), "w", encoding="utf-8") as f:
    f.write("long long apply_cb(long long n, long long (*cb)(long long))\n"
            "{\n"
            "    long long acc = 0;\n"
            "    for (long long i = 0; i < n; i++) acc += cb(i);\n"
            "    return acc;\n"
            "}\n")
built = subprocess.run(["cc", "-shared", "-fPIC", "-O2", os.path.join(TMP, "libcbraise.c"),
                        "-o", LIB], capture_output=True, text=True, timeout=120)
assert built.returncode == 0, built.stderr
HEADER = os.path.join(TMP, "libcbraise.h")
with open(HEADER, "w", encoding="utf-8") as f:
    f.write("long long apply_cb(long long n, long long (*cb)(long long));\n")

HEAD = ('extern "%s" fn apply_cb(n: i64, cb: fn(i64) -> i64) -> i64;\n\n' % LIB)

RAISES = HEAD + (
    'fn boom(x: i64) -> i64 {\n'
    '  if x == 2 { raise("bad {x}") }\n'
    '  return x\n'
    '}\n'
    '\n'
    'fn main() -> i64 {\n'
    '  try {\n'
    '    print(apply_cb(5, "boom"))\n'
    '  } catch e {\n'
    '    print("caught {e}")\n'
    '  }\n'
    '  return 0\n'
    '}\n'
)

CLEAN = HEAD + (
    'fn twice(x: i64) -> i64 {\n'
    '  return x * 2\n'
    '}\n'
    '\n'
    'fn main() -> i64 {\n'
    '  print(apply_cb(5, "twice"))\n'
    '  return 0\n'
    '}\n'
)


def native(src, name):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    c = subprocess.run([sys.executable, CLI, "emit-c", "--unsafe-ffi", path], capture_output=True, text=True,
                       timeout=120)
    assert c.returncode == 0, "emit-c %s: %r" % (name, (c.stdout + c.stderr)[-600:])
    cfile = os.path.join(TMP, name + ".c")
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(c.stdout)
    exe = os.path.join(TMP, name + ".exe")
    cc = subprocess.run(["cc", "-O2", "-I", ENGINE, cfile, "-o", exe, "-L", TMP, "-lcbraise",
                         "-lm", "-Wl,-rpath," + TMP], capture_output=True, text=True, timeout=600)
    assert cc.returncode == 0, "cc %s: %r" % (name, (cc.stdout + cc.stderr)[-600:])
    run = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    return path, run


# a raise in the callback: both backends say the same thing and exit 1
path, built_run = native(RAISES, "raises")
interp = subprocess.run(
    [sys.executable, CLI, "run", "--interp", "--header", HEADER, path], capture_output=True,
    text=True, timeout=300)
want_err = "unhandled: ('err', 'bad 2')\n"
assert interp.returncode == 1, "interp rc=%d, want 1 (stdout=%r)" % (interp.returncode,
                                                                    interp.stdout)
assert interp.stdout == "", "interp printed a value from an abandoned callback: %r" % interp.stdout
assert interp.stderr.endswith(want_err), "interp stderr=%r want suffix=%r" % (interp.stderr, want_err)
assert "binding(s) proven against" in interp.stderr, "FFI proof notice missing: %r" % interp.stderr
assert built_run.returncode == 1, "native rc=%d, want 1 (stdout=%r)" % (built_run.returncode,
                                                                      built_run.stdout)
assert built_run.stdout == "", "native printed a value from an abandoned callback: %r" % built_run.stdout
assert built_run.stderr == want_err, "native stderr=%r want=%r" % (built_run.stderr, want_err)

# a callback that returns normally does not trip the check
path, built_run = native(CLEAN, "clean")
interp = subprocess.run(
    [sys.executable, CLI, "run", "--interp", "--header", HEADER, path], capture_output=True,
    text=True, timeout=300)
assert interp.returncode == 0 and interp.stdout == "20\n", \
    "interp rc=%d out=%r err=%r" % (interp.returncode, interp.stdout, interp.stderr)
assert built_run.returncode == 0 and built_run.stdout == "20\n", \
    "native rc=%d out=%r err=%r" % (built_run.returncode, built_run.stdout, built_run.stderr)

print("CALLBACK RAISE ALL GREEN (runtime error is preserved across both backends, "
      "a clean callback still returns)")
