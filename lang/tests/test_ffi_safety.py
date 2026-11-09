"""FFI soundness pins.

Expected values are literals taken from the C standard (strcmp sign, getenv
returning NULL for an unset variable) and from the rule that FFI misuse is an
error value, not a crash. Both backends must agree.
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENG = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-ffisafe-")

CASES = {
    # i32 return must be sign-extended in the -O0 build too (C: strcmp < 0).
    "i32_negative_return": (
        'extern fn strcmp(a: str, b: str) -> i32;\n'
        'fn main() -> i32 {\n'
        '    print(strcmp("a", "b"))\n'
        '    return 0;\n'
        '}\n',
        b"-1\n", 0),
    # NULL char* from C must be a catchable error, not a segfault or TypeError.
    "null_str_caught": (
        'extern fn getenv(name: str) -> str;\n'
        'fn main() -> i64 {\n'
        '    try {\n'
        '        let v = getenv("BRAT_FFI_SURELY_UNSET_9Q")\n'
        '        print(len(v))\n'
        '    } catch e {\n'
        '        print("caught")\n'
        '    }\n'
        '    return 0;\n'
        '}\n',
        b"caught\n", 0),
    # A non-str argument to a str parameter must be a catchable error in run.
    "arg_type_caught_interp": (
        'extern fn puts(s: str) -> i32;\n'
        'fn main() -> i64 {\n'
        '    try {\n'
        '        puts(5)\n'
        '    } catch e {\n'
        '        print("caught")\n'
        '    }\n'
        '    return 0;\n'
        '}\n',
        b"caught\n", 0),
}

BUILD_REFUSED = {
    # `brat build` must refuse a program the typer rejects (native would crash).
    "build_refuses_type_error": (
        'extern fn puts(s: str) -> i32;\n'
        'fn main() -> i32 {\n'
        '    puts(5);\n'
        '    return 0;\n'
        '}\n'),
}


def write(name, src):
    p = os.path.join(TMP, name + ".brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)
    return p


def run_interp(path, no_check=False):
    cmd = _cxx.cli_args("run", "-O0", "--unsafe-ffi")
    if no_check:
        cmd.append("--no-check")
    cmd.append(path)
    p = subprocess.run(cmd, capture_output=True, timeout=120)
    return p.returncode, p.stdout


def run_native(src, tag):
    path = write(tag, src)
    exe = os.path.join(TMP, tag + ".exe")
    b = subprocess.run(_cxx.cli_args("build", "--unsafe-ffi", path, "-o", exe),
                       capture_output=True, timeout=300)
    assert b.returncode == 0, "build failed for %s:\n%s" % (
        tag, (b.stdout + b.stderr)[-800:].decode("utf-8", "replace"))
    q = subprocess.run([exe], capture_output=True, timeout=120)
    return q.returncode, q.stdout


def main():
    n = 0
    for name, (src, want, want_rc) in CASES.items():
        ri, oi = run_interp(write(name, src), no_check=(name == "arg_type_caught_interp"))
        assert oi == want and ri == want_rc, (
            "%s run_o0: rc=%d out=%r want rc=%d out=%r"
            % (name, ri, oi, want_rc, want))
        if name != "arg_type_caught_interp":
            rn, on = run_native(src, name)
            assert on == want and rn == want_rc, (
                "%s native: rc=%d out=%r want rc=%d out=%r"
                % (name, rn, on, want_rc, want))
        n += 1
        print("ffisafe ok: %s" % name)

    for name, src in BUILD_REFUSED.items():
        path = write(name, src)
        exe = os.path.join(TMP, name + ".exe")
        b = subprocess.run(_cxx.cli_args("build", "--unsafe-ffi", path, "-o", exe),
                           capture_output=True, timeout=300)
        assert b.returncode != 0 and not os.path.exists(exe), (
            "%s: build should refuse, rc=%d" % (name, b.returncode))
        n += 1
        print("ffisafe ok: %s" % name)

    print("FFI SAFETY ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()
