"""A string literal with NUL is refused, so `len` cannot differ between backends."""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = _scratch.mkdtemp(prefix="brat-nul-")

BS = chr(92)
ESC_NUL = "ab" + BS + "0cd"
RAW_NUL = "ab" + chr(0) + "cd"


def write(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def both_fail(path, text):
    for cmd in ("run", "build", "check"):
        argv = _cxx.cli_args(cmd, path)
        if cmd == "build":
            argv += ["-o", os.path.join(TMP, "nul.exe")]
        r = subprocess.run(argv, capture_output=True, text=True, timeout=120)
        out = r.stdout + r.stderr
        assert r.returncode != 0 and text in out, "%s: rc=%d out=%r" % (cmd, r.returncode, out)


def program(literal):
    return 'fn main() -> i64 {\n  let s = "%s";\n  print(len(s));\n  return 0;\n}\n' % literal


both_fail(write("escaped.brat", program(ESC_NUL)), "string literal contains NUL")
both_fail(write("raw.brat", program(RAW_NUL)), "string literal contains NUL")
both_fail(write("tpl.brat", program("a{" + BS + "0}b")), "string literal contains NUL")

good = write("good.brat",
             'fn main() -> i64 {\n'
             '  let s = "abcd";\n'
             '  print(len(s));\n'
             '  print(len("ab' + BS + 'tcd"));\n'
             '  return 0;\n'
             '}\n')
interp = subprocess.run(_cxx.cli_args("run", "--interp", good), capture_output=True,
                        text=True, timeout=120)
assert interp.returncode == 0 and interp.stdout == "4\n5\n", \
    "run: rc=%d out=%r" % (interp.returncode, interp.stdout + interp.stderr)
exe = os.path.join(TMP, "good.exe")
built = subprocess.run(_cxx.cli_args("build", good, "-o", exe),
                       capture_output=True, text=True, timeout=300)
assert built.returncode == 0, "build: %r" % (built.stdout + built.stderr)
native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
assert native.returncode == 0 and native.stdout == "4\n5\n", \
    "native: rc=%d out=%r" % (native.returncode, native.stdout + native.stderr)

print("NUL STRING ALL GREEN")
