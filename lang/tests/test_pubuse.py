"""`pub use`: a module re-exports a function from a module it imports."""
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-pubuse-")


def write(rel, src):
    path = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def run(path):
    return subprocess.run([sys.executable, CLI, "run", "--interp", path], capture_output=True, timeout=120)


def expect_both(path, tag, stdout):
    interp = run(path)
    assert interp.returncode == 0 and interp.stdout == stdout, \
        "interp: rc=%d out=%r" % (interp.returncode, interp.stdout + interp.stderr)
    exe = os.path.join(TMP, tag + ".exe")
    built = subprocess.run([sys.executable, CLI, "build", path, "-o", exe],
                           capture_output=True, timeout=300)
    assert built.returncode == 0, (built.stdout + built.stderr).decode("utf-8", "replace")
    native = subprocess.run([exe], capture_output=True, timeout=60)
    assert native.returncode == 0 and native.stdout == stdout, \
        "native: rc=%d out=%r" % (native.returncode, native.stdout + native.stderr)


def expect_error(path, text):
    interp = run(path)
    out = (interp.stdout + interp.stderr).decode("utf-8", "replace")
    assert interp.returncode != 0 and text in out, "run: rc=%d out=%r" % (interp.returncode, out)
    exe = os.path.join(TMP, "bad.exe")
    if os.path.exists(exe):
        os.unlink(exe)
    built = subprocess.run([sys.executable, CLI, "build", path, "-o", exe],
                           capture_output=True, timeout=300)
    out = (built.stdout + built.stderr).decode("utf-8", "replace")
    assert built.returncode != 0 and text in out and not os.path.exists(exe), \
        "build: rc=%d out=%r" % (built.returncode, out)


write("api/a.brat",
      "pub fn helper() -> i64 { return 7 }\n"
      "fn secret() -> i64 { return 1 }\n")
write("api/b.brat",
      "import a\n"
      "pub use a.helper\n"
      "pub fn own() -> i64 { return a.helper() + 1 }\n")
ok = write("api/main.brat",
           "import b\n"
           "fn main() -> i64 {\n"
           "  print(b.helper())\n"
           "  print(b.own())\n"
           "  print(helper())\n"
           "  return 0\n"
           "}\n")
expect_both(ok, "pubuse", b"7\n8\n7\n")

write("bad/a.brat", "pub fn helper() -> i64 { return 7 }\nfn secret() -> i64 { return 1 }\n")
write("bad/b.brat", "import a\npub use a.secret\n")
private = write("bad/main.brat", "import b\nfn main() -> i64 { print(b.secret()); return 0 }\n")
expect_error(private, "function `secret` is private in module `a`")

write("missing/a.brat", "pub fn helper() -> i64 { return 7 }\n")
write("missing/b.brat", "import a\npub use a.nope\n")
missing = write("missing/main.brat", "import b\nfn main() -> i64 { return 0 }\n")
expect_error(missing, "module `a` has no function `nope`")

# A facade one directory up: `geo.brat` re-exports a function from `geo/circle.brat`
# and a caller of `import geo` writes `geo.area(...)`. The cases above are flat, so
# this is the one that would catch a re-export that only works at depth one.
write("facade/geo/circle.brat",
      "pub fn area(r: i64) -> i64 { return r * r * 3 }\n"
      "fn secret(r: i64) -> i64 { return r }\n")
write("facade/geo.brat", "import geo.circle\npub use geo.circle.area\n")
facade = write("facade/main.brat",
               "import geo\n"
               "fn main() -> i64 {\n"
               "  print(geo.area(2))\n"
               "  return 0\n"
               "}\n")
expect_both(facade, "facade", b"12\n")

write("facadepriv/geo/circle.brat",
      "pub fn area(r: i64) -> i64 { return r * r * 3 }\n"
      "fn secret(r: i64) -> i64 { return r }\n")
write("facadepriv/geo.brat", "import geo.circle\npub use geo.circle.secret\n")
facadepriv = write("facadepriv/main.brat",
                   "import geo\nfn main() -> i64 { print(geo.secret(2)); return 0 }\n")
expect_error(facadepriv, "function `secret` is private in module `geo.circle`")

write("plain/a.brat", "pub fn helper() -> i64 { return 7 }\n")
write("plain/b.brat", "import a\npub use a.helper\n")
plain = write("plain/main.brat",
              "import a\n"
              "import b\n"
              "fn main() -> i64 {\n"
              "  print(helper())\n"
              "  return 0\n"
              "}\n")
expect_both(plain, "plain", b"7\n")

print("PUBUSE ALL GREEN")
