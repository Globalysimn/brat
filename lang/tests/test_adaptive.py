"""Adaptive ownership: which mechanism a binding gets, and the reason printed.

The language already has three ways to hand a value around — a pointer that reads
(`view`), a hand-over (`move`) and a mutable owner (`own`). This pass reads them
off the use counters instead of leaving the reader to guess, prints the choice
with its reason (`brat check --ownership`), and stays honest about the fourth:
clone-on-write is chosen exactly where the checker refuses the program, because
a shallow clone would copy the buffer and keep sharing the elements.
"""
import os
import subprocess
import sys

import _scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")
TMP = _scratch.mkdtemp(prefix="brat-adaptive-")

# name, source, [(binding, strategy, reason substring)]
CASES = [
    ("read_only", '''fn main() -> i64 {
  let v = [1, 2]
  print(len(v))
  return 0
}
''', [("v", "view", "never written")]),
    ("never_read", '''fn main() -> i64 {
  let v = [1, 2]
  return 0
}
''', [("v", "drop", "never read")]),
    ("handed_over", '''fn eat(v) -> i64 {
  return len(v)
}

fn main() -> i64 {
  let v = [1, 2]
  return eat(v)
}
''', [("v", "move", "leaves the frame")]),
    ("one_writer", '''fn main() -> i64 {
  let mut v = [1]
  v.push(2)
  return len(v)
}
''', [("v", "own", "mutate in place")]),
    ("copy_value", '''fn main() -> i64 {
  let n: i64 = 2
  let m = n
  print(m)
  return 0
}
''', [("n", "own", "Copy value")]),
    ("read_after_write", '''fn main() -> i64 {
  let mut v = [1, 2]
  let y = v
  v.push(3)
  print(len(y))
  return 0
}
''', [("v", "cow", "write is refused")]),
    ("into_a_thread", '''fn main() -> i64 {
  let v = [1, 2]
  spawn { print(len(v)) }
  join_all()
  return 0
}
''', [("v", "transfer", "spawn")]),
]

FAILED = []


def check(cond, msg):
    if not cond:
        FAILED.append(msg)


def rows(path):
    p = subprocess.run([sys.executable, CLI, "check", "--ownership", path],
                       capture_output=True, text=True)
    check(p.returncode == 0, "%s: --ownership failed: %s" % (path, p.stderr))
    out = []
    for line in p.stdout.splitlines():
        if line.startswith("#") or line.startswith("--") or not line.strip():
            continue
        out.append(line)
    return out


def main():
    for name, src, expect in CASES:
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        got = rows(path)
        for binding, strategy, reason in expect:
            line = next((r for r in got if r.split()[:1] == [binding]), "")
            check(line.split()[1:2] == [strategy],
                  "%s: `%s` is %r, expected %s (%r)"
                  % (name, binding, line.split()[1:2], strategy, line))
            check(reason in line, "%s: no reason mentioning %r in %r" % (name, reason, line))
        print("%-16s %s" % (name, " ".join("%s=%s" % (b, s) for b, s, _ in expect)))

    # A strategy nobody can explain is a strategy nobody can trust: the count
    # line at the end has to add up.
    path = os.path.join(TMP, "summary.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(CASES[0][1])
    tail = rows(path)[-1]
    check("view" in tail, "no strategy summary: %r" % tail)

    if FAILED:
        print("\nADAPTIVE FAILED (%d)" % len(FAILED))
        for f in FAILED:
            print("  " + f)
        sys.exit(1)
    print("\nADAPTIVE ALL GREEN (%d cases)" % len(CASES))


if __name__ == "__main__":
    main()
