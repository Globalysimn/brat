"""`f32`: the other float width a C library can ask for, and a kernel's own type.

Every kernel, every CUDA/OpenGL-style API and most audio and graphics libraries
take `float`, not `double`. BRAT has one float type, so `f32` is a boundary
property: the value narrows once on the way into C and widens once on the way
back, and the arithmetic stays BRAT's own. That is the whole contract, and it is
checked here in all four places a type can cross: an argument, a return value, a
field of an `extern struct` (by value, both directions), and a `float *` buffer
read through a pointer.

The library below is compiled here, so the numbers are exact and a mismatch
cannot be blamed on a system header.
"""
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CLI = os.path.join(ROOT, "lang", "engine", "cli.py")
CXX = os.path.join(ROOT, "lang", "cxx", "bin", "brat-cxx")
TMP = _scratch.mkdtemp(prefix="brat-f32")

LIB_C = """
float scale(float x, float k) { return x * k; }
float half_of(double d) { return (float)(d * 0.5); }
double as_double(float f) { return (double)f; }
void fill_f32(float *p, long long n) { for (long long i = 0; i < n; i++) p[i] = (float)i * 0.5f; }
typedef struct { float x; float y; } Vec2;
float dot2(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
Vec2 make2(float x, float y) { Vec2 v = {x, y}; return v; }
"""

PROGRAM = """extern "LIB" fn scale(x: f32, k: f32) -> f32;
extern "LIB" fn half_of(d: f64) -> f32;
extern "LIB" fn as_double(f: f32) -> f64;
extern "LIB" fn fill_f32(p: ptr, n: i64);
extern "LIB" fn dot2(a: Vec2, b: Vec2) -> f32;
extern "LIB" fn make2(x: f32, y: f32) -> Vec2;
extern struct Vec2 { x: f32, y: f32 }
extern fn malloc(n: i64) -> move ptr;
extern fn free(p: move ptr);

fn main() -> i64 {
  print(scale(2.5, 4.0))
  print(half_of(9.0))
  print(as_double(1.5))
  print(dot2(Vec2 { x: 1.5, y: 2.0 }, Vec2 { x: 3.0, y: 0.5 }))
  print(make2(1.25, 2.5).y)
  let p = malloc(16)
  fill_f32(p, 4)
  print(ptr_read_f32(p))
  print(ptr_read_f32(ptr_offset(p, 8)))
  free(move p)
  return 0
}
"""

WANT = "10.0\n4.5\n1.5\n5.5\n2.5\n0.0\n1.0\n"


def build_lib():
    src = os.path.join(TMP, "libf32.c")
    lib = os.path.join(TMP, "libf32.so")
    with open(src, "w", encoding="utf-8") as f:
        f.write(LIB_C)
    r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", src, "-o", lib],
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    return lib


def main():
    n = 0

    def check(cond, msg):
        nonlocal n
        assert cond, msg
        n += 1

    lib = build_lib()
    path = os.path.join(TMP, "f32.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(PROGRAM.replace("LIB", lib))

    # 1. The interpreter: ctypes c_float in, out, and inside a struct.
    p = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", path],
                       capture_output=True, text=True, timeout=300)
    check(p.returncode == 0, "interp rc=%d: %s" % (p.returncode, (p.stdout + p.stderr)[-600:]))
    check(p.stdout == WANT, "interp stdout:\nwant %r\ngot  %r" % (WANT, p.stdout))

    # 2. The Python CLI build: the same program through native C.
    exe = os.path.join(TMP, "f32.exe")
    b = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", path, "-o", exe],
                       capture_output=True, text=True, timeout=600)
    check(b.returncode == 0, "build failed: %s" % (b.stdout + b.stderr)[-600:])
    q = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    check(q.returncode == 0 and q.stdout == WANT,
          "native stdout:\nwant %r\ngot  %r" % (WANT, q.stdout))

    # 3. Native `cc` plus a manual C compile: the driver-independent route.
    if os.path.exists(CXX):
        c = subprocess.run([CXX, "cc", "--no-check", path], capture_output=True,
                           text=True, timeout=300)
        check(c.returncode == 0, "brat-cxx cc failed: %s" % (c.stdout + c.stderr)[-400:])
        cfile = os.path.join(TMP, "cxx.c")
        with open(cfile, "w", encoding="utf-8") as f:
            f.write(c.stdout)
        check("float bf_x;" in c.stdout,
              "an f32 field must be a C float, not int64_t:\n%s"
              % [ln for ln in c.stdout.splitlines() if "typedef struct" in ln])
        cexe = os.path.join(TMP, "cxx.exe")
        k = subprocess.run(["cc", "-std=c99", "-O2", "-w", "-I", os.path.join(ROOT, "lang", "engine"),
                            cfile, lib, "-o", cexe, "-lm", "-lpthread"],
                           capture_output=True, text=True, timeout=300)
        check(k.returncode == 0, "cc failed: %s" % k.stderr[-400:])
        r = subprocess.run([cexe], capture_output=True, text=True, timeout=120)
        check(r.stdout == WANT, "brat-cxx cc stdout:\nwant %r\ngot  %r" % (WANT, r.stdout))
        n += 1

    # 4. A kind the window cannot describe is a checker error, not a guess: a
    #    byte window over floats would have to invent an element size.
    bad = os.path.join(TMP, "bad.brat")
    with open(bad, "w", encoding="utf-8") as f:
        f.write('extern fn malloc(n: i64) -> move ptr;\n'
                'extern fn free(p: move ptr);\n'
                'fn main() -> i64 {\n'
                '  let p = malloc(8)\n'
                '  let s = slice_of_ptr(p, 2, "f16")\n'
                '  free(move p)\n'
                '  return 0\n'
                '}\n')
    bs = subprocess.run([sys.executable, CLI, "check", bad], capture_output=True, text=True, timeout=120)
    check(bs.returncode != 0, "slice_of_ptr accepted a kind it cannot describe")
    n += 1

    print("F32 FFI ALL GREEN (%d asserts, interpreter + native routes agree)" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
