"""Modules: name clashes are errors; non-clashing imports work at -O0 and -O2.

Before this change, two imported files defining `name()` silently kept the first
definition, in the -O0 build and in native code alike, so the program ran and
gave the wrong answer.
"""
import _cxx
import _scratch
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = _scratch.mkdtemp(prefix="brat-mods-")


def write(rel, src):
    p = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)
    return p


def main():
    n = 0
    write("clash/a.brat", 'pub fn name() -> str {\n  return "A"\n}\n')
    write("clash/b.brat", 'pub fn name() -> str {\n  return "B"\n}\n')
    main_clash = write("clash/main.brat",
                       "import a\nimport b\nfn main() -> i64 {\n  print(name())\n  return 0\n}\n")

    # Unoptimized (-O0) refuses to run it, and the message names both files.
    r = subprocess.run(_cxx.cli_args("run", "-O0", main_clash), capture_output=True, timeout=120)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    assert r.returncode != 0 and "defined in both" in out and "`name`" in out, \
        "run should refuse a name clash: rc=%d out=%r" % (r.returncode, out[-400:])
    n += 1
    print("modules ok: run refuses clash")

    # Build refuses it too: no binary is produced.
    exe = os.path.join(TMP, "clash.exe")
    b = subprocess.run(_cxx.cli_args("build", main_clash, "-o", exe),
                       capture_output=True, timeout=300)
    assert b.returncode != 0 and not os.path.exists(exe), "build should refuse a name clash"
    n += 1
    print("modules ok: build refuses clash")

    # Non-clashing imports keep working at -O0 and -O2.
    write("ok/geo/circle.brat", 'pub fn circle_name() -> str {\n  return "circle"\n}\n')
    write("ok/geo/square.brat", 'pub fn square_name() -> str {\n  return "square"\n}\n')
    ok_main = write("ok/main.brat",
                    "import geo.circle\nimport geo.square\n"
                    "fn main() -> i64 {\n  print(circle_name())\n  print(square_name())\n  return 0\n}\n")
    want = b"circle\nsquare\n"
    ri = subprocess.run(_cxx.cli_args("run", "-O0", ok_main), capture_output=True, timeout=120)
    assert ri.returncode == 0 and ri.stdout == want, "run_o0 ok-case rc=%d out=%r" % (ri.returncode, ri.stdout)
    exe2 = os.path.join(TMP, "ok.exe")
    b2 = subprocess.run(_cxx.cli_args("build", ok_main, "-o", exe2),
                        capture_output=True, timeout=300)
    assert b2.returncode == 0, "build ok-case failed:\n" + (b2.stdout + b2.stderr)[-500:].decode("utf-8", "replace")
    rn = subprocess.run([exe2], capture_output=True, timeout=60)
    assert rn.returncode == 0 and rn.stdout == want, "native ok-case rc=%d out=%r" % (rn.returncode, rn.stdout)
    n += 2
    print("modules ok: non-clashing imports at -O0 and -O2")

    print("MODULES ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()
