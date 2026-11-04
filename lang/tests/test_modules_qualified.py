import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-qualified-")


def write(rel, src):
    path = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def run(path):
    return subprocess.run([sys.executable, CLI, "run", "--interp", path],
                          capture_output=True, timeout=120)


def build_and_run(path, tag):
    exe = os.path.join(TMP, tag + ".exe")
    built = subprocess.run([sys.executable, CLI, "build", path, "-o", exe],
                           capture_output=True, timeout=300)
    assert built.returncode == 0, (built.stdout + built.stderr).decode("utf-8", "replace")
    return subprocess.run([exe], capture_output=True, timeout=60)


def expect_both(path, tag, stdout):
    interp = run(path)
    assert interp.returncode == 0 and interp.stdout == stdout, \
        "interp: rc=%d out=%r" % (interp.returncode, interp.stdout + interp.stderr)
    native = build_and_run(path, tag)
    assert native.returncode == 0 and native.stdout == stdout, \
        "native: rc=%d out=%r" % (native.returncode, native.stdout + native.stderr)


def expect_error(path, text):
    interp = run(path)
    out = (interp.stdout + interp.stderr).decode("utf-8", "replace")
    assert interp.returncode != 0 and text in out, "run: rc=%d out=%r" % (interp.returncode, out)
    exe = os.path.join(TMP, "bad.exe")
    built = subprocess.run([sys.executable, CLI, "build", path, "-o", exe],
                           capture_output=True, timeout=300)
    out = (built.stdout + built.stderr).decode("utf-8", "replace")
    assert built.returncode != 0 and text in out and not os.path.exists(exe), \
        "build: rc=%d out=%r" % (built.returncode, out)


def main():
    write("qualified/a/b.brat",
          "fn helper(x: i64) -> i64 { return x + 1 }\n"
          "pub fn f(x: i64) -> i64 { return helper(x) * 2 }\n")
    qualified = write("qualified/main.brat",
                      "import a.b\nfn main() -> i64 {\n"
                      "  print(b.f(20))\n  print(f(5))\n  return 0\n}\n")
    expect_both(qualified, "qualified", b"42\n12\n")
    print("modules qualified ok: qualified call and unique pub on both backends")

    private = write("qualified/private.brat",
                    "import a.b\nfn main() -> i64 {\n"
                    "  print(b.helper(1))\n  return 0\n}\n")
    expect_error(private, "function `helper` is private")
    print("modules qualified ok: private function rejected on both backends")

    write("ambiguous/a.brat", 'pub fn name() -> str { return "A" }\n')
    write("ambiguous/b.brat", 'pub fn name() -> str { return "B" }\n')
    ambiguous = write("ambiguous/main.brat",
                      "import a\nimport b\nfn main() -> i64 {\n"
                      "  print(name())\n  return 0\n}\n")
    expect_error(ambiguous, "ambiguous unqualified name `name`")
    print("modules qualified ok: ambiguous unqualified name rejected on both backends")

    distinct = write("ambiguous/distinct.brat",
                     "import a\nimport b\nfn main() -> i64 {\n"
                     "  print(a.name())\n  print(b.name())\n  return 0\n}\n")
    expect_both(distinct, "distinct", b"A\nB\n")
    print("modules qualified ok: qualification disambiguates on both backends")

    print("MODULES QUALIFIED ALL GREEN (8 asserts)")


if __name__ == "__main__":
    main()
