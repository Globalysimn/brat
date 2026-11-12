"""Associated functions `Type.f(x)` declared in `impl` without `self`, and clear diagnostics when `self` is required."""
import _cxx
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CXX = os.path.join(ROOT, "lang", "cxx", "bin", "brat-cxx")

ASSOC = """impl Point {
  fn make(v) -> i64 {
    return v + 1
  }
}
fn main() -> i64 {
  print(Point.make(41))
  return 0
}
"""

WANTS_SELF = """enum Color { Red, Green(v) }
impl Color {
  fn code(self) -> i64 {
    return 1
  }
}
fn main() -> i64 {
  print(Color.code())
  return 0
}
"""

VARIANT = """enum Color { Red, Green(v) }
impl Color {
  fn code(self) -> i64 {
    match self {
      Red => { return 1 },
      Green(v) => { return v },
      _ => { return 0 }
    }
    return 0
  }
}
fn main() -> i64 {
  print(Red.code())
  return 0
}
"""


def write(tmp, name, src):
    path = os.path.join(tmp, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=600)


def main():
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        assoc = write(tmp, "assoc.brat", ASSOC)

        r = run(_cxx.cli_args("run", "-O0", assoc))
        assert r.returncode == 0 and r.stdout == "42\n", \
            "run_o0: rc=%d %r %r" % (r.returncode, r.stdout, r.stderr)

        exe = os.path.join(tmp, "assoc.exe")
        b = run(_cxx.cli_args("build", assoc, "-o", exe))
        assert b.returncode == 0, "build: %r" % (b.stdout + b.stderr)
        n = run([exe])
        assert n.returncode == 0 and n.stdout == "42\n", \
            "native: rc=%d %r" % (n.returncode, n.stdout)

        # A variant receiver still dispatches on the variant, not on the name.
        variant = write(tmp, "variant.brat", VARIANT)
        r = run(_cxx.cli_args("run", "-O0", variant))
        assert r.returncode == 0 and r.stdout == "1\n", \
            "variant run_o0: rc=%d %r %r" % (r.returncode, r.stdout, r.stderr)

        # Calling an instance method through the type name: one sentence that
        # names the fix, where the emitter alone said `unresolved call`.
        needs = write(tmp, "needs.brat", WANTS_SELF)
        r = run(_cxx.cli_args("run", "-O0", needs))
        assert r.returncode != 0, "run_o0 accepted Color.code()"
        assert "needs instance" in (r.stdout + r.stderr), \
            "run_o0 wording: %r" % (r.stdout + r.stderr)
        if os.path.exists(CXX):
            c = run([CXX, "build", needs, "-o", os.path.join(tmp, "needs.exe")])
            assert c.returncode != 0, "cxx accepted Color.code()"
            assert "needs instance" in (c.stdout + c.stderr), \
                "cxx wording: %r" % (c.stdout + c.stderr)
    print("assoc fn tests ok")


main()
