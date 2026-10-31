"""Inferred effect rows: a function's row is the union of its body and its callees.

The row is information, not a gate, so every case asserts the printed row from
`brat check --effects` and nothing about exit status.
"""
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-effects-")

SRC = '''fn add(a: i64, b: i64) -> i64 {
  return a + b
}
fn shout(a: i64) -> i64 {
  print(a)
  return add(a, 1)
}
fn risky(a: i64) -> i64 {
  if a < 0 {
    raise("neg")
  }
  return shout(a)
}
fn parse(s: str) -> i64 {
  return s.parse_int()?
}
fn build(n: i64) -> vec {
  let mut v = []
  v.push(n)
  return v
}
fn thread() -> i64 {
  let ch = chan(2)
  spawn {
    ch.send(1)
  }
  join_all()
  return ch.recv()
}
extern fn strlen(s: str) -> i64
fn measure(s: str) -> i64 {
  return strlen(s)
}
fn main() -> i64 {
  return risky(1) + parse("2") + len(build(3)) + thread() + measure("x")
}
'''

WANT = {
    "add": "pure",
    "shout": "io",
    "risky": "raise, io",
    "parse": "raise",
    "build": "alloc",
    "thread": "spawn",
    "measure": "ffi",
    "main": "raise, io, alloc, spawn, ffi",
}


def rows_for(path):
    p = subprocess.run([sys.executable, CLI, "check", "--effects", path],
                       capture_output=True, text=True, timeout=300)
    assert p.returncode == 0, "check --effects failed: %s" % (p.stdout + p.stderr)[-400:]
    rows = {}
    for line in p.stdout.splitlines():
        if line.startswith("#") or "effects:" not in line:
            continue
        name, rest = line.split(" effects:", 1)
        rows[name.strip()] = rest.strip()
    return rows


PURE_SRC = '''fn sq(x: i64) -> i64 {
  return x * x
}
fn twice(x: i64) -> i64 {
  let a = sq(x)
  return a + sq(a)
}
fn main() -> i64 {
  return twice(3)
}
'''

FAILING_SRC = '''fn sq(x: i64) -> i64 {
  return x * x
}
fn half(x: i64) -> i64 {
  let a = sq(x)
  return a / x
}
fn main() -> i64 {
  return half(3)
}
'''


def emit(text, name):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    p = subprocess.run([sys.executable, CLI, "emit-c", path],
                       capture_output=True, text=True, timeout=300)
    assert p.returncode == 0, (p.stdout + p.stderr)[-400:]
    return p.stdout


def fast_bodies(c):
    """The bodies of the emitted bf_fN helpers, keyed by name."""
    out = {}
    lines = c.splitlines()
    for i, line in enumerate(lines):
        if line.startswith("static int64_t bf_f") and line.endswith("{"):
            name = line.split("(", 1)[0].split()[-1]
            j = i
            while lines[j] != "}":
                j += 1
            out[name] = "\n".join(lines[i:j])
    return out


def check_fast_path():
    """The fast path is typed: a call to a callee that cannot fail needs no check."""
    pure = fast_bodies(emit(PURE_SRC, "pure.brat"))
    assert pure, "no fast helper was emitted for pure arithmetic"
    for name, body in pure.items():
        if "verr_pending" in body:
            raise SystemExit("FAIL %s emitted a needless verr_pending check:\n%s" % (name, body))
    failing = fast_bodies(emit(FAILING_SRC, "failing.brat"))
    guarded = [n for n, b in failing.items() if "bf_idiv" in b and "verr_pending" in b]
    if not guarded:
        raise SystemExit("FAIL a division on the fast path lost its verr_pending guard")


def main():
    path = os.path.join(TMP, "effects.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(SRC)
    got = rows_for(path)
    for name, want in WANT.items():
        if got.get(name) != want:
            raise SystemExit("FAIL %s: want %r, got %r" % (name, want, got.get(name)))
    if got.get("strlen") != "ffi":
        raise SystemExit("FAIL extern row: %r" % (got.get("strlen"),))
    if len(got) != len(WANT) + 1:
        raise SystemExit("FAIL row count: %r" % (sorted(got),))
    if "pure" not in got.get("add", ""):
        raise SystemExit("FAIL a function with no effects must print `pure`")
    check_fast_path()
    print("EFFECTS ALL GREEN (%d rows, fast path by type)" % len(got))


if __name__ == "__main__":
    main()
