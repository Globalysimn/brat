"""FFI buffers: (ptr, len) in, checked slice over C memory, NUL and UTF-8 refused."""
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-buf-")
LIB = os.path.join(TMP, "libbuf.so")

with open(os.path.join(TMP, "libbuf.c"), "w", encoding="utf-8") as f:
    f.write(
        'char *plain(void) { return "hello"; }\n'
        'char *with_nul(void) { static char b[6] = {\'a\', \'b\', 0, \'c\', \'d\', 0}; return b; }\n'
        'char *bad_utf8(void) { static char b[3] = {(char)0xC3, \'(\', 0}; return b; }\n'
        'long long sum_i64(long long *p, long long n) { long long s = 0, i;\n'
        '  for (i = 0; i < n; i++) s += p[i]; return s; }\n'
        'void fill_f64(double *p, long long n) { long long i; for (i = 0; i < n; i++) p[i] = (double)i + 0.5; }\n')
r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", os.path.join(TMP, "libbuf.c"), "-o", LIB],
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


env = dict(os.environ, LD_LIBRARY_PATH=TMP)

SRC = (
    'extern "%s" fn plain() -> ptr;\n'
    'extern "%s" fn with_nul() -> ptr;\n'
    'extern "%s" fn bad_utf8() -> ptr;\n'
    'extern "%s" fn sum_i64(p: ptr, n: i64) -> i64;\n'
    'extern "%s" fn fill_f64(p: ptr, n: i64);\n'
    'extern fn malloc(n: i64) -> move ptr;\n'
    'extern fn free(move p: ptr);\n'
    '\n'
    'fn main() -> i64 {\n'
    '  print(str_from_ptr(plain(), 5));\n'
    '  print(str_from_ptr(plain(), 0));\n'
    '  let nums: ptr[i64] = malloc(32);\n'
    '  let s: slice[i64] = slice_of_ptr(nums, 4, "i64");\n'
    '  slice_set(s, 0, 1);\n'
    '  slice_set(s, 1, 2);\n'
    '  slice_set(s, 2, 3);\n'
    '  slice_set(s, 3, 4);\n'
    '  print(sum_i64(nums, slice_len(s)));\n'
    '  free(nums);\n'
    '  let d: ptr[f64] = malloc(24);\n'
    '  fill_f64(d, 3);\n'
    '  let ds: slice[f64] = slice_of_ptr(d, 3, "f64");\n'
    '  print(slice_get(ds, 2));\n'
    '  let mut total = 0.0;\n'
    '  let mut i = 0;\n'
    '  while i < 3 {\n'
    '    total = total + slice_get(ds, i);\n'
    '    i = i + 1;\n'
    '  }\n'
    '  print(total);\n'
    '  free(d);\n'
    '  try {\n'
    '    print(str_from_ptr(with_nul(), 5));\n'
    '  } catch e {\n'
    '    print(e);\n'
    '  }\n'
    '  try {\n'
    '    print(str_from_ptr(bad_utf8(), 2));\n'
    '  } catch e {\n'
    '    print(e);\n'
    '  }\n'
    '  try {\n'
    '    print(str_from_ptr(ptr_null(), 3));\n'
    '  } catch e {\n'
    '    print(e);\n'
    '  }\n'
    '  return 0;\n'
    '}\n') % (LIB, LIB, LIB, LIB, LIB)

ok = write("ok.brat", SRC)
want = ("hello\n"
        "\n"
        "10\n"
        "2.5\n"
        "4.5\n"
        "str_from_ptr: NUL byte at 2\n"
        "str_from_ptr: invalid UTF-8\n"
        "null pointer dereference\n")

interp = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", ok], capture_output=True, text=True,
                        timeout=120, env=env)
check(interp.returncode == 0 and interp.stdout == want,
      "run rc=%d out=%r" % (interp.returncode, interp.stdout + interp.stderr))

exe = os.path.join(TMP, "ok.exe")
built = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", ok, "-o", exe], capture_output=True,
                       text=True, timeout=300, env=env)
check(built.returncode == 0, "build: %r" % (built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60, env=env)
check(native.returncode == 0 and native.stdout == want,
      "native rc=%d out=%r" % (native.returncode, native.stdout + native.stderr))


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
        check(r.returncode != 0 and text in out, "%s %s: rc=%d out=%r" % (name, cmd, r.returncode, out))


head = 'extern fn malloc(n: i64) -> ptr;\n'
type_error("kind", head +
           'fn main() -> i64 {\n'
           '  let p: ptr[i64] = malloc(8);\n'
           '  let s = slice_of_ptr(p, 1, "f16");\n'
           '  return 0;\n'
           '}\n', 'slice_of_ptr: kind must be the literal "i64", "i32", "f64", "f32" or "u8"')

# A byte view of an integer pointer is a reinterpretation and stays legal;
# asking for bytes of a float pointer is a mix-up and is not.
type_error("byteptr", head +
           'fn main() -> i64 {\n'
           '  let p: ptr[f64] = malloc(8);\n'
           '  let s = slice_of_ptr(p, 1, "u8");\n'
           '  return 0;\n'
           '}\n', 'slice_of_ptr: want ptr[u8], got ptr[f64]')

type_error("ptrmix", head +
           'fn main() -> i64 {\n'
           '  let p: ptr[i64] = malloc(8);\n'
           '  let s = slice_of_ptr(p, 1, "f64");\n'
           '  return 0;\n'
           '}\n', "slice_of_ptr: want ptr[f64], got ptr[i64]")

type_error("elemmix", head +
           'fn main() -> i64 {\n'
           '  let p: ptr[i64] = malloc(8);\n'
           '  let s = slice_of_ptr(p, 1, "i64");\n'
           '  slice_set(s, 0, 1.5);\n'
           '  return 0;\n'
           '}\n', "slice_set: want i64, got f64")

RUNTIME = (
    'extern fn malloc(n: i64) -> move ptr;\n'
    'extern fn free(move p: ptr);\n'
    '\n'
    'fn main() -> i64 {\n'
    '  let p: ptr[i64] = malloc(8);\n'
    '  let s = slice_of_ptr(p, 1, "i64");\n'
    '  %s\n'
    '  free(p);\n'
    '  return 0;\n'
    '}\n')

for name, stmt, text in (
        ("neg_n", 'let t = slice_of_ptr(p, -1, "i64");\n  print(slice_len(t));',
         "slice_of_ptr: n must be >= 0"),
        ("oob", 'print(slice_get(s, 1));', "slice index 1 out of range 0..1"),
        ("null", 'let q: ptr[i64] = ptr_null();\n  let t = slice_of_ptr(q, 4, "i64");\n  print(slice_len(t));',
         "null pointer dereference")):
    path = write(name + ".brat", RUNTIME % stmt)
    ri = subprocess.run([sys.executable, CLI, "run", "--interp", "--unsafe-ffi", path], capture_output=True, text=True,
                        timeout=120, env=env)
    out = ri.stdout + ri.stderr
    check(ri.returncode != 0 and text in out, "%s run: rc=%d out=%r" % (name, ri.returncode, out))
    exe = os.path.join(TMP, name + ".exe")
    b = subprocess.run([sys.executable, CLI, "build", "--unsafe-ffi", path, "-o", exe], capture_output=True,
                       text=True, timeout=300, env=env)
    check(b.returncode == 0, "%s build: %r" % (name, b.stdout + b.stderr))
    rn = subprocess.run([exe], capture_output=True, text=True, timeout=60, env=env)
    out = rn.stdout + rn.stderr
    check(rn.returncode != 0 and text in out, "%s native: rc=%d out=%r" % (name, rn.returncode, out))

print("FFI BUFFER ALL GREEN (%d asserts)" % n)
