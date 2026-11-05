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
sys.path.insert(0, HERE)
import _cxx
BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-ffi-verify")

LIB_C = """
#include <stdint.h>
typedef struct { float x; float y; } Vec2;
typedef struct { int value; } Pointed;
float scale(float x, float k) { return x * k; }
double widen(float f) { return (double)f; }
void fill(float *p, int64_t n) { for (int64_t i = 0; i < n; i++) p[i] = (float)i; }
float dot2(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
int takes_ptr(const char *s) { return s ? 1 : 0; }
int64_t use_mut_cb(int64_t (*cb)(int32_t *)) { (void)cb; return 0; }
int64_t use_const_cb(int64_t (*cb)(const int32_t *)) { (void)cb; return 0; }
Pointed *get_pointed(void) { static Pointed p; return &p; }
"""

LIB_H = """
#include <stdint.h>
typedef struct { float x; float y; } Vec2;
typedef struct { int value; } Pointed;
float scale(float x, float k);
double widen(float f);
void fill(float *p, int64_t n);
float dot2(Vec2 a, Vec2 b);
int takes_ptr(const char *s);
int64_t use_mut_cb(int64_t (*cb)(int32_t *));
int64_t use_const_cb(int64_t (*cb)(const int32_t *));
Pointed *get_pointed(void);
"""

GOOD = """extern "LIB" fn scale(x: f32, k: f32) -> f32;
extern "LIB" fn widen(f: f32) -> f64;
extern "LIB" fn fill(p: ptr[f32], n: i64);
extern "LIB" fn dot2(a: Vec2, b: Vec2) -> f32;
extern "LIB" fn takes_ptr(s: str) -> i32;
extern struct Vec2 { x: f32, y: f32 }

fn main() -> i64 {
  return 0
}
"""


def run(args):
    p = subprocess.run([BIN] + args, capture_output=True, text=True, timeout=300)
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

    no_scale_header = os.path.join(TMP, "no_scale.h")
    with open(no_scale_header, "w", encoding="utf-8") as f:
        f.write(LIB_H.replace("float scale(float x, float k);", ""))
    unproven = program(
        "unproven_scale",
        'extern "LIB" fn scale(x: f32, k: f32) -> f32\n'
        'fn main() -> i64 { return 0 }\n')
    rc, out = run(["ffi", unproven, "--header", no_scale_header])
    check(rc == 1 and "not declared" in out,
          "a library-only symbol was treated as header-proven:\n%s" % out)
    unproven_exe = os.path.join(TMP, "unproven_scale.exe")
    rc, out = run(["build", unproven, "--header", no_scale_header,
                   "-o", unproven_exe])
    check(rc != 0 and not os.path.exists(unproven_exe),
          "build accepted an extern with no prototype in its proof header:\n%s" % out)

    abi_header = os.path.join(TMP, "abi.h")
    with open(abi_header, "w", encoding="utf-8") as f:
        f.write("__attribute__((ms_abi)) int audit_msabi(int value);\n")
    abi_program = program(
        "abi_attribute",
        'extern fn audit_msabi(value: i32) -> i32\n'
        'fn main() -> i64 { return 0 }\n')
    rc, out = run(["ffi", abi_program, "--header", abi_header])
    check(rc == 1 and "calling convention" in out,
          "an ABI-changing header attribute was erased by the parser:\n%s" % out)

    raw_pointer = program("raw_pointer", GOOD.replace("p: ptr[f32]", "p: ptr"))
    rc, out = run(["ffi", raw_pointer, "--header", hdr])
    check(rc == 1 and "incompatible" in out,
            "an erased void pointer was accepted for float *:\n%s" % out)

    wrong_pointee = program("wrong_pointee", GOOD.replace("p: ptr[f32]", "p: ptr[i32]"))
    rc, out = run(["ffi", wrong_pointee, "--header", hdr])
    check(rc == 1 and "parameter 1" in out,
            "an int pointer was accepted for float *:\n%s" % out)

    cb_good = program("cb_good", 'extern "LIB" fn use_mut_cb(cb: fn(ptr[i32]) -> i64) -> i64\n'
                      'fn main() -> i64 { return 0 }\n')
    rc, out = run(["ffi", cb_good, "--header", hdr])
    check(rc == 0, "matching typed callback pointer was refused:\n%s" % out)

    cb_wrong_pointee = program(
        "cb_wrong_pointee",
        'extern "LIB" fn use_mut_cb(cb: fn(ptr[u8]) -> i64) -> i64\n'
        'fn main() -> i64 { return 0 }\n')
    rc, out = run(["ffi", cb_wrong_pointee, "--header", hdr])
    check(rc == 1 and "argument 1" in out,
          "callback pointee mismatch was accepted:\n%s" % out)

    cb_const = program(
        "cb_const",
        'extern "LIB" fn use_const_cb(cb: fn(ptr[i32]) -> i64) -> i64\n'
        'fn main() -> i64 { return 0 }\n')
    rc, out = run(["ffi", cb_const, "--header", hdr])
    check(rc == 1 and "argument 1" in out,
          "mutable callback pointer was accepted for const C pointee:\n%s" % out)

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

    pointer_header = os.path.join(TMP, "pointer_struct.h")
    with open(pointer_header, "w", encoding="utf-8") as f:
        f.write("typedef struct { float *p; } PointerBox;\n")
    pointer_struct = program(
        "pointer_struct",
        "extern struct PointerBox { p: ptr[f32] }\n"
        "fn main() -> i64 { return 0 }\n")
    rc, out = run(["ffi", pointer_struct, "--header", pointer_header])
    check(rc == 0, "a matching typed struct pointer was refused:\n%s" % out)
    pointer_exe = os.path.join(TMP, "pointer_struct.exe")
    rc, out = run(["build", pointer_struct, "--header", pointer_header,
                   "-o", pointer_exe])
    check(rc == 0, "the compiler rejected a matching struct pointer layout:\n%s" % out)
    wrong_pointer_struct = program(
        "wrong_pointer_struct",
        "extern struct PointerBox { p: ptr[i32] }\n"
        "fn main() -> i64 { return 0 }\n")
    rc, out = run(["ffi", wrong_pointer_struct, "--header", pointer_header])
    check(rc == 1 and "ptr[i32]" in out,
          "a struct pointer to the wrong scalar type was accepted:\n%s" % out)

    raw_record = program(
        "raw_record",
        'extern "LIB" struct Pointed { value: i32 }\n'
        'extern "LIB" fn get_pointed() -> ptr\n'
        'fn main() -> i64 { return 0 }\n')
    rc, out = run(["ffi", raw_record, "--header", hdr])
    check(rc == 1 and "ptr[Pointed]" in out,
          "an erased pointer was accepted for a known record return:\n%s" % out)

    array_header = os.path.join(TMP, "array_struct.h")
    with open(array_header, "w", encoding="utf-8") as f:
        f.write("typedef struct { int values[4]; } ArrayBox;\n")
    array_struct = program(
        "array_struct",
        'extern struct ArrayBox { values: ptr[i32] }\n'
        'fn main() -> i64 { return 0 }\n')
    rc, out = run(["ffi", array_struct, "--header", array_header])
    check(rc == 1 and "inline array fields" in out,
          "an inline C array was treated as a pointer field:\n%s" % out)
    rc, out = run(["bindgen", array_header])
    check(rc == 0 and "extern struct ArrayBox" not in out
          and "inline array fields" in out,
          "bindgen invented a pointer layout for an inline array:\n%s" % out)

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
    arity = program("arity", GOOD.replace("fn fill(p: ptr[f32], n: i64);",
                                          "fn fill(p: ptr, n: i64, extra: i64);"))
    rc, out = run(["ffi", arity, "--header", hdr])
    check(rc == 1 and "takes 3 argument(s)" in out, "arity mismatch:\n%s" % out)

    variadic = program("variadic", GOOD.replace("fn fill(p: ptr[f32], n: i64);",
                                                "fn fill(p: str, ..ptr);"))
    rc, out = run(["ffi", variadic, "--header", hdr])
    check(rc == 1, "a variadic tail the header does not have was accepted:\n%s" % out)

    result = program("result", GOOD.replace("extern \"LIB\" fn fill(p: ptr[f32], n: i64);",
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
