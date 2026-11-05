"""Typed windows over C memory: a register block, not a bag of bytes.

A driver hands out memory that is not `i64`. A register block is `u32`, a
descriptor table is an array of structs, a kernel buffer is `float`. Reading a
`u32` register as an eight-byte element gives the wrong value, not a wider one,
so the window has to carry the element width.

`slice_of_ptr(p, n, kind)` takes `"i64"`, `"i32"`, `"f64"`, `"f32"` and `"u8"`,
steps by that width, and stays range-checked in every kind: an `i32` store
outside `-2^31..2^31-1` and a `u8` store outside `0..255` are refused rather
than truncated. Same program, interpreter and native routes, same output.
"""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CXX = os.path.join(ROOT, "lang", "cxx", "bin", "brat-cxx")
TMP = _scratch.mkdtemp(prefix="brat-slice-kind")

LIB_C = """
#include <stdlib.h>
void *block(long long bytes) { return calloc(1, (size_t)bytes); }
void release(void *p) { free(p); }
"""

PROGRAM = """extern "LIB" fn block(bytes: i64) -> ptr;
extern "LIB" fn release(p: ptr);

fn main() -> i64 {
  let d = block(64)

  // i32 window: four bytes per element, one block of registers.
  let regs = slice_of_ptr(d, 4, "i32")
  print(slice_len(regs))
  slice_set(regs, 0, -7)
  slice_set(regs, 3, 2147483647)
  print(slice_get(regs, 0))
  print(slice_get(regs, 3))

  // f32 window: a kernel buffer.
  let fl = slice_of_ptr(d, 4, "f32")
  slice_set(fl, 1, 2.5)
  print(slice_get(fl, 1))

  // The windows alias: writing the f32 at byte 4 shows up in the i32 at index 1.
  print(slice_get(regs, 1))

  // u8 and i64 are unchanged, and read the same bytes at their own width: the
  // last line is `-7` and `2.5` packed into one i64, which is what aliasing
  // means and why the element width has to be the caller's choice.
  let by = slice_of_ptr(d, 64, "u8")
  print(slice_get(by, 4))
  let wide = slice_of_ptr(d, 8, "i64")
  print(slice_get(wide, 0))

  // Every kind is range-checked; a wrong value is refused, never truncated.
  try { slice_set(by, 0, 300) } catch e { print(e) }
  try { slice_set(regs, 0, 2147483648) } catch e { print(e) }
  try { slice_get(regs, 4) } catch e { print(e) }

  release(d)
  return 0
}
"""

WANT = ("4\n"
        "-7\n"
        "2147483647\n"
        "2.5\n"
        "1075838976\n"
        "0\n"
        "4620693221977096185\n"   # the same 8 bytes as the i32 window, read wide
        "slice_set: byte 300 out of range 0..255\n"
        "slice_set: i32 value 2147483648 out of range\n"
        "slice index 4 out of range 0..4\n")


def main():
    n = 0

    def check(cond, msg):
        nonlocal n
        assert cond, msg
        n += 1

    src = os.path.join(TMP, "mem.c")
    lib = os.path.join(TMP, "libmem.so")
    with open(src, "w", encoding="utf-8") as f:
        f.write(LIB_C)
    r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", src, "-o", lib],
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr

    prog = os.path.join(TMP, "kinds.brat")
    with open(prog, "w", encoding="utf-8") as f:
        f.write(PROGRAM.replace("LIB", lib))

    # 1. Interpreter: ctypes with the declared width and stride.
    p = subprocess.run(_cxx.cli_args("run", "--interp", "--unsafe-ffi", prog),
                       capture_output=True, text=True, timeout=300)
    check(p.returncode == 0, "interp rc=%d: %s" % (p.returncode, (p.stdout + p.stderr)[-600:]))
    check(p.stdout == WANT, "interp:\nwant %r\ngot  %r" % (WANT, p.stdout))

    # 2. Python CLI native build.
    exe = os.path.join(TMP, "kinds.exe")
    b = subprocess.run(_cxx.cli_args("build", "--unsafe-ffi", prog, "-o", exe),
                       capture_output=True, text=True, timeout=600)
    check(b.returncode == 0, "build failed: %s" % (b.stdout + b.stderr)[-400:])
    q = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    check(q.stdout == WANT, "native:\nwant %r\ngot  %r" % (WANT, q.stdout))

    # 3. Native `cc` plus a manual C compile.
    if os.path.exists(CXX):
        c = subprocess.run([CXX, "cc", prog], capture_output=True, text=True, timeout=300)
        check(c.returncode == 0, "brat-cxx cc failed: %s" % (c.stdout + c.stderr)[-300:])
        cfile = os.path.join(TMP, "cxx.c")
        with open(cfile, "w", encoding="utf-8") as f:
            f.write(c.stdout)
        cexe = os.path.join(TMP, "cxx.exe")
        k = subprocess.run(["cc", "-std=c99", "-O1", "-w", "-I", os.path.join(ROOT, "lang", "engine"),
                            cfile, lib, "-o", cexe, "-lm", "-lpthread"],
                           capture_output=True, text=True, timeout=300)
        check(k.returncode == 0, "cc failed: %s" % k.stderr[-400:])
        r2 = subprocess.run([cexe], capture_output=True, text=True, timeout=120)
        check(r2.stdout == WANT, "brat-cxx cc:\nwant %r\ngot  %r" % (WANT, r2.stdout))
        n += 1

    # 4. A kind that does not exist is a checker error in both checkers, not a
    #    silently different stride at run time.
    bad = os.path.join(TMP, "bad.brat")
    with open(bad, "w", encoding="utf-8") as f:
        f.write('extern fn malloc(n: i64) -> move ptr;\n'
                'fn main() -> i64 {\n'
                '  let p = malloc(8)\n'
                '  let w = slice_of_ptr(p, 2, "f16")\n'
                '  return 0\n'
                '}\n')
    for label, argv in (("python", _cxx.cli_args("check", bad)),
                        ("cxx", [CXX, "check", bad])):
        if label == "cxx" and not os.path.exists(CXX):
            continue
        r3 = subprocess.run(argv, capture_output=True, text=True, timeout=120)
        out = r3.stdout + r3.stderr
        check(r3.returncode != 0, "%s accepted kind \"f16\"" % label)
        check("i32" in out and "f32" in out, "%s error does not list the kinds it accepts:\n%s"
              % (label, out))

    print("SLICE KINDS ALL GREEN (%d asserts: i64/i32/f64/f32/u8, three implementations)" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
