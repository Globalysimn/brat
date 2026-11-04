"""Callbacks with context: a `fn(...)` extern parameter names a BRAT function,
and a trailing `ptr` in that signature is the C `void *user_data`.

The test builds its own shared library, so it runs on a clean checkout.
"""
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-cbctx-")
LIB = os.path.join(TMP, "libcbctx.so")

with open(os.path.join(TMP, "libcbctx.c"), "w", encoding="utf-8") as f:
    f.write(
        "long long apply_cb(long long x, long long (*cb)(long long)) { return cb(x); }\n"
        "long long apply_ctx(long long x, long long (*cb)(long long, void *), void *ctx)\n"
        "{ return cb(x, ctx); }\n")
r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", os.path.join(TMP, "libcbctx.c"),
                    "-o", LIB], capture_output=True, text=True, timeout=120)
assert r.returncode == 0, r.stderr

SRC = (
    'extern "%s" fn apply_cb(x: i64, cb: fn(i64) -> i64) -> i64;\n'
    'extern "%s" fn apply_ctx(x: i64, cb: fn(i64, ptr) -> i64, ctx: ptr) -> i64;\n'
    'extern fn malloc(n: i64) -> ptr;\n'
    'extern fn free(move p: ptr);\n'
    '\n'
    'fn double_it(x: i64) -> i64 { return x * 2 }\n'
    'fn add_ctx(x: i64, ctx: ptr) -> i64 {\n'
    '  if ptr_is_null(ctx) { return x }\n'
    '  return x + ptr_read_i64(ctx)\n'
    '}\n'
    '\n'
    'fn main() -> i64 {\n'
    '  let ctx = malloc(8);\n'
    '  ptr_write_i64(ctx, 100);\n'
    '  print(apply_cb(21, "double_it"));\n'
    '  print(apply_ctx(5, "add_ctx", ctx));\n'
    '  print(apply_ctx(5, "add_ctx", ptr_null()));\n'
    '  free(ctx);\n'
    '  return 0;\n'
    '}\n') % (LIB, LIB)

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


env = dict(os.environ, LD_LIBRARY_PATH=TMP)
ok = write("ok.brat", SRC)

interp = subprocess.run(
    [sys.executable, CLI, "run", "--interp", "--unsafe-ffi", ok], capture_output=True,
    text=True, timeout=120, env=env)
check(interp.returncode == 0, "run exit=%d out=%r" % (interp.returncode, interp.stdout + interp.stderr))
check(interp.stdout == "42\n105\n5\n", "run stdout %r" % (interp.stdout,))

exe = os.path.join(TMP, "ok.exe")
built = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", ok, "-o", exe], capture_output=True,
                       text=True, timeout=300, env=env)
check(built.returncode == 0, "build exit=%d out=%r" % (built.returncode, built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60, env=env)
check(native.returncode == 0, "native exit=%d out=%r" % (native.returncode, native.stdout + native.stderr))
check(native.stdout == "42\n105\n5\n", "native stdout %r" % (native.stdout,))


def expect_type_error(name, src, text):
    path = write(name + ".brat", src)
    r = subprocess.run([sys.executable, CLI, "check", path], capture_output=True, text=True,
                       timeout=120, env=env)
    out = r.stdout + r.stderr
    check(r.returncode != 0 and text in out, "%s check: rc=%d out=%r" % (name, r.returncode, out))
    b = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", path, "-o", os.path.join(TMP, name + ".exe")],
                       capture_output=True, text=True, timeout=300, env=env)
    out = b.stdout + b.stderr
    check(b.returncode != 0 and text in out, "%s build: rc=%d out=%r" % (name, b.returncode, out))


head = 'extern "%s" fn apply_cb(x: i64, cb: fn(i64) -> i64) -> i64;\n' % LIB
expect_type_error("notname", head + 'fn main() -> i64 { print(apply_cb(21, 5)); return 0 }\n',
                  "arg 2 of `apply_cb`: callback must be a function name")
expect_type_error("unknown", head + 'fn main() -> i64 { print(apply_cb(21, "nope")); return 0 }\n',
                  "unknown callback `nope`")
expect_type_error("arity", head + 'fn two(a: i64, b: i64) -> i64 { return a + b }\n'
                  'fn main() -> i64 { print(apply_cb(21, "two")); return 0 }\n',
                  "callback `two` takes 2 parameter(s), fn(i64)->i64 wants 1")
expect_type_error("default_arity", head + 'fn two(a: i64, b: i64 = 0) -> i64 { return a + b }\n'
                  'fn main() -> i64 { print(apply_cb(21, "two")); return 0 }\n',
                  "callback `two` takes 2 parameter(s), fn(i64)->i64 wants 1")
expect_type_error("parameter_abi", head + 'fn wrong(x: f64) -> i64 { return 1 }\n'
                  'fn main() -> i64 { print(apply_cb(21, "wrong")); return 0 }\n',
                  "callback `wrong` has signature fn(f64)->i64, expected fn(i64)->i64")
expect_type_error("return_abi", head + 'fn wrong(x: i64) -> f64 { return 1.0 }\n'
                  'fn main() -> i64 { print(apply_cb(21, "wrong")); return 0 }\n',
                  "callback `wrong` has signature fn(i64)->f64, expected fn(i64)->i64")
expect_type_error("width_abi", head + 'fn wrong(x: i32) -> i64 { return x }\n'
                  'fn main() -> i64 { print(apply_cb(21, "wrong")); return 0 }\n',
                  "callback `wrong` has signature fn(i32)->i64, expected fn(i64)->i64")

# Callback names inside an imported module must follow the same mangling as the
# function definition they designate.
module_dir = os.path.join(TMP, "ffi")
os.makedirs(module_dir)
module = os.path.join(module_dir, "callbacks.brat")
with open(module, "w", encoding="utf-8") as f:
    f.write(('extern "%s" fn apply_cb(x: i64, cb: fn(i64) -> i64) -> i64;\n'
             'fn double_it(x: i64) -> i64 { return x * 2 }\n'
             'pub fn run() -> i64 { return apply_cb(21, "double_it") }\n') % LIB)
app = write("callback_module.brat",
            'import ffi.callbacks\n'
            'fn main() -> i64 { print(ffi.callbacks.run()); return 0 }\n')
checked = subprocess.run([sys.executable, CLI, "check", app], capture_output=True, text=True,
                         timeout=120)
assert checked.returncode == 0, checked.stdout + checked.stderr
interpreted = subprocess.run(
    [sys.executable, CLI, "run", "--interp", "--unsafe-ffi", app], capture_output=True,
    text=True, timeout=120, env=env)
assert interpreted.returncode == 0 and interpreted.stdout == "42\n", (
    interpreted.returncode, interpreted.stdout, interpreted.stderr)
module_exe = os.path.join(TMP, "callback_module.exe")
module_build = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", app, "-o", module_exe],
                              capture_output=True, text=True, timeout=300, env=env)
assert module_build.returncode == 0, module_build.stdout + module_build.stderr
module_run = subprocess.run([module_exe], capture_output=True, text=True, timeout=60, env=env)
assert module_run.returncode == 0 and module_run.stdout == "42\n", (
    module_run.returncode, module_run.stdout, module_run.stderr)
n += 4

print("CALLBACK CONTEXT ALL GREEN (%d asserts)" % n)
