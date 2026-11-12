"""Out-parameters, and the memory a program allocates and fills itself.

A C function that returns by writing through a pointer is the shape of most of
libc and of every driver ioctl wrapper: `ioctl(fd, req, &arg)`, `clock_gettime`,
`getaddrinfo`, `curl_easy_getinfo`. Before this file, BRAT could only pass a
pointer it got from somewhere else, which meant every out-parameter went through
`alloc`, a cast and a manual `ptr_read_*`.

What this file pins:

* `&x` at an extern's `ptr[T]` argument hands the callee the address of the
  binding's storage, and the binding has the value the callee wrote as soon as
  the call returns -- in one integer, two at once, and in a narrow width (`u8`,
  `i16`), where the storage is one and two bytes rather than a word.
* The width comes from the pointee, not from the word: an `i16 *` writes two
  bytes, and a program that wrote eight would corrupt the neighbour.
* `&x` is only meaningful as that argument: written anywhere else it is refused
  by both checkers and, at run time, by the -O0 build. It takes a name, not an
  expression.
* `let p = &x` needs `mut`: the callee writes through it, so the binding changes.
* Memory the program allocates itself is ordinary FFI now: `malloc` returns
  `ptr[u8]`, a `u8` window fills it, and C reads what the program wrote.
"""
import _cxx
import _scratch

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CXX = os.path.join(ROOT, "lang", "cxx", "bin", "brat-cxx")
TMP = _scratch.mkdtemp(prefix="brat-outs")

#include <stddef.h>
#include <stdint.h>

HEADER = """int fill(int *n, int v);
void fill_f32(float *out);
void fill_bool(_Bool *out);
void fill_size(size_t *out);
void fill_scalars(signed char *small, unsigned short *medium, unsigned int *wide);
int divmod2(int64_t a, int64_t b, int64_t *q, int64_t *r);
void narrow_down(unsigned char *small, short *med, int *big);
void fill_ptr(void **out, int v);
int read_ptr(int *p);
int64_t sum_bytes(unsigned char *p, int n);
void *malloc(unsigned long n);
void free(void *p);
"""

LIB_C = """#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
int fill(int *n, int v) { if (n) *n = v * 2; return 7; }
void fill_f32(float *out) { *out = 3.5f; }
void fill_bool(_Bool *out) { *out = 1; }
void fill_size(size_t *out) { *out = 1234567; }
void fill_scalars(signed char *small, unsigned short *medium, unsigned int *wide) {
  *small = -7; *medium = 40000; *wide = 3000000000U;
}
int divmod2(int64_t a, int64_t b, int64_t *q, int64_t *r) {
    *q = a / b; *r = a % b; return 0;
}
void narrow_down(unsigned char *small, short *med, int *big) {
    *small = 200; *med = -3; *big = 123456;
}
static int ptr_slot;
void fill_ptr(void **out, int v) { ptr_slot = v; *out = &ptr_slot; }
int read_ptr(int *p) { return p ? *p : -1; }
int64_t sum_bytes(unsigned char *p, int n) {
  int64_t s = 0; for (int i = 0; i < n; i++) s += p[i]; return s;
}
"""

CDECLS = '''extern "LIB" fn fill(out: ptr[i32], v: i32) -> i32
extern "LIB" fn fill_f32(out: ptr[f32])
extern "LIB" fn fill_bool(out: ptr[bool])
extern "LIB" fn fill_size(out: ptr[usize])
extern "LIB" fn fill_scalars(small: ptr[i8], medium: ptr[u16], wide: ptr[u32])
extern "LIB" fn divmod2(a: i64, b: i64, q: ptr[i64], r: ptr[i64]) -> i32
extern "LIB" fn narrow_down(small: ptr[u8], med: ptr[i16], big: ptr[i32])
extern "LIB" fn fill_ptr(out: ptr[ptr], v: i32)
extern "LIB" fn read_ptr(p: ptr[i32]) -> i32
extern "LIB" fn sum_bytes(p: ptr[u8], n: i32) -> i64
'''

PROGRAM = CDECLS + '''
fn main() -> i64 {
  // one out-parameter: the callee writes, the binding has it
  let mut n = 0
  let rc = fill(&n, 21)
  print(rc)
  print(n)

  let mut f = 1.25
  fill_f32(&f)
  print(f)

  let mut b = false
  fill_bool(&b)
  print(b)

  let mut size = 0
  fill_size(&size)
  print(size)

  let mut small = 0
  let mut medium = 0
  let mut wide = 0
  fill_scalars(&small, &medium, &wide)
  print(small)
  print(medium)
  print(wide)

  // two at once, in one call
  let mut q = 0
  let mut r = 0
  divmod2(17, 5, &q, &r)
  print(q)
  print(r)

  // the width comes from the pointee: one byte, two bytes, four
  let mut small = 1
  let mut med = 2
  let mut big = 3
  narrow_down(&small, &med, &big)
  print(small)
  print(med)
  print(big)

  // the value the callee wrote is an ordinary value again
  let mut again = 0
  fill(&again, 5)
  print(again * 2)

  let mut p = 0
  fill_ptr(&p, 77)
  print(read_ptr(p))
  return 0
}
'''

WANT = "7\n42\n3.5\ntrue\n1234567\n-7\n40000\n3000000000\n3\n2\n200\n-3\n123456\n20\n77\n"

# Memory the program owns: malloc returns `ptr[u8]`, a `u8` window fills it, and
# C reads back exactly the bytes the program wrote.
BUFFER = '''extern "LIBC" fn malloc(n: usize) -> ptr[u8]
extern "LIBC" fn free(p: ptr[u8])
extern "LIB" fn sum_bytes(p: ptr[u8], n: i32) -> i64

fn main() -> i64 {
  let mut buf = malloc(64)
  let bytes = slice_of_ptr(buf, 16, "u8")
  let mut i = 0
  while i < 16 {
    slice_set(bytes, i, i + 1)
    i = i + 1
  }
  print(slice_get(bytes, 15))
  print(sum_bytes(buf, 16))
  free(buf)
  return 0
}
'''

BUFFER_WANT = "16\n136\n"


def run(args, **kw):
    if args and args[0] in ("build", "run", "emit-c", "dist") \
            and "--unsafe-ffi" not in args:
        args = [args[0], "--unsafe-ffi"] + list(args[1:])
    return subprocess.run(_cxx.cli_args(*(args)), capture_output=True,
                          text=True, timeout=600, **kw)


def write(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def build_lib():
    src = write("lib.c", LIB_C)
    lib = os.path.join(TMP, "libouts.so")
    r = subprocess.run(["cc", "-shared", "-fPIC", "-o", lib, src], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return lib, write("outs.h", HEADER)


def native(prog, name):
    exe = os.path.join(TMP, name)
    rc = run(["build", prog, "-o", exe])
    assert rc.returncode == 0, rc.stdout + rc.stderr
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def main():
    lib, header = build_lib()
    libc = subprocess.run(["cc", "-print-file-name=libc.so.6"], capture_output=True,
                          text=True).stdout.strip()
    prog = write("outs.brat", PROGRAM.replace('"LIB"', '"%s"' % lib))
    buf = write("buf.brat", BUFFER.replace('"LIB"', '"%s"' % lib)
                .replace('"LIBC"', '"%s"' % libc))

    # the proof: the header says `int *`, BRAT says `ptr[i32]`
    p = run(["ffi", prog, "--header", header])
    assert p.returncode == 0, p.stdout + p.stderr
    assert "0 error(s)" in p.stdout, p.stdout

    # A pointee of the wrong width is a different function: the header's
    # `short *` is not BRAT's `ptr[i32]`.
    wrong = write("wrong.h", HEADER.replace("short *med", "int *med"))
    p = run(["ffi", prog, "--header", wrong])
    assert p.returncode != 0, p.stdout + p.stderr
    assert "error" in p.stdout.lower(), p.stdout

    p = run(["run", "-O0", prog])
    assert p.returncode == 0, p.stdout + p.stderr
    assert p.stdout == WANT, "run_o0:\n%r\nwant %r" % (p.stdout, WANT)

    got = native(prog, "outs.bin")
    assert got == WANT, "native:\n%r\nwant %r" % (got, WANT)

    target_c = run(["emit-c", "--target", "i686-linux-gnu", prog])
    assert target_c.returncode == 0, target_c.stdout + target_c.stderr
    assert "size_t _out" in target_c.stdout, \
      "usize out-parameter scratch must follow the target's size_t"
    assert "void* _out" in target_c.stdout, \
      "ptr out-parameter scratch must be pointer-sized"
    p = run(["run", "-O0", buf])
    assert p.returncode == 0, p.stdout + p.stderr
    assert p.stdout == BUFFER_WANT, "run_o0 buffer:\n%r" % p.stdout

    got = native(buf, "buf.bin")
    assert got == BUFFER_WANT, "native buffer:\n%r" % got

    # `&` outside an extern argument: it is not a general address-of, because
    # only a name can be written back through.
    stray = write("stray.brat", CDECLS.replace('"LIB"', '"%s"' % lib) + '''
fn main() -> i64 {
  let mut n = 1
  let p = &n
  print(p)
  return 0
}
''')
    p = run(["check", stray])
    assert p.returncode != 0, "a stray & must not type-check"
    assert "out-parameter" in (p.stdout + p.stderr), p.stdout + p.stderr
    p = run(["run", "-O0", stray])
    assert p.returncode != 0 and "out-parameter" in (p.stdout + p.stderr), p.stdout + p.stderr

    # `&(a + b)` is not a binding.
    expr = write("expr.brat", CDECLS.replace('"LIB"', '"%s"' % lib) + '''
fn main() -> i64 {
  let mut n = 0
  fill(&(n + 1), 2)
  return 0
}
''')
    p = run(["check", expr])
    assert p.returncode != 0 and "takes a binding" in (p.stdout + p.stderr), p.stdout + p.stderr

    print("OUT PARAMS ALL GREEN (integer, float, bool and size_t outputs; "
          "both backends and target-aware native emission)")


main()
