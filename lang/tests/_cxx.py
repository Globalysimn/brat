"""The C++ front end: where it lives, and how to build it exactly once.

A machine without a compiler skips with `_skip.SKIP`, not with 0: the four
oracle tests below prove nothing there, and a green ok would say they did.

Tests run in parallel (run_all.py takes one file per core) and two of them need
the same binary, so the build goes to a temp name and is renamed into place: a
reader either sees no binary or a complete one. Without that, the second test
execs a half-written file and gets EACCES.
"""

import _skip

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CXX = os.path.abspath(os.path.join(HERE, "..", "cxx"))
BIN = os.path.join(CXX, "bin", "brat-cxx")
SRC = sorted(os.path.join(CXX, "src", n) for n in os.listdir(os.path.join(CXX, "src")))


def build():
    if os.path.exists(BIN) and all(os.path.getmtime(s) < os.path.getmtime(BIN) for s in SRC):
        return BIN
    cc = shutil.which("c++") or shutil.which("g++")
    if not cc:
        _skip.skip("no c++ and no g++, the C++ front end cannot be built")
    os.makedirs(os.path.dirname(BIN), exist_ok=True)
    tmp = "%s.tmp%d" % (BIN, os.getpid())
    r = subprocess.run([cc, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror=switch"]
                       + SRC + ["-o", tmp], capture_output=True, text=True)
    if r.returncode != 0:
        if os.path.exists(tmp):
            os.remove(tmp)
        sys.exit("c++ failed:\n" + r.stderr[-2000:])
    os.replace(tmp, BIN)
    return BIN


def sx(node):
    """The reference s-expression printer, used by the two tree-oracle tests.

    Same bytes as `lang/engine/brat/parser.py`'s `dump`, rebuilt here so a test
    can compare against any Python value, not only a parsed file.
    """
    if isinstance(node, tuple):
        return "(" + " ".join(sx(x) for x in node) + ")"
    if isinstance(node, list):
        return "[" + " ".join(sx(x) for x in node) + "]"
    if isinstance(node, bool):
        return "true" if node else "false"
    if node is None:
        return "None"
    if isinstance(node, float):
        return repr(node)
    if isinstance(node, int):
        return str(node)
    if isinstance(node, str):
        out = (node.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")
               .replace("\r", "\\r").replace('"', '\\"'))
        return '"%s"' % out
    return repr(node)
