"""The C++ front end builds the same tree as the Python one, byte for byte.

Two processes for a whole corpus, not two per file: the reference dump is built
in this process, the C++ dump comes from one run of one binary, and the files
are compared line by line. On this corpus that is about half a second, most of
it Python.

The binary is rebuilt only when a source is newer than it, and with a single
`c++` call, because the front end has no dependencies: a handful of .cpp files
against the standard library, which every machine that can run `brat build`
already has.
"""
import _cxx
import _scratch

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
BIN = _cxx.BIN
TMP = _scratch.mkdtemp(prefix="brat-cxx-")

sys.path.insert(0, ENGINE)
import brat.parser as P                      # noqa: E402
from brat.lexer import Cur, lex              # noqa: E402
from brat.parser import parse_prog           # noqa: E402


def corpus():
    out = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in ("vendor", "bin", ".git", "__pycache__")]
        for f in files:
            if f.endswith(".brat"):
                out.append(os.path.join(base, f))
    return sorted(out)


def main():
    binary = _cxx.build()
    files = corpus()
    t0 = time.perf_counter()

    want, ok_files = [], []
    for path in files:
        src = open(path, encoding="utf-8").read()
        P._seq = 0
        try:
            want.append(_cxx.sx(parse_prog(Cur(lex(src)))))
            ok_files.append(path)
        except SyntaxError:
            pass

    got = subprocess.run([binary, "ast"] + ok_files, capture_output=True, text=True, timeout=300)
    dt = time.perf_counter() - t0
    if got.returncode != 0:
        sys.exit("brat-cxx ast failed:\n" + got.stderr[-2000:])
    lines = got.stdout.rstrip("\n").split("\n")
    assert len(lines) == len(ok_files), "%d dumps for %d files" % (len(lines), len(ok_files))
    assert len(ok_files) == len(files), "%d of %d files parse in Python" % (len(ok_files), len(files))

    bad = 0
    for path, a, b in zip(ok_files, want, lines):
        if a == b:
            continue
        bad += 1
        i = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
        print("DIFF %s at %d" % (os.path.relpath(path, ROOT), i))
        print("  python: %r" % a[max(0, i - 60):i + 80])
        print("  c++   : %r" % b[max(0, i - 60):i + 80])
    assert bad == 0, "%d of %d files differ" % (bad, len(ok_files))

    # The error path is the same contract: what the old parser refuses, the new
    # one refuses too, with a line number and a non-zero exit.
    broken = {
        "unclosed.brat": "fn main() -> i64 {\n  let x = (1 + 2\n  return 0\n}\n",
        "nobraces.brat": "fn main() -> i64\n  return 0\n",
        "eof.brat": "let x = 1 +\n",
        "noname.brat": "fn () -> i64 { return 0 }\n",
    }
    for name, src in broken.items():
        path = os.path.join(TMP, name)
        open(path, "w", encoding="utf-8").write(src)
        P._seq = 0
        try:
            parse_prog(Cur(lex(src)))
            py_failed = False
        except SyntaxError:
            py_failed = True
        r = subprocess.run([binary, "check", path], capture_output=True, text=True, timeout=120)
        assert py_failed and r.returncode == 1, "%s: python_failed=%s cxx_rc=%d" % (
            name, py_failed, r.returncode)
        assert "line" in r.stderr, "%s: no line in %r" % (name, r.stderr)

    print("CXX FRONTEND ALL GREEN (%d files, identical trees, %d refusals, %.2fs)"
          % (len(ok_files), len(broken), dt))


main()
