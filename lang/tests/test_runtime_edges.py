"""Runtime edge cases, both backends, literal expected output (Python semantics).

Covers: Unicode simple and multi-char case mapping, replace with an empty pattern
(which used to size its buffer with an overflowable product), and str() of an int.
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = _scratch.mkdtemp(prefix="brat-edges-")

SRC = r'''fn main() -> i64 {
    print("αβγ".upper())
    print("ΑΒΓ".lower())
    print("ſ".upper())
    print("ı".upper())
    print("ΣΑΣ".lower())
    print("ß".upper())
    print("İ".lower())
    print("ǅ".upper())
    print("ab".replace("", "-"))
    print("".replace("", "-"))
    print("abc".replace("", ""))
    print("a-b-c".replace("-", "::"))
    print(str(42) + "!")
    return 0;
}
'''

WANT = (
    "ΑΒΓ\n"
    "αβγ\n"
    "S\n"
    "I\n"
    "σας\n"
    "SS\n"
    "i\u0307\n"
    "Ǆ\n"
    "-a-b-\n"
    "-\n"
    "abc\n"
    "a::b::c\n"
    "42!\n"
).encode("utf-8")


def main():
    path = os.path.join(TMP, "edges.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(SRC)
    ri = subprocess.run(_cxx.cli_args("run", "--interp", path), capture_output=True, timeout=120)
    assert ri.returncode == 0 and ri.stdout == WANT, "interp rc=%d out=%r" % (ri.returncode, ri.stdout)
    exe = os.path.join(TMP, "edges.exe")
    b = subprocess.run(_cxx.cli_args("build", path, "-o", exe),
                       capture_output=True, timeout=300)
    assert b.returncode == 0, "build failed:\n" + (b.stdout + b.stderr)[-600:].decode("utf-8", "replace")
    rn = subprocess.run([exe], capture_output=True, timeout=60)
    assert rn.returncode == 0 and rn.stdout == WANT, "native rc=%d out=%r" % (rn.returncode, rn.stdout)
    print("EDGES ALL GREEN (13 lines, 2 backends)")


if __name__ == "__main__":
    main()
