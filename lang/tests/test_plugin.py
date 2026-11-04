"""Plugins at run time: lib_open/lib_sym/lib_close plus call() with a literal signature."""
import _scratch

import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-plugin-")
LIB = os.path.join(TMP, "libplug.so")

with open(os.path.join(TMP, "plug.c"), "w", encoding="utf-8") as f:
    f.write(
        'long long add(long long a, long long b) { return a + b; }\n'
        'double half(double x) { return x / 2.0; }\n'
        'const char *greet(void) { return "hi from plugin"; }\n'
        'const char *bad_text(void) { static char b[3] = {(char)0xff, \'x\', 0}; return b; }\n'
        'void poke(long long *p, long long v) { *p = v; }\n'
        'long long scale(long long v, double k) { return (long long)(v * k); }\n'
        '#include <unistd.h>\n'
        'long long pause_echo(long long v) { usleep(150000); return v; }\n')
r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", os.path.join(TMP, "plug.c"), "-o", LIB],
                   capture_output=True, text=True, timeout=120)
assert r.returncode == 0, r.stderr

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


OK = '''
extern fn malloc(n: i64) -> ptr;
extern fn free(move p: ptr);

fn main() -> i64 {
  let h = lib_open("{lib}")
  let add = lib_sym(h, "add")
  print(call(add, "i64(i64,i64)", 20, 22))
  let half = lib_sym(h, "half")
  print(call(half, "f64(f64)", 5.0))
  print(call(half, "f64(f64)", 1.0))
  let greet = lib_sym(h, "greet")
  print(call(greet, "str()"))
  let scale = lib_sym(h, "scale")
  print(call(scale, "i64(i64,f64)", 8, 1.25))
  let buf: ptr[i64] = malloc(8)
  ptr_write_i64(buf, 7)
  let poke = lib_sym(h, "poke")
  call(poke, "none(ptr,i64)", buf, 99)
  print(ptr_read_i64(buf))
  free(buf)
  lib_close(h)
  let h2 = lib_open("{lib}")
  let add2 = lib_sym(h2, "add")
  print(call(add2, "i64(i64,i64)", 1, 1))
  lib_close(h2)
  return 0
}
'''.replace("{lib}", LIB)

want = "42\n2.5\n0.5\nhi from plugin\n10\n99\n2\n"
env = dict(os.environ, LD_LIBRARY_PATH=TMP)
path = write("ok.brat", OK)

interp = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", path], capture_output=True, text=True,
                        timeout=120, env=env)
check(interp.returncode == 0 and interp.stdout == want,
      "run rc=%d out=%r" % (interp.returncode, interp.stdout + interp.stderr))

exe = os.path.join(TMP, "ok.exe")
built = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", path, "-o", exe], capture_output=True,
                       text=True, timeout=300, env=env)
check(built.returncode == 0, "build: %r" % (built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60, env=env)
check(native.returncode == 0 and native.stdout == want,
      "native rc=%d out=%r" % (native.returncode, native.stdout + native.stderr))

def runtime_fail(name, src, text):
    path = write(name + ".brat", src)
    r = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", path], capture_output=True, text=True,
                       timeout=120, env=env)
    out = r.stdout + r.stderr
    check(r.returncode != 0 and text in out, "%s run: rc=%d %r" % (name, r.returncode, out))
    exe = os.path.join(TMP, name + ".exe")
    b = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", path, "-o", exe], capture_output=True,
                       text=True, timeout=300, env=env)
    check(b.returncode == 0, "%s build: %r" % (name, b.stdout + b.stderr))
    n_r = subprocess.run([exe], capture_output=True, text=True, timeout=60, env=env)
    out = n_r.stdout + n_r.stderr
    check(n_r.returncode != 0 and text in out, "%s native: rc=%d %r" % (name, n_r.returncode, out))


runtime_fail("missing", '''
fn main() -> i64 {
  let h = lib_open("%s")
  let f = lib_sym(h, "nope")
  lib_close(h)
  return 0
}
''' % LIB, "lib_sym: no symbol nope")

runtime_fail("badlib", '''
fn main() -> i64 {
  let h = lib_open("%s/not-there.so")
  return h
}
''' % TMP, "lib_open: ")

runtime_fail("bad_text", '''
fn main() -> i64 {
  let h = lib_open("%s")
  let f = lib_sym(h, "bad_text")
  print(call(f, "str()"))
  lib_close(h)
  return 0
}
''' % LIB, "call: invalid UTF-8 str result")

def static_fail(name, src, text):
    """The checker refuses it, and `brat run` refuses to run it too."""
    path = write(name + ".brat", src)
    for cmd in ("check", "build"):
        argv = [sys.executable, CLI, cmd, path]
        if cmd == "build":
            # No header: the C here is written by the test, and the message
            # under test is the checker's, so the build opts out of the proof.
            argv += ["--unsafe-ffi", "-o", os.path.join(TMP, name + ".exe")]
        r = subprocess.run(argv, capture_output=True, text=True, timeout=300, env=env)
        out = r.stdout + r.stderr
        check(r.returncode != 0 and text in out,
              "%s %s: rc=%d %r" % (name, cmd, r.returncode, out))
    r = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", path], capture_output=True,
                       text=True, timeout=120, env=env)
    out = r.stdout + r.stderr
    check(r.returncode != 0 and "refusing to run" in out,
          "%s run gate: rc=%d %r" % (name, r.returncode, out))


# A value made from a library belongs to it: `lib_close` ends every symbol and
# every pointer that came through that handle. These used to be run-time
# crashes; the checker now sees the chain because it tracks the three calls.
static_fail("closed_symbol", '''
fn main() -> i64 {
  let h = lib_open("%s")
  let f = lib_sym(h, "add")
  lib_close(h)
  return call(f, "i64(i64,i64)", 1, 2)
}
''' % LIB, "is from library `h`, closed on line 5")

static_fail("closed_handle_lookup", '''
fn main() -> i64 {
  let h = lib_open("%s")
  lib_close(h)
  let f = lib_sym(h, "add")
  return f
}
''' % LIB, "`h` is closed on line 4")

static_fail("double_close", '''
fn main() -> i64 {
  let h = lib_open("%s")
  lib_close(h)
  lib_close(h)
  return 0
}
''' % LIB, "`h` is closed twice (the first close is on line 4)")

static_fail("closed_pointer", '''
fn main() -> i64 {
  let h = lib_open("%s")
  let mk = lib_sym(h, "malloc")
  let p = call(mk, "ptr(i64)", 16)
  lib_close(h)
  print(ptr_read_i64(p))
  return 0
}
''' % LIB, "`p` is from library `h`, closed on line 6")

# The literal signature is compared with the declaration instead of believed.
SIG = '''
extern fn add(a: i64, b: i64) -> i64
fn main() -> i64 {
  let h = lib_open("%s")
  let add = lib_sym(h, "add")
  print(call(add, "%s", 1, 2))
  lib_close(h)
  return 0
}
'''

static_fail("sig_param", SIG % (LIB, "i64(f64,i64)"),
            "call: `add` parameter 1 is i64, the signature says f64")
static_fail("sig_ret", SIG % (LIB, "f64(i64,i64)"),
            "call: `add` returns i64, the signature says f64")
static_fail("sig_arity", SIG % (LIB, "i64(i64)"),
            "call: `add` is declared to take 2 argument(s), the signature has 1")

# A matching signature with a matching declaration is accepted and runs.
good = write("sig_ok.brat", SIG % (LIB, "i64(i64,i64)"))
r = subprocess.run([sys.executable, CLI, "check", good], capture_output=True,
                   text=True, timeout=120, env=env)
check(r.returncode == 0, "sig_ok check: %r" % (r.stdout + r.stderr))

# The runtime keeps its own guard for a handle the checker cannot follow: here
# it arrives as a parameter, so no `lib_open` is visible at the close.
runtime_fail("untracked_close", '''
fn shut(h: ptr) -> i64 {
  return lib_close(h)
}
fn main() -> i64 {
  let h = lib_open("%s")
  shut(h)
  let f = lib_sym(h, "add")
  return f
}
''' % LIB, "lib_sym: no such handle")

sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "engine")))
from brat import runtime as brat_runtime

handle = brat_runtime._plugin("lib_open", [LIB])
symbol = brat_runtime._plugin("lib_sym", [handle, "pause_echo"])
result = []
started = threading.Event()


def active_call():
    started.set()
    result.append(brat_runtime._plugin("call", [symbol, "i64(i64)", 42]))


worker = threading.Thread(target=active_call)
worker.start()
check(started.wait(2), "plugin call thread did not start")
deadline = time.monotonic() + 2
while time.monotonic() < deadline:
    with brat_runtime._PLUGIN_LOCK:
        entry = brat_runtime._PLUGIN_LIBS.get(handle)
        active = entry["active"] if entry else 0
    if active:
        break
    time.sleep(0.001)
check(active == 1, "plugin call did not acquire its library")
brat_runtime._plugin("lib_close", [handle])
with brat_runtime._PLUGIN_LOCK:
    entry = brat_runtime._PLUGIN_LIBS.get(handle)
    check(entry is not None and entry["closing"], "active plugin close was not deferred")
worker.join(timeout=3)
check(not worker.is_alive() and result == [42], "active plugin call did not finish")
with brat_runtime._PLUGIN_LOCK:
    check(handle not in brat_runtime._PLUGIN_LIBS, "deferred plugin close did not finish")
try:
    brat_runtime._plugin("call", [symbol, "i64(i64)", 1])
except RuntimeError as error:
    check("call: no such function" in str(error), "stale interpreter symbol: %s" % error)
else:
    raise AssertionError("closed interpreter symbol remained callable")


def type_error(name, src, text):
    path = write(name + ".brat", src)
    for cmd in ("check", "build"):
        argv = [sys.executable, CLI, cmd, path]
        if cmd == "build":
            # No header: the C here is written by the test, and the message
            # under test is the checker's, so the build opts out of the proof.
            argv += ["--unsafe-ffi", "-o", os.path.join(TMP, name + ".exe")]
        r = subprocess.run(argv, capture_output=True, text=True, timeout=300, env=env)
        out = r.stdout + r.stderr
        check(r.returncode != 0 and text in out, "%s %s: rc=%d %r" % (name, cmd, r.returncode, out))


head = ('fn main() -> i64 {\n'
        '  let h = lib_open("%s")\n' % LIB)
type_error("count", head + '  let f = lib_sym(h, "add")\n  print(call(f, "i64(i64,i64)", 1))\n'
           '  return 0\n}\n', "call: signature wants 2 argument(s), got 1")
type_error("argtype", head + '  let f = lib_sym(h, "add")\n'
           '  print(call(f, "i64(i64,i64)", "x", 1))\n  return 0\n}\n',
           "call: arg 1 wants i64, got str")
type_error("rettype", head + '  let f = lib_sym(h, "add")\n'
           '  print(call(f, "vec(i64)", 1))\n  return 0\n}\n',
           "call: unsupported return type `vec`")
type_error("nosig", head + '  let f = lib_sym(h, "add")\n  let s = "i64(i64)"\n'
           '  print(call(f, s, 1))\n  return 0\n}\n', "call: signature must be a string literal")
type_error("bad", head + '  let f = lib_sym(h, "add")\n  print(call(f, "i64", 1))\n  return 0\n}\n',
           "call: bad signature `i64`")

print("PLUGIN ALL GREEN (%d asserts)" % n)
