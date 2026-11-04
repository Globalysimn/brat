"""FFI hardening: inputs that used to reach C and crash, or diverge between backends.

- An unknown argument type (`u128`) is refused by both back ends, as the
  interpreter already refuses it at call time.
- A negative pointer argument raises `ffi: arg N of f wants ptr` in both backends
  instead of segfaulting inside C.
- A str argument containing a NUL byte raises in the interpreter. Native strings
  are NUL-terminated C strings, so the known length divergence is documented in
  CHANGELOG, not hidden here.
"""
import _scratch
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-ffihard-")

def build_mini(cc, text, flag):
    """Compile a struct-plus-asserts snippet, or None when the flag is unavailable."""
    if not cc:
        return None, ""
    src = os.path.join(TMP, "mini%d.c" % abs(hash((text, flag))))
    with open(src, "w", encoding="utf-8") as f:
        f.write(text)
    r = subprocess.run([cc, flag, "-std=c99", "-c", src, "-o", src + ".o"],
                       capture_output=True, text=True, timeout=180)
    if r.returncode != 0 and ("unrecognized" in r.stderr or "not found" in r.stderr
                              or "cannot find" in r.stderr):
        return None, r.stderr[-300:]      # no 32-bit support on this machine
    return r.returncode == 0, (r.stderr or r.stdout)[-400:]


NEG_PTR = (
    'extern fn free(move p: ptr[i32]);\n'
    'fn main() -> i64 {\n'
    '    try {\n'
    '        free(-5)\n'
    '    } catch e {\n'
    '        print(e)\n'
    '    }\n'
    '    return 0;\n'
    '}\n')

NUL_STR = (
    'extern fn strlen(s: str) -> usize;\n'
    'fn main() -> i64 {\n'
    '    try {\n'
    '        print(strlen("a\\0b"))\n'
    '    } catch e {\n'
    '        print(e)\n'
    '    }\n'
    '    return 0;\n'
    '}\n')

VARARG_STR = (
    'extern fn printf(fmt: str, ..ptr) -> i32;\n'
    'fn main() -> i64 {\n'
    '    let n = printf("[%s] %s\\n", "hi", "there")\n'
    '    return n\n'
    '}\n')

VARARG_I64 = (
    'extern fn printf(fmt: str, ..i64) -> i32;\n'
    'fn main() -> i64 {\n'
    '    let n = printf("%d\\n", "hi")\n'
    '    return n\n'
    '}\n')

FIXED_PTR = (
    'extern fn strlen(s: ptr) -> i64;\n'
    'fn main() -> i64 {\n'
    '    print(strlen("hi"))\n'
    '    return 0\n'
    '}\n')

NEG_READ = (
    'fn main() -> i64 {\n'
    '    try {\n'
    '        print(ptr_read_i64(-8))\n'
    '    } catch e {\n'
    '        print(e)\n'
    '    }\n'
    '    try {\n'
    '        ptr_write_i64(-8, 1)\n'
    '    } catch e {\n'
    '        print(e)\n'
    '    }\n'
    '    try {\n'
    '        print(ptr_read_i64(0))\n'
    '    } catch e {\n'
    '        print(e)\n'
    '    }\n'
    '    return 0\n'
    '}\n')

# Names spelled into generated C as written. A token that is not a name used to
# be copied into the C text, so `brat check` said 0 errors and `cc` failed on
# source nobody wrote; the parser now refuses the token.
BAD_NAMES = {
    "extern_str_name":
        'extern "libfoo" fn "abc"(n: i64) -> i64;\n'
        'fn main() -> i64 {\n    return 0\n}\n',
    "extern_kw_name":
        'extern "libfoo" fn while(n: i64) -> i64;\n'
        'fn main() -> i64 {\n    return 0\n}\n',
    # A member the BRAT lexer refuses; a C keyword (`do`, `int`) is fine there,
    # because the emitter writes it as `bf_do` (test_bindgen_c.py runs one).
    "struct_kw_field":
        'extern struct Foo { while: i64 }\n'
        'fn main() -> i64 {\n    return 0\n}\n',
    "export_str_name":
        'export fn "abc"() -> i64 {\n    return 1\n}\n'
        'fn main() -> i64 {\n    return 0\n}\n',
}

# The other half: a name the runtime already declares cannot be spelled as a C
# type, so the emitter prefixes FFI structs and members instead of copying them.
RUNTIME_NAME = (
    'extern struct Val { tag: i64, len: i64 }\n'
    'fn main() -> i64 {\n    print("ok")\n    return 0\n}\n')

UNKNOWN_ARG = (
    'extern fn abs(x: u128) -> i32;\n'
    'fn main() -> i64 {\n'
    '    print(abs(5))\n'
    '    return 0;\n'
    '}\n')


def write(name, src):
    p = os.path.join(TMP, name + ".brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)
    return p


def run(cmd, timeout=300):
    # The suite builds the C it calls and asserts on the ABI itself, so it takes
    # the proof gate's own way out instead of writing a header per fixture.
    if cmd and cmd[0] in ("build", "run", "emit-c", "dist") \
            and "--interp" not in cmd:
        cmd = [cmd[0], "--unsafe-ffi"] + list(cmd[1:])
    elif cmd and cmd[0] == "run" and "--interp" in cmd and "--unsafe-ffi" not in cmd:
        i = cmd.index("--interp") + 1
        cmd = list(cmd[:i]) + ["--unsafe-ffi"] + list(cmd[i:])
    return subprocess.run([sys.executable, CLI] + cmd, capture_output=True,
                          timeout=timeout)


def main():
    n = 0

    # Negative pointer: same message on both backends, no segfault.
    want = b"ffi: arg 1 of free wants ptr[i32]\n"
    path = write("neg_ptr", NEG_PTR)
    ri = run(["run", "--interp", path], 120)
    assert ri.returncode == 0 and ri.stdout == want, "neg_ptr interp rc=%d out=%r" % (ri.returncode, ri.stdout)
    exe = os.path.join(TMP, "neg_ptr.exe")
    b = run(["build", path, "-o", exe])
    assert b.returncode == 0, "neg_ptr build failed"
    rn = subprocess.run([exe], capture_output=True, timeout=60)
    assert rn.returncode == 0 and rn.stdout == want, "neg_ptr native rc=%d out=%r" % (rn.returncode, rn.stdout)
    n += 1
    print("ffihard ok: neg_ptr")

    # NUL in a string literal: refused at parse, before any FFI call can truncate it.
    path = write("nul_str", NUL_STR)
    ri = run(["run", "--interp", path], 120)
    assert ri.returncode != 0 and b"string literal contains NUL" in (ri.stdout + ri.stderr), \
        "nul_str interp rc=%d out=%r" % (ri.returncode, ri.stdout + ri.stderr)
    rb = run(["build", path, "-o", os.path.join(TMP, "nul.exe")], 120)
    assert rb.returncode != 0 and b"string literal contains NUL" in (rb.stdout + rb.stderr), \
        "nul_str build rc=%d out=%r" % (rb.returncode, rb.stdout + rb.stderr)
    n += 1
    print("ffihard ok: nul_str")

    # Unknown argument type: native build and gated `--interp` refuse statically,
    # and `--interp --no-check` still refuses at the runtime FFI guard.
    path = write("unknown_arg", UNKNOWN_ARG)
    ri_chk = run(["run", "--interp", path], 120)
    assert ri_chk.returncode != 0 and b"unsupported FFI type `u128`" in (ri_chk.stdout + ri_chk.stderr), \
        "unknown_arg interp gate should refuse: rc=%d" % ri_chk.returncode
    ri = run(["run", "--interp", "--no-check", path], 120)
    assert ri.returncode != 0 and b"unsupported type u128" in (ri.stdout + ri.stderr), \
        "unknown_arg interp --no-check should refuse: rc=%d" % ri.returncode
    exe = os.path.join(TMP, "unknown_arg.exe")
    b = run(["build", path, "-o", exe])
    assert b.returncode != 0 and not os.path.exists(exe), \
        "unknown_arg: build must refuse, rc=%d" % b.returncode
    n += 1
    print("ffihard ok: unknown_arg")

    # `%s`: a str fills a variadic `..ptr` slot as `const char *`, both backends.
    path = write("vararg_str", VARARG_STR)
    ri = run(["run", "--interp", path], 120)
    exe = os.path.join(TMP, "vararg_str.exe")
    b = run(["build", path, "-o", exe])
    assert b.returncode == 0, "vararg_str build failed: %r" % b.stdout
    rn = subprocess.run([exe], capture_output=True, timeout=60)
    assert (ri.returncode, ri.stdout) == (11, b"[hi] there\n"), \
        "vararg_str interp rc=%d out=%r" % (ri.returncode, ri.stdout)
    assert (rn.returncode, rn.stdout) == (ri.returncode, ri.stdout), \
        "vararg_str native rc=%d out=%r" % (rn.returncode, rn.stdout)
    n += 1
    print("ffihard ok: vararg_str")

    # The rule is the `..ptr` annotation, not the slot: `..i64` and a fixed `ptr`
    # still refuse a str.
    for name, src in (("vararg_i64", VARARG_I64), ("fixed_ptr", FIXED_PTR)):
        path = write(name, src)
        ri = run(["run", "--interp", path], 120)
        assert ri.returncode != 0, "%s: run must fail, rc=%d" % (name, ri.returncode)
        rc = run(["check", path], 120)
        assert rc.returncode != 0 and b"want i64, got str" in (rc.stdout + rc.stderr), \
            "%s: checker should refuse: rc=%d out=%r" % (name, rc.returncode, rc.stdout + rc.stderr)
        exe = os.path.join(TMP, name + ".exe")
        b = run(["build", path, "-o", exe])
        assert b.returncode != 0 and not os.path.exists(exe), \
            "%s: build must refuse, rc=%d" % (name, b.returncode)
        n += 1
        print("ffihard ok: %s refused" % name)

    # A negative address is never one: read and write raise instead of faulting,
    # the same words in both backends, and 0 keeps its own message.
    want = b"pointer is negative\npointer is negative\nnull pointer dereference\n"
    path = write("neg_read", NEG_READ)
    ri = run(["run", "--interp", path], 120)
    assert ri.returncode == 0 and ri.stdout == want, "neg_read interp rc=%d out=%r" % (ri.returncode, ri.stdout)
    exe = os.path.join(TMP, "neg_read.exe")
    b = run(["build", path, "-o", exe])
    assert b.returncode == 0, "neg_read build failed: %r" % b.stdout
    rn = subprocess.run([exe], capture_output=True, timeout=60)
    assert (rn.returncode, rn.stdout) == (0, want), "neg_read native rc=%d out=%r" % (rn.returncode, rn.stdout)
    n += 1
    print("ffihard ok: neg_read")

    # C-visible names: refused before any C exists, in every entry point.
    for name, src in BAD_NAMES.items():
        path = write(name, src)
        for cmd in (["check", path], ["run", path],
                    ["build", path, "-o", os.path.join(TMP, name + ".exe")],
                    ["emit-c", path]):
            r = run(cmd, 120)
            assert r.returncode != 0 and b"wants a plain name" in (r.stdout + r.stderr), \
                "%s %s should refuse: rc=%d out=%r" % (name, cmd[0], r.returncode,
                                                      (r.stdout + r.stderr)[-300:])
        assert not os.path.exists(os.path.join(TMP, name + ".exe")), \
            "%s: no binary may be produced" % name
        n += 1
        print("ffihard ok: %s refused" % name)

    # Names the runtime owns are prefixed, so the same program builds and runs.
    path = write("runtime_name", RUNTIME_NAME)
    rc = run(["check", path], 120)
    assert rc.returncode == 0, "runtime_name check: %r" % (rc.stdout + rc.stderr)
    re_ = run(["emit-c", path], 120)
    c_src = re_.stdout
    assert b"} bf_Val;" in c_src and b"int64_t bf_tag;" in c_src, \
        "runtime_name: emitted C must prefix the type and its members: %r" % c_src[:400]
    assert b"} Val;" not in c_src, "runtime_name: raw `Val` would clash with crt.h"
    exe = os.path.join(TMP, "runtime_name.exe")
    b = run(["build", path, "-o", exe])
    assert b.returncode == 0, "runtime_name build failed: %r" % (b.stdout + b.stderr)
    rn = subprocess.run([exe], capture_output=True, timeout=60)
    ri = run(["run", "--interp", path], 120)
    assert (rn.returncode, rn.stdout) == (ri.returncode, ri.stdout) == (0, b"ok\n"), \
        "runtime_name native rc=%d out=%r" % (rn.returncode, rn.stdout)
    n += 1
    print("ffihard ok: runtime_name prefixed")


    # The signature is checked, not only emitted
    # A type the emitter cannot spell used to pass `check` and die in `build`,
    # so `check` was not the verdict it claims to be. Both checkers now refuse
    # it at the declaration, with the same bytes and a stable code.
    UNUSED_BAD = 'extern fn abs(x: u128) -> i32;\nprint(1)\n'
    path = write("unused_bad_decl", UNUSED_BAD)
    py = run(["check", "--py", path], 120)
    cx = run(["check", path], 120)
    assert py.returncode == 1 and cx.returncode == 1, \
        "unused_bad_decl: check must refuse, rc py=%d cxx=%d" % (py.returncode, cx.returncode)
    assert py.stdout == cx.stdout, \
        "unused_bad_decl: checkers disagree\n--- py ---\n%s\n--- cxx ---\n%s" % (
            py.stdout.decode("utf-8", "replace"), cx.stdout.decode("utf-8", "replace"))
    out = py.stdout
    assert b"unsupported FFI type `u128`" in out, "unused_bad_decl: %r" % out
    # D3: a declaration that is never called is still checked -- the surface of
    # the language does not depend on whether a call exists.
    assert b"export fn" not in out, "unused_bad_decl: %r" % out
    n += 1
    print("ffihard ok: unsupported signature refused by check, both checkers")

    # D2: the same program now fails `check` and `build`, so the verdicts agree.
    BAD_CALL = ('extern fn abs(x: u128) -> i32;\n'
                'fn main() -> i64 {\n    print(abs(5))\n    return 0\n}\n')
    path = write("bad_call", BAD_CALL)
    assert run(["check", path], 120).returncode == 1, "bad_call: check must refuse"
    exe = os.path.join(TMP, "bad_call.exe")
    assert run(["build", path, "-o", exe], 120).returncode != 0 and not os.path.exists(exe), \
        "bad_call: build must refuse too"
    n += 1
    print("ffihard ok: check and build agree on an unsupported signature")

    # D4: the refusal is an FFI error with a stable code, not a `SyntaxError`.
    js = run(["check", "--json", path], 120)
    doc = json.loads(js.stdout.decode("utf-8"))
    codes = [d["code"] for d in doc["diagnostics"]]
    assert codes and set(codes) == {"F0001"}, "bad_call: codes %r" % codes
    assert all(d["helps"] for d in doc["diagnostics"]), "bad_call: a code with no help"
    n += 1
    print("ffihard ok: unsupported signature is F0001 with a help")

    # C spellings a header actually carries are accepted, and mean the BRAT
    # type of that width: `brat bindgen` may write either name. The library is
    # named on purpose -- a libc symbol carries no declaration in the emitted C
    # (the system header already has it), and that is not what this checks.
    SPELLINGS = (
        'extern "libkern.so" fn len_c(s: str) -> size_t;\n'
        'extern "libkern.so" fn abs_c(x: int64_t) -> int64_t;\n'
        'extern "libkern.so" fn ip_c(x: intptr_t) -> ssize_t;\n'
        'extern "libkern.so" fn tail_c(n: i64, ..uint64_t) -> i64;\n'
        'print(1)\n')
    path = write("c_spellings", SPELLINGS)
    rp = run(["check", "--py", path], 120)
    rc2 = run(["check", path], 120)
    assert rp.returncode == 0 and rc2.returncode == 0, \
        "c_spellings: both checkers must accept, py=%r cxx=%r" % (
            rp.stdout + rp.stderr, rc2.stdout + rc2.stderr)
    cs = run(["emit-c", path], 120).stdout
    # Each C spelling reaches the emitted C as itself, not as a machine word:
    # a `size_t` that became `int64_t` is an ABI bug on a 32-bit target.
    for want in (b"extern size_t len_c(const char *)",
                 b"extern int64_t abs_c(int64_t)",
                 b"extern int64_t ip_c(int64_t)",
                 b"extern int64_t tail_c(int64_t, ...)"):
        assert want in cs, "c_spellings: want %r in emitted C, got %r" % (want, cs[:600])
    n += 1
    print("ffihard ok: size_t/int64_t/intptr_t/ssize_t/uint64_t keep their C width")

    # And `size_t` means a real width at run time: strlen through libc.
    SZ_USE = ('extern fn strlen(s: str) -> size_t;\n'
              'fn main() -> i64 {\n    print(strlen("abcd"))\n    return 0\n}\n')
    path = write("size_t_run", SZ_USE)
    exe = os.path.join(TMP, "size_t_run.exe")
    b = run(["build", path, "-o", exe], 120)
    assert b.returncode == 0, "size_t_run build failed: %r" % (b.stdout + b.stderr)
    rn = subprocess.run([exe], capture_output=True, timeout=60)
    ri = run(["run", "--interp", path], 120)
    assert rn.stdout == ri.stdout == b"4\n", \
        "size_t_run: native %r interp %r" % (rn.stdout, ri.stdout)
    n += 1
    print("ffihard ok: size_t works end to end on both backends")

    # Every slot is checked, not just parameters: a field and a variadic tail
    # are the two places where a wrong width corrupts memory quietly.
    FIELD_BAD = ('extern struct B { a: u128 }\n'
                 'print(1)\n')
    path = write("bad_field", FIELD_BAD)
    r1 = run(["check", "--py", path], 120)
    r2 = run(["check", path], 120)
    assert r1.returncode == 1 and r2.returncode == 1, "bad_field: must be refused"
    assert r1.stdout == r2.stdout, "bad_field: checkers disagree"
    assert b"for field `a` of extern struct `B`" in r1.stdout, "bad_field: %r" % r1.stdout

    VAR_BAD = ('extern "libkern.so" fn t(n: i64, ..u128) -> i64;\n'
               'print(1)\n')
    path = write("bad_varargs", VAR_BAD)
    r3 = run(["check", "--py", path], 120)
    r4 = run(["check", path], 120)
    assert r3.returncode == 1 and r4.returncode == 1, "bad_varargs: must be refused"
    assert r3.stdout == r4.stdout, "bad_varargs: checkers disagree"
    assert b"in the variadic tail of extern fn `t`" in r3.stdout, "bad_varargs: %r" % r3.stdout
    n += 1
    print("ffihard ok: struct fields and variadic tails are checked too")

    # Layout is proven against the compiler, not assumed
    # The emitter writes what it believes the layout is; `_Static_assert` makes
    # the C compiler agree or the build stops. Compile-time only: no code, no
    # data, no run-time cost.
    LAYOUT = (
        'extern struct H { kind: u8, len: i32 }\n'
        'extern packed struct P { kind: u8, len: i32 }\n'
        'extern struct N { a: i64, s: str }\n'
        'extern "libx.so" fn h(x: H) -> i64;\n'
        'extern "libx.so" fn p(x: P) -> i64;\n'
        'extern "libx.so" fn n(x: N) -> i64;\n'
        'print(1)\n')
    path = write("layout_asserts", LAYOUT)
    cs = run(["emit-c", path], 120).stdout.decode("utf-8")
    want = [
        # u8 then i32: padded to 8, the int at offset 4.
        '_Static_assert(sizeof(bf_H) == 8,',
        '_Static_assert(offsetof(bf_H, bf_kind) == 0,',
        '_Static_assert(offsetof(bf_H, bf_len) == 4,',
        # packed: no padding, so the int sits at byte 1 and the record is 5.
        '_Static_assert(sizeof(bf_P) == 5,',
        '_Static_assert(offsetof(bf_P, bf_len) == 1,',
        '_Static_assert(sizeof(bf_N) == 16,',
    ]
    for w in want:
        assert w in cs, "layout_asserts: want %r in emitted C" % w
    assert "#include <stddef.h>" in cs, "layout_asserts: offsetof needs stddef.h"
    n += 1
    print("ffihard ok: layout asserted for padded, packed and pointer structs")

    # The interpreter-selected Python CLI path must delegate to the same C,
    # assertion text included.
    import os as _os
    env = dict(_os.environ, BRAT_INTERP="1")
    cli_c = subprocess.run([sys.executable, CLI, "emit-c", "--unsafe-ffi", path], capture_output=True,
                           timeout=120, env=env).stdout.decode("utf-8")
    assert cli_c == cs, "layout_asserts: Python CLI delegation changed the emitted C"
    n += 1
    print("ffihard ok: Python CLI emits the same layout assertions")

    # The assertions are live: a wrong expectation must stop the C compiler.
    # This is the mutation half -- an assertion that never fails proves nothing.
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        print("ffihard skip: no C compiler for the mutation check")
    else:
        good_c = _os.path.join(TMP, "layout_good.c")
        bad_c = _os.path.join(TMP, "layout_bad.c")
        with open(good_c, "w", encoding="utf-8") as f:
            f.write(cs)
        with open(bad_c, "w", encoding="utf-8") as f:
            f.write(cs.replace("sizeof(bf_H) == 8", "sizeof(bf_H) == 9", 1))
        inc = os.path.dirname(CLI)  # lang/engine, where crt.h lives
        ok = subprocess.run([cc, "-std=c99", "-c", good_c, "-o", good_c + ".o", "-I", inc],
                            capture_output=True, text=True, timeout=180)
        assert ok.returncode == 0, \
            "layout_asserts: the honest layout must compile:\n%s" % ok.stderr[-800:]
        bad = subprocess.run([cc, "-std=c99", "-c", bad_c, "-o", bad_c + ".o", "-I", inc],
                             capture_output=True, text=True, timeout=180)
        assert bad.returncode != 0, \
            "layout_asserts: a wrong size must fail the C compiler, and it did not"
        assert "bf_H" in bad.stderr, \
            "layout_asserts: the failure must name the struct:\n%s" % bad.stderr[-400:]
        n += 1
        print("ffihard ok: a wrong layout breaks the C build (mutation check)")

    # Platform widths are the target's, not the host's
    # `long` is 8 bytes here and 4 on i686/Windows. The comparison table carries
    # the usual answer; the probe makes the compiler confirm it. The mutation
    # below pretends the target is 32-bit and the probe must then refuse.
    import importlib.util as _ilu
    spec = _ilu.spec_from_file_location(
        "ffi_probe_mod", os.path.join(os.path.dirname(CLI), "ffi.py"))
    fm = _ilu.module_from_spec(spec)
    spec.loader.exec_module(fm)

    wide_h = os.path.join(TMP, "wide.h")
    with open(wide_h, "w", encoding="utf-8") as f:
        f.write("long lf(long x);\nint plain(int y);\n")
    cdecl_w = fm.parse_header(open(wide_h, encoding="utf-8").read())
    assert fm.platform_width_uses(cdecl_w) == {"long"}, \
        "wide.h: platform widths %r" % fm.platform_width_uses(cdecl_w)
    src = fm.probe_source([], wide_h, cdecl_w)
    assert src and "_Static_assert(sizeof(long) == 8" in src, \
        "wide.h: the probe must assert the width of `long`: %r" % src
    # A type whose width C fixes needs no confirmation from the target.
    fixed_h = os.path.join(TMP, "fixed.h")
    with open(fixed_h, "w", encoding="utf-8") as f:
        f.write("int32_t only(int32_t x);\n")
    cdecl_f = fm.parse_header(open(fixed_h, encoding="utf-8").read())
    assert fm.platform_width_uses(cdecl_f) == set(), \
        "fixed.h: a fixed-width type is not a platform width"
    n += 1
    print("ffihard ok: only platform-width spellings are asserted")

    # The honest width passes; a 32-bit target's `long` must fail the probe.
    ok, out = fm.compile_probe([], wide_h, cdecl=cdecl_w)
    assert ok, "wide.h: the honest width must compile: %r" % out
    saved = fm.PLATFORM_WIDTH_C["long"]
    try:
        fm.PLATFORM_WIDTH_C["long"] = 4          # what an i686 target says
        ok32, out32 = fm.compile_probe([], wide_h, cdecl=cdecl_w)
    finally:
        fm.PLATFORM_WIDTH_C["long"] = saved
    assert not ok32, "a 4-byte `long` target must be refused, and it passed"
    assert "long" in out32 and "wrong width" in out32, \
        "wide.h: the refusal must name the type: %r" % out32
    n += 1
    print("ffihard ok: a 32-bit `long` is refused by the target compiler")

    # A cross build is judged by the target compiler or not at all: without
    # `zig` the stage declines instead of silently applying host widths.
    ok_x, out_x = fm.compile_probe([], wide_h, cdecl=cdecl_w, target="i686-linux-gnu")
    assert ok_x and out_x == "", \
        "cross target without zig must decline quietly, got %r" % out_x
    n += 1
    print("ffihard ok: a cross build never proves widths with the host compiler")

    # The verdict depends on the target, not on the host
    # `long` is 8 bytes on LP64 and 4 on i686/Windows. A program that binds it
    # as `i64` is right on one target and an ABI bug on the other, and `check`
    # has to say which -- otherwise the same source is silently wrong after a
    # `--target`.
    ld_c = fm.parse_header("long f(long x);\n")
    body_i64 = [("extern", None, "f", [("x", "ref", "i64", None)], "i64",
                 None, False, False)]
    models = {"lp64": (None, 0), "ilp32": ("i686-linux-gnu", 2),
              "llp64": ("x86_64-w64-windows-gnu", 2)}
    try:
        for model, (triple, want_errors) in models.items():
            got = fm.set_target(triple)
            rep = fm.check(body_i64, ld_c, "ld.h")
            assert got == model, "set_target(%r) -> %r, want %r" % (triple, got, model)
            assert rep.errors == want_errors, \
                "%s: want %d error(s), got %d: %r" % (model, want_errors, rep.errors,
                                                      rep.lines[:2])
            if want_errors:
                assert "4 bytes" in " ".join(rep.lines), \
                    "%s: the message must name the target width" % model
    finally:
        fm.set_target(None)
    n += 1
    print("ffihard ok: the same binding is right on LP64 and refused on ilp32/llp64")

    # `size_t` follows the pointer, so `usize` stays correct on every target:
    # BRAT emits `size_t`, and C sizes it for the target.
    sz_c = fm.parse_header("size_t g(size_t n);\n")
    body_usize = [("extern", None, "g", [("n", "ref", "usize", None)], "usize",
                   None, False, False)]
    try:
        for triple in (None, "i686-linux-gnu", "x86_64-w64-windows-gnu"):
            fm.set_target(triple)
            rep = fm.check(body_usize, sz_c, "sz.h")
            assert rep.errors == 0, \
                "usize must match size_t on %r: %r" % (triple, rep.lines[:2])
    finally:
        fm.set_target(None)
    n += 1
    print("ffihard ok: size_t/usize agree on every data model")

    body_i64_size = [("extern", None, "g", [("n", "ref", "i64", None)], "i64",
                      None, False, False)]
    try:
        for triple, want_errors in ((None, 0), ("i686-linux-gnu", 2),
                                    ("x86_64-w64-windows-gnu", 0)):
            fm.set_target(triple)
            rep = fm.check(body_i64_size, sz_c, "sz.h")
            assert rep.errors == want_errors, \
                "i64/size_t on %r: expected %d error(s), got %r" % (
                    triple, want_errors, rep.lines[:2])
    finally:
        fm.set_target(None)
    n += 1
    print("ffihard ok: i64 does not match 32-bit size_t")

    # Mutability at the boundary. Two claims are proven here and each one is
    # refused by *both* checkers with the same sentence:
    #   `&x` is an exclusive loan (C gets a writable pointer), so two of them in
    #   one call is the aliasing Rust refuses as `f(&mut x, &mut x)`, and `&x`
    #   of a `let` that never said `mut` is a write the program did not allow.
    #   `str` is a read-only, possibly shared buffer, so it may only be handed to
    #   a pointer the header marks `const`.
    CXX = os.path.abspath(os.path.join(HERE, "..", "cxx", "bin", "brat-cxx"))

    def both_checkers(src, name):
        """(reference verdict, native verdict): the two checkers must agree.

        Without a built `brat-cxx` there is no second checker; the reference
        verdict is then the only one, and it is checked on its own rather than
        crashing on a path that does not exist.
        """
        path = write(name, src)
        ref = subprocess.run([sys.executable, os.path.join(HERE, "..", "engine", "borrow.py"),
                              path], capture_output=True, timeout=300)
        if not os.path.exists(CXX):
            return ref, ref
        nat = subprocess.run([CXX, "check", path], capture_output=True, timeout=300)
        return ref, nat

    ALIAS = ('extern fn swap2(a: ptr[i64], b: ptr[i64])\n'
             'fn main() -> i64 {\n'
             '  let mut x = 1\n'
             '  swap2(&x, &x)\n'
             '  return 0\n'
             '}\n')
    ref, nat = both_checkers(ALIAS, "alias_twice")
    msg = b"two `&x` in one call to `swap2`: C may write the same place through both"
    assert ref.returncode != 0 and msg in ref.stdout, "ref alias: %r" % ref.stdout
    assert nat.returncode != 0 and msg in nat.stdout, "native alias: %r" % nat.stdout
    assert ref.stdout.splitlines()[1] == nat.stdout.splitlines()[1], \
        "the two checkers must word it the same:\n%r\n%r" % (ref.stdout, nat.stdout)
    n += 1
    print("ffihard ok: p5 two &x in one call refused by both checkers")

    IMMUT = ('extern fn wr(p: ptr[i64])\n'
             'fn main() -> i64 {\n'
             '  let x = 1\n'
             '  wr(&x)\n'
             '  return 0\n'
             '}\n')
    ref, nat = both_checkers(IMMUT, "amp_immutable")
    msg = b"pass immutable `x` by address: C may write through `&x`, so declare it `let mut x`"
    assert ref.returncode != 0 and msg in ref.stdout, "ref immut: %r" % ref.stdout
    assert nat.returncode != 0 and msg in nat.stdout, "native immut: %r" % nat.stdout
    n += 1
    print("ffihard ok: p5 &x of an immutable binding refused by both checkers")

    # The text side: a writable `char *` may not be bound as `str`, and a BRAT
    # `str` may not be handed to one. The reverse (a `const char *`) still binds.
    hdr = os.path.join(TMP, "mut.h")
    with open(hdr, "w", encoding="utf-8") as f:
        f.write("void fill(char *buf);\n"
                "void show(const char *s);\n"
                "void blank(const unsigned char *b);\n")
    gen = run(["bindgen", hdr], 120).stdout.decode()
    assert "extern fn show(s: str)" in gen, "a `const char *` is a `str`: %r" % gen
    assert "extern fn fill(buf: ptr[u8])" in gen, \
        "a writable `char *` is a buffer, not a `str`: %r" % gen
    assert "extern fn blank(b: ptr[u8])" in gen, \
        "`const unsigned char *` is bytes, never text: %r" % gen

    prog = write("str_to_writable", 'extern fn fill(buf: str)\n'
                                     'fn main() -> i64 { fill("x")\n  return 0 }\n')
    rc = run(["ffi", "--header", hdr, prog], 120)
    out = rc.stdout + rc.stderr
    assert rc.returncode == 1 and b"which is read-only" in out, \
        "handing a str to a writable buffer must be refused: %r" % out
    prog2 = write("str_to_const", 'extern fn show(s: str)\n'
                                   'fn main() -> i64 { show("x")\n  return 0 }\n')
    assert run(["ffi", "--header", hdr, prog2], 120).returncode == 0, \
        "a `str` beside `const char *` is proven"
    n += 1
    print("ffihard ok: p5 str needs a const pointee")

    # `-> move` on a copy type was reported as a leak of a value that has no
    # storage. A scalar is copied; the declaration is the thing that is wrong.
    SCALAR_MOVE = ('extern fn f() -> move i64\n'
                   'fn main() -> i64 { let n = f()\n  print(n)\n  return 0 }\n')
    ref, nat = both_checkers(SCALAR_MOVE, "move_scalar")
    msg = b"`-> move` on `i64` as the result of extern fn `f`: a scalar is copied, " \
          b"there is nothing to own (drop the mode)"
    assert ref.returncode != 0 and msg in ref.stdout, "ref move_scalar: %r" % ref.stdout
    assert nat.returncode != 0 and msg in nat.stdout, "native move_scalar: %r" % nat.stdout
    assert b"leaked owned pointer" not in ref.stdout + nat.stdout, \
        "a copy type cannot leak: %r" % (ref.stdout + nat.stdout)
    n += 1
    print("ffihard ok: p4 move on a scalar refused, no phantom leak")

    # `-> none` beside an out-parameter used to be the one declaration shape the
    # emitter could not spell: `void fill(char *)` generated `-> none`, and the
    # call `fill(&b)` failed with "unsupported FFI return type `none`".
    NONE_OUT = ('extern fn fill(buf: ptr[u8]) -> none\n'
                'extern fn read1(buf: ptr[u8]) -> u8\n'
                'fn main() -> i64 {\n'
                '  let mut b = 0\n'
                '  fill(&b)\n'
                '  print(read1(&b))\n'
                '  return 0\n'
                '}\n')
    path = write("none_out", NONE_OUT)
    ref, nat = both_checkers(NONE_OUT, "none_out")
    assert ref.returncode == 0 and nat.returncode == 0, \
        "`-> none` with `&x` must check in both: %r / %r" % (ref.stdout, nat.stdout)
    n += 1
    print("ffihard ok: p5 `-> none` is the same declaration as no arrow")

    # A C function that returns a number returns a *value*. Unannotated, `let n =
    # c_fn(x)` used to stay "dynamic, therefore a conservative borrow", so the very
    # next line of the out-parameter idiom -- `let mut out_len = n` -- was refused
    # with "cannot borrow immutable `n` as mut", in both checkers. The declared
    # result type is right there in the extern; the checker now reads it.
    COPY_RET = ('extern fn bound(n: i64) -> u64\n'
                'extern fn wr(p: ptr[u64])\n'
                'fn main() -> i64 {\n'
                '  let n = bound(23)\n'
                '  let mut out_len = n\n'
                '  wr(&out_len)\n'
                '  print(out_len)\n'
                '  return 0\n'
                '}\n')
    ref, nat = both_checkers(COPY_RET, "copy_ret")
    assert ref.returncode == 0, "ref copy_ret: %r" % ref.stdout
    assert nat.returncode == 0, "native copy_ret: %r" % nat.stdout
    n += 1
    print("ffihard ok: an extern scalar result is a copy, not a borrow")

    # The rule is about *scalars*. A struct coming back by value is also fresh, but
    # declaring every result a copy would quietly disable borrow tracking for
    # `str` and the records, so the rejection that must survive is checked too.
    STR_RET = ('extern fn name_of() -> str\n'
               'extern fn take(move s: str) -> i64\n'
               'fn main() -> i64 {\n'
               '  let s = name_of()\n'
               '  let t = s\n'
               '  take(s)\n'
               '  print(t)\n'
               '  return 0\n'
               '}\n')
    ref, nat = both_checkers(STR_RET, "str_ret")
    msg = b"cannot move `s` while borrowed by `t`"
    assert ref.returncode != 0 and msg in ref.stdout, "ref str_ret: %r" % ref.stdout
    assert nat.returncode != 0 and msg in nat.stdout, "native str_ret: %r" % nat.stdout
    n += 1
    print("ffihard ok: a `str` result is still tracked as a borrow")

    # An extern struct is laid out by the target, not by the host
    # The size and offsets the emitter asserts with were computed for 8-byte
    # pointers whatever the target was, so a 32-bit build of a program with an
    # extern struct failed its own `_Static_assert` -- a correct program refused
    # by its own proof. The numbers now follow the target, and a real 32-bit
    # compiler is the judge below when this machine has one.
    lay_src = os.path.join(TMP, "layout_target.brat")
    with open(lay_src, "w", encoding="utf-8") as f:
        f.write('extern struct Node { next: ptr, tag: u8 }\n'
                'extern fn malloc(n: i64) -> ptr;\n'
                'fn main() -> i64 {\n'
                '  return 0\n'
                '}\n')
    lay_py = os.path.join(TMP, "layout_target.py.c")
    emitted = subprocess.run([sys.executable, CLI, "emit-c", "--unsafe-ffi", lay_src,
                              "--target", "i686-linux-gnu"],
                             capture_output=True, text=True, timeout=180)
    assert emitted.returncode == 0, "emit-c --target: %r" % emitted.stderr[-400:]
    with open(lay_py, "w", encoding="utf-8") as f:
        f.write(emitted.stdout)
    assert 'sizeof(bf_Node) == 8' in emitted.stdout and 'bf_tag) == 4' in emitted.stdout, \
        "a 32-bit target lays `ptr` out at 4 bytes:\n%s" % emitted.stdout[-600:]
    n += 1
    print("ffihard ok: an extern struct is laid out by the target's pointer width")

    def asserts_of(text):
        return [ln for ln in text.splitlines()
                if ln.startswith("typedef struct") or "_Static_assert" in ln]

    m32 = shutil.which("cc") or shutil.which("gcc")
    mini = "#include <stddef.h>\n%s\n" % "\n".join(asserts_of(emitted.stdout))
    ok, out = build_mini(m32, mini, "-m32")
    if ok is None:
        print("ffihard skip: this cc has no 32-bit target; the numbers are untested here")
    else:
        assert ok, "a 32-bit compiler must accept the 32-bit layout: %r" % out
        # And the numbers this used to emit must still be refused, or the
        # check above proves nothing.
        host_mini = mini.replace("sizeof(bf_Node) == 8", "sizeof(bf_Node) == 16") \
                        .replace("bf_tag) == 4", "bf_tag) == 8")
        bad_ok, _bad_out = build_mini(m32, host_mini, "-m32")
        assert bad_ok is False, "the host layout must fail a 32-bit compiler"
        n += 1
        print("ffihard ok: a 32-bit compiler accepts the 32-bit layout, and only that")

    print("FFI HARDENING ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()
