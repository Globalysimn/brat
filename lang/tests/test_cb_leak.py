"""A callback called from C does not leak per call.

The emitter generates a `brat_cb_<name>` wrapper: it packs the C arguments into a
`Vec`, calls the BRAT function through the normal entry, and unpacks the result.
The vector was never released, so every call from C left a `Vec` (48 bytes) and
its element array behind -- a program that sums a million numbers through a C
callback leaked about 50 MB before this test existed.

The library here calls the callback in a loop, once with an integer and once with
a string, and pings a context pointer, so the wrapper has to handle both packing
paths. The native binary runs under AddressSanitizer and LeakSanitizer and the
interpreter prints the same thing.
"""

import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")
TMP = _scratch.mkdtemp(prefix="brat-cbleak-")
LIB = os.path.join(TMP, "libcbleak.so")

C_SRC = (
    "long long sum_cb(long long n, long long (*cb)(long long, void *), void *ctx)\n"
    "{\n"
    "    long long acc = 0;\n"
    "    for (long long i = 0; i < n; i++) acc += cb(i, ctx);\n"
    "    return acc;\n"
    "}\n"
    "long long tag_cb(long long n, long long (*cb)(long long, void *), void *ctx)\n"
    "{\n"
    "    long long acc = 0;\n"
    "    for (long long i = 0; i < n; i++) acc += cb(i, ctx);\n"
    "    return acc;\n"
    "}\n"
)

with open(os.path.join(TMP, "libcbleak.c"), "w", encoding="utf-8") as f:
    f.write(C_SRC)
built = subprocess.run(["cc", "-shared", "-fPIC", "-O2", os.path.join(TMP, "libcbleak.c"),
                        "-o", LIB], capture_output=True, text=True, timeout=120)
assert built.returncode == 0, built.stderr

SRC = (
    'extern "%s" fn sum_cb(n: i64, cb: fn(i64, ptr) -> i64, ctx: ptr) -> i64;\n'
    'extern "%s" fn tag_cb(n: i64, cb: fn(i64, ptr) -> i64, ctx: ptr) -> i64;\n'
    'extern fn malloc(n: i64) -> ptr;\n'
    'extern fn free(move p: ptr);\n'
    '\n'
    'fn add_ctx(x: i64, ctx: ptr) -> i64 {\n'
    '  return x + ptr_read_i64(ctx)\n'
    '}\n'
    '\n'
    'fn name_ctx(x: i64, ctx: ptr) -> i64 {\n'
    '  let s = "item-{x}".replace("-", "_")\n'
    '  return len(s) + ptr_read_i64(ctx)\n'
    '}\n'
    '\n'
    'fn main() -> i64 {\n'
    '  let ctx = malloc(8)\n'
    '  ptr_write_i64(ctx, 3)\n'
    '  print(sum_cb(100, "add_ctx", ctx))\n'
    '  print(tag_cb(100, "name_ctx", ctx))\n'
    '  free(ctx)\n'
    '  return 0\n'
    '}\n'
) % (LIB, LIB)

path = os.path.join(TMP, "leak.brat")
with open(path, "w", encoding="utf-8") as f:
    f.write(SRC)

want = "5250\n990\n"

interp = subprocess.run([sys.executable, CLI, "run", "--interp", path], capture_output=True, text=True,
                        timeout=300)
assert interp.returncode == 0, "interp rc=%d %r" % (interp.returncode,
                                                   (interp.stdout + interp.stderr)[-800:])
assert interp.stdout == want, "interp out=%r want=%r" % (interp.stdout, want)

c = subprocess.run([sys.executable, CLI, "emit-c", "--unsafe-ffi", path], capture_output=True, text=True,
                   timeout=120)
assert c.returncode == 0, "emit-c: %r" % (c.stdout + c.stderr)[-800:]
cfile = os.path.join(TMP, "leak.c")
with open(cfile, "w", encoding="utf-8") as f:
    f.write(c.stdout)
exe = os.path.join(TMP, "leak.asan")
cc = subprocess.run(["cc", "-fsanitize=address", "-g", "-I", ENGINE, cfile, "-o", exe,
                     "-L", TMP, "-lcbleak", "-lm", "-Wl,-rpath," + TMP],
                    capture_output=True, text=True, timeout=600)
assert cc.returncode == 0, "cc: %r" % (cc.stdout + cc.stderr)[-800:]

run = subprocess.run([exe], capture_output=True, text=True, timeout=300,
                     env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=0"))
out = run.stdout + run.stderr
assert "AddressSanitizer" not in out, "callback wrapper not clean: %s" % out[-1500:]
assert run.stdout == want, "native out=%r want=%r" % (run.stdout, want)

print("CALLBACK LEAK ALL GREEN (200 calls through C, interpreter == native, ASan+LSan clean)")
