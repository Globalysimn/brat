"""`brat ffi` gives one verdict, whichever front end runs it.

The Python front end and the C++ one each carry their own copy of the rules
that decide whether a BRAT declaration matches a C header. The guide promises
the same verdict from both, so this test feeds them the same cross product of
BRAT and C types -- as a parameter, as a result, as a callback argument and as a
callback result -- and compares the set of declarations each one rejects.

Messages are free to differ; only accept versus reject must not. A handful of
verdicts the guide states in words are pinned on top, because two front ends
can agree and both be wrong (the callback `void *user_data` slot and a record
passed through a callback were refused by one of them for a long time).
"""
import _cxx
import _scratch

import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-ffi-agree-")

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


BRAT_SCALARS = ["i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "f32", "f64"]
BRAT_POINTERS = ["str", "ptr", "ptr[i64]", "ptr[f64]", "ptr[u8]", "ptr[i32]", "ptr[str]", "P", "ptr[P]"]
C_SCALARS = ["char", "signed char", "unsigned char", "short", "unsigned short", "int", "unsigned int",
             "long", "unsigned long", "long long", "unsigned long long", "size_t", "int8_t", "uint8_t",
             "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t", "float", "double",
             "_Bool", "intptr_t", "uintptr_t"]
C_NAMED = ["handle_t", "const handle_t", "byte_t", "P_t", "enum E", "struct P", "const struct P"]
C_POINTERS = ["const char *", "char *", "const unsigned char *", "unsigned char *", "void *",
              "const void *", "void **", "long *", "const long *", "long long *", "double *",
              "const double *", "int *", "const int *", "int64_t *", "char **", "const char **",
              "struct P *", "const struct P *", "P_t *", "byte_t *", "handle_t *"]

BRAT_ALL = BRAT_SCALARS + BRAT_POINTERS
BRAT_SLOT = BRAT_POINTERS + ["i32", "i64", "f32"]
C_ALL = C_SCALARS + C_NAMED + C_POINTERS
C_SLOT = C_NAMED + C_POINTERS + ["int", "long", "float", "double"]

# Shape -> BRAT declaration, C declaration, BRAT types tried, C types tried. A header usually leaves callback
# parameters unnamed, so `arg-unnamed` and the callback shapes exercise the part of the parser that must tell a
# parameter name from a type name (`struct P`, `const handle_t`).
SHAPES = {
    "arg": ("extern \"libz.so\" fn {n}(a: {b}) -> none", "void {n}({c} a);", BRAT_ALL, C_ALL),
    "arg-unnamed": ("extern \"libz.so\" fn {n}(a: {b}) -> none", "void {n}({c});", BRAT_ALL, C_ALL),
    "ret": ("extern \"libz.so\" fn {n}() -> {b}", "{c} {n}(void);", BRAT_ALL, C_ALL),
    "cb-arg": ("extern \"libz.so\" fn {n}(cb: fn({b}) -> none) -> none",
               "typedef void (*{n}_t)({c});\nvoid {n}({n}_t cb);", BRAT_SLOT, C_SLOT),
    "cb-ret": ("extern \"libz.so\" fn {n}(cb: fn() -> {b}) -> none",
               "typedef {c} (*{n}_t)(void);\nvoid {n}({n}_t cb);", BRAT_SLOT, C_SLOT),
}

STRUCT_BRAT = "extern struct P { x: i64 }"
STRUCT_C = ("struct P { int64_t x; };\ntypedef struct P P_t;\ntypedef void *handle_t;\ntypedef unsigned char byte_t;\n"
            "enum E { E_A, E_B };")
MAIN = "fn main() -> i64 {\n  return 0\n}\n"

# BRAT declaration, C declaration, must it be accepted. Each is a sentence of ffi.md.
PINNED = [
    ("user_data", True,
     "extern \"libz.so\" fn reg(cb: fn(i64, ptr), ud: ptr) -> none",
     "#include <stdint.h>\ntypedef void (*cb_t)(int64_t, void *);\nvoid reg(cb_t cb, void *ud);"),
    ("record_result", True,
     "extern struct Pt { x: i32, y: i32 }\nextern \"libz.so\" fn apply_pt(cb: fn(i64) -> Pt, n: i64) -> Pt",
     "#include <stdint.h>\nstruct Pt { int x; int y; };\ntypedef struct Pt (*pt_cb)(int64_t);\n"
     "struct Pt apply_pt(pt_cb cb, int64_t n);"),
    ("str_is_not_bytes", False,
     "extern \"libz.so\" fn f(s: str) -> none",
     "void f(const unsigned char *s);"),
    ("str_is_const_void", True,
     "extern \"libz.so\" fn f(s: str) -> none",
     "void f(const void *s);"),
    ("writable_is_not_const_result", False,
     "extern \"libz.so\" fn reg(cb: fn() -> ptr[i64]) -> i64",
     "typedef const long *(*get_cb)(void);\nlong reg(get_cb cb);"),
    ("plain_ptr_is_not_const_char", False,
     "extern \"libz.so\" fn f(p: ptr) -> none",
     "void f(const char *p);"),
    ("typed_ptr_needs_a_known_pointee", False,
     "extern \"libz.so\" fn f(p: ptr[i64]) -> none",
     "void f(void *p);"),
    ("ptr_to_ptr_out", True,
     "extern \"libz.so\" fn f(p: ptr[ptr]) -> i32",
     "typedef struct sqlite3 sqlite3;\nint f(sqlite3 **p);"),
    ("ptr_to_ptr_wrong_elem", False,
     "extern \"libz.so\" fn f(p: ptr[i64]) -> i32",
     "typedef struct sqlite3 sqlite3;\nint f(sqlite3 **p);"),
    ("ptr_to_ptr_needs_second_star", False,
     "extern \"libz.so\" fn f(p: ptr[ptr]) -> i32",
     "typedef struct sqlite3 sqlite3;\nint f(sqlite3 *p);"),
]


def declarations():
    brat, header, shape_of = [STRUCT_BRAT], ["#include <stddef.h>", "#include <stdint.h>", STRUCT_C], {}
    for shape, (bt, ct, brat_types, c_types) in SHAPES.items():
        for i, b in enumerate(brat_types):
            for j, c in enumerate(c_types):
                name = "%s_%d_%d" % (shape.replace("-", "_"), i, j)
                brat.append(bt.format(n=name, b=b))
                header.append(ct.format(n=name, c=c))
                shape_of[name] = (shape, b, c)
    return "\n".join(brat) + "\n" + MAIN, "\n".join(header) + "\n", shape_of


def write(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def rejected(command, env_extra, brat_path, header_path):
    env = dict(os.environ, **env_extra)
    run = subprocess.run(command + ["ffi", brat_path, "--header", header_path, "--json"],
                         capture_output=True, text=True, timeout=300, env=env)
    check(run.stdout.startswith("{"), "%s printed no JSON report (exit %d): %s"
          % (os.path.basename(command[-1]), run.returncode, (run.stderr or run.stdout)[-300:]))
    report = json.loads(run.stdout)
    names = set()
    for finding in report["findings"]:
        m = re.search(r"extern fn (\w+):", finding)
        if m:
            names.add(m.group(1))
    return names


def both(brat_path, header_path):
    python = rejected(_cxx.cli_args(), {"BRAT_NO_NATIVE_CLI": "1"}, brat_path, header_path)
    native = rejected([NATIVE], {}, brat_path, header_path)
    return python, native


brat_src, header_src, shape_of = declarations()
python, native = both(write("matrix.brat", brat_src), write("matrix.h", header_src))

for shape in SHAPES:
    total = sum(1 for s, _, _ in shape_of.values() if s == shape)
    refused = sum(1 for name in python if shape_of[name][0] == shape)
    check(0 < refused < total, "%s matrix refuses %d of %d: it must contain both verdicts" % (shape, refused, total))

split = sorted((name in python, name in native, shape_of[name]) for name in python ^ native)
check(not split, "front ends disagree on %d declaration(s) (python rejects, native rejects, shape/brat/c): %s"
      % (len(split), split[:12]))

for label, accepted, brat_decl, c_decl in PINNED:
    python, native = both(write(label + ".brat", brat_decl + "\n" + MAIN), write(label + ".h", c_decl + "\n"))
    check(bool(python) != accepted and bool(native) != accepted,
          "%s: python rejects %s, native rejects %s, the guide says %s"
          % (label, sorted(python), sorted(native), "accept" if accepted else "reject"))

print("FFI FRONT ENDS AGREE (%d asserts, %d declarations)" % (n, len(shape_of)))
