"""errno-checked extern calls.

`-> ptr errno` and `-> i32 errno` turn a failing C call into a BRAT error. Failure
means NULL for `ptr` and -1 for integers; the message is `name: strerror(errno)`.
`run -O0` and the built binary must raise the same message, catchable with
try/catch.
"""
import _cxx
import _scratch
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = _scratch.mkdtemp(prefix="brat-ffierr-")

CASES = {
    # NULL + errno -> error; message carries the libc strerror text.
    "fopen_missing": (
        'extern fn fopen(path: str, mode: str) -> ptr errno;\n'
        'fn main() -> i64 {\n'
        '    try {\n'
        '        let f = fopen("/no/such/brat/file", "r")\n'
        '        print("opened")\n'
        '    } catch e {\n'
        '        print(e)\n'
        '    }\n'
        '    return 0;\n'
        '}\n',
        b"fopen: No such file or directory\n"),
    # Success path: non-NULL result passes through unchanged.
    "fopen_ok": (
        'extern fn fopen(path: str, mode: str) -> ptr errno;\n'
        'fn main() -> i64 {\n'
        '    try {\n'
        '        let f = fopen("/dev/null", "r")\n'
        '        print("opened")\n'
        '    } catch e {\n'
        '        print(e)\n'
        '    }\n'
        '    return 0;\n'
        '}\n',
        b"opened\n"),
    # Integer convention: -1 + errno -> error.
    "access_missing": (
        'extern fn access(path: str, mode: i32) -> i32 errno;\n'
        'fn main() -> i64 {\n'
        '    try {\n'
        '        let r = access("/no/such/brat/file", 0)\n'
        '        print(r)\n'
        '    } catch e {\n'
        '        print(e)\n'
        '    }\n'
        '    return 0;\n'
        '}\n',
        b"access: No such file or directory\n"),
    # Integer success: 0 passes through.
    "access_ok": (
        'extern fn access(path: str, mode: i32) -> i32 errno;\n'
        'fn main() -> i64 {\n'
        '    try {\n'
        '        print(access("/dev/null", 0))\n'
        '    } catch e {\n'
        '        print(e)\n'
        '    }\n'
        '    return 0;\n'
        '}\n',
        b"0\n"),
}


def run_o0(path):
    p = subprocess.run(_cxx.cli_args("run", "-O0", "--unsafe-ffi", path),
                       capture_output=True, timeout=120)
    return p.returncode, p.stdout


def run_native(src, tag):
    path = os.path.join(TMP, tag + ".brat")
    exe = os.path.join(TMP, tag + ".exe")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    b = subprocess.run(_cxx.cli_args("build", "--unsafe-ffi", path, "-o", exe),
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
        ri, oi = run_o0(path)
        assert ri == 0 and oi == want, "%s run_o0: rc=%d out=%r want %r" % (name, ri, oi, want)
        rn, on = run_native(src, name)
        assert rn == 0 and on == want, "%s native: rc=%d out=%r want %r" % (name, rn, on, want)
        n += 1
        print("ffierr ok: %s" % name)
    print("FFI ERRNO ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()
