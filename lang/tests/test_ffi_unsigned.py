"""Unsigned and size FFI types.

u32 / u64 / usize are FFI-only types: BRAT values stay i64, and the boundary checks
the range. Arguments outside the range raise `ffi: arg N of f wants T`. A u64 or
usize result above i64::MAX raises `ffi: result of f does not fit i64`.
Both backends must produce the same output.
"""
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-ffiuns-")

CASES = {
    # u32 return must be read unsigned: htonl(all ones) is all ones.
    "u32_return_unsigned": (
        'extern fn htonl(x: u32) -> u32;\n'
        'fn main() -> i64 {\n'
        '    print(htonl(4294967295))\n'
        '    return 0;\n'
        '}\n',
        b"4294967295\n"),
    # Out-of-range u32 argument is a catchable error, not silent truncation.
    "u32_arg_too_big": (
        'extern fn htonl(x: u32) -> u32;\n'
        'fn main() -> i64 {\n'
        '    try {\n'
        '        print(htonl(4294967296))\n'
        '    } catch e {\n'
        '        print(e)\n'
        '    }\n'
        '    return 0;\n'
        '}\n',
        b"ffi: arg 1 of htonl wants u32\n"),
    "u32_arg_negative": (
        'extern fn htonl(x: u32) -> u32;\n'
        'fn main() -> i64 {\n'
        '    try {\n'
        '        print(htonl(-1))\n'
        '    } catch e {\n'
        '        print(e)\n'
        '    }\n'
        '    return 0;\n'
        '}\n',
        b"ffi: arg 1 of htonl wants u32\n"),
    # usize result that fits i64 passes through.
    "usize_return": (
        'extern fn strlen(s: str) -> usize;\n'
        'fn main() -> i64 {\n'
        '    print(strlen("hello"))\n'
        '    return 0;\n'
        '}\n',
        b"5\n"),
    # u64 result above i64::MAX cannot be a BRAT int: error, not a negative number.
    "u64_result_too_big": (
        'extern fn strtoul(s: str, end: ptr, base: i32) -> u64;\n'
        'fn main() -> i64 {\n'
        '    try {\n'
        '        print(strtoul("18446744073709551615", 0, 10))\n'
        '    } catch e {\n'
        '        print(e)\n'
        '    }\n'
        '    return 0;\n'
        '}\n',
        b"ffi: result of strtoul does not fit i64\n"),
    # i32 arguments are range-checked too (was silent truncation).
    "i32_arg_too_big": (
        'extern fn abs(x: i32) -> i32;\n'
        'fn main() -> i64 {\n'
        '    try {\n'
        '        print(abs(2147483648))\n'
        '    } catch e {\n'
        '        print(e)\n'
        '    }\n'
        '    return 0;\n'
        '}\n',
        b"ffi: arg 1 of abs wants i32\n"),
}


def run_interp(path):
    p = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", path],
                       capture_output=True, timeout=120)
    return p.returncode, p.stdout


def run_native(src, tag):
    path = os.path.join(TMP, tag + ".brat")
    exe = os.path.join(TMP, tag + ".exe")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    b = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", path, "-o", exe],
                       capture_output=True, timeout=300)
    assert b.returncode == 0, "build failed for %s:\n%s" % (
        tag, (b.stdout + b.stderr)[-800:].decode("utf-8", "replace"))
    q = subprocess.run([exe], capture_output=True, timeout=120)
    return q.returncode, q.stdout


def main():
    n = 0
    for name, (src, want) in CASES.items():
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        ri, oi = run_interp(path)
        assert ri == 0 and oi == want, "%s interp: rc=%d out=%r want %r" % (name, ri, oi, want)
        rn, on = run_native(src, name)
        assert rn == 0 and on == want, "%s native: rc=%d out=%r want %r" % (name, rn, on, want)
        n += 1
        print("ffiuns ok: %s" % name)
    print("FFI UNSIGNED ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()
