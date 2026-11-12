"""`brat` is one binary: `run` and `check` start within a time budget and `fmt` needs nothing on PATH."""
import _cxx
import _scratch

import os
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-native-startup")

BUDGET_MS = 80.0

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
    best = None
    for _ in range(rounds):
        t0 = time.time()
        p = run(args)
        dt = (time.time() - t0) * 1000.0
        assert p.returncode == 0, "%r failed: %s%s" % (args, p.stdout, p.stderr)
        best = dt if best is None else min(best, dt)
    return best


def main():
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

    # Prime compile cache once so we measure cached `brat run` dispatch.
    p0 = run([BIN, "run", src])
    check(p0.returncode == 0, "initial run failed: %s%s" % (p0.stdout, p0.stderr))

    t_run = best_ms([BIN, "run", src])
    t_check = best_ms([BIN, "check", chk])
    check(t_run < BUDGET_MS, "run took %.0f ms, want under %.0f" % (t_run, BUDGET_MS))
    check(t_check < BUDGET_MS, "check took %.0f ms, want under %.0f" % (t_check, BUDGET_MS))

    no_path = dict(os.environ, PATH="/nonexistent")
    messy = os.path.join(TMP, "messy.brat")
    with open(messy, "w", encoding="utf-8") as f:
        f.write("fn main() -> i64 {\n      return 0\n}\n")
    p = run([BIN, "fmt", messy], env=no_path)
    check(p.returncode == 0, "brat fmt failed without PATH: %s%s" % (p.stdout, p.stderr))
    with open(messy, encoding="utf-8") as f:
        formatted = f.read()
    check("\n  return 0\n" in formatted, "brat fmt left file unformatted: %r" % formatted)

    p_bad = run([BIN, "run", "--unknown-flag", src])
    check(p_bad.returncode == 2 and "unknown flag" in p_bad.stderr,
          "unknown CLI flag must be rejected cleanly: rc=%d stderr=%r" % (p_bad.returncode, p_bad.stderr))

    print("NATIVE STARTUP ALL GREEN (%d asserts, run %.1f ms, check %.1f ms)" % (n, t_run, t_check))


if __name__ == "__main__":
    main()
