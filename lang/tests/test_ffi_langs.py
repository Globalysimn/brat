"""One C header, four languages (P11) and a warning-clean C boundary (P10).

The bug this file exists to prevent: a second generator. `brat bindgen` used to
be a regex scanner separate from `ffi.emit_bindings`, and the two had already
drifted -- one bound `const char *` as `str`, the other as `i32`. Writing Python,
Go and Rust from the *same* parsed C view is how that cannot happen again: the
skipped set, the widths and the record order come from one place.

What is proved here, and how:

* Python -- generated, loaded with ctypes and **called against a real .so**, and
  its answers are compared with what BRAT itself prints for the same calls. Two
  languages, one ABI, same numbers. This is the strongest check available here
  because Python is the only toolchain besides C that is installed.
* Go and Rust -- the text is asserted, including that an unrepresentable type is
  skipped with the same reason in every language, and if `rustc`/`go` happen to
  exist the file is compiled; otherwise nothing pretends it was.
* `-Wall -Wextra -Werror` -- the C a program emits (with an `export fn`, which is
  the other half of the boundary) must be warning-clean. `crt.h` is included with
  `-isystem`, so its own `-Wswitch` warning is not part of this check.
"""
import _cxx
import _scratch

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TMP = _scratch.mkdtemp(prefix="brat-langs-")

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def write(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def cli(*args, timeout=600):
    if args and args[0] in ("build", "run", "emit-c", "dist") \
            and "--interp" not in args:
        args = [args[0], "--unsafe-ffi"] + list(args[1:])
    elif args and args[0] == "run" and "--interp" in args and "--unsafe-ffi" not in args:
        i = args.index("--interp") + 1
        args = list(args[:i]) + ["--unsafe-ffi"] + list(args[i:])
    return subprocess.run(_cxx.cli_args(*(list(args))), capture_output=True,
                          text=True, timeout=timeout)


HEADER = """
#include <stdint.h>
int8_t neg(void);
int8_t take(int8_t x);
int16_t add16(int16_t a, int16_t b);
uint16_t u16max(void);
_Bool is_true(_Bool x);
double scale(double x, double k);
const char *greet(void);
void *identity(void *p);
struct Narrow { int8_t a; int16_t b; int32_t c; };
struct Narrow shift(struct Narrow n);
/* no BRAT spelling: 80-bit extended, 16 bytes in a box */
long double no_spelling(long double x);
"""

LIB_C = r"""
#include <stdint.h>
struct Narrow { int8_t a; int16_t b; int32_t c; };
int8_t neg(void) { return (int8_t)-1; }
int8_t take(int8_t x) { return (int8_t)(x - 1); }
int16_t add16(int16_t a, int16_t b) { return (int16_t)(a + b); }
uint16_t u16max(void) { return (uint16_t)65535; }
_Bool is_true(_Bool x) { return x; }
double scale(double x, double k) { return x * k; }
const char *greet(void) { return "hello"; }
void *identity(void *p) { return p; }
struct Narrow shift(struct Narrow n) {
    struct Narrow r = { (int8_t)(n.a + 1), (int16_t)(n.b + 1), n.c + 1 };
    return r;
}
long double no_spelling(long double x) { return x; }
"""

BRAT_USE = """
extern "LIB" fn neg() -> i8
extern "LIB" fn take(x: i8) -> i8
extern "LIB" fn add16(a: i16, b: i16) -> i16
extern "LIB" fn u16max() -> u16
extern "LIB" fn is_true(x: bool) -> bool
extern "LIB" fn scale(x: f64, k: f64) -> f64
extern "LIB" fn greet() -> str
extern struct Narrow { a: i8, b: i16, c: i32 }
extern "LIB" fn shift(n: Narrow) -> Narrow

fn main() -> i64 {
  print(neg())
  print(take(-4))
  print(add16(-1000, 234))
  print(u16max())
  print(is_true(true))
  print(scale(1.5, 4.0))
  print(greet())
  let m = shift(Narrow { a: 1, b: 2, c: 3 })
  print(m.a)
  print(m.b)
  print(m.c)
  return 0
}
"""

WANT = "-1\n-5\n-766\n65535\ntrue\n6.0\nhello\n2\n3\n4\n"


def main():
    hdr = write("langs.h", HEADER)
    lib_c = write("langs.c", LIB_C)
    lib = os.path.join(TMP, "liblangs.so")
    r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", lib_c, "-o", lib],
                       capture_output=True, text=True, timeout=300)
    check(r.returncode == 0, "cc: %r" % r.stderr)

    prog = write("use.brat", BRAT_USE.replace('"LIB"', '"%s"' % lib))
    interp = cli("run", "--interp", prog)
    check(interp.returncode == 0, "brat interp: %r" % (interp.stdout + interp.stderr))
    check(interp.stdout == WANT, "brat interp: %r" % interp.stdout)
    exe = os.path.join(TMP, "use.exe")
    b = cli("build", prog, "-o", exe)
    check(b.returncode == 0, "brat build: %r" % (b.stdout + b.stderr))
    native = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    check(native.stdout == interp.stdout, "native %r vs interp %r"
          % (native.stdout, interp.stdout))
    print("langs ok: brat is the reference (%r)" % interp.stdout.replace("\n", " "))

    # Python: generated, loaded, called, compared
    out = os.path.join(TMP, "langs_bind.py")
    g = cli("bindgen", "--lang", "python", hdr, "-o", out)
    check(g.returncode == 0, "bindgen python: %r" % (g.stdout + g.stderr))
    call = write("call.py", """
import sys
sys.path.insert(0, %r)
import langs_bind
lib = langs_bind.load(%r)
print(lib.neg())
print(lib.take(-4))
print(lib.add16(-1000, 234))
print(lib.u16max())
print(str(lib.is_true(True)).lower())
print(float(lib.scale(1.5, 4.0)))
print(lib.greet().decode())
m = lib.shift(langs_bind.Narrow(a=1, b=2, c=3))
print(m.a)
print(m.b)
print(m.c)
""" % (TMP, lib))
    py = subprocess.run([sys.executable, call], capture_output=True, text=True, timeout=300)
    check(py.returncode == 0, "python bindings: %r" % (py.stdout + py.stderr))
    check(py.stdout == WANT,
          "python and BRAT must agree on the same library: %r vs %r" % (py.stdout, WANT))
    print("langs ok: python (ctypes) and BRAT agree value for value")

    # Rust and Go: same C view, asserted as text
    rs = os.path.join(TMP, "langs.rs")
    gr = cli("bindgen", "--lang", "rust", hdr, "-o", rs)
    check(gr.returncode == 0, "bindgen rust: %r" % (gr.stdout + gr.stderr))
    rtext = open(rs, encoding="utf-8").read()
    check("pub fn take(a0: i8) -> i8;" in rtext, "rust keeps the width: %r" % rtext)
    check("pub fn is_true(a0: bool) -> bool;" in rtext,
          "rust keeps C _Bool as bool: %r" % rtext)
    check("pub struct Narrow {" in rtext and "pub a: i8," in rtext, "rust record: %r" % rtext)
    check("no_spelling" not in rtext.split("extern \"C\" {")[1],
          "an unrepresentable type must not be declared in rust: %r" % rtext)
    check("// skipped: no_spelling" in rtext,
          "and it must be named in the notes, not silently dropped: %r" % rtext)

    go = os.path.join(TMP, "langs.go")
    gg = cli("bindgen", "--lang", "go", hdr, "-o", go)
    check(gg.returncode == 0, "bindgen go: %r" % (gg.stdout + gg.stderr))
    gtext = open(go, encoding="utf-8").read()
    check('import "C"' in gtext and '#include "' in gtext, "go is a cgo file: %r" % gtext)
    check("func takeC(a0 C.int8_t) C.int8_t" in gtext, "go keeps the width: %r" % gtext)
    check("func is_trueC(a0 C.bool) C.bool" in gtext,
          "go keeps C _Bool as bool: %r" % gtext)
    check("func greetC() *C.char" in gtext, "go spells char pointers: %r" % gtext)
    check("func identityC(a0 unsafe.Pointer) unsafe.Pointer" in gtext,
          "go spells void pointers: %r" % gtext)
    check("func shiftC(a0 Narrow) Narrow" in gtext,
          "go uses the declared struct alias: %r" % gtext)
    check("// skipped: no_spelling" in gtext, "go notes the same skip: %r" % gtext)

    # `no_spelling` is skipped by every language for the same reason: the reason
    # comes from one C view, so the three notes must agree.
    for lang, path in (("rust", rs), ("go", go)):
        text = open(path, encoding="utf-8").read()
        check("has no %s spelling" % lang in text,
              "%s must say why it skipped: %r" % (lang, text[:400]))
    print("langs ok: rust and go carry the same widths and the same skips")

    # If a compiler for those two happens to exist, use it; if not, say so
    # rather than implying the file was compiled.
    for tool, path, cmd in (("rustc", rs, ["rustc", "--crate-type", "lib", "-o",
                                           os.path.join(TMP, "langs.rlib")]),
                            ("go", go, ["go", "build", "-o", os.path.join(TMP, "langs.a")])):
        if shutil.which(tool):
            if tool == "go":
                write("go.mod", "module example.com/brat-bindings-test\n\ngo 1.18\n")
            r = subprocess.run(cmd + ([path] if tool == "rustc" else []),
                               capture_output=True, text=True, timeout=600, cwd=TMP)
            check(r.returncode == 0, "%s rejected the binding: %r" % (tool, r.stderr))
            print("langs ok: %s accepted the generated file" % tool)
        else:
            print("langs note: %s is not installed, the file is only shape-checked" % tool)

    # P10: the C boundary is warning-clean
    # `export fn` is the other direction: BRAT as the library. Its emitted C must
    # survive -Werror, or a C consumer cannot compile what BRAT gave them.
    EXPORT = """
export fn add(a: i32, b: i32) -> i32 {
  return a + b
}
fn main() -> i64 {
  print(add(2, 40))
  return 0
}
"""
    epath = write("export.brat", EXPORT)
    e = cli("emit-c", epath)
    check(e.returncode == 0, "emit-c: %r" % (e.stdout + e.stderr))
    cfile = write("export.c", e.stdout)
    cc = subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2",
                         "-isystem", os.path.join(ROOT, "lang", "engine"),
                         "-c", cfile, "-o", os.path.join(TMP, "export.o")],
                        capture_output=True, text=True, timeout=600)
    check(cc.returncode == 0, "emitted C is not warning-clean: %r" % cc.stderr[:800])
    run = cli("run", "--interp", epath)
    check(run.stdout == "42\n", "export fn program: %r" % run.stdout)
    print("langs ok: emitted C survives -Wall -Wextra -Werror")

    print("FFI LANGS ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()
