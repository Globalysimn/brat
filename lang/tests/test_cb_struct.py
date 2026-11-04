"""A callback may return a record by value: both backends, every layout.

CPython's ctypes answers `invalid result type for callback function` for that
shape, so the reference interpreter used to be the one place where a legal C
callback was refused. There is a way through: the callback slot is only an
address, so the interpreter compiles a small C trampoline that keeps the real
prototype and shows Python nothing but `void *`. The trampoline reuses the same
widths ctypes uses and `_Static_assert`s the size and every field offset, so a
disagreement is a compile error instead of a wrong number.

Six shapes are checked, because the ABI treats them differently: 8 bytes in two
integer registers, 16 bytes in a mixed integer/SSE pair, 32 bytes through the
hidden return pointer, `float`+`char` (packing), a nested record, a packed
record, and a record holding a `const char *` (the string must stay alive after
the callback returns).
"""
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-cbstruct-")
LIB = os.path.join(TMP, "libcbs.so")

C_SRC = r"""
typedef struct { int x; int y; } Pt;
typedef struct { double a; long long b; } Mix;
typedef struct { long long a, b, c, d; } Big;
typedef struct { float f; char c; } F32S;
typedef struct { Pt p; int z; } Nest;
#pragma pack(push, 1)
typedef struct { char a; long long b; } Packed;
#pragma pack(pop)
typedef struct { const char *s; int n; } Named;

long long apply_pt(Pt (*cb)(long long), long long n) { Pt v = cb(n); return v.x * 1000 + v.y; }
long long apply_mix(Mix (*cb)(long long), long long n) { Mix v = cb(n); return (long long)(v.a * 2) + v.b; }
long long apply_big(Big (*cb)(long long), long long n) { Big v = cb(n); return v.a + v.b * 10 + v.c * 100 + v.d * 1000; }
long long apply_f32s(F32S (*cb)(long long), long long n) { F32S v = cb(n); return (long long)(v.f * 4) + v.c; }
long long apply_nest(Nest (*cb)(long long), long long n) { Nest v = cb(n); return v.p.x * 1000000 + v.p.y * 1000 + v.z; }
long long apply_packed(Packed (*cb)(long long), long long n) { Packed v = cb(n); return v.b + v.a * 1000000; }
long long apply_named(Named (*cb)(long long), long long n) { Named v = cb(n); return v.n + (v.s ? v.s[0] : 0); }
long long apply_arg(Pt (*cb)(Pt), long long n) { Pt a = { (int)n, 3 }; Pt v = cb(a); return v.x * 1000 + v.y; }
long long apply_two(Pt (*cb)(long long, double), long long n) { Pt v = cb(n, 2.5); return v.x * 1000 + v.y; }
"""

PROG = r"""
extern struct Pt { x: i32, y: i32 }
extern struct Mix { a: f64, b: i64 }
extern struct Big { a: i64, b: i64, c: i64, d: i64 }
extern struct F32S { f: f32, c: i8 }
extern struct Nest { p: Pt, z: i32 }
extern packed struct Packed { a: i8, b: i64 }
extern struct Named { s: str, n: i32 }

extern "LIB" fn apply_pt(cb: fn(i64) -> Pt, n: i64) -> i64
extern "LIB" fn apply_mix(cb: fn(i64) -> Mix, n: i64) -> i64
extern "LIB" fn apply_big(cb: fn(i64) -> Big, n: i64) -> i64
extern "LIB" fn apply_f32s(cb: fn(i64) -> F32S, n: i64) -> i64
extern "LIB" fn apply_nest(cb: fn(i64) -> Nest, n: i64) -> i64
extern "LIB" fn apply_packed(cb: fn(i64) -> Packed, n: i64) -> i64
extern "LIB" fn apply_named(cb: fn(i64) -> Named, n: i64) -> i64
extern "LIB" fn apply_arg(cb: fn(Pt) -> Pt, n: i64) -> i64
extern "LIB" fn apply_two(cb: fn(i64, f64) -> Pt, n: i64) -> i64

fn mk_pt(n: i64) -> Pt { return Pt { x: n, y: 2 } }
fn mk_mix(n: i64) -> Mix { return Mix { a: 1.5, b: n } }
fn mk_big(n: i64) -> Big { return Big { a: n, b: 2, c: 3, d: 4 } }
fn mk_f32s(n: i64) -> F32S { return F32S { f: 2.5, c: 7 } }
fn mk_nest(n: i64) -> Nest { return Nest { p: Pt { x: 5, y: 6 }, z: 7 } }
fn mk_packed(n: i64) -> Packed { return Packed { a: 3, b: n } }
fn mk_named(n: i64) -> Named { return Named { s: "hello", n: 4 } }
fn mk_arg(p: Pt) -> Pt { return Pt { x: p.x + p.y, y: p.y } }
fn mk_two(n: i64, d: f64) -> Pt { return Pt { x: n, y: 2 } }

fn main() -> i64 {
  print(apply_pt("mk_pt", 7))
  print(apply_mix("mk_mix", 5))
  print(apply_big("mk_big", 9))
  print(apply_f32s("mk_f32s", 1))
  print(apply_nest("mk_nest", 1))
  print(apply_packed("mk_packed", 11))
  print(apply_named("mk_named", 1))
  print(apply_arg("mk_arg", 4))
  print(apply_two("mk_two", 8))
  return 0
}
"""

n = 0
WANT = None


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-o", LIB, "-x", "c", "-"],
                   input=C_SRC, capture_output=True, text=True, timeout=120)
check(r.returncode == 0, "cc: %s" % r.stderr)

path = os.path.join(TMP, "cbs.brat")
with open(path, "w", encoding="utf-8") as f:
    f.write(PROG.replace('"LIB"', '"%s"' % LIB))

env = dict(os.environ, LD_LIBRARY_PATH=TMP)


def run(args, timeout=600, **kw):
    if args and args[0] in ("build", "run", "emit-c", "dist") \
            and "--unsafe-ffi" not in args:
        args = [args[0], "--unsafe-ffi"] + list(args[1:])
    return subprocess.run([sys.executable, CLI] + args, capture_output=True, text=True,
                          timeout=timeout, env=env, **kw)


# Both backends must print the same bytes for every shape: the numbers are read
# by C out of the record the callback returned, so they prove the layout, not the
# BRAT-side copy of it.
native_exe = os.path.join(TMP, "cbs.exe")
built = run(["build", path, "-o", native_exe])
check(built.returncode == 0, "build: %s" % (built.stdout + built.stderr))
native = subprocess.run([native_exe], capture_output=True, text=True, timeout=120, env=env)
check(native.returncode == 0, "native exit=%d %s" % (native.returncode, native.stdout + native.stderr))
WANT = native.stdout
check(WANT == "7002\n8\n4329\n17\n5006007\n3000011\n108\n7003\n8002\n", "native stdout %r" % (WANT,))

interp = run(["run", "--interp", path])
check(interp.returncode == 0, "interp exit=%d %s" % (interp.returncode, interp.stdout + interp.stderr))
check(interp.stdout == WANT, "interp %r != native %r" % (interp.stdout, WANT))

# The trampoline is compiled once per shape and cached on disk, so a second run
# with the cache in place has to give the same answer.
again = run(["run", "--interp", path])
check(again.returncode == 0 and again.stdout == WANT, "second interp run %r" % (again.stdout,))

# No compiler, no guess: the message names the reason and the way out. The shim
# cache is keyed by the source and lives under TMPDIR, so pointing TMPDIR at an
# empty directory is what makes this a cold start rather than a cache hit.
cold = os.path.join(TMP, "cold")
os.makedirs(cold, exist_ok=True)
no_cc = dict(env, CC=os.path.join(TMP, "definitely-not-a-compiler"), TMPDIR=cold)
broken = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", path], capture_output=True,
                        text=True, timeout=120, env=no_cc)
out = broken.stdout + broken.stderr
check(broken.returncode != 0, "a missing compiler must not silently pass")
check("needs a C compiler" in out and "brat build" in out, "no-cc message %r" % (out,))

print("CB STRUCT ALL GREEN (%d asserts)" % n)
