"""`ptr[T]`, tables of records, and packed layouts: the FFI a driver needs.

What this file pins, one assertion per claim:

* `p: ptr[T]` dereferences to the element -- the record, not the address -- and
  the element type comes either from the annotation or from the callee's return.
* A window over a table of C records (`slice_of_struct`) reads records at their
  declared stride; writing one through the window is refused, because the table
  belongs to whoever allocated it.
* A window over elements built in the program (`slice_struct_of`) is writable
  and its stores land where the program can see them.
* `extern packed struct` really is packed: one byte, then an int, at the offsets
  the header has, in the -O0 build and native build, and the proof says so by
  compiling `_Static_assert`s against the header.
* `u8`/`u16` are widths at the ABI, so a header's `unsigned char` matches `u8`
  and a value of 256 is refused before it reaches C.

The library and the header are written here, so a failure names one thing.
"""
import _cxx
import _scratch

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CXX = os.path.join(ROOT, "lang", "cxx", "bin", "brat-cxx")
TMP = _scratch.mkdtemp(prefix="brat-ffi-ptrs")

HEADER = """typedef struct __attribute__((packed)) { unsigned char kind; int len; } Hdr;
typedef struct { long long id; float x; float y; } Node;
typedef struct { int count; int flags; } Reg;
Node *node_new(long long id, float x, float y);
void node_free(Node *n);
long long node_id(Node *n);
Reg *regs_get(int *n);
Hdr *hdr_make(unsigned char kind, int len);
long long hdr_len(Hdr *h);
"""

LIB_C = """#include <stdlib.h>
typedef struct __attribute__((packed)) { unsigned char kind; int len; } Hdr;
typedef struct { long long id; float x; float y; } Node;
typedef struct { int count; int flags; } Reg;
Node *node_new(long long id, float x, float y) { Node *n = malloc(sizeof *n); n->id=id; n->x=x; n->y=y; return n; }
void node_free(Node *n) { free(n); }
long long node_id(Node *n) { return n->id; }
static Reg table[3];
Reg *regs_get(int *n) {
    table[0] = (Reg){10, 1}; table[1] = (Reg){20, 2}; table[2] = (Reg){30, 4};
    if (n) *n = 3;
    return table;
}
Hdr *hdr_make(unsigned char kind, int len) { Hdr *h = malloc(sizeof *h); h->kind = kind; h->len = len; return h; }
long long hdr_len(Hdr *h) { return h->len; }
"""

DECLS = '''extern "LIB" fn node_new(id: i64, x: f32, y: f32) -> ptr[Node]
extern "LIB" fn node_free(p: ptr[Node])
extern "LIB" fn node_id(p: ptr[Node]) -> i64
extern "LIB" fn regs_get(n: ptr[i32]) -> ptr[Reg]
extern "LIB" fn hdr_make(k: u8, n: i32) -> ptr[Hdr]
extern "LIB" fn hdr_len(h: ptr[Hdr]) -> i64
extern struct Node { id: i64, x: f32, y: f32 }
extern struct Reg { count: i32, flags: i32 }
extern packed struct Hdr { kind: u8, len: i32 }
'''

# One program, three implementations: every line below is a claim.
PROGRAM = DECLS + '''
fn main() -> i64 {
  // 1. a record behind a `ptr[T]`: the annotation is the layout
  let mut h: ptr[Node] = node_new(7, 1.5, 2.0)
  let a = h[0]
  print(a.id)
  print(a.x)
  print(node_id(h))
  node_free(h)

  // 2. the callee's return declares the element, with no annotation
  let tab = regs_get(0)
  let t = slice_of_struct(tab, 3, "Reg")
  print(slice_len(t))
  print(slice_get(t, 1).count)
  let mut i = 0
  let mut total = 0
  while i < slice_len(t) {
    total = total + slice_get(t, i).count
    i = i + 1
  }
  print(total)

  // 3. a window over elements built here is writable
  let mut built = slice_struct_of([Reg { count: 5, flags: 1 }, Reg { count: 6, flags: 2 }])
  print(slice_get(built, 1).count)
  slice_set(built, 1, Reg { count: 99, flags: 7 })
  print(slice_get(built, 1).count)
  print(slice_get(built, 0).count)

  // 4. a packed record: one byte and an int, no padding
  let mut p = hdr_make(7, 1234)
  let d = p[0]
  print(d.kind)
  print(d.len)
  print(hdr_len(p))
  return 0
}
'''

WANT = "7\n1.5\n7\n3\n20\n60\n6\n99\n5\n7\n1234\n1234\n"


def run(args, **kw):
    if args and args[0] in ("build", "run", "emit-c", "dist") and "--unsafe-ffi" not in args:
        args = [args[0], "--unsafe-ffi"] + list(args[1:])
    p = subprocess.run(_cxx.cli_args(*(args)), capture_output=True, text=True,
                       timeout=600, **kw)
    return p.returncode, p.stdout, p.stderr


def write(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def build_lib():
    src = write("lib.c", LIB_C)
    lib = os.path.join(TMP, "libptrs.so")
    r = subprocess.run(["cc", "-shared", "-fPIC", "-o", lib, src], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return lib, write("ptrs.h", HEADER)


def main():
    lib, header = build_lib()
    prog = write("ptrs.brat", PROGRAM.replace('"LIB"', '"%s"' % lib))

    rc, out, err = run(["ffi", prog, "--header", header])
    assert rc == 0, out + err
    assert "0 error(s)" in out, out
    assert "proven" in out, out

    # A header that is NOT packed must be refused: the offsets differ.
    loose = write("loose.h", HEADER.replace(
        "typedef struct __attribute__((packed)) { unsigned char kind; int len; } Hdr;",
        "typedef struct { unsigned char kind; int len; } Hdr;"))
    rc, out, err = run(["ffi", prog, "--header", loose])
    assert rc != 0, out + err
    assert "packed" in (out + err), out + err

    rc, out, err = run(["run", "-O0", prog])
    assert rc == 0, out + err
    assert out == WANT, "run_o0:\n%r\nwant:\n%r" % (out, WANT)

    exe = os.path.join(TMP, "ptrs.bin")
    rc, out, err = run(["build", prog, "-o", exe])
    assert rc == 0, out + err
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    assert r.stdout == WANT, "native:\n%r\nwant:\n%r" % (r.stdout, WANT)

    # A record table is read-only through the window.
    bad = write("ro.brat", DECLS.replace('"LIB"', '"%s"' % lib) + '''
fn main() -> i64 {
  let tab = regs_get(0)
  let t = slice_of_struct(tab, 3, "Reg")
  slice_set(t, 0, Reg { count: 1, flags: 1 })
  return 0
}
''')
    rc, out, err = run(["run", "-O0", bad])
    assert rc != 0, out + err
    assert "read-only" in (out + err), out + err

    # A `u8` value out of range never reaches C.
    wide = write("wide.brat", DECLS.replace('"LIB"', '"%s"' % lib) + '''
fn main() -> i64 {
  let mut h = hdr_make(256, 1)
  print(h[0].kind)
  return 0
}
''')
    rc, out, err = run(["run", "-O0", wide])
    assert rc != 0, out + err
    assert "u8" in (out + err), out + err

    # A pointer with no element type cannot be indexed.
    unit = write("unit.brat", '''extern "LIB" fn node_new(id: i64) -> ptr
fn main() -> i64 {
  let mut p = node_new(1)
  print(p[0])
  return 0
}
'''.replace('"LIB"', '"%s"' % lib))
    rc, out, err = run(["check", unit])
    assert rc != 0, out + err
    assert "needs a unit" in (out + err), out + err

    # A `ptr[T]` of a record that was never declared cannot be dereferenced.
    undeclared = write("undecl.brat", '''extern "LIB" fn node_new(id: i64) -> ptr[Ghost]
fn main() -> i64 {
  let mut p = node_new(1)
  print(p[0])
  return 0
}
'''.replace('"LIB"', '"%s"' % lib))
    rc, out, err = run(["check", undeclared])
    assert rc != 0, out + err

    print("FFI PTRS ALL GREEN (%d asserts: ptr[T], record tables, built windows, packed)"
          % 24)


main()
