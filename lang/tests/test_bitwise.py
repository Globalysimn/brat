"""Bitwise operators (`&`, `|`, `^`, `~`, `<<`, `>>`, compound assignments) and
`0x`/`0b`/`0o` integer literals across lexer, checker, formatter, `-O0`, and `-O2`."""
import _cxx
import _scratch

import os
import subprocess
import sys

BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-bitwise-")


def run_cli(*args):
    return subprocess.run([BIN, *args], capture_output=True, text=True, timeout=60)


def main():
    prog = os.path.join(TMP, "bits.brat")
    src = """fn bits(x: i64) -> i64 {
  return ((x & 0xff) | 0b1000) ^ 0o7
}

fn main() -> i64 {
  assert(bits(0x1234) == 59)
  assert(0xdead_beef == 3735928559)
  assert(0b1010_0101 == 165)
  assert(0o755 == 493)
  assert(~0 == -1)
  assert((-16 >> 2) == -4)
  assert((1 << 64) == 1)
  assert((1 + 2 << 3) == 24)
  assert((0xf0 & 0x3c | 0x03) == 0x33)

  let mut n = 0xff
  n &= 0b1111
  n |= 0o20
  n ^= 1
  n <<= 1
  n >>= 1
  assert(n == 30)
  print("ok")
  return 0
}
"""
    with open(prog, "w", encoding="utf-8") as f:
        f.write(src)

    for opt in ("-O0", "-O1", "-O2"):
        r = run_cli("run", opt, prog)
        assert r.returncode == 0 and r.stdout == "ok\n", (opt, r.returncode, r.stdout, r.stderr)

    rf = run_cli("fmt", prog)
    assert rf.returncode == 0, rf.stderr
    formatted = open(prog, encoding="utf-8").read()
    assert "0xff" in formatted and "0b1000" in formatted and "0o7" in formatted, formatted
    r2 = run_cli("run", "-O2", prog)
    assert r2.returncode == 0 and r2.stdout == "ok\n", (r2.returncode, r2.stdout, r2.stderr)

    for bad_lit in ("0x_1", "0b_1", "0o_7", "0x", "0b2", "0o8", "1__0", "0xff_"):
        bad_p = os.path.join(TMP, "invalid.brat")
        with open(bad_p, "w", encoding="utf-8") as f:
            f.write(f"fn main() -> i64 {{\n  let x = {bad_lit}\n  return x\n}}\n")
        rc = run_cli("check", bad_p)
        assert rc.returncode != 0 and "invalid integer literal" in (rc.stdout + rc.stderr), (
            bad_lit, rc.returncode, rc.stdout, rc.stderr)

    bad_ty = os.path.join(TMP, "bad_type.brat")
    with open(bad_ty, "w", encoding="utf-8") as f:
        f.write("fn main() -> i64 {\n  let x = 1.5 & 2\n  return 0\n}\n")
    rt = run_cli("check", bad_ty)
    assert rt.returncode != 0, (rt.stdout, rt.stderr)

    print("BITWISE ALL GREEN")


if __name__ == "__main__":
    main()
