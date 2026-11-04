"""BRAT_RECURSION_BUDGET means the same thing to the interpreter and to compiled programs.

A program may nest 2000 BRAT frames by default. The variable sets another limit
when it is an integer in 1..1000000 (strtoll rules: leading blanks and a sign are
fine, trailing characters are not); any other value is ignored. The interpreter
used to call int() on it, so "abc" killed it at import with a misleading
error and "0" raised a ValueError, while compiled programs
shrugged both off.
"""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-recursion-budget-")

SRC = os.path.join(TMP, "depth.brat")
EXE = os.path.join(TMP, "depth")
with open(SRC, "w", encoding="utf-8") as f:
    f.write("""fn depth(n: i64) -> i64 {
  if n == 0 {
    return 0
  }
  return 1 + depth(n - 1)
}

let n = unwrap(args()[0].parse_int())
try {
  print(depth(n))
} catch e {
  print(e)
}
""")

built = subprocess.run([NATIVE, "build", SRC, "-o", EXE], capture_output=True, text=True, timeout=300)
assert built.returncode == 0, built.stderr

DEFAULT = 2000
CASES = [
    (None, DEFAULT),
    ("abc", DEFAULT),
    ("", DEFAULT),
    ("0", DEFAULT),
    ("-5", DEFAULT),
    ("1000001", DEFAULT),
    ("50 ", DEFAULT),
    ("5_0", DEFAULT),
    ("50", 50),
    (" 50", 50),
    ("+60", 60),
]

RUNNERS = {
    "interpreter": lambda depth: [sys.executable, CLI, "run", "--interp", SRC, "--", str(depth)],
    "compiled": lambda depth: [EXE, str(depth)],
}

n = 0


def run(argv, budget):
    env = {k: v for k, v in os.environ.items() if k != "BRAT_RECURSION_BUDGET"}
    if budget is not None:
        env["BRAT_RECURSION_BUDGET"] = budget
    return subprocess.run(argv, capture_output=True, text=True, timeout=300, env=env)


for budget, effective in CASES:
    for label, argv in RUNNERS.items():
        inside = run(argv(effective - 1), budget)
        assert inside.returncode == 0 and inside.stdout == "%d\n" % (effective - 1), \
            "%s, budget %r: depth %d should fit: rc=%d out=%r err=%r" % (
                label, budget, effective - 1, inside.returncode, inside.stdout, inside.stderr[-300:])
        outside = run(argv(effective), budget)
        assert outside.returncode == 0 and outside.stdout == "recursion too deep\n", \
            "%s, budget %r: depth %d should be refused: rc=%d out=%r err=%r" % (
                label, budget, effective, outside.returncode, outside.stdout, outside.stderr[-300:])
        n += 2

print("RECURSION BUDGET ALL GREEN (%d runs)" % n)
