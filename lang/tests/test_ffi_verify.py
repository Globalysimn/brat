"""`brat ffi`: the bindings are proven against the header, not trusted.

Every other FFI trusts the prototype you wrote. bindgen, cgo, ctypes and
`@cImport` all read it and call through it; if it is wrong the program runs and
returns garbage, or corrupts memory somewhere else, and nothing says a word. The
header already carries the truth.

Three stages, each answering a question the others cannot:

* **declarations** -- widths, arity, field order, variadic tail, compared as
  text. This catches the mistake that costs a day: `f64` where C says `float`,
  both floats, only the ABI knows the difference.
* **layout** -- struct size and member offsets, answered by the C compiler
  through `_Static_assert`, because padding depends on the target.
* **symbols** -- `dlopen` says whether the library really exports the name.

A negative case for each is checked here, so a stage that stops working is a
failure and not a quiet pass.
"""
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CLI = os.path.join(ROOT, "lang", "engine", "cli.py")
TMP = _scratch.mkdtemp(prefix="brat-ffi-verify")

LIB_C = """
typedef struct { float x; float y; } Vec2;
float scale(float x, float k) { return x * k; }
double widen(float f) { return (double)f; }
void fill(float *p, long long n) { for (long long i = 0; i < n; i++) p[i] = (float)i; }
float dot2(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
int takes_ptr(const char *s) { return s ? 1 : 0; }
"""

LIB_H = """
typedef struct { float x; float y; } Vec2;
float scale(float x, float k);
double widen(float f);
void fill(float *p, long long n);
float dot2(Vec2 a, Vec2 b);
int takes_ptr(const char *s);
"""

GOOD = """extern "LIB" fn scale(x: f32, k: f32) -> f32;
extern "LIB" fn widen(f: f32) -> f64;
extern "LIB" fn fill(p: ptr, n: i64);
extern "LIB" fn dot2(a: Vec2, b: Vec2) -> f32;
extern "LIB" fn takes_ptr(s: str) -> i32;
extern struct Vec2 { x: f32, y: f32 }

fn main() -> i64 {
  return 0
}
"""


def run(args):
    p = subprocess.run([sys.executable, CLI] + args, capture_output=True, text=True, timeout=300)
    return p.returncode, p.stdout + p.stderr


def main():
    n = 0

    def check(cond, msg):
        nonlocal n
        assert cond, msg
        n += 1

    libc = os.path.join(TMP, "libprobe.c")
    lib = os.path.join(TMP, "libprobe.so")
    hdr = os.path.join(TMP, "probe.h")
    with open(libc, "w", encoding="utf-8") as f:
        f.write(LIB_C)
    with open(hdr, "w", encoding="utf-8") as f:
        f.write(LIB_H)
    r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", libc, "-o", lib], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr

    def program(name, text):
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text.replace("LIB", lib))
        return path

    # 1. Correct bindings: proven, and the command says so.
    good = program("good", GOOD)
    rc, out = run(["ffi", good, "--header", hdr])
    check(rc == 0, "correct bindings were refused:\n%s" % out)
    check("6 binding(s) proven" in out, "the count is wrong:\n%s" % out)

    # 2. A width mistake: `f64` for a C `float`. Both are floats; only the ABI
    #    knows the difference, and this is the case the whole command exists for.
    bad_width = program("bad_width", GOOD.replace("fn scale(x: f32, k: f32) -> f32",
                                                  "fn scale(x: f64, k: f64) -> f64"))
    rc, out = run(["ffi", bad_width, "--header", hdr])
    check(rc == 1, "a float/double mix-up was accepted:\n%s" % out)
    check("8 bytes" in out and "4 bytes" in out, "the error does not name both widths:\n%s" % out)

    # 3. A wrong struct layout: same fields, wrong order.
    bad_layout = program("bad_layout", GOOD.replace("extern struct Vec2 { x: f32, y: f32 }",
                                                    "extern struct Vec2 { y: f32, x: f32 }"))
    rc, out = run(["ffi", bad_layout, "--header", hdr])
    check(rc == 1, "a reordered struct was accepted:\n%s" % out)
    check("field 1" in out, "the error does not point at the field:\n%s" % out)

    # 4. An extra struct member changes the layout, which only the compiler can
    #    see: the static assertion has to be the one that fires.
    sized = program("sized", GOOD.replace("extern struct Vec2 { x: f32, y: f32 }",
                                          "extern struct Vec2 { x: f32, y: f32, z: i32 }"))
    rc, out = run(["ffi", sized, "--header", hdr])
    check(rc == 1, "a struct with an extra member was accepted:\n%s" % out)

    # 5. A name the library does not export: dlopen knows, the text does not.
    ghost = program("ghost", GOOD + 'extern "LIB" fn no_such_symbol(x: i64) -> i64;\n'
                    .replace("extern struct Vec2 { x: f32, y: f32 }\n", ""))
    rc, out = run(["ffi", ghost, "--header", hdr])
    check(rc == 1, "a missing symbol was accepted:\n%s" % out)
    check("exports no `no_such_symbol`" in out, "the missing symbol is not named:\n%s" % out)

    # 6. Arity, a variadic tail that is not there, and a void result claimed as a
    #    value: three different sentences, all from the declaration stage.
    arity = program("arity", GOOD.replace("fn fill(p: ptr, n: i64);",
                                          "fn fill(p: ptr, n: i64, extra: i64);"))
    rc, out = run(["ffi", arity, "--header", hdr])
    check(rc == 1 and "takes 3 argument(s)" in out, "arity mismatch:\n%s" % out)

    variadic = program("variadic", GOOD.replace("fn fill(p: ptr, n: i64);",
                                                "fn fill(p: str, ..ptr);"))
    rc, out = run(["ffi", variadic, "--header", hdr])
    check(rc == 1, "a variadic tail the header does not have was accepted:\n%s" % out)

    result = program("result", GOOD.replace("extern \"LIB\" fn fill(p: ptr, n: i64);",
                                            "extern \"LIB\" fn fill(p: ptr, n: i64) -> i64;"))
    rc, out = run(["ffi", result, "--header", hdr])
    check(rc == 1 and "returns `void`" in out, "a void function returning a value:\n%s" % out)

    # 7. JSON is what a CI job reads, so it has to carry the same verdict.
    import json
    rc, out = run(["ffi", good, "--header", hdr, "--json"])
    doc = json.loads(out)
    check(rc == 0 and doc["errors"] == 0 and doc["proven"] == 6,
          "json verdict disagrees: %r" % doc)
    check(doc["compiler"]["checked"] is True, "the compiler stage was skipped: %r" % doc)
    check(str(lib) in doc["libraries"], "the library is not reported: %r" % doc["libraries"])

    # 8. Without a header the command still answers the question it can answer.
    rc, out = run(["ffi", good])
    check("checked against the library only" in out, "headerless run says nothing:\n%s" % out)

    print("FFI VERIFY ALL GREEN (%d asserts: declarations, compiler layout, symbols)" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
