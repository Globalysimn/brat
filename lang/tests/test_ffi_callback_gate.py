"""An `extern` callback slot is refused where it is written, by every path.

A callback parameter takes the *name* of a BRAT function, written as a string
literal. Everything else -- a closure, a bare fn name, a name with no function
behind it, a function of the wrong arity or the wrong widths -- used to be
handed to the emitter, which wrote `brat_cb_<name>` for it. That is C that does
not compile when the name is unknown, and C that calls through a lying
prototype when the signature does not match; the interpreter's version of the
same program raised a `ctypes` traceback from inside a call C was in the middle
of. Neither is a diagnostic.

So all three paths refuse, in the words `check` already used:

* `brat-cxx cc` and `brat emit-c` exit 1 with an empty stdout (stdout is the C;
  a refusal writes nothing to it);
* the interpreter stops with the same sentence;

and the sentence is what `check` says, which is the point: one rule, one text.
No library has to exist for any of this -- a refusal happens before a call.

The last case is the shape that must keep working: a real callback still gets
its `brat_cb_` wrapper, and the Python CLI and native `cc` doors write the
same C for it.
"""
import _cxx
import _scratch
import os
import subprocess
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ENG = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENG, "cli.py")
CXX = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-ffi-cbgate-")
version = tuple(int(part) for part in unicodedata.unidata_version.split("."))
case_version = 1600 if version >= (16, 0) else 1500
CXX_ENV = dict(os.environ, BRAT_CASE_MAP_VERSION=str(case_version))
CLI_ENV = dict(CXX_ENV, BRAT_CXX=CXX, BRAT_NO_NATIVE_CLI="1")

BODY = (
    'fn add_two(x: i64) -> i64 { return x + 2 }\n'
    'fn add3(x: i64, y: i64) -> i64 { return x + y + 1 }\n'
    'fn halve(x: f32) -> f32 { return x / 2.0 }\n'
)

# The interpreter opens a library when the call runs, so the refusal it has to
# make needs a library that really exists and a symbol that really is there.
# The native emitter refuses before any of that, so it gets a name that never loads.
LIB_C = "int64_t call_cb(int64_t (*f)(int64_t), int64_t v) { return f(v); }\n"
LIB_C += "int64_t call_ptr(int64_t (*f)(int32_t *), int32_t *p) { return f(p); }\n"
LIB_C += "int32_t *call_ptr_ret(int32_t *(*f)(int32_t *), int32_t *p) { return f(p); }\n"

# name -> (call written into main, the sentence every path must say)
REFUSED = {
    "cb_closure": (
        'print(call_cb(fn(x) { return x + 2 }, 40))',
        "arg 1 of `call_cb`: callback must be a function name"),
    "cb_unknown": (
        'print(call_cb("nope", 40))',
        "unknown callback `nope`"),
    "cb_arity": (
        'print(call_cb("add3", 40))',
        "callback `add3` takes 2 parameter(s), fn(i64)->i64 wants 1"),
    "cb_signature": (
        'print(call_cb("halve", 40))',
        "callback `halve` has signature fn(f32)->f32, expected fn(i64)->i64"),
}


def source(body, lib):
    return ('extern "%s" fn call_cb(f: fn(i64) -> i64, v: i64) -> i64\n' % lib
            + BODY + "fn main() -> i64 {\n  " + body + "\n  return 0\n}\n")


def build_probe_lib():
    """A one-function library, or None when there is no C compiler."""
    import shutil
    cc = shutil.which(os.environ.get("BRAT_CC", "cc"))
    if cc is None:
        return None
    src = os.path.join(TMP, "libcb.c")
    out = os.path.join(TMP, "libcb.so")
    with open(src, "w", encoding="utf-8") as f:
        f.write("#include <stdint.h>\n" + LIB_C)
    r = subprocess.run([cc, "-shared", "-fPIC", "-O2", "-o", out, src],
                       capture_output=True, timeout=300)
    return out if r.returncode == 0 else None


def write(name, text):
    p = os.path.join(TMP, name + ".brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
    return p


def main():
    n = 0
    lib = build_probe_lib()
    if lib is None:
        print("note: no C compiler; the interpreter half of each case is skipped")
    for name, (body, sentence) in REFUSED.items():
        path = write(name, source(body, "libnosuch.so"))
        ipath = write(name + "_interp", source(body, lib)) if lib else None

        # Both front ends, through the two doors that write C.
        cxx = subprocess.run([CXX, "cc", path], capture_output=True, timeout=120,
                     env=CXX_ENV)
        ref = subprocess.run([sys.executable, CLI, "emit-c", "--unsafe-ffi", path],
                             capture_output=True, timeout=120, env=CLI_ENV)
        for tag, r in (("brat-cxx cc", cxx), ("brat emit-c", ref)):
            assert r.returncode == 1, "%s %s: rc=%d want 1" % (name, tag, r.returncode)
            assert r.stdout == b"", "%s %s: stdout must stay empty, got %r" % (
                name, tag, r.stdout[:80])
            assert sentence in r.stderr.decode("utf-8", "replace"), (
                "%s %s: stderr has no %r:\n%s" % (
                    name, tag, sentence, r.stderr.decode("utf-8", "replace")[-400:]))
        n += 1

        # And the interpreter, which compiles none of this.
        if ipath is None:
            print("ffi cb gate ok: %s (native only)" % name)
            continue
        run = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", ipath],
                             capture_output=True, timeout=120)
        assert run.returncode != 0, "%s interp: ran a program it cannot honour" % name
        assert sentence in run.stderr.decode("utf-8", "replace"), (
            "%s interp: stderr has no %r:\n%s" % (
                name, sentence, run.stderr.decode("utf-8", "replace")[-400:]))
        assert b"Exception ignored" not in run.stderr, (
            "%s interp: a ctypes traceback reached the user" % name)
        print("ffi cb gate ok: %s" % name)

    # The shape that works: a real callback still gets its wrapper.
    ok_src = source('print(call_cb("add_two", 40))', "libnosuch.so")
    path = write("cb_ok", ok_src)
    cxx = subprocess.run([CXX, "cc", path], capture_output=True, timeout=120,
                         env=CXX_ENV)
    assert cxx.returncode == 0 and b"brat_cb_add_two" in cxx.stdout, (
        "cb_ok: a valid callback must emit its wrapper")
    ref = subprocess.run([sys.executable, CLI, "emit-c", "--unsafe-ffi", path],
                         capture_output=True, timeout=120, env=CLI_ENV)
    assert ref.returncode == 0, "cb_ok: Python CLI refused a valid program"
    # `emit-c` appends the contract block the driver records for the binary;
    # `brat-cxx cc` is the emitter alone, so the C is what must match.
    code, sep, contract = ref.stdout.rpartition(b"\n/* BRAT FFI contract")
    if sep:
        assert contract.count(b"static const char brat_ffi_contract[]") == 1, contract[-400:]
        ref_stdout = code + b"\n"
    else:
        ref_stdout = ref.stdout
    assert ref_stdout == cxx.stdout, "cb_ok: the CLI doors wrote different C"
    n += 1
    print("ffi cb gate ok: cb_ok (wrapper emitted, CLI doors byte-identical)")

    ptr_src = (
        'extern "%s" fn call_ptr(f: fn(ptr[i32]) -> i64, p: ptr[i32]) -> i64\n' % lib
        + 'extern "%s" fn call_ptr_ret(f: fn(ptr[i32]) -> ptr[i32], p: ptr[i32]) -> ptr[i32]\n' % lib
        + 'fn read_ptr(p: ptr[i32]) -> i64 { return p[0] }\n'
        + 'fn return_ptr(p: ptr[i32]) -> ptr[i32] { return p }\n'
        + 'fn main() -> i64 { call_ptr("read_ptr", ptr_null()); return call_ptr_ret("return_ptr", ptr_null()) }\n')
    ptr_path = write("cb_ptr", ptr_src)
    cxx = subprocess.run([CXX, "cc", ptr_path], capture_output=True, timeout=120,
                         env=CXX_ENV)
    assert cxx.returncode == 0, cxx.stderr.decode("utf-8", "replace")[-400:]
    ref = subprocess.run([sys.executable, CLI, "emit-c", "--unsafe-ffi", ptr_path],
                         capture_output=True, timeout=120, env=CLI_ENV)
    code, sep, _contract = ref.stdout.rpartition(b"\n/* BRAT FFI contract")
    ref_stdout = code + b"\n" if sep else ref.stdout
    assert ref.returncode == 0 and ref_stdout == cxx.stdout, (
        "cb_ptr: typed pointer callback CLI doors disagree")
    assert b"call_ptr(int64_t (*) (int *), int *)" in cxx.stdout, cxx.stdout[:300]
    cfile = os.path.join(TMP, "cb_ptr.c")
    obj = os.path.join(TMP, "cb_ptr.o")
    with open(cfile, "wb") as f:
        f.write(cxx.stdout)
    cc = subprocess.run(["cc", "-std=c99", "-Werror=incompatible-pointer-types",
                         "-I", ENG, "-c", cfile, "-o", obj],
                        capture_output=True, timeout=120)
    assert cc.returncode == 0, cc.stderr.decode("utf-8", "replace")[-600:]
    n += 1
    print("ffi cb gate ok: typed pointer callback is C-compatible")

    print("FFI CALLBACK GATE ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()
