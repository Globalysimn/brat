"""`brat bindgen` on C headers: the output must parse, check and run.

The names in the generated `extern fn` / `extern struct` are the C symbols and C
members, written into the generated C as is, so a name the BRAT parser reads as
a keyword cannot be renamed without breaking the link. Those declarations are
skipped with a comment that says why, and everything else still produces a
binding file that builds.
"""
import _cxx
import _scratch

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = _scratch.mkdtemp(prefix="brat-bindc-")

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


def cli(*args):
    if args and args[0] in ("build", "run", "emit-c", "dist") \
            and "-O0" not in args:
        args = [args[0], "--unsafe-ffi"] + list(args[1:])
    return subprocess.run(_cxx.cli_args(*(list(args))), capture_output=True,
                          text=True, timeout=300)


# `int` and `do` are legal C member names and fine as BRAT field names now, since
# the emitter writes them as `bf_int` / `bf_do`. `while` and `fn` are not: they
# are BRAT keywords, so the declaration is skipped rather than emitted broken.
HEADER = """
struct Point { int x; int do; };
struct while { int y; };
int fn(int a);
int puts(const char *s);
"""

header = write("surface.h", HEADER)
out = os.path.join(TMP, "surface.brat")
r = cli("bindgen", header, "-o", out)
check(r.returncode == 0, "bindgen rc=%d %r" % (r.returncode, r.stdout + r.stderr))
text = open(out, encoding="utf-8").read()

check("extern struct Point { x: i32, do: i32 }" in text,
      "Point should keep its C member names: %r" % text)
check("// skipped: while: a C struct name" in text, "the struct `while` should be skipped")
check("// skipped: fn: a C function name" in text, "the function `fn` should be skipped")
check("extern fn puts(s: str) -> i32;" in text, "puts should be generated: %r" % text)
check("extern fn fn(" not in text, "a keyword name must not reach the binding file")

rc = cli("check", out)
check(rc.returncode == 0, "generated bindings must check clean: %r" % (rc.stdout + rc.stderr))

# The generated names reach C as `bf_<name>`, so the build has to accept them and
# read the right member back.
SRC = text + """
fn main() -> i64 {
  let p = Point { x: 7, do: 35 }
  print(p.do + p.x)
  puts("bindgen c ok")
  return 0
}
"""
path = write("use.brat", SRC)
check(cli("check", path).returncode == 0, "the user of the bindings must check clean")
run_o0 = cli("run", path)
check(run_o0.returncode == 0 and run_o0.stdout == "42\nbindgen c ok\n",
      "run_o0: rc=%d out=%r" % (run_o0.returncode, run_o0.stdout + run_o0.stderr))
exe = os.path.join(TMP, "use.exe")
built = cli("build", path, "-o", exe)
check(built.returncode == 0, "build: %r" % (built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
check((native.returncode, native.stdout) == (run_o0.returncode, run_o0.stdout),
      "native %r vs run_o0 %r" % ((native.returncode, native.stdout),
                                  (run_o0.returncode, run_o0.stdout)))

# A type with no BRAT width must be skipped with a note, never bound as `ptr`:
# `long double` is 80-bit extended in 16 bytes here, so a `ptr` parameter would
# pass a 16-byte value where the callee reads an address.
LD_HDR = write("ld.h", "double ok(double x);\ndouble bad_param(long double x);\n"
                       "long double bad_ret(void);\n")
gen = cli("bindgen", LD_HDR)
check(gen.returncode == 0, "bindgen on long double: rc=%d %r"
      % (gen.returncode, gen.stdout + gen.stderr))
check("extern fn ok(" in gen.stdout, "the representable function must survive")
check("bad_param" in gen.stdout and "has no BRAT spelling" in gen.stdout,
      "an unrepresentable parameter must be skipped with a note: %r" % gen.stdout)
check("bad_ret" in gen.stdout and "has no BRAT spelling" in gen.stdout,
      "an unrepresentable result must be skipped with a note: %r" % gen.stdout)
check("ptr" not in gen.stdout.split("extern {")[1] if "extern {" in gen.stdout else True,
      "no `ptr` binding may be generated for `long double`: %r" % gen.stdout)

# Widths belong to the target: `long` is 8 bytes on LP64 and 4 on i686/Windows,
# while `long long` is 8 everywhere. A generator that ignores --target writes a
# binding that is wrong on the machine it was generated for.
WIDE_HDR = write("wide.h", "long f(long x);\nlong long g(long long y);\n"
                           "size_t h(size_t n);\n")
host = cli("bindgen", WIDE_HDR).stdout
check("extern fn f(x: i64) -> i64;" in host, "host `long` must be i64: %r" % host)
check("extern fn g(y: i64) -> i64;" in host, "`long long` is i64 everywhere: %r" % host)
check("extern fn h(n: usize) -> usize;" in host,
      "`size_t` must bind as usize, which C sizes for the target: %r" % host)
for triple in ("i686-linux-gnu", "x86_64-w64-windows-gnu"):
    out32 = cli("bindgen", WIDE_HDR, "--target", triple).stdout
    check("extern fn f(x: i32) -> i32;" in out32,
          "%s: `long` is 4 bytes there, want i32: %r" % (triple, out32))
    check("extern fn g(y: i64) -> i64;" in out32,
          "%s: `long long` stays 8 bytes: %r" % (triple, out32))
    check("extern fn h(n: usize) -> usize;" in out32,
          "%s: `size_t` stays usize: %r" % (triple, out32))


# One generator, not two. Before this, `brat bindgen` and `brat ffi --emit` were
# separate scanners and had silently diverged (`const char *` bound as `i32` by
# one of them); the only way that stays fixed is if both paths are the same code
# and their output is compared, not eyeballed.
SAME_HDR = write("same.h", "struct Point { int x; const char *name; };\n"
                            "int puts(const char *s);\n"
                            "double scale(double x, double k);\n"
                            "int opaque_ok(struct Point *p);\n")
by_bindgen = cli("bindgen", SAME_HDR).stdout
by_ffi = cli("ffi", "--emit", "--header", SAME_HDR).stdout
check(by_bindgen.strip() != "" and by_bindgen == by_ffi,
      "`bindgen` and `ffi --emit` must be one generator: %r vs %r"
      % (by_bindgen, by_ffi))
check("extern fn puts(s: str) -> i32;" in by_bindgen,
      "`const char *` is a `str`, never an `i32`: %r" % by_bindgen)
check("extern fn scale(x: f64, k: f64) -> f64;" in by_bindgen,
      "parameter names from the header must survive: %r" % by_bindgen)

# A header may legally spell a parameter name with a BRAT keyword, and a C
# prototype may omit names entirely. Both must still bind: the name is cosmetic,
# the slot is not, so an unusable name falls back to its position.
NAME_HDR = write("names.h", "int dup(int while, int fn);\nint anon(int, double);\n")
named = cli("bindgen", NAME_HDR).stdout
check("extern fn dup(a0: i32, a1: i32) -> i32;" in named,
      "keyword parameter names must fall back to `aN`: %r" % named)
check("extern fn anon(a0: i32, a1: f64) -> i32;" in named,
      "unnamed parameters must bind as `aN`: %r" % named)
check(cli("check", write("names_use.brat", named)).returncode == 0,
      "a fallback-named binding must still check clean")

# The merged path keeps the target: the width decision is the triple's, and a
# single generator means there is no second place for it to be forgotten.
check("extern fn f(x: i32) -> i32;" in cli("bindgen", WIDE_HDR,
                                           "--target", "i686-linux-gnu").stdout,
      "the merged generator must honour --target")

print("BINDGEN C ALL GREEN (%d asserts)" % n)
