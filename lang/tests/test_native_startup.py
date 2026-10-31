"""`brat` is one binary: `run` and `check` must not start Python.

The native front end answers the verbs it owns (`ast lex check flat cc libs
build run lsp`) and hands every other verb to `lang/engine/cli.py` by exec, so
the Python interpreter stays reachable for `fmt api deps explain pkg test` and
`--interp`. This measures the difference the split is for: a native `run` of a
program that prints nothing is a process start and nothing else, while the same
job through Python pays the interpreter start on every invocation.

The fallback is checked by behaviour, not by source: a Python-only verb must
still work through the native binary, and `--interp` must still find the
interpreter. A broken path prints the interpreter's own error, which is the
failure this test exists to catch.
"""
import _scratch
import _skip

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BIN = os.path.join(ROOT, "lang", "cxx", "bin", "brat-cxx")
TMP = _scratch.mkdtemp(prefix="brat-native-startup")

# A native start is a few milliseconds. Anything near Python's own start is the
# shim not being on the path at all, which is what this bound is for; it is
# deliberately loose so a loaded machine cannot fail the test.
BUDGET_MS = 60.0

EMPTY = """fn main() -> i64 {
  return 0
}
"""

CHECK_SRC = """fn add(a, b) -> i64 {
  return a + b
}
fn main() -> i64 {
  return add(1, 2)
}
"""


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def best_ms(args, rounds=5):
    """Fastest of `rounds`, because a cold page cache is not the subject."""
    best = None
    for _ in range(rounds):
        t0 = time.time()
        p = run(args)
        dt = (time.time() - t0) * 1000.0
        assert p.returncode == 0, "%r failed: %s%s" % (args, p.stdout, p.stderr)
        best = dt if best is None else min(best, dt)
    return best


def main():
    if not os.path.exists(BIN):
        _skip.skip("no native binary at lang/cxx/bin/brat-cxx (build it first)")

    src = os.path.join(TMP, "empty.brat")
    with open(src, "w", encoding="utf-8") as f:
        f.write(EMPTY)
    chk = os.path.join(TMP, "check.brat")
    with open(chk, "w", encoding="utf-8") as f:
        f.write(CHECK_SRC)

    n = 0

    def check(cond, msg):
        nonlocal n
        assert cond, msg
        n += 1

    # Native dispatch: the verbs in the whitelist are answered in-process.
    t_run = best_ms([BIN, "run", src])
    t_check = best_ms([BIN, "check", chk])
    check(t_run < BUDGET_MS, "native run took %.0f ms, want under %.0f" % (t_run, BUDGET_MS))
    check(t_check < BUDGET_MS, "native check took %.0f ms, want under %.0f" % (t_check, BUDGET_MS))

    # The Python start is what the shim avoids. Measured here, not assumed: if
    # CPython ever started faster than a native process this test would stop
    # meaning anything, and it says so instead of passing quietly.
    t_py = best_ms([sys.executable, "-c", "pass"])
    check(t_run < t_py, "native run (%.0f ms) is not faster than starting python (%.0f ms)"
          % (t_run, t_py))

    # Fallback: a verb the native side does not own still works through the
    # same binary, which means the exec into cli.py happened and cli.py found
    # its engine directory from /proc/self/exe.
    messy = os.path.join(TMP, "messy.brat")
    with open(messy, "w", encoding="utf-8") as f:
        f.write("fn main() -> i64 {\n      return 0\n}\n")
    p = run([BIN, "fmt", messy])
    check(p.returncode == 0, "brat fmt through the native binary failed: %s%s"
          % (p.stdout, p.stderr))
    with open(messy, encoding="utf-8") as f:
        formatted = f.read()
    check("\n  return 0\n" in formatted,
          "brat fmt left the file unformatted, so Python did not run: %r" % formatted)

    # `--interp` is Python-only by design: the native binary must hand it over
    # rather than refuse it as an unknown flag.
    p = run([BIN, "run", "--interp", src])
    check(p.returncode == 0, "run --interp through the native binary failed: %s%s"
          % (p.stdout, p.stderr))
    check("unknown" not in p.stderr.lower(), "run --interp was refused: %s" % p.stderr)

    print("NATIVE STARTUP ALL GREEN (%d asserts, run %.1f ms, check %.1f ms, python %.1f ms)"
          % (n, t_run, t_check, t_py))
    return 0


if __name__ == "__main__":
    sys.exit(main())
