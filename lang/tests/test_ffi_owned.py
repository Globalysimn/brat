"""Owned FFI pointers.

`extern fn f(move p: ptr)` takes ownership: the checker moves `p` at the call,
so a second free or any later use is `use of moved`. `-> move ptr` marks a
returned pointer as owned: the binding must be freed or returned, or the
checker reports a leak. Expected results are literal.
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = _scratch.mkdtemp(prefix="brat-ffiown-")

DECLS = (
    "extern fn malloc(size: i64) -> move ptr;\n"
    "extern fn free(move p: ptr);\n"
)

# name -> (source, expected check exit code, message fragment or None)
CASES = {
    "free_once_ok": (
        DECLS +
        "fn main() -> i64 {\n"
        "    let p = malloc(8);\n"
        "    ptr_write_i64(p, 42);\n"
        "    print(ptr_read_i64(p));\n"
        "    free(p);\n"
        "    return 0;\n"
        "}\n", 0, None),
    "double_free": (
        DECLS +
        "fn main() -> i64 {\n"
        "    let p = malloc(8);\n"
        "    free(p);\n"
        "    free(p);\n"
        "    return 0;\n"
        "}\n", 1, "use of moved `p`"),
    "use_after_free": (
        DECLS +
        "fn main() -> i64 {\n"
        "    let p = malloc(8);\n"
        "    free(p);\n"
        "    print(ptr_read_i64(p));\n"
        "    return 0;\n"
        "}\n", 1, "use of moved `p`"),
    "leak": (
        DECLS +
        "fn main() -> i64 {\n"
        "    let p = malloc(8);\n"
        "    print(ptr_read_i64(p));\n"
        "    return 0;\n"
        "}\n", 1, "leaked owned pointer `p`"),
    "returned_to_caller_ok": (
        DECLS +
        "fn make() -> ptr {\n"
        "    let p = malloc(8);\n"
        "    return p;\n"
        "}\n"
        "fn main() -> i64 {\n"
        "    let q = make();\n"
        "    free(q);\n"
        "    return 0;\n"
        "}\n", 0, None),
}


def write(name, src):
    p = os.path.join(TMP, name + ".brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)
    return p


def check(path):
    p = subprocess.run(_cxx.cli_args("check", path),
                       capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout + p.stderr


def main():
    n = 0
    for name, (src, want_rc, frag) in CASES.items():
        rc, out = check(write(name, src))
        assert rc == want_rc, "%s: check rc=%d want %d\n%s" % (name, rc, want_rc, out[-600:])
        if frag is not None:
            assert frag in out, "%s: want %r in\n%s" % (name, frag, out[-600:])
        n += 1
        print("ffiown ok: %s" % name)

    # Correct program must also build and run natively, printing 42.
    path = write("free_once_run", CASES["free_once_ok"][0])
    exe = os.path.join(TMP, "free_once_run.exe")
    b = subprocess.run(_cxx.cli_args("build", "--unsafe-ffi", path, "-o", exe),
                       capture_output=True, text=True, timeout=300)
    assert b.returncode == 0, "build failed:\n" + (b.stdout + b.stderr)[-600:]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and r.stdout == "42\n", "native out=%r rc=%d" % (r.stdout, r.returncode)
    n += 1
    print("ffiown ok: native_free_once")

    # The header does not state who owns a pointer -- C has no syntax for it --
    # so the generator must not guess: a wrong guess (`getenv` marked owned, then
    # freed) is worse than none. What it can do is *prove* the ownership the
    # program declares: the binding file upgrades a plain `ptr` to `move`, and
    # two independent checks then hold at once.
    hdr = os.path.join(TMP, "own.h")
    with open(hdr, "w", encoding="utf-8") as f:
        f.write("char *dup_str(const char *s);\n"
                "void release(void *p);\n")
    gen = subprocess.run(_cxx.cli_args("bindgen", hdr), capture_output=True,
                         text=True, timeout=300)
    assert gen.returncode == 0, "bindgen failed: %r" % (gen.stdout + gen.stderr)
    assert "extern fn dup_str(s: str) -> ptr[u8];" in gen.stdout, \
        "an unannotated header yields a plain ptr, not a guess: %r" % gen.stdout
    assert "extern fn release(p: ptr) -> none;" in gen.stdout, \
        "the release side is a plain ptr too: %r" % gen.stdout
    n += 1
    print("ffiown ok: the generator does not guess ownership")

    LIB = os.path.join(TMP, "libown.so")
    C = os.path.join(TMP, "own.c")
    with open(C, "w", encoding="utf-8") as f:
        f.write("#include <stdlib.h>\n#include <string.h>\n"
                "char *dup_str(const char *s) { return strdup(s); }\n"
                "void release(void *p) { free(p); }\n")
    cc = subprocess.run(["cc", "-shared", "-fPIC", "-O2", C, "-o", LIB],
                        capture_output=True, text=True, timeout=300)
    assert cc.returncode == 0, "cc failed: %r" % cc.stderr
    n += 1

    # Same header, same types: only the modes are added, and `ffi --header` still
    # proves the binding against C -- ownership is a mode, not a different type.
    OWNED = ('extern "%s" fn dup_str(s: str) -> move ptr[u8]\n'
             'extern "%s" fn release(move p: ptr)\n' % (LIB, LIB))
    OK = OWNED + ('fn main() -> i64 {\n'
                  '  let p = dup_str("owned")\n'
                  '  print(str_from_ptr(p, 5))\n'
                  '  release(p)\n'
                  '  return 0\n'
                  '}\n')
    path = write("owned_roundtrip", OK)
    gate = subprocess.run(_cxx.cli_args("ffi", "--header", hdr, path),
                          capture_output=True, text=True, timeout=300)
    assert gate.returncode == 0, "the owned binding must still be proven: %r" % (gate.stdout + gate.stderr)
    n += 1
    print("ffiown ok: `move` is a mode, the header proof still holds")

    ri = subprocess.run(_cxx.cli_args("run", "--interp", "--unsafe-ffi", path), capture_output=True,
                        text=True, timeout=120)
    exe = os.path.join(TMP, "owned_roundtrip.exe")
    b = subprocess.run(_cxx.cli_args("build", "--unsafe-ffi", path, "-o", exe), capture_output=True,
                       text=True, timeout=300)
    assert b.returncode == 0, "build failed:\n" + (b.stdout + b.stderr)[-600:]
    rn = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    assert ri.returncode == 0 and ri.stdout == "owned\n", "interp out=%r" % ri.stdout
    assert (rn.returncode, rn.stdout) == (ri.returncode, ri.stdout), \
        "native %r vs interp %r" % ((rn.returncode, rn.stdout), (ri.returncode, ri.stdout))
    n += 1
    print("ffiown ok: allocate, use, release -- interpreter and native agree")

    # The leak and the double free are refused *before* the binary exists, and the
    # sentence is the same in both checkers.
    for name, body, want in (
        ("owned_leak", '  let p = dup_str("x")\n  print(str_from_ptr(p, 1))\n',
         "leaked owned pointer `p`"),
        ("owned_double", '  let p = dup_str("x")\n  release(p)\n  release(p)\n',
         "use of moved `p`"),
    ):
        src = OWNED + "fn main() -> i64 {\n" + body + "  return 0\n}\n"
        p = write(name, src)
        nat = subprocess.run([_cxx.build(), "check", p], capture_output=True, text=True, timeout=300)
        assert nat.returncode != 0 and want in nat.stdout, "%s native: %r" % (name, nat.stdout)
        n += 1
    print("ffiown ok: a leak and a double free are refused by the checker")

    print("FFI OWNED ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()
