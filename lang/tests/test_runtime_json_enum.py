"""Regression tests for boolean/enum runtime display, type predicates, C-backed
std.json RFC 8259 edge cases, Option RC ownership, and const-char* FFI ptrs."""

import _cxx
import _scratch

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
BIN = _cxx.build()
CC = os.environ.get("BRAT_CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
assert CC, "no C compiler found"
TMP = _scratch.mkdtemp(prefix="brat-rt-json-enum-")


def has_asan():
    if sys.platform == "win32":
        return False
    probe = os.path.join(TMP, "asan_probe.c")
    out = os.path.join(TMP, "asan_probe.exe")
    with open(probe, "w", encoding="utf-8") as f:
        f.write("int main(void) { return 0; }\n")
    r = subprocess.run([CC, "-fsanitize=address", probe, "-o", out],
                       capture_output=True, text=True)
    return r.returncode == 0


USE_ASAN = has_asan()
ASAN_ENV = (
    dict(os.environ, ASAN_OPTIONS="detect_leaks=1")
    if (USE_ASAN and sys.platform != "darwin")
    else dict(os.environ)
)


def build_and_run(name, src, extra_flags=(), link_libs=()):
    p = os.path.join(TMP, name + ".brat")
    exe = os.path.join(TMP, name + (".exe" if sys.platform == "win32" else ".bin"))
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)
    if USE_ASAN and not extra_flags:
        chk = subprocess.run([BIN, "check", p], capture_output=True, text=True, timeout=60)
        assert chk.returncode == 0, "check %s failed:\n%s\n%s" % (name, chk.stdout, chk.stderr)
        em = subprocess.run([BIN, "emit-c", p], capture_output=True, text=True, timeout=60)
        assert em.returncode == 0, "emit-c %s failed:\n%s\n%s" % (name, em.stdout, em.stderr)
        cpath = os.path.join(TMP, name + ".c")
        with open(cpath, "w", encoding="utf-8") as f:
            f.write(em.stdout)
        b = subprocess.run([CC, "-std=gnu11", "-O1", "-g", "-fsanitize=address",
                            "-I", ENGINE, cpath, "-lm", "-lpthread", *link_libs, "-o", exe],
                           capture_output=True, text=True, timeout=120)
    else:
        b = subprocess.run([BIN, "build", "-O2", *extra_flags, p, "-o", exe],
                           capture_output=True, text=True, timeout=120)
    assert b.returncode == 0, "build %s failed:\n%s\n%s" % (name, b.stdout, b.stderr)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60, env=ASAN_ENV)
    assert r.returncode == 0, "run %s failed (%d):\n%s\n%s" % (name, r.returncode, r.stdout, r.stderr)
    return r.stdout


# 1. Booleans, constructor-style V_TAG display, and type predicates.
out1 = build_and_run("disp_and_preds", """
enum Color { Red, Rgb(r, g, b) }

fn box_any(x: dyn) -> dyn { return x }

fn main() -> i64 {
    print(true)
    print(false)
    print([true, false])
    print(Ok(42))
    print(Err("boom"))
    print(Some(Red))
    print(Ok(Rgb(10, 20, 30)))
    let b = box_any(true)
    let i = box_any(1)
    let f = box_any(1.5)
    print(is_bool(b) && !is_int(b) && !is_float(b))
    print(is_int(i) && !is_bool(i) && !is_float(i))
    print(is_float(f) && !is_int(f) && !is_bool(f))
    return 0
}
""")
assert out1.splitlines() == [
    "true",
    "false",
    "[true, false]",
    "Ok(42)",
    "Err('boom')",
    "Some(Red)",
    "Ok(Rgb(10, 20, 30))",
    "true",
    "true",
    "true",
], out1

# 2. Non-recursive and self-recursive Option-returning functions release owned
#    Some(...) temporaries cleanly under ASan + LeakSanitizer.
out2 = build_and_run("option_rc_drop", """
fn opt_str(flag: bool) -> Option[str] {
    if flag { return Some("alpha" + "-beta") }
    return None
}

fn walk_opt(xs: vec[str], idx: i64) -> Option[str] {
    if idx >= len(xs) { return None }
    if idx == len(xs) - 1 { return Some(xs[idx] + "!") }
    return walk_opt(xs, idx + 1)
}

fn count_down(n: i64) -> i64 {
    if n <= 0 { return 0 }
    return 1 + count_down(n - 1)
}

fn main() -> i64 {
    print(unwrap(opt_str(true)))
    print(opt_str(false) == None)
    let items = ["a", "b", "end"]
    print(unwrap(walk_opt(items, 0)))
    print(walk_opt(items, 9) == None)
    print(count_down(10))
    return 0
}
""")
assert out2.splitlines() == ["alpha-beta", "true", "end!", "true", "10"], out2

# 3. C-accelerated std.json RFC 8259 semantics & security invariants.
out3 = build_and_run("json_rfc8259", r"""
import std.json

fn main() -> i64 {
    // Surrogate pairs, sorted keys, booleans, null, floats, ints
    let doc = unwrap(json.parse("{\"z\":true,\"meta\":{\"n\":7},\"a\":[1,-2.5,null,\"\\uD83D\\uDE00\"]}"))
    print(json.get_bool(doc, "z", false))
    print(unwrap(json.get_path(doc, "meta.n")))
    print(len(doc["a"][3]))
    print(json.stringify(doc))
    print(json.stringify_indent({"b": [1, false], "a": None}, 2))

    // Rejections: $type spoofing, \u0000, unpaired surrogates, raw control byte, trailing junk
    print(is_err(json.parse("{\"$type\":\"Spoof\"}")))
    print(is_err(json.parse("\"\\u0000\"")))
    print(is_err(json.parse("\"\\uD800\"")))
    print(is_err(json.parse("\"\\uDC00\"")))
    print(is_err(json.parse("\"\t\"")))
    print(is_err(json.parse("123 456")))

    // Depth >= BRAT_MAX_VALUE_DEPTH (2000) rejected cleanly without leaks
    let mut deep = "0"
    let mut i = 0
    while i < 2005 {
        deep = "[" + deep + "]"
        i = i + 1
    }
    print(is_err(json.parse(deep)))
    return 0
}
""")
assert out3 == "\n".join([
    "true",
    "7",
    "1",
    '{"a":[1,-2.5,null,"😀"],"meta":{"n":7},"z":true}',
    '{\n  "a": null,\n  "b": [\n    1,\n    false\n  ]\n}',
    "true",
    "true",
    "true",
    "true",
    "true",
    "true",
    "true",
]) + "\n", out3

# 4. Nullable `const char *` C FFI parameter typed as `ptr[u8]`.
hdr = os.path.join(TMP, "nullable_cstr.h")
csrc = os.path.join(TMP, "nullable_cstr.c")
so = os.path.join(TMP, "libnullable_cstr" + (".dylib" if sys.platform == "darwin" else ".so"))
with open(hdr, "w", encoding="utf-8") as f:
    f.write("int check_opt_cstr(const char *s);\n")
with open(csrc, "w", encoding="utf-8") as f:
    f.write('#include "nullable_cstr.h"\n'
            "int check_opt_cstr(const char *s) { return s == 0 ? 77 : (unsigned char)s[0]; }\n")
sh_flags = ["-dynamiclib"] if sys.platform == "darwin" else ["-shared", "-fPIC"]
rc = subprocess.run([CC, *sh_flags, csrc, "-o", so], capture_output=True, text=True)
if rc.returncode == 0 and sys.platform != "win32":
    out4 = build_and_run("ffi_nullable_cstr", """
extern "%s" {
    extern fn check_opt_cstr(s: ptr[u8]) -> i32
}

fn main() -> i64 {
    let z: i64 = check_opt_cstr(0)
    print(z)
    return 0
}
""" % so.replace("\\", "/"), extra_flags=("--header", hdr))
    assert out4.strip() == "77", out4

print("RUNTIME / JSON / ENUM / FFI REGRESSIONS ALL GREEN")
