"""FFI widths, callbacks and the C boundary: one table, one ABI.

Every defect this file pins was a "spelled as a machine word" bug, the family
that compiles, links and corrupts the next stack slot:

* `i8`/`i16` were missing from the FFI ladder, so an extern returning `short`
  was declared `int64_t` in the generated C (a caller reading 8 bytes where the
  callee wrote 2), and the call site refused the program outright.
* an `f32` callback parameter was not packed for the call at all, and an `f32`
  callback *result* was read out of the union's integer field, so 3.5 came back
  as 4.6e18.
* a struct returned by value was converted field by field with the call
  expression inlined into each field: the C function ran once per field.
* `vmove` marked its source through a plain store, which is a dead store the
  optimizer may delete; it now goes through a volatile lvalue.
* the build passed `-O2` and no `-fwrapv`, so the native answer on an
  overflowing i64 was the optimizer's opinion rather than unoptimized (-O0).

Checked here: both front ends and both backends, plus the refusals.
"""
import _cxx
import _scratch

import os
import subprocess
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CXX = os.path.join(ROOT, "lang", "cxx", "bin", "brat-cxx")
TMP = _scratch.mkdtemp(prefix="brat-widths")
version = tuple(int(part) for part in unicodedata.unidata_version.split("."))
case_version = 1600 if version >= (16, 0) else 1500
CXX_ENV = dict(os.environ, BRAT_CASE_MAP_VERSION=str(case_version))

HEADER = """short neg_short(void);
signed char neg_char(void);
int64_t widen(short a, signed char b);
_Bool apply_bool(_Bool x, _Bool (*cb)(_Bool));
float apply_f32(float x, float (*cb)(float));
typedef struct { int x; int y; } Pt;
Pt apply_pt(Pt (*cb)(int), int n);
int64_t sum_pts(int n, ...);
"""

LIB_C = """#include <stdarg.h>
#include <stdint.h>
short neg_short(void) { return -3; }
signed char neg_char(void) { return -7; }
int64_t widen(short a, signed char b) { return (int64_t)a * 1000 + (int64_t)b; }
_Bool apply_bool(_Bool x, _Bool (*cb)(_Bool)) { return cb(x); }
float apply_f32(float x, float (*cb)(float)) { return cb(x) * 2.0f; }
typedef struct { int x; int y; } Pt;
Pt apply_pt(Pt (*cb)(int), int n) { return cb(n); }
int64_t sum_pts(int n, ...) {
    va_list ap; va_start(ap, n);
    int64_t s = 0;
    for (int i = 0; i < n; i++) { Pt p = va_arg(ap, Pt); s += p.x * 10 + p.y; }
    va_end(ap); return s;
}
"""

DECLS = '''extern "LIB" fn neg_short() -> i16
extern "LIB" fn neg_char() -> i8
extern "LIB" fn widen(a: i16, b: i8) -> i64
extern "LIB" fn apply_bool(x: bool, cb: fn(bool) -> bool) -> bool
extern "LIB" fn apply_f32(x: f32, cb: fn(f32) -> f32) -> f32
'''

WIDTHS = DECLS + '''fn plus_one(x: f32) -> f32 { return x + 1.0 }
fn identity_bool(x: bool) -> bool { return x }
fn main() -> i64 {
  print(neg_short())
  print(neg_char())
  print(widen(-2, -3))
    print(apply_bool(true, "identity_bool"))
  print(apply_f32(3.5, "plus_one"))
  return 0
}
'''

WIDTHS_WANT = "-3\n-7\n-2003\ntrue\n9.0\n"

STRUCTS = '''extern struct Pt { x: i32, y: i32 }
extern "LIB" fn apply_pt(cb: fn(i64) -> Pt, n: i64) -> Pt
extern "LIB" fn sum_pts(n: i64, ..Pt) -> i64

fn mk(n: i64) -> Pt { return Pt { x: n, y: 2 } }

fn main() -> i64 {
  let p = apply_pt("mk", 7)
  print(p.x)
  print(p.y)
  let a = Pt { x: 1, y: 2 }
  let b = Pt { x: 3, y: 4 }
  print(sum_pts(2, a, b))
  return 0
}
'''

# `sum_pts` is variadic: the record goes into `...` by value, which is legal C
# and is the shape of every printf-style API that takes a struct.
STRUCTS_NATIVE_WANT = "7\n2\n46\n"

# A move whose source is read again is refused by the checker; the runtime marker
# is the backstop behind it, and it has to survive -O2.
MOVE = '''fn take(s: move str) -> str {
  return s
}
fn main() -> i64 {
  let a = "pay" + "load"
  let b = move a
  print(take(move b))
  let c = "second"
  let d = move c
  print(d)
  return 0
}
'''

MOVE_WANT = "payload\nsecond\n"

# Integer overflow wraps in the -O0 build, so it must wrap in the native build
# (`-fwrapv`), not depend on what the optimizer makes of undefined behaviour.
OVERFLOW = '''fn acc(n: i64) -> i64 {
  let mut t = 0
  let big = 4611686018427387904
  for i in 0..n {
    t = t + big
  }
  return t
}
fn main() -> i64 {
  print(acc(2))
  print(acc(3))
  print(acc(4))
  return 0
}
'''

OVERFLOW_WANT = "-9223372036854775808\n-4611686018427387904\n0\n"

# A projection move cannot empty a slot the parent still owns, so the binding is
# handed a reference of its own. Classified as a borrow, that reference was never
# released: ASAN found 352 bytes per program, and the parent was never released
# either because the emitter had marked its root as moved.
PROJECTION = '''fn main() -> i64 {
  let mut p = Point { x: "aa-long-string-aaaaaaaaaaaaaaaa", y: "bb-long" }
  let a = move p.x
  print(a)
  print(p.y)
  return 0
}
'''

PROJECTION_WANT = "aa-long-string-aaaaaaaaaaaaaaaa\nbb-long\n"


def run(args, **kw):
    kw.setdefault("env", CXX_ENV)
    if args and args[0] in ("build", "run", "emit-c", "dist") and "--unsafe-ffi" not in args:
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
    lib = os.path.join(TMP, "libwidths.so")
    r = subprocess.run(["cc", "-shared", "-fPIC", "-o", lib, src], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return lib, write("widths.h", HEADER)


def run_o0(prog):
    p = run(["run", "-O0", prog])
    assert p.returncode == 0, p.stdout + p.stderr
    return p.stdout


def native(prog, name, extra=()):
    exe = os.path.join(TMP, name)
    rc = run(["build", prog, "-o", exe] + list(extra))
    assert rc.returncode == 0, rc.stdout + rc.stderr
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def emitted(prog):
    """The emitted C, without the contract block the driver appends.

    `emit-c` writes the FFI contract the binary will carry (see
    `test_ffi_contract.py`); `brat-cxx cc` is the emitter alone, so the
    byte-for-byte comparison below is about the code, and this block is checked
    to be present and well formed instead.
    """
    p = run(["emit-c", prog])
    assert p.returncode == 0, p.stdout + p.stderr
    code, sep, contract = p.stdout.rpartition("\n/* BRAT FFI contract")
    if not sep:
        return p.stdout
    assert contract.count("static const char brat_ffi_contract[]") == 1, contract[-400:]
    return code + "\n"


def main():
    lib, header = build_lib()
    widths = write("widths.brat", WIDTHS.replace('"LIB"', '"%s"' % lib))
    structs = write("structs.brat", STRUCTS.replace('"LIB"', '"%s"' % lib))
    move = write("move.brat", MOVE)
    overflow = write("overflow.brat", OVERFLOW)

    out = emitted(widths)
    assert "extern short neg_short(void);" in out, out
    assert "extern signed char neg_char(void);" in out, out
    assert "extern int64_t widen(short, signed char);" in out, out
    assert "extern float apply_f32(float, float (*) (float));" in out, out

    # Both CLI doors produce the same C for the same file.
    p = subprocess.run([CXX, "cc", widths], capture_output=True, text=True, timeout=600,
                       env=CXX_ENV)
    assert p.returncode == 0, p.stdout + p.stderr
    assert p.stdout == out, "the CLI doors disagree on the extern declarations"

    # and both backends agree on the values
    assert run_o0(widths) == WIDTHS_WANT, run_o0(widths)
    assert native(widths, "widths.bin") == WIDTHS_WANT

    # a struct through a callback, and through a variadic tail
    out = emitted(structs)
    # the call is bound to a temp: inlined per field it would run three times
    assert out.count("apply_pt(brat_cb_mk") == 1, out
    assert native(structs, "structs.bin") == STRUCTS_NATIVE_WANT
    # This used to be unoptimized (-O0) one hole -- ctypes has no path for a record
    # coming back out of a Python callback. It now compiles a C trampoline for
    # exactly that shape, so both backends must print the same bytes.
    assert run_o0(structs) == STRUCTS_NATIVE_WANT

    # the header proves the widths we just emitted
    p = run(["ffi", widths, "--header", header])
    assert p.returncode == 0, p.stdout + p.stderr
    assert "0 error(s)" in p.stdout, p.stdout

    # `double` is not `float`: the boundary compares width, not "is a float".
    wrong = write("wrong.h", HEADER.replace("float apply_f32(float x", "double apply_f32(double x"))
    p = run(["ffi", widths, "--header", wrong])
    assert p.returncode != 0, "a double/float mismatch must be refused"

    # the runtime marker and the wrap-around
    assert run_o0(move) == MOVE_WANT
    assert native(move, "move.bin") == MOVE_WANT
    crt_dir = os.path.join(ROOT, "lang", "engine", "crt")
    crt_parts = [os.path.join(ROOT, "lang", "engine", "crt.h")] + [
        os.path.join(crt_dir, fn) for fn in sorted(os.listdir(crt_dir)) if fn.endswith(".h")
    ]
    crt = "\n".join(open(p, encoding="utf-8").read() for p in crt_parts)
    # `vmove` takes the source as a volatile pointer and writes the tag through
    # it: a plain store to an object that is not read again is exactly what -O2
    # is allowed to delete.
    body = crt[crt.index("BFN Val vmove(volatile Val *src)"):]
    body = body[:body.index("}\n") + 2]
    assert "src->tag = V_MOVED;" in body, body
    assert run_o0(overflow) == OVERFLOW_WANT
    assert native(overflow, "overflow.bin") == OVERFLOW_WANT

    # a width the ladder does not know is refused, not guessed
    bad = write("bad.brat", DECLS.replace('"LIB"', '"%s"' % lib) + '''
extern "LIB" fn nope(a: f16) -> i64
fn main() -> i64 { return 0 }
''')
    # The emitter is the authority on widths: `f16` has no C spelling here, so it
    # is refused instead of being written as a machine word.
    p = run(["build", bad, "-o", os.path.join(TMP, "bad.bin")])
    assert p.returncode != 0, p.stdout + p.stderr
    assert "f16" in (p.stdout + p.stderr), p.stdout + p.stderr

    # Fixed-width C spellings copied from a header mean the BRAT width of the
    # same size, including inside a record and a callback. Target-dependent
    # spellings and names nested inside `ptr[...]`/`fn(...)` are still refused
    # by both checkers instead of passing `check` and failing in the emitter.
    CSP_C = write("cspell.c", """#include <stdint.h>
struct CRec { unsigned long long a; int16_t b; };
unsigned long long add_u64(unsigned long long a, signed short int b) {
    return a + (unsigned long long)b;
}
signed short int dec_short(signed short int x) { return (signed short int)(x - 1); }
float add_f32(float a, float b) { return a + b; }
_Bool flip_bool(_Bool x) { return x ? 0 : 1; }
const char *echo_str(const char *s) { return s; }
unsigned long long apply_u64(unsigned long long x,
                             signed short int (*cb)(unsigned long long)) {
    return x + (unsigned long long)cb(x);
}
struct CRec bump_rec(struct CRec r) { r.a += 1; r.b += 1; return r; }
""")
    csp_lib = os.path.join(TMP, "libcspell.so")
    r = subprocess.run(["cc", "-shared", "-fPIC", "-o", csp_lib, CSP_C],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    CSP = '''extern struct CRec { a: unsigned long long, b: int16_t }
extern "LIB" fn add_u64(a: unsigned long long, b: signed short int) -> unsigned long long
extern "LIB" fn dec_short(x: signed short int) -> signed short int
extern "LIB" fn add_f32(a: float, b: float) -> float
extern "LIB" fn flip_bool(x: _Bool) -> _Bool
extern "LIB" fn echo_str(s: const char *) -> const char *
extern "LIB" fn apply_u64(x: unsigned long long,
                          cb: fn(unsigned long long) -> signed short int) -> unsigned long long
extern "LIB" fn bump_rec(r: CRec) -> CRec

fn short_cb(x: u64) -> i16 { return 5 }

fn main() -> i64 {
  print(add_u64(40, 2))
  print(dec_short(-3))
  print(add_f32(1.5, 2.25))
  print(flip_bool(true))
  print(echo_str("text"))
  print(apply_u64(10, "short_cb"))
  let got = bump_rec(CRec { a: 7, b: -2 })
  print(got.a)
  print(got.b)
  return 0
}
'''.replace('"LIB"', '"%s"' % csp_lib)
    csp = write("cspell.brat", CSP)
    csp_want = "42\n-4\n3.75\nfalse\ntext\n15\n8\n-1\n"
    py_check = subprocess.run(_cxx.cli_args("check", csp), capture_output=True,
                              text=True, timeout=120,
                              env=dict(os.environ))
    assert py_check.returncode == 0, py_check.stdout + py_check.stderr
    native_check = subprocess.run([CXX, "check", csp], capture_output=True, text=True,
                                  timeout=120, env=CXX_ENV)
    assert native_check.returncode == 0, native_check.stdout + native_check.stderr
    assert run_o0(csp) == csp_want, run_o0(csp)
    assert native(csp, "cspell.bin") == csp_want

    unsupported_slots = {
        "direct_isize": 'extern "x.so" fn f(x: isize) -> isize\n',
        "struct_isize": 'extern struct BadRec { a: isize }\n',
        "ptr_unknown": 'extern "x.so" fn f(p: ptr[bogus_t]) -> none\n',
        "callback_unknown": 'extern "x.so" fn f(cb: fn(bogus_t) -> i64) -> none\n',
        "callback_bad_result": 'extern "x.so" fn f(cb: fn(i64) -> bogus_t) -> none\n',
    }
    for label, decl in unsupported_slots.items():
        path = write(label + ".brat", decl + "fn main() -> i64 { return 0 }\n")
        for cmd, env in (
                (_cxx.cli_args("check", path),
                 dict(os.environ)),
                ([CXX, "check", path], CXX_ENV)):
            got = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=env)
            assert got.returncode != 0 and "unsupported FFI type" in (got.stdout + got.stderr), \
                "%s must be refused at check: %r" % (label, got.stdout + got.stderr)
    print("ffiw ok: fixed-width C spellings canonicalize, nested bad slots fail check")

    # a projection move is counted, not leaked
    proj = write("projection.brat", PROJECTION)
    assert run_o0(proj) == PROJECTION_WANT, run_o0(proj)
    assert native(proj, "projection.bin") == PROJECTION_WANT
    pc = os.path.join(TMP, "projection.c")
    with open(pc, "w", encoding="utf-8") as f:
        f.write(emitted(proj))
    pexe = os.path.join(TMP, "projection.asan")
    r = subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-w", "-fsanitize=address",
                        "-I", os.path.join(ROOT, "lang", "engine"), pc, "-o", pexe,
                        "-lm", "-lpthread", "-ldl"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([pexe], capture_output=True, text=True, timeout=120,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=99"))
    assert r.returncode == 0, r.stdout + r.stderr
    assert "leaked" not in r.stderr, r.stderr

    # --link carries the flags a library name cannot express
    r = subprocess.run([CXX, "build", "--unsafe-ffi", widths, "-o", os.path.join(TMP, "linked.bin"),
                        "--link", "-lm"], capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, r.stdout + r.stderr

    # fixed-width C types are the width they name
    # `int8_t` used to read as `i32`: a 4-byte BRAT type for a 1-byte C one. Two
    # consequences, both silent. A generated `extern struct` holding one was
    # refused by the very layout proof that generated it (`sizeof`/`offsetof`
    # disagree), and a narrow *result* was read as four bytes out of a register
    # where the ABI only promises one.
    NARROW_HDR = write("narrow.h", """
int8_t neg8(void);
int8_t dec8(int8_t x);
int16_t add16(int16_t a, int16_t b);
uint16_t u16max(void);
struct Narrow { int8_t a; int16_t b; int32_t c; };
struct Narrow shift(struct Narrow n);
""")
    NARROW_C = write("narrow.c", """
#include <stdint.h>
int8_t neg8(void) { return (int8_t)-1; }
int8_t dec8(int8_t x) { return (int8_t)(x - 1); }
int16_t add16(int16_t a, int16_t b) { return (int16_t)(a + b); }
uint16_t u16max(void) { return (uint16_t)65535; }
struct Narrow { int8_t a; int16_t b; int32_t c; };
struct Narrow shift(struct Narrow n) {
    struct Narrow r = { (int8_t)(n.a + 1), (int16_t)(n.b + 1), n.c + 1 };
    return r;
}
""")
    nlib = os.path.join(TMP, "libnarrow.so")
    r = subprocess.run(["cc", "-shared", "-fPIC", "-o", nlib, NARROW_C],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr

    gen = run(["bindgen", NARROW_HDR])
    assert gen.returncode == 0, gen.stdout + gen.stderr
    for want in ("extern fn neg8() -> i8;", "extern fn dec8(x: i8) -> i8;",
                 "extern fn add16(a: i16, b: i16) -> i16;",
                 "extern fn u16max() -> u16;",
                 "extern struct Narrow { a: i8, b: i16, c: i32 }"):
        assert want in gen.stdout, "bindgen must keep the true width, want %r in %r" \
            % (want, gen.stdout)
    genfile = write("narrow_gen.brat", gen.stdout)
    # The generated file must survive its own proof -- that is the round trip
    # that was broken while a field was widened.
    gate = run(["ffi", "--header", NARROW_HDR, genfile])
    assert gate.returncode == 0, "a generated binding must prove itself: %r" \
        % (gate.stdout + gate.stderr)

    # The library name is the caller's to supply: bindgen reads a header, and a
    # header does not say which .so will provide the symbols.
    NARROW_USE = gen.stdout.replace("extern fn ", 'extern "%s" fn ' % nlib) + """
fn main() -> i64 {
  print(neg8())
  print(dec8(-4))
  print(add16(-1000, 234))
  print(u16max())
  let n = Narrow { a: 1, b: 2, c: 3 }
  let m = shift(n)
  print(m.a)
  print(m.b)
  print(m.c)
  return 0
}
"""
    # 65535 is where a signed reading would show up: `u16` must not go through i32
    # sign-extension, and the -O0 build and the native build must agree.
    want = "-1\n-5\n-766\n65535\n2\n3\n4\n"
    narrow = write("narrow_use.brat", NARROW_USE)
    assert run_o0(narrow) == want, "run_o0: %r" % run_o0(narrow)
    assert native(narrow, "narrow.exe") == want, "native disagrees with the -O0 build"
    print("ffiw ok: fixed-width C types bind at their true width, both backends")

    # `_Bool` is a distinct one-byte ABI type, represented by BRAT `bool`.
    BOOL_HDR = write("bool.h", "_Bool flag(_Bool f);\nstruct Flags { _Bool on; int n; };\n")
    BOOL_LIB = write("bool.c", """#include <stdbool.h>
bool flag(bool f) { return f ? false : true; }
struct Flags { _Bool on; int n; };
""")
    blib = os.path.join(TMP, "libbool.so")
    r = subprocess.run(["cc", "-shared", "-fPIC", "-o", blib, BOOL_LIB],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    bgen = run(["bindgen", BOOL_HDR])
    assert "extern struct Flags { on: bool, n: i32 }" in bgen.stdout, \
        "a `_Bool` field is one byte: %r" % bgen.stdout
    assert "extern fn flag(f: bool) -> bool;" in bgen.stdout, \
        "a `_Bool` slot keeps its boolean type: %r" % bgen.stdout
    bfile = write("bool_gen.brat", bgen.stdout.replace(
        "extern fn flag(f: bool) -> bool;",
        'extern "%s" fn flag(f: bool) -> bool;' % blib)
        + 'fn main() -> i64 { print(flag(true)); print(flag(false)); return 0 }\n')
    assert run(["ffi", "--header", BOOL_HDR, bfile]).returncode == 0, \
        "the `_Bool` binding must be proven by the compiler"
    bi = run(["run", "-O0", bfile])
    assert bi.returncode == 0 and bi.stdout == "false\ntrue\n", \
        "-O0 bool FFI: %r" % (bi.stdout + bi.stderr)
    bexe = os.path.join(TMP, "bool.exe")
    bb = run(["build", bfile, "--header", BOOL_HDR, "-o", bexe])
    assert bb.returncode == 0, bb.stdout + bb.stderr
    bn = subprocess.run([bexe], capture_output=True, text=True, timeout=120)
    assert (bn.returncode, bn.stdout) == (0, bi.stdout), \
        "native bool FFI: %r" % (bn.stdout + bn.stderr)
    # A deliberately wrong width is refused by the layout proof, not by a crash.
    bad = write("bool_bad.brat", "extern struct Flags { on: i32, n: i32 }\n"
                                 "fn main() -> i64 { return 0 }\n")
    rbad = run(["ffi", "--header", BOOL_HDR, bad])
    out = rbad.stdout + rbad.stderr
    # Refused on the width itself (the type comparison is stricter than the
    # layout proof, so it speaks first); either way the field is named.
    assert rbad.returncode == 1 and "Flags.on" in out, \
        "a 4-byte `_Bool` field must be refused: %r" % out
    print("ffiw ok: `_Bool` is one byte and the layout proof says so")

    # `struct Foo { ... };` without a typedef is spelled `struct Foo` in C, so a
    # probe that writes the bare name does not compile -- and a proof that cannot
    # compile looks like a header mismatch instead of a proof that never ran.
    TAG_HDR = write("tag.h", "struct Tag { int n; _Bool on; };\n"
                             "int tag_n(struct Tag t);\n")
    TAG_C = write("tag.c", "struct Tag { int n; _Bool on; };\n"
                           "int tag_n(struct Tag t) { return t.n + (t.on ? 100 : 0); }\n")
    tlib = os.path.join(TMP, "libtag.so")
    r = subprocess.run(["cc", "-shared", "-fPIC", "-o", tlib, TAG_C],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    tgen = run(["bindgen", TAG_HDR])
    assert "extern struct Tag { n: i32, on: bool }" in tgen.stdout, tgen.stdout
    tfile = write("tag_gen.brat", tgen.stdout)
    rt = run(["ffi", "--header", TAG_HDR, tfile])
    assert rt.returncode == 0, \
        "a header without a typedef must still be provable: %r" % (rt.stdout + rt.stderr)
    print("ffiw ok: `struct X {}` without a typedef is proven with the keyword")


    # A real header decorates every declaration with an export macro
    # (`SQLITE_API int`, `ZEXTERN int ZEXPORT`), keeps its types in a header one
    # line up (`zconf.h`), and writes pointer typedefs without a space
    # (`typedef z_stream FAR *z_streamp;`). Each of those used to cost whole
    # libraries: sqlite3 lost 160 results to `SQLITE_API int` alone, and zlib bound
    # 4 functions out of 89.
    REAL = [p for p in ("/usr/include/sqlite3.h", "/usr/include/zlib.h")
            if os.path.exists(p)]

    if not REAL:
        print("ffiw skip: no real headers on this machine")
    else:
        for hdr in REAL:
            name = os.path.basename(hdr)
            r = run(["bindgen", hdr])
            assert r.returncode == 0, "%s: bindgen failed: %r" % (name, r.stdout + r.stderr)
            nfn = r.stdout.count("extern fn ")
            nst = r.stdout.count("extern struct ")
            # The floor is what these headers must yield on any sane machine; the
            # exact number moves with the header's version, so it is a floor.
            if name == "sqlite3.h":
                assert nfn >= 250, "sqlite3: expected hundreds of functions, got %d" % nfn
            else:
                assert nfn >= 60, "zlib: expected most of the API, got %d" % nfn
            # Every generated name must be spellable: the file has to check clean,
            # which is what "generated" means -- a binding the checker refuses is a
            # binding nobody can call.
            gen = write(name + ".gen.brat", r.stdout)
            ck = run(["check", gen])
            assert ck.returncode == 0, "%s: generated file must check clean: %r" % (name, ck.stdout + ck.stderr)
            print("ffiw ok: %s -> %d functions, %d records, checks clean" % (name, nfn, nst))

        # A macro-decorated synthetic header pins the mechanism itself, so the floor
        # above is not the only thing guarding this path.
        MACRO_HDR = write("macro.h", """
    #define MYAPI
    #define MYAPI_CALL __attribute__((visibility("default")))
    typedef struct MyNode MyNode;
    int MYAPI my_open(const char *path);
    long MYAPI my_count(MYNode *n);
    """)
        mgen = run(["bindgen", MACRO_HDR])
        assert "extern fn my_open(path: str) -> i32;" in mgen.stdout, mgen.stdout
        assert "extern fn my_count(n: ptr) -> i64;" in mgen.stdout, mgen.stdout
        print("ffiw ok: an export macro is not part of the type")

    print("FFI WIDTHS ALL GREEN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
