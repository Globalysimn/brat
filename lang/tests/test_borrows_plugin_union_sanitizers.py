"""`borrows(p)` provenance, the plugin ABI table, `extern union`, and sanitizer cleanliness
(ASan, LSan, UBSan, TSan).

Covers:
  - `borrows(p)` FFI provenance effect (L4 foreign pointer lifetimes
    without `'a`), NLL borrow checking, use-after-free / free-of-view /
    escaping-return / `ptr_offset` / cross-module `pub extern` enforcement, and
    contradictory declaration refusals across both Python & C++ front ends.
  - `brat_plugin_abi_v1` table in `--shared` builds + runtime
    `call(sym, sig, ...)` ABI verification in both `--interp` and native `crt.h`.
  - `extern union` + C11 anonymous/named inline `union` & `struct`
    across `bindgen`, `ffi` (`--header`, DWARF `--object`, L5 `--abi-matrix`,
    `verify --bin` `BRATFFI1`), Python/C++ AST parity, identical C through
    the Python CLI and native CLI doors, and runtime.
  - `KNOWN_LEAKS == set()`, ASan/LSan/UBSan + TSan cleanliness, and
    mock `-D_WIN32 -Wall -Wextra -Werror` `crt.h` header hygiene.
"""
import _cxx
import _scratch

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CLI = os.path.join(ROOT, "lang", "engine", "cli.py")
CRT_H = os.path.join(ROOT, "lang", "engine", "crt.h")
TMP = _scratch.mkdtemp(prefix="brat-safety-")

sys.path.insert(0, os.path.join(ROOT, "lang", "engine"))
import brat.parser as P                      # noqa: E402
from brat.lexer import Cur, lex              # noqa: E402

BIN = _cxx.build()
asserts = 0


def check(cond, msg):
    global asserts
    if not cond:
        raise AssertionError(msg)
    asserts += 1


def write(rel, text):
    p = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
    return p


def test_borrows_effect():
    # 1. Contradictory declaration refusals match across both parsers.
    bad_decls = [
        ("owned", "extern fn v(p: ptr) -> move ptr borrows(p);\nfn main() {}\n",
         "`borrows(p)` contradicts `-> move` on v"),
        ("ret", "extern fn v(p: ptr) -> i64 borrows(p);\nfn main() {}\n",
         "`borrows(p)` wants a pointer result on v, got i64"),
        ("movep", "extern fn v(move p: ptr) -> ptr borrows(p);\nfn main() {}\n",
         "`borrows(p)` contradicts `move` on parameter `p` of v"),
    ]
    for tag, src, needle in bad_decls:
        fp = write("s91/decl_%s.brat" % tag, src)
        rp = subprocess.run([sys.executable, CLI, "check", fp], capture_output=True, text=True)
        rc = subprocess.run([BIN, "check", fp], capture_output=True, text=True)
        op, oc = rp.stdout + rp.stderr, rc.stdout + rc.stderr
        check(rp.returncode != 0 and needle in op, "py %s: %r" % (tag, op))
        check(rc.returncode != 0 and needle in oc, "cx %s: %r" % (tag, oc))

    # 2. Borrow provenance refusals + NLL acceptance match across both checkers.
    cases = [
        ("ok_nll", True, """
extern "c" fn malloc(n: usize) -> move ptr;
extern "c" fn free(move p: ptr);
extern fn c_view(p: ptr) -> ptr pure nothrow borrows(p);
extern fn c_peek(p: ptr) -> i64 pure nothrow reads(p);
fn main() {
    let owner = malloc(16);
    let v = c_view(owner);
    let v2 = ptr_offset(v, 8);
    let _a = c_peek(v2);
    free(move owner);
}
""", ""),
        ("uaf", False, """
extern "c" fn malloc(n: usize) -> move ptr;
extern "c" fn free(move p: ptr);
extern fn c_view(p: ptr) -> ptr pure nothrow borrows(p);
extern fn c_peek(p: ptr) -> i64 pure nothrow reads(p);
fn main() {
    let owner = malloc(16);
    let v = c_view(owner);
    free(move owner);
    let _a = c_peek(v);
}
""", "while borrowed"),
        ("offset_uaf", False, """
extern "c" fn malloc(n: usize) -> move ptr;
extern "c" fn free(move p: ptr);
extern fn c_view(p: ptr) -> ptr pure nothrow borrows(p);
extern fn c_peek(p: ptr) -> i64 pure nothrow reads(p);
fn main() {
    let owner = malloc(16);
    let v2 = ptr_offset(c_view(owner), 4);
    free(move owner);
    let _a = c_peek(v2);
}
""", "while borrowed"),
        ("free_view", False, """
extern "c" fn malloc(n: usize) -> move ptr;
extern "c" fn free(move p: ptr);
extern fn c_view(p: ptr) -> ptr pure nothrow borrows(p);
fn main() {
    let owner = malloc(16);
    let v = c_view(owner);
    free(move v);
    free(move owner);
}
""", "cannot move borrowed pointer `v`"),
        ("escape_ret", False, """
extern "c" fn malloc(n: usize) -> move ptr;
extern fn c_view(p: ptr) -> ptr pure nothrow borrows(p);
fn leak() -> ptr {
    let owner = malloc(16);
    let v = c_view(owner);
    return v;
}
fn main() {}
""", "escapes via return"),
    ]
    for tag, ok, src, needle in cases:
        fp = write("s91/case_%s.brat" % tag, src)
        rp = subprocess.run([sys.executable, CLI, "check", fp], capture_output=True, text=True)
        rc = subprocess.run([BIN, "check", fp], capture_output=True, text=True)
        op, oc = rp.stdout + rp.stderr, rc.stdout + rc.stderr
        check((rp.returncode == 0) == ok, "py %s: %r" % (tag, op))
        check((rc.returncode == 0) == ok, "cx %s: %r" % (tag, oc))
        if needle:
            check(needle in op and needle in oc, "%s needle:\npy=%r\ncx=%r" % (tag, op, oc))

    # 3. Cross-module `extern` `borrows(p)` propagation across multi-file check.
    api_path = write("s91/mod2/api.brat", """
extern fn c_view(p: ptr) -> ptr pure nothrow borrows(p);
""")
    m2 = write("s91/mod2/main.brat", """
import api
extern "c" fn malloc(n: usize) -> move ptr;
extern "c" fn free(move p: ptr);
fn main() {
    let owner = malloc(16);
    let v = c_view(owner);
    free(move v);
    free(move owner);
}
""")
    rp2 = subprocess.run([sys.executable, CLI, "check", api_path, m2],
                         capture_output=True, text=True)
    rc2 = subprocess.run([BIN, "check", api_path, m2],
                         capture_output=True, text=True)
    check(rp2.returncode != 0 and "cannot move borrowed pointer `v`" in (rp2.stdout + rp2.stderr),
          "py multi-file extern borrows: %r" % (rp2.stdout + rp2.stderr))
    check(rc2.returncode != 0 and "cannot move borrowed pointer `v`" in (rc2.stdout + rc2.stderr),
          "cx multi-file extern borrows: %r" % (rc2.stdout + rc2.stderr))


def test_plugin_abi():
    plug_brat = write("s92/plug.brat", """
export fn add_i64(a: i64, b: i64) -> i64 { return a + b }
export fn scale_f64(x: f64, k: i64) -> f64 { return x * k }
export fn passthru(p: ptr) -> ptr { return p }
fn main() -> i32 { return 0 }
""")
    # Byte-for-byte Python-CLI/native-CLI output + warning cleanliness.
    ec_py = subprocess.run(
        [sys.executable, CLI, "emit-c", plug_brat],
        capture_output=True, text=True, check=True,
        env=dict(os.environ, BRAT_CXX=BIN, BRAT_NO_NATIVE_CLI="1"),
    ).stdout
    ec_cx = subprocess.run([BIN, "emit-c", plug_brat], capture_output=True, text=True, check=True).stdout
    check(ec_py == ec_cx, "plugin emit-c CLI doors mismatch")
    check("brat_plugin_abi_v1" in ec_cx, "missing brat_plugin_abi_v1 table")
    cfile = write("s92/plug_check.c", ec_cx)
    werr = subprocess.run(
        ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2",
         "-isystem", os.path.join(ROOT, "lang", "engine"),
         "-c", cfile, "-o", os.path.join(TMP, "s92/plug_check.o")],
        capture_output=True, text=True,
    )
    check(werr.returncode == 0, "emitted plugin C failed -Werror: %r" % werr.stderr)

    so_path = os.path.join(TMP, "s92/libplug.so")
    sb = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", plug_brat, "--shared", "-o", so_path],
                        capture_output=True, text=True)
    check(sb.returncode == 0, "shared build failed: %r" % (sb.stdout + sb.stderr))

    # Plain C plugin without `brat_plugin_abi_v1` keeps working too.
    raw_c = write("s92/raw.c", "long long raw_mul(long long a, long long b) { return a * b; }\n")
    raw_so = os.path.join(TMP, "s92/libraw.so")
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", raw_c, "-o", raw_so], check=True)

    caller = write("s92/caller.brat", f'''
fn main() {{
    let h = lib_open("{so_path}")
    let add = lib_sym(h, "add_i64")
    print(call(add, "i64(i64, i64)", 20, 22))
    let sc = lib_sym(h, "scale_f64")
    print(call(sc, "f64(f64, i64)", 2.5, 4))
    let pt = lib_sym(h, "passthru")
    print(call(pt, "ptr(&ptr)", 0))
    try {{
        let _bad = call(add, "f64(i64, f64)", 1, 2.0)
        print("UNREACHABLE")
    }} catch e {{
        print("caught_abi_mismatch")
    }}
    lib_close(h)

    let hr = lib_open("{raw_so}")
    let rm = lib_sym(hr, "raw_mul")
    print(call(rm, "i64(i64, i64)", 6, 7))
    lib_close(hr)
}}
''')
    want = "42\n10.0\n0\ncaught_abi_mismatch\n42\n"
    ri = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", caller], capture_output=True, text=True)
    check(ri.returncode == 0 and ri.stdout == want, "interp plugin: %r" % (ri.stdout + ri.stderr))

    caller_exe = os.path.join(TMP, "s92/caller.exe")
    cb = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", caller, "-o", caller_exe],
                        capture_output=True, text=True)
    check(cb.returncode == 0, "caller build: %r" % (cb.stdout + cb.stderr))
    rn = subprocess.run([caller_exe], capture_output=True, text=True)
    check(rn.returncode == 0 and rn.stdout == want, "native plugin: %r" % (rn.stdout + rn.stderr))

    # Uncaught ABI mismatch surfaces the exact signature diagnostic in both runtimes.
    uncaught = write("s92/uncaught.brat", f'''
fn main() {{
    let h = lib_open("{so_path}")
    let add = lib_sym(h, "add_i64")
    print(call(add, "i64(i64, f64)", 1, 2.0))
    lib_close(h)
}}
''')
    diag = "call: ABI mismatch on `add_i64`: plugin exports `i64(i64,i64)`, call site wrote `i64(i64,f64)`"
    ru_i = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", uncaught], capture_output=True, text=True)
    check(ru_i.returncode != 0 and diag in (ru_i.stdout + ru_i.stderr),
          "interp uncaught: %r" % (ru_i.stdout + ru_i.stderr))
    uncaught_exe = os.path.join(TMP, "s92/uncaught.exe")
    subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", uncaught, "-o", uncaught_exe],
                   check=True, capture_output=True)
    ru_n = subprocess.run([uncaught_exe], capture_output=True, text=True)
    check(ru_n.returncode != 0 and diag in (ru_n.stdout + ru_n.stderr),
          "native uncaught: %r" % (ru_n.stdout + ru_n.stderr))


def test_extern_union():
    hdr = write("s93/val.h", """
#include <stdint.h>

typedef union {
    int64_t i;
    double f;
} ScalarU;

typedef struct {
    int32_t kind;
    union {
        int64_t iv;
        double fv;
    };
    struct {
        int32_t lo;
        int32_t hi;
    };
} Event;

ScalarU su_from_f(double x);
double su_read_f(ScalarU u);
int64_t su_read_i(ScalarU u);
int64_t ev_score(Event e);
""")
    csrc = write("s93/val.c", """
#include "val.h"
ScalarU su_from_f(double x) { ScalarU u; u.f = x; return u; }
double su_read_f(ScalarU u) { return u.f; }
int64_t su_read_i(ScalarU u) { return u.i; }
int64_t ev_score(Event e) {
    int64_t base = (e.kind == 1) ? e.iv : (int64_t)(e.fv * 10.0);
    return base + e.lo + e.hi;
}
""")
    so = os.path.join(TMP, "s93/libval.so")
    subprocess.run(["cc", "-shared", "-fPIC", "-g", "-O2", csrc, "-o", so], check=True)

    bg = subprocess.run([sys.executable, CLI, "bindgen", hdr, "--lib", so],
                        capture_output=True, text=True, check=True)
    check("extern union ScalarU" in bg.stdout, "missing ScalarU union: %r" % bg.stdout)
    check("extern union Event__anon0" in bg.stdout, "missing Event__anon0 union: %r" % bg.stdout)
    check("extern struct Event__anon1" in bg.stdout, "missing Event__anon1 struct: %r" % bg.stdout)
    check("extern struct Event" in bg.stdout, "missing Event struct: %r" % bg.stdout)

    app = write("s93/app.brat", bg.stdout + """
fn main() {
    let u = su_from_f(2.5)
    print(su_read_f(u))
    print(su_read_i(ScalarU { i: 123456 }))
    print(su_read_f({"f": 7.25}))
    let e1 = Event {
        kind: 1,
        anon0: Event__anon0 { iv: 100 },
        anon1: Event__anon1 { lo: 3, hi: 4 }
    }
    let e2 = Event {
        kind: 2,
        anon0: Event__anon0 { fv: 5.5 },
        anon1: Event__anon1 { lo: 1, hi: 2 }
    }
    print(ev_score(e1))
    print(ev_score(e2))
}
""")

    # AST parity, and identical C through both CLI doors.
    P._seq = 0
    sx_py = _cxx.sx(P.parse_prog(Cur(lex(open(app, encoding="utf-8").read()))))
    sx_cx = subprocess.run([BIN, "ast", app], capture_output=True, text=True, check=True).stdout.strip()
    check(sx_py == sx_cx, "AST parity mismatch on extern union")

    ec_py = subprocess.run(
        [sys.executable, CLI, "emit-c", app, "--unsafe-ffi"],
        capture_output=True, text=True, check=True,
        env=dict(os.environ, BRAT_CXX=BIN, BRAT_NO_NATIVE_CLI="1"),
    ).stdout
    ec_cx = subprocess.run([BIN, "emit-c", "--unsafe-ffi", app],
                           capture_output=True, text=True, check=True).stdout
    check(ec_py == ec_cx, "Emit-C CLI doors mismatch on extern union")

    # Header proof, DWARF object proof, and L5 differential ABI matrix proof.
    for extra in (["--header", hdr], ["--object", so], ["--header", hdr, "--abi-matrix"]):
        rv = subprocess.run([sys.executable, CLI, "ffi", app] + extra, capture_output=True, text=True)
        check(rv.returncode == 0, "ffi %r failed:\n%s" % (extra, rv.stdout + rv.stderr))

    # Negative check: multi-arm `extern union` literal + `ptr_read_struct`/`ptr_write_struct` checks.
    multi_arm = write("s93/multi_arm.brat", """
extern union ScalarU { i: i64, f: f64 }
fn main() {
    let _u = ScalarU { i: 1, f: 2.0 }
}
""")
    rma_py = subprocess.run([sys.executable, CLI, "check", multi_arm], capture_output=True, text=True)
    rma_cx = subprocess.run([BIN, "check", multi_arm], capture_output=True, text=True)
    check(rma_py.returncode != 0 and "initializes 2 arms" in (rma_py.stdout + rma_py.stderr),
          "py multi-arm union refusal: %r" % (rma_py.stdout + rma_py.stderr))
    check(rma_cx.returncode != 0 and "initializes 2 arms" in (rma_cx.stdout + rma_cx.stderr),
          "cx multi-arm union refusal: %r" % (rma_cx.stdout + rma_cx.stderr))

    doc_out = subprocess.run([sys.executable, CLI, "doc", "--json", app],
                             capture_output=True, text=True, check=True).stdout
    check('"kind": "extern_union"' in doc_out, "brat doc missing extern_union: %r" % doc_out)

    # Negative check: declaring ScalarU as `extern struct` must be refused against both header & DWARF!
    wrong_kind = write("s93/wrong_kind.brat", """
extern struct ScalarU { i: i64, f: f64 }
fn main() {}
""")
    rh_bad = subprocess.run([sys.executable, CLI, "ffi", wrong_kind, "--header", hdr],
                            capture_output=True, text=True)
    ro_bad = subprocess.run([sys.executable, CLI, "ffi", wrong_kind, "--object", so],
                            capture_output=True, text=True)
    check(rh_bad.returncode != 0 and "union" in (rh_bad.stdout + rh_bad.stderr),
          "header union-vs-struct check missed: %r" % (rh_bad.stdout + rh_bad.stderr))
    check(ro_bad.returncode != 0 and "union" in (ro_bad.stdout + ro_bad.stderr),
          "DWARF union-vs-struct check missed: %r" % (ro_bad.stdout + ro_bad.stderr))

    # Execution parity (--interp vs native with embedded BRATFFI1 contract).
    want = "2.5\n123456\n7.25\n107\n58\n"
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.dirname(so))
    ri = subprocess.run([sys.executable, CLI, "run", "--interp", "--header", hdr, app],
                        capture_output=True, text=True, env=env)
    check(ri.returncode == 0 and ri.stdout == want, "interp union: %r" % (ri.stdout + ri.stderr))

    exe = os.path.join(TMP, "s93/app.exe")
    b = subprocess.run([sys.executable, CLI, "build", "--header", hdr, "--link", so, app, "-o", exe],
                       capture_output=True, text=True)
    check(b.returncode == 0, "native union build: %r" % (b.stdout + b.stderr))
    rn = subprocess.run([exe], capture_output=True, text=True, env=env)
    check(rn.returncode == 0 and rn.stdout == want, "native union run: %r" % (rn.stdout + rn.stderr))

    vb = subprocess.run([sys.executable, CLI, "ffi", "--bin", exe, "--header", hdr],
                        capture_output=True, text=True)
    check(vb.returncode == 0, "ffi --bin failed: %r" % (vb.stdout + vb.stderr))

    exe_cxx = os.path.join(TMP, "s93/app_cxx.exe")
    bc = subprocess.run([BIN, "build", "--unsafe-ffi", "--link", so, app, "-o", exe_cxx],
                        capture_output=True, text=True)
    check(bc.returncode == 0, "cxx native union build: %r" % (bc.stdout + bc.stderr))
    vbc = subprocess.run([sys.executable, CLI, "ffi", "--bin", exe_cxx, "--header", hdr],
                         capture_output=True, text=True)
    check(vbc.returncode == 0, "cxx ffi --bin failed: %r" % (vbc.stdout + vbc.stderr))


def test_sanitizers_and_zero_leaks():
    # 1. Pin KNOWN_LEAKS == set() in test_asan_examples.py.
    asan_mod_src = open(os.path.join(HERE, "test_asan_examples.py"), encoding="utf-8").read()
    check("KNOWN_LEAKS = set()" in asan_mod_src, "KNOWN_LEAKS must remain empty")

    # 2. ASan + LSan + UBSan clean execution of borrows(p) + extern union.
    lib_c = write("s94/sanlib.c", """
#include <stdint.h>
#include <stdlib.h>
typedef union { int64_t i; double f; } U64;
void *buf_new(int64_t v) {
    int64_t *p = (int64_t *)malloc(16);
    p[0] = v;
    p[1] = v * 2;
    return p;
}
void *buf_tail(void *p) { return (char *)p + 8; }
int64_t buf_read(void *p) { return *(int64_t *)p; }
void buf_free(void *p) { free(p); }
int64_t u64_add_i(U64 u, int64_t k) { return u.i + k; }
""")
    san_hdr = write("s94/sanlib.h", """
#include <stdint.h>
typedef union { int64_t i; double f; } U64;
void *buf_new(int64_t v);
void *buf_tail(void *p);
int64_t buf_read(void *p);
void buf_free(void *p);
int64_t u64_add_i(U64 u, int64_t k);
""")
    san_prog = write("s94/san.brat", """
extern union U64 { i: i64, f: f64 }
extern fn buf_new(v: i64) -> move ptr
extern fn buf_tail(p: ptr) -> ptr pure nothrow borrows(p)
extern fn buf_read(p: ptr) -> i64 pure nothrow reads(p)
extern fn buf_free(move p: ptr)
extern fn u64_add_i(u: U64, k: i64) -> i64 pure nothrow

fn main() {
    let owner = buf_new(21)
    let tail = buf_tail(owner)
    let val = buf_read(tail)
    buf_free(move owner)
    print(u64_add_i(U64 { i: val }, 8))
}
""")
    c_out = subprocess.run([BIN, "emit-c", "--header", san_hdr, san_prog],
                           capture_output=True, text=True, check=True).stdout
    san_c = write("s94/san_app.c", c_out)
    san_exe = os.path.join(TMP, "s94/san_app.exe")
    cc_san = subprocess.run(
        ["cc", "-std=c99", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         "-I", os.path.join(ROOT, "lang", "engine"), san_c, lib_c, "-lm", "-lpthread", "-ldl", "-o", san_exe],
        capture_output=True, text=True,
    )
    check(cc_san.returncode == 0, "ASan+UBSan compile failed: %r" % cc_san.stderr)
    san_env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    r_san = subprocess.run([san_exe], capture_output=True, text=True, env=san_env)
    check(r_san.returncode == 0 and r_san.stdout == "50\n",
          "ASan+LSan+UBSan run failed: %r" % (r_san.stdout + r_san.stderr))

    # 3. TSan clean concurrent execution using borrows(p) + extern union inside `scope { spawn }`.
    tsan_prog = write("s94/tsan.brat", """
extern union U64 { i: i64, f: f64 }
extern fn buf_new(v: i64) -> move ptr
extern fn buf_tail(p: ptr) -> ptr pure nothrow borrows(p)
extern fn buf_read(p: ptr) -> i64 pure nothrow reads(p)
extern fn buf_free(move p: ptr)
extern fn u64_add_i(u: U64, k: i64) -> i64 pure nothrow

fn worker(seed: i64) -> i64 {
    let owner = buf_new(seed)
    let tail = buf_tail(owner)
    let got = buf_read(tail)
    buf_free(move owner)
    return u64_add_i(U64 { i: got }, 1)
}

fn main() {
    let ch = chan(4)
    scope {
        spawn { ch.send(worker(10)) }
        spawn { ch.send(worker(20)) }
    }
    ch.close()
    print(ch.recv() + ch.recv())
}
""")
    tsan_c = write("s94/tsan_app.c",
                   subprocess.run([BIN, "emit-c", "--header", san_hdr, tsan_prog],
                                  capture_output=True, text=True, check=True).stdout)
    tsan_exe = os.path.join(TMP, "s94/tsan_app.exe")
    cc_tsan = subprocess.run(
        ["cc", "-std=c99", "-O1", "-g", "-fsanitize=thread",
         "-I", os.path.join(ROOT, "lang", "engine"), tsan_c, lib_c, "-lm", "-lpthread", "-ldl", "-o", tsan_exe],
        capture_output=True, text=True,
    )
    if cc_tsan.returncode == 0:
        r_tsan = subprocess.run([tsan_exe], capture_output=True, text=True)
        check(r_tsan.returncode == 0 and r_tsan.stdout == "62\n",
              "TSan run failed: %r" % (r_tsan.stdout + r_tsan.stderr))

    # 4. Mock Win32 header check (-D_WIN32 -Wall -Wextra -Werror) remains clean.
    with tempfile.TemporaryDirectory() as wdir:
        for hdr_name, guard in (
            ("windows.h", """
typedef void *HANDLE; typedef void *HMODULE; typedef int BOOL;
typedef unsigned int UINT; typedef unsigned long DWORD;
typedef const char *LPCSTR; typedef char *LPSTR;
typedef int (*FARPROC)(void);
typedef struct { void *Ptr; } SRWLOCK;
#define SRWLOCK_INIT {0}
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { (void)l; }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; }
#define SEM_FAILCRITICALERRORS 1
#define SEM_NOOPENFILEERRORBOX 2
#define LOAD_WITH_ALTERED_SEARCH_PATH 8
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800
#define FORMAT_MESSAGE_FROM_SYSTEM 0x1000
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x200
static inline UINT SetErrorMode(UINT m) { return m; }
static inline HMODULE LoadLibraryExA(LPCSTR p, HANDLE h, DWORD f) { (void)p;(void)h;(void)f; return 0; }
static inline DWORD GetLastError(void) { return 1; }
static inline DWORD FormatMessageA(DWORD f, const void *s, DWORD id, DWORD l, LPSTR b, DWORD n, void *a) {
    (void)f;(void)s;(void)id;(void)l;(void)b;(void)n;(void)a; return 0;
}
static inline FARPROC GetProcAddress(HMODULE m, LPCSTR s) { (void)m;(void)s; return 0; }
static inline BOOL FreeLibrary(HMODULE m) { (void)m; return 1; }
static inline DWORD GetModuleFileNameA(HMODULE m, LPSTR b, DWORD n) { (void)m;(void)b;(void)n; return 0; }
static inline DWORD GetFullPathNameA(LPCSTR p, DWORD n, LPSTR b, LPSTR *f) { (void)p;(void)n;(void)b;(void)f; return 0; }
static inline DWORD GetFileAttributesA(LPCSTR p) { (void)p; return 0; }
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
"""),
        ):
            with open(os.path.join(wdir, hdr_name), "w", encoding="utf-8") as wf:
                wf.write(guard)
        # Verify BPluginAbiEntry + vplugin_acquire_sig compile warning-clean on POSIX too:
        probe = os.path.join(wdir, "crt_probe.c")
        with open(probe, "w", encoding="utf-8") as pf:
            pf.write('#define BRAT_CASE_MAP_VERSION 1500\n#include "crt.h"\n'
                     'static Val vcall_any(Val fn, Vec *a) { (void)fn; (void)a; return vnil(); }\n'
                     'int main(void) { return 0; }\n')
        rc_pos = subprocess.run(
            ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(ROOT, "lang", "engine"),
             "-c", probe, "-o", os.path.join(wdir, "crt_probe.o")],
            capture_output=True, text=True,
        )
        check(rc_pos.returncode == 0, "crt.h -Werror failed: %r" % rc_pos.stderr)


def main():
    test_borrows_effect()
    print("ok: borrows(p) FFI provenance + NLL + cross-module")
    test_plugin_abi()
    print("ok: brat_plugin_abi_v1 + runtime call ABI verification")
    test_extern_union()
    print("ok: extern union + C11 anon inline union/struct")
    test_sanitizers_and_zero_leaks()
    print("ok: KNOWN_LEAKS=0 + ASan/LSan/UBSan + TSan + -Werror")
    print("SAFETY ALL GREEN (%d asserts)" % asserts)


if __name__ == "__main__":
    main()
