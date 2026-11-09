import _cxx
import _scratch
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENG = os.path.join(ROOT, "lang", "engine")
TMP = _scratch.mkdtemp(prefix="brat-ffi-")

CASES = {
    "puts": (
        'extern fn puts(s: str) -> i32;\n\n'
        'fn main() -> i32 {\n'
        '    puts("ffi puts");\n'
        '    return 0;\n'
        '}\n'
    ),
    "malloc_free": (
        'extern fn malloc(size: i64) -> ptr;\n'
        'extern fn free(p: ptr);\n\n'
        'fn main() -> i32 {\n'
        '    let p = malloc(16);\n'
        '    free(p);\n'
        '    return 0;\n'
        '}\n'
    ),
    "getenv": (
        'extern fn getenv(name: str) -> str;\n\n'
        'fn main() -> i32 {\n'
        '    if getenv("PATH") == "" { return 1; }\n'
        '    return 0;\n'
        '}\n'
    ),
}


def run_interp(src):
    path = os.path.join(TMP, "t.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    p = subprocess.run(_cxx.cli_args("run", "-O0", "--unsafe-ffi", path), capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout.replace("\r\n", "\n")


def run_native(src, tag):
    path = os.path.join(TMP, tag + ".brat")
    exe = os.path.join(TMP, tag + ".exe")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    p = subprocess.run(_cxx.cli_args("build", "--unsafe-ffi", path, "-o", exe),
                       capture_output=True, text=True, timeout=300)
    assert p.returncode == 0, "build failed for %s:\n%s" % (tag, (p.stdout + p.stderr)[-1000:])
    q = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    return q.returncode, q.stdout.replace("\r\n", "\n")


# In a subprocess: a wrapper that packs the wrong number of arguments reads past
# the vector, and the symptom is a segfault. A crash here must be one failed case,
# not a dead test runner.
EXPORT_CHECK = """
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
lib.add.restype = ctypes.c_longlong
lib.add.argtypes = [ctypes.c_longlong, ctypes.c_longlong]
assert lib.add(2, 3) == 5, "add"
class P(ctypes.Structure):
    _fields_ = [("x", ctypes.c_longlong), ("y", ctypes.c_double)]
lib.make.restype = P
lib.make.argtypes = [ctypes.c_longlong, ctypes.c_double]
p = lib.make(7, 2.25)
assert p.x == 7 and abs(p.y - 2.25) < 1e-9, "make: %r" % ((p.x, p.y),)
lib.echo_bool.restype = ctypes.c_bool
lib.echo_bool.argtypes = [ctypes.c_bool]
assert lib.echo_bool(True) is True and lib.echo_bool(False) is False, "echo_bool"
"""


def check_export(libpath, tag):
    """An exported `f64` argument reaches the BRAT function, not just the prototype."""
    p = subprocess.run([sys.executable, "-c", EXPORT_CHECK, libpath],
                       capture_output=True, text=True, timeout=120)
    assert p.returncode == 0, "export (%s): rc=%d %r" % (
        tag, p.returncode, (p.stdout + p.stderr)[-400:])


def main():
    n = 0
    for name, src in CASES.items():
        ri, oi = run_interp(src)
        rn, on = run_native(src, name)
        assert ri == rn, "%s exit: run_o0=%d native=%d" % (name, ri, rn)
        assert oi == on, "%s stdout differs:\ninterp=%r\nnative=%r" % (name, oi, on)
        n += 1
        print("ffi ok: %s" % name)

    # Custom shared library (backend agreement via dynamic linking).
    lib = "/tmp/opencode/libtest/libtest.so"
    if os.path.exists(lib):
        src = (
            'extern "%s" fn add42(x: i64) -> i64;\n\n'
            'fn main() -> i64 {\n'
            '    return add42(1);\n'
            '}\n' % lib
        )
        ri, oi = run_interp(src)
        rn, on = run_native(src, "custom_add42")
        assert ri == rn == 43, "custom exit: run_o0=%d native=%d" % (ri, rn)
        assert oi == on, "custom stdout differs"
        n += 1
        print("ffi ok: custom_add42")

        src = (
            'extern "%s" fn greet(s: str) -> str;\n\n'
            'fn main() -> i32 {\n'
            '    if greet("hi") == "hi" { return 0; }\n'
            '    return 1;\n'
            '}\n' % lib
        )
        ri, oi = run_interp(src)
        rn, on = run_native(src, "custom_greet")
        assert ri == rn == 0, "greet exit: run_o0=%d native=%d" % (ri, rn)
        assert oi == on
        n += 1
        print("ffi ok: custom_greet")

        src = (
            'extern "/tmp/opencode/libtest/libvar.so" fn varsum(n: i64, ..i64) -> i64;\n\n'
            'fn main() -> i64 {\n'
            '    return varsum(3, 10, 20, 30);\n'
            '}\n'
        )
        ri, oi = run_interp(src)
        rn, on = run_native(src, "custom_varargs")
        assert ri == rn == 60, "varargs exit: run_o0=%d native=%d" % (ri, rn)
        assert oi == on
        n += 1
        print("ffi ok: custom_varargs")

        src = (
            'extern struct Point { x: i64, y: i64 }\n\n'
            'extern "/tmp/opencode/libtest/libstruct.so" fn make_point(x: i64, y: i64) -> Point;\n'
            'extern "/tmp/opencode/libtest/libstruct.so" fn add_point(a: Point, b: Point) -> Point;\n'
            'extern "/tmp/opencode/libtest/libstruct.so" fn sum_point(p: Point) -> i64;\n\n'
            'fn main() -> i64 {\n'
            '    let p = make_point(1, 2);\n'
            '    let q = make_point(3, 4);\n'
            '    let r = add_point(p, q);\n'
            '    if sum_point(r) != 10 { return 1; }\n'
            '    if r.x != 4 { return 2; }\n'
            '    if r.y != 6 { return 3; }\n'
            '    return 0;\n'
            '}\n'
        )
        ri, oi = run_interp(src)
        rn, on = run_native(src, "custom_struct")
        assert ri == rn == 0, "struct exit: run_o0=%d native=%d" % (ri, rn)
        assert oi == on
        n += 1
        print("ffi ok: custom_struct")

        src = (
            'extern "/tmp/opencode/libtest/libcb.so" fn apply_cb(x: i64, cb: fn(i64) -> i64) -> i64;\n'
            'extern "/tmp/opencode/libtest/libcb.so" fn sum_cb(a: i64, b: i64, cb: fn(i64, i64) -> i64) -> i64;\n\n'
            'fn double_it(x: i64) -> i64 { return x * 2; }\n'
            'fn add_ab(a: i64, b: i64) -> i64 { return a + b; }\n\n'
            'fn main() -> i64 {\n'
            '    if apply_cb(21, "double_it") != 42 { return 1; }\n'
            '    if sum_cb(10, 20, "add_ab") != 30 { return 2; }\n'
            '    return 0;\n'
            '}\n'
        )
        ri, oi = run_interp(src)
        rn, on = run_native(src, "custom_callback")
        assert ri == rn == 0, "callback exit: run_o0=%d native=%d" % (ri, rn)
        assert oi == on
        n += 1
        print("ffi ok: custom_callback")

    # Exported BRAT function as a C-callable shared library.
    src = (
        'export fn add(a: i64, b: i64) -> i64 { return a + b; }\n'
        'export fn make(x: i64, y: f64) -> Point { return Point { x: x, y: y }; }\n'
        'export fn echo_bool(x: bool) -> bool { return x; }\n'
        'extern struct Point { x: i64, y: f64 }\n'
        'fn main() -> i32 { return 0; }\n'
    )
    epath = os.path.join(TMP, "export.brat")
    with open(epath, "w", encoding="utf-8") as f:
        f.write(src)
    # The wrapper packs the C arguments into a `Vec`; a missed push is an
    # argument the callee never sees, so every exported width is called here.
    libpath = os.path.join(TMP, "libexport.so")
    p = subprocess.run(_cxx.cli_args("build", "--unsafe-ffi", epath, "--shared", "-o", libpath),
                       capture_output=True, text=True, timeout=300)
    assert p.returncode == 0, "shared build failed:\n" + (p.stdout + p.stderr)[-500:]
    check_export(libpath, "native")
    n += 1
    print("ffi ok: export_shared (native emitter)")

    print("FFI ALL GREEN (%d cases)" % n)


if __name__ == "__main__":
    main()