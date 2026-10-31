"""`par for ... reduce(op, unit)`: a fold the checker has proved is a monoid.

Three things are pinned here: the fold is the left-to-right fold of the same
values (so the result is the sequential one), both backends agree on the value,
and both checkers refuse the same non-monoids byte for byte.
"""
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(HERE, "..", "engine")
CLI = os.path.abspath(os.path.join(ENGINE, "cli.py"))
CXX = os.path.abspath(os.path.join(HERE, "..", "cxx", "bin", "brat-cxx"))
TMP = _scratch.mkdtemp(prefix="brat-parfor-")
HAVE_CXX = os.path.exists(CXX)

INT_SRC = '''fn add(a: i64, b: i64) -> i64 { return a + b }
fn sq(x: i64) -> i64 { return x * x }

fn main() -> i64 {
  let mut v = []
  for i in 1..9 {
    v.push(i)
  }
  let par = par for x in v reduce(add, 0) { return sq(x) }
  let mut seq = 0
  for x in v {
    seq = add(seq, sq(x))
  }
  if par != seq {
    print("MISMATCH")
    return 1
  }
  print(par)
  return 0
}
'''

STR_SRC = '''fn cat(a: str, b: str) -> str { return a + b }

fn main() -> i64 {
  let words = ["b", "c", "d"]
  let joined = par for w in words reduce(cat, "a") { return w }
  print(joined)
  return 0
}
'''

EMPTY_SRC = '''fn add(a: i64, b: i64) -> i64 { return a + b }

fn main() -> i64 {
  let v = []
  let t = par for x in v reduce(add, 7) { return x }
  print(t)
  return 0
}
'''

REFUSALS = [
    ("impure_op", '''fn shout(a: i64, b: i64) -> i64 { print(a) return b }
fn main() -> i64 {
  let v = [1, 2]
  return par for x in v reduce(shout, 0) { return x }
}
''', "`shout` cannot fold in parallel: it can io"),
    ("spawning_op", '''fn go(a: i64, b: i64) -> i64 {
  spawn { print(a) }
  join_all()
  return b
}
fn main() -> i64 {
  let v = [1, 2]
  return par for x in v reduce(go, 0) { return x }
}
''', "`go` cannot fold in parallel: it can io, spawn"),
    ("arity", '''fn add3(a: i64, b: i64, c: i64) -> i64 { return a + b + c }
fn main() -> i64 {
  let v = [1, 2]
  return par for x in v reduce(add3, 0) { return x }
}
''', "`add3` takes 3 parameter(s); a fold needs exactly two"),
    ("unknown", '''fn main() -> i64 {
  let v = [1, 2]
  return par for x in v reduce(nope, 0) { return x }
}
''', "`nope` is not a function: `par for` needs the folding operation by name"),
    ("impure_body", '''fn add(a: i64, b: i64) -> i64 { return a + b }
fn main() -> i64 {
  let v = [1, 2]
  return par for x in v reduce(add, 0) { print(x) return x }
}
''', "the `par for` body cannot io"),
    ("outer_write", '''fn add(a: i64, b: i64) -> i64 { return a + b }
fn main() -> i64 {
  let v = [1, 2]
  let mut seen = 0
  let t = par for x in v reduce(add, 0) { seen = seen + x return x }
  return t
}
''', "the `par for` body writes `seen`, which lives outside the loop"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)


def write(name, src):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def both(path):
    """(interpreter output, native output) — building the native one on demand."""
    interp = run([sys.executable, CLI, "run", "--interp", path])
    if interp.returncode not in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10) and interp.stderr:
        raise SystemExit("run failed:\n" + interp.stdout + interp.stderr)
    native = None
    if HAVE_CXX:
        exe = path[:-5] + ".bin"
        b = run([sys.executable, CLI, "build", path, "-o", exe])
        if b.returncode != 0:
            raise SystemExit("build failed:\n" + b.stdout + b.stderr)
        native = run([exe])
        if native.stdout != interp.stdout:
            raise SystemExit("backends disagree on %s:\n--- interp\n%s\n--- native\n%s"
                             % (path, interp.stdout, native.stdout))
    return interp.stdout, native


def check_both(path):
    py = run([sys.executable, CLI, "check", "--py", path])
    text = "\n".join(l for l in (py.stdout + py.stderr).splitlines()
                     if "type error" not in l)
    if HAVE_CXX:
        c = run([CXX, "check", path])
        cx = "\n".join(l for l in (c.stdout + c.stderr).splitlines()
                       if "type error" not in l)
        if text.strip() != cx.strip():
            raise SystemExit("checkers disagree on %s\n--- python\n%s\n--- cxx\n%s"
                             % (path, text, cx))
    return py.returncode, text


def main():
    failures = 0

    out, _ = both(write("int", INT_SRC))
    if out.strip() != "204":        # sum of squares 1..8
        print("FAIL int fold: %r" % out)
        failures += 1

    out, _ = both(write("str", STR_SRC))
    if out.strip() != "abcd":
        print("FAIL string monoid: %r" % out)
        failures += 1

    out, _ = both(write("empty", EMPTY_SRC))
    if out.strip() != "7":
        print("FAIL empty fold should be the unit: %r" % out)
        failures += 1

    for name, src, want in REFUSALS:
        path = write(name, src)
        rc, text = check_both(path)
        if want not in text:
            print("FAIL %s: missing %r\n%s" % (name, want, text))
            failures += 1
        if rc == 0:
            print("FAIL %s: accepted" % name)
            failures += 1

    example = os.path.join(ROOT, "lang", "examples", "parfor.brat")
    if os.path.exists(example):
        rc, text = check_both(example)
        if rc != 0:
            print("FAIL examples/parfor.brat is refused:\n" + text)
            failures += 1
        first, _ = both(example)
        if "parfor ok" not in first:
            print("FAIL examples/parfor.brat printed %r" % first)
            failures += 1

    if failures:
        raise SystemExit("PARFOR FAILED (%d)" % failures)
    print("PARFOR ALL GREEN (3 folds, %d refusals, both checkers%s)"
          % (len(REFUSALS), ", both backends" if HAVE_CXX else ""))


if __name__ == "__main__":
    main()
