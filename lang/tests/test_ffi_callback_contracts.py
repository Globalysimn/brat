"""Callback contracts: what the C side may do with the function it is handed.

`test_ffi_callback_gate.py` settled *what* may be passed -- a BRAT function
name, checked for arity and widths. It said nothing about how that function will
be called, and the two ways it can be called that no check covered were:

* from a thread the program never made (`thread(any)`), where the callback's
  body reaches state the program's own thread also owns -- the runtime is not
  thread-safe, so that is a crash and not a slow program;
* from inside the callee itself (`reenter(no)` absent), which is the shape that
  re-enters `qsort` from its own comparator.

Both contracts are written on the callback parameter because that is where a
reader looks, and stored on the declaration because that is whose behaviour they
describe. The defaults are the dangerous pair -- the caller's thread, re-entry
allowed -- so a program that says nothing is checked as if the callee may do the
worst thing the language can express.

Run: python3 test_ffi_callback_contracts.py
"""
import _cxx
import _scratch
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
CXX = None
TMP = _scratch.mkdtemp(prefix="brat-cbctr-")

# A library that really calls back, so the accepted cases run rather than only
# type-check: `call_cb` calls once on the calling thread, `keep_cb` calls once
# and is done, and `nested` calls the callback which may call `nested` again.
LIB_C = """
int64_t call_cb(int64_t (*f)(int64_t), int64_t v) { return f(v); }
int64_t nested(int64_t (*f)(int64_t), int64_t v) { return f(v); }
"""

# The declarations and the header are the same functions, so `--header` proves
# them and the build needs no `--unsafe-ffi`: a contract must not cost the
# program its proof, or nobody would write one.
LIB_H = """
#include <stdint.h>
int64_t call_cb(int64_t (*f)(int64_t), int64_t v);
int64_t nested(int64_t (*f)(int64_t), int64_t v);
"""


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)


def fail(name, msg):
    sys.stdout.write("FAIL %s: %s\n" % (name, msg))
    sys.stdout.flush()
    raise SystemExit(1)


def write(name, src):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def diagnostics(cmd):
    p = sh(cmd)
    text = (p.stdout or "") + (p.stderr or "")
    return p.returncode, "\n".join(l for l in text.splitlines() if ": error: " in l)


def both_checkers(path):
    return {"py": diagnostics([sys.executable, CLI, "check", "--py", path]),
            "cxx": diagnostics([CXX, "check", path])}


def build_lib():
    cc = shutil.which(os.environ.get("BRAT_CC", "cc"))
    if cc is None:
        return None
    src = os.path.join(TMP, "libcb.c")
    out = os.path.join(TMP, "libcb.so")
    with open(src, "w", encoding="utf-8") as f:
        f.write("#include <stdint.h>\n" + LIB_C)
    with open(os.path.join(TMP, "libcb.h"), "w", encoding="utf-8") as f:
        f.write(LIB_H)
    r = sh([cc, "-shared", "-fPIC", "-O2", "-o", out, src])
    return out if r.returncode == 0 else None


# Declarations that cannot mean what they say; the parser refuses each, so the
# message is identical in both front ends without a second implementation.
REFUSALS = {
    "unknown_contract": 'extern fn f(cb: fn(i64) -> i64 frobnicate(x)) -> i64\n',
    "unknown_value": 'extern fn f(cb: fn(i64) -> i64 thread(sometimes)) -> i64\n',
    "on_non_callback": 'extern fn f(v: i64 thread(any)) -> i64\n',
    "twice": 'extern fn f(a: fn() -> none thread(any), b: fn() -> none thread(same)) -> none\n',
    "at_the_end": 'extern fn f(cb: fn(i64) -> i64) -> i64 reenter(no)\n',
}

# name -> (source, diagnostic fragment or None, expected stdout when accepted)
# Checked but not built: with the default (`thread(same)`) a callback that
# touches a script-level binding is legal, and that is the whole point of the
# contract -- refusing it would make the default useless. The build is not
# asserted here because *this program does not build for a second reason*, one
# that has nothing to do with callbacks: a function reading a top-level `let` is
# accepted by both checkers, runs in the interpreter, and produces no working
# binary from either emitter (LEDGER, "Open", 2026-10-30).
CHECK_ONLY = {
    "same_thread_may_touch_state": '''extern "%(lib)s" fn call_cb(cb: fn(i64) -> i64, v: i64) -> i64
let mut total = 100
fn bump(x: i64) -> i64 {
  total = total + x
  return total
}
fn main() -> i64 {
  print(call_cb("bump", 1))
  return 0
}
''',
}

CASES = {
    # Same program, `thread(any)`: the callback may run beside the caller's
    # thread, so reaching its global is a race the runtime cannot survive.
    "any_thread_may_not_touch_state": ('''extern "%(lib)s" fn call_cb(cb: fn(i64) -> i64 thread(any), v: i64) -> i64
let mut total = 100
fn bump(x: i64) -> i64 {
  total = total + x
  return total
}
fn main() -> i64 {
  print(call_cb("bump", 1))
  return 0
}
''', "`bump` is registered as a callback of `call_cb`, which declares `thread(any)`, but it touches `total`", ""),
    # A `thread(any)` callback that only uses what it is given is accepted.
    "any_thread_self_contained_ok": ('''extern "%(lib)s" fn call_cb(cb: fn(i64) -> i64 thread(any), v: i64) -> i64
fn pure_cb(x: i64) -> i64 {
  return x * 2
}
fn main() -> i64 {
  print(call_cb("pure_cb", 21))
  return 0
}
''', None, "42\n"),
    # A callback that calls its own library again: a re-entry the callee never
    # agreed to.
    "reentry_refused": ('''extern "%(lib)s" fn nested(cb: fn(i64) -> i64, v: i64) -> i64
fn again(x: i64) -> i64 {
  return nested("again", x)
}
fn main() -> i64 {
  print(nested("again", 1))
  return 0
}
''', "`again` is registered as a callback of `nested` and calls `nested` again", ""),
    # The same re-entry, with the promise made: legal, and it runs.
    "reentry_allowed": ('''extern "%(lib)s" fn nested(cb: fn(i64) -> i64 reenter(no), v: i64) -> i64
fn again(x: i64) -> i64 {
  if x > 100 {
    return x
  }
  return nested("again", x + 1)
}
fn main() -> i64 {
  print(nested("again", 1))
  return 0
}
''', None, "101\n"),
    # Re-entry through a helper still counts: the call graph is followed.
    "reentry_via_helper": ('''extern "%(lib)s" fn nested(cb: fn(i64) -> i64, v: i64) -> i64
fn again(x: i64) -> i64 {
  return go(x)
}
fn go(x: i64) -> i64 {
  return nested("again", x)
}
fn main() -> i64 {
  print(nested("again", 1))
  return 0
}
''', "calls `nested` again", ""),
}


def check_refusals():
    for name, src in sorted(REFUSALS.items()):
        path = write(name, src)
        seen = both_checkers(path)
        if len(set(seen.values())) != 1:
            fail(name, "checkers disagree: %r" % (seen,))
        rc, text = seen["py"]
        if rc == 0 or not text:
            fail(name, "must be refused with a message, got rc=%d" % rc)
    print("CONTRACTS refusals ok (%d cases, identical messages)" % len(REFUSALS))


def check_only(lib):
    for name, src in sorted(CHECK_ONLY.items()):
        path = write(name, src % {"lib": lib or "libcb.so"})
        seen = both_checkers(path)
        if len(set(seen.values())) != 1:
            fail(name, "checkers disagree: %r" % (seen,))
        if seen["py"][0] != 0:
            fail(name, "the default contract must accept this, got:\n%s" % seen["py"][1])
    print("CONTRACTS defaults ok (%d case, check only)" % len(CHECK_ONLY))


def check_cases(lib):
    if lib is None:
        print("CONTRACTS cases skipped (no cc to build the probe library)")
        return
    for name, (src, want, stdout) in sorted(CASES.items()):
        path = write(name, src % {"lib": lib})
        seen = both_checkers(path)
        if len(set(seen.values())) != 1:
            fail(name, "checkers disagree: %r" % (seen,))
        rc, text = seen["py"]
        if want is None:
            if rc != 0:
                fail(name, "must be accepted, got:\n%s" % text)
            exe = os.path.join(TMP, name + ".exe")
            header = os.path.join(TMP, "libcb.h")
            for tag, cmd in (("py", [sys.executable, CLI, "build", path, "-o", exe,
                                     "--header", header]),
                             ("cxx", [CXX, "build", path, "-o", exe, "--header", header])):
                b = sh(cmd)
                if b.returncode != 0:
                    fail(name, "%s build: %s" % (tag, (b.stdout + b.stderr)[-400:]))
                run = sh([exe])
                if run.stdout != stdout:
                    fail(name, "%s stdout %r, want %r" % (tag, run.stdout, stdout))
        elif want not in text:
            fail(name, "want %r in the diagnostic, got:\n%s" % (want, text))
    print("CONTRACTS cases ok (%d cases, both CLI build paths, binaries run)" % len(CASES))


def check_dump_agrees():
    """The contracts are part of the tree, so both parsers must print them alike."""
    src = ('extern fn f(a: fn(i64) -> i64 thread(any), b: fn() -> none reenter(no)) -> none\n'
           'extern fn g(cb: fn() -> none) -> none\n')
    path = write("dumps", src)
    sys.path.insert(0, os.path.join(HERE, "..", "engine"))
    import _cxx as ref
    from brat.lexer import Cur, lex
    from brat.parser import parse_prog
    mine = ref.sx(parse_prog(Cur(lex(src))))
    theirs = sh([CXX, "ast", path]).stdout.strip()
    if mine != theirs:
        fail("dumps", "front ends disagree:\n  py : %s\n  cxx: %s" % (mine, theirs))
    for want in ('("thread:any" "reenter:no")', '()'):
        if want not in mine:
            fail("dumps", "want %r in the tree, got %s" % (want, mine))
    if "thread(any)" in mine or "reenter(no)" in mine:
        fail("dumps", "a contract leaked into a type: %s" % mine)
    print("CONTRACTS dumps ok (both front ends print the list alike)")


def main():
    global CXX
    CXX = _cxx.build()
    check_dump_agrees()
    check_refusals()
    lib = build_lib()
    check_only(lib)
    check_cases(lib)
    print("FFI CALLBACK CONTRACTS ALL GREEN")


if __name__ == "__main__":
    main()
