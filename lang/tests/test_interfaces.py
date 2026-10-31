"""Module interfaces: circle.bri lists exported functions with their parameter counts."""
import _scratch
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-ifaces-")

CIRCLE_OK = (
    "fn area(r: i64) -> i64 {\n"
    "  return r * r\n"
    "}\n")

MAIN_AREA = (
    "import circle\n"
    "fn main() -> i64 {\n"
    "  print(circle.area(4))\n"
    "  return 0\n"
    "}\n")


def write(rel, src):
    p = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)
    return p


def run(path):
    return subprocess.run([sys.executable, CLI, "run", "--interp", path], capture_output=True, timeout=120)


def build(path, exe):
    return subprocess.run([sys.executable, CLI, "build", path, "-o", exe],
                          capture_output=True, timeout=300)


def case(name, impl, bri, main_src):
    d = os.path.join(TMP, name)
    write(os.path.join(name, "circle.brat"), impl)
    if bri is not None:
        write(os.path.join(name, "circle.bri"), bri)
    return write(os.path.join(name, "main.brat"), main_src)


def main():
    n = 0

    # Non-pub function listed in the interface is exported; both backends agree.
    main_ok = case("ok", CIRCLE_OK, "fn area(r: i64) -> i64\n", MAIN_AREA)
    ri = run(main_ok)
    assert ri.returncode == 0 and ri.stdout == b"16\n", \
        "interp ok rc=%d out=%r err=%r" % (ri.returncode, ri.stdout, ri.stderr[-300:])
    exe = os.path.join(TMP, "ok.exe")
    b = build(main_ok, exe)
    assert b.returncode == 0, "build ok failed:\n" + (b.stdout + b.stderr)[-500:].decode("utf-8", "replace")
    rn = subprocess.run([exe], capture_output=True, timeout=60)
    assert rn.returncode == 0 and rn.stdout == b"16\n", "native ok rc=%d out=%r" % (rn.returncode, rn.stdout)
    n += 3
    print("interfaces ok: matching interface, both backends")

    # Declared but not implemented.
    main_missing = case("missing", CIRCLE_OK,
                        "fn area(r: i64) -> i64\nfn perimeter(r: i64) -> i64\n", MAIN_AREA)
    r = run(main_missing)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    assert r.returncode != 0 and "`perimeter`" in out and "declared in the interface but not defined" in out, \
        "missing: rc=%d out=%r" % (r.returncode, out[-400:])
    n += 1
    print("interfaces ok: missing implementation rejected")

    # Parameter count differs from the interface.
    main_arity = case("arity", CIRCLE_OK, "fn area(r: i64, s: i64) -> i64\n", MAIN_AREA)
    r = run(main_arity)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    assert r.returncode != 0 and "`area`" in out and "2 in the interface" in out, \
        "arity: rc=%d out=%r" % (r.returncode, out[-400:])
    n += 1
    print("interfaces ok: arity mismatch rejected")

    # pub but not listed: stays private.
    impl_hidden = CIRCLE_OK + "pub fn secret() -> i64 {\n  return 1\n}\n"
    main_hidden = case("hidden", impl_hidden, "fn area(r: i64) -> i64\n",
                       "import circle\nfn main() -> i64 {\n  print(circle.secret())\n  return 0\n}\n")
    r = run(main_hidden)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    assert r.returncode != 0 and "`secret` is private in module `circle`" in out, \
        "hidden: rc=%d out=%r" % (r.returncode, out[-400:])
    n += 1
    print("interfaces ok: unlisted pub function stays private")

    # Malformed interface line.
    main_bad = case("badbri", CIRCLE_OK, "fn area(r: i64) -> i64 { return 0 }\n", MAIN_AREA)
    r = run(main_bad)
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    assert r.returncode != 0 and "bad interface" in out, "badbri: rc=%d out=%r" % (r.returncode, out[-400:])
    n += 1
    print("interfaces ok: malformed interface rejected")

    print("INTERFACES ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()
