"""Runtime edge cases at -O0 and -O2, literal expected output.

Covers: Unicode simple and multi-char case mapping, replace with an empty pattern
(which used to size its buffer with an overflowable product), str() of an int,
`for` over a string, which walks code points, the message of a closure called
with the wrong number of arguments, `i64()` / `f64()` between the two number
types, and `chr()` / `ord()` between a code point and its string.
"""
import _cxx
import _scratch
import os
import subprocess

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
    let word = "añb"
    let mut seen = ""
    for ch in word {
        if ch == "b" {
            break
        }
        seen = ch + seen
    }
    print(seen)
    for ch in word.upper() {
        print(ch)
    }
    let add = fn(a, b) { return a + b }
    try {
        print(add(1))
    } catch e {
        print(e)
    }
    print(i64(3.99), i64(-3.99), f64(7) / 2)
    for bad in [1e30, "7"] {
        try {
            print(i64(bad))
        } catch e {
            print(e)
        }
    }
    print(chr(98), chr(0x44F), ord("b"), ord("я"), ord(chr(0x1F600)))
    for bad in [-1, 0xD800, 0x110000, "b"] {
        try {
            print(chr(bad))
        } catch e {
            print(e)
        }
    }
    for bad in ["", "ab", 98] {
        try {
            print(ord(bad))
        } catch e {
            print(e)
        }
    }
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
    "ña\n"
    "A\n"
    "Ñ\n"
    "B\n"
    "function wants 2 args, got 1\n"
    "3 -3 3.5\n"
    "integer out of range\n"
    "not a number\n"
    "b я 98 1103 128512\n"
    "not a code point\n"
    "not a code point\n"
    "not a code point\n"
    "not a code point\n"
    "ord wants one character\n"
    "ord wants one character\n"
    "ord wants one character\n"
).encode("utf-8")


def main():
    path = os.path.join(TMP, "edges.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(SRC)
    ri = subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True, timeout=120)
    assert ri.returncode == 0 and ri.stdout == WANT, "run_o0 rc=%d out=%r" % (ri.returncode, ri.stdout)
    exe = os.path.join(TMP, "edges.exe")
    b = subprocess.run(_cxx.cli_args("build", path, "-o", exe),
                       capture_output=True, timeout=300)
    assert b.returncode == 0, "build failed:\n" + (b.stdout + b.stderr)[-600:].decode("utf-8", "replace")
    rn = subprocess.run([exe], capture_output=True, timeout=60)
    assert rn.returncode == 0 and rn.stdout == WANT, "native rc=%d out=%r" % (rn.returncode, rn.stdout)
    print("EDGES ALL GREEN (29 lines, -O0 and -O2)")


if __name__ == "__main__":
    main()
