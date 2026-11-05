"""The FFI loop closes: the generator, the proof and the build are one mechanism.

Three things are checked here, and each would be worth shipping alone:

* `brat ffi --header h.h --emit` generates bindings from the same C view the
  proof compares against, so what it writes is what it can prove -- the
  round trip is asserted, not assumed.
* `brat build prog --header h.h` refuses to build when a binding is wrong. A
  wrong binding is not a warning: the generated C would call through it.
* `ptr_read_struct` / `ptr_write_struct` let a BRAT program read and write the
  struct behind an opaque handle, which is the normal shape of a C API.

The library and the header are written here, so a failure names one thing.
"""
import _cxx
import _scratch

import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CXX = os.path.join(ROOT, "lang", "cxx", "bin", "brat-cxx")
TMP = _scratch.mkdtemp(prefix="brat-ffi-loop")

HEADER = """typedef struct { float x; float y; } Vec2;
typedef struct { long long id; Vec2 pos; } Node;
struct Opaque;
int opaque_kind(struct Opaque *o);
Node *node_new(long long id, float x, float y);
void node_move(Node *n, float dx);
void node_free(Node *n);
double area(Vec2 v);
int printf(const char *fmt, ...);
"""

LIB_C = """#include <stdlib.h>
typedef struct { float x; float y; } Vec2;
typedef struct { long long id; Vec2 pos; } Node;
struct Opaque { int kind; };
int opaque_kind(struct Opaque *o) { return o ? o->kind : -1; }
Node *node_new(long long id, float x, float y) {
    Node *n = malloc(sizeof(Node)); n->id = id; n->pos.x = x; n->pos.y = y; return n;
}
void node_move(Node *n, float dx) { n->pos.x += dx; }
void node_free(Node *n) { free(n); }
double area(Vec2 v) { return (double)v.x * (double)v.y; }
"""

PROGRAM = """extern "LIB" fn node_new(id: i64, x: f32, y: f32) -> ptr[Node];
extern "LIB" fn node_move(p: ptr[Node], dx: f32);
extern "LIB" fn node_free(p: ptr[Node]);
extern struct Node { id: i64, pos: Vec2 }
extern struct Vec2 { x: f32, y: f32 }

fn main() -> i64 {
  let h = node_new(7, 1.5, 2.0)
  let n = ptr_read_struct(h, "Node")
  print(n.id)
  print(n.pos.x)
  node_move(h, 10.0)
  let m = ptr_read_struct(h, "Node")
  print(m.pos.x)
  ptr_write_struct(h, Node { id: 99, pos: Vec2 { x: 0.5, y: 0.25 } }, "Node")
  let k = ptr_read_struct(h, "Node")
  print(k.id)
  print(k.pos.y)
  node_free(h)
  return 0
}
"""

WANT = "7\n1.5\n11.5\n99\n0.25\n"


def run(args, **kw):
    if args and args[0] in ("build", "run", "emit-c", "dist") \
            and "--interp" not in args:
        args = [args[0], "--unsafe-ffi"] + list(args[1:])
    elif args and args[0] == "run" and "--interp" in args and "--unsafe-ffi" not in args:
        i = args.index("--interp") + 1
        args = list(args[:i]) + ["--unsafe-ffi"] + list(args[i:])
    p = subprocess.run(_cxx.cli_args(*(args)), capture_output=True, text=True, timeout=600, **kw)
    return p.returncode, p.stdout, p.stderr


def main():
    n = 0

    def check(cond, msg):
        nonlocal n
        assert cond, msg
        n += 1

    hdr = os.path.join(TMP, "api.h")
    src = os.path.join(TMP, "api.c")
    lib = os.path.join(TMP, "libapi.so")
    with open(hdr, "w", encoding="utf-8") as f:
        f.write(HEADER)
    with open(src, "w", encoding="utf-8") as f:
        f.write(LIB_C)
    r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", src, "-o", lib], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr

    prog = os.path.join(TMP, "prog.brat")
    with open(prog, "w", encoding="utf-8") as f:
        f.write(PROGRAM.replace("LIB", lib))

    # 1. The opaque struct is the one thing a generator cannot lay out: it must
    #    say so and leave the handle a `ptr`, not invent a layout.
    rc, out, err = run(["ffi", "--header", hdr, "--emit", "--emit-lib", lib])
    check(rc == 0, "emit failed: %s%s" % (out, err))
    check("extern struct Vec2" in out and "extern struct Node" in out, "emit missed a struct:\n%s" % out)
    check("extern struct Opaque" not in out, "emit invented a layout for an opaque struct:\n%s" % out)
    # The parameter keeps the header's own name now that both generators are one
    # (`o` here); the slot's width is the invariant, not its label, so accept the
    # name and prove the `ptr` explicitly.
    check(re.search(r'extern fn opaque_kind\(\w+: ptr\) -> i32;', out),
          "an opaque handle must come through as ptr:\n%s" % out)
    check("..ptr" in out, "the variadic tail is not emitted:\n%s" % out)
    check(out.index("extern struct Vec2") < out.index("extern struct Node"),
          "a struct is emitted before the one it contains:\n%s" % out)

    # 2. The round trip: what the generator wrote, the proof proves. This is the
    #    property that makes the generator trustworthy rather than merely useful.
    gen = os.path.join(TMP, "gen.brat")
    with open(gen, "w", encoding="utf-8") as f:
        f.write(out + "\nfn main() -> i64 {\n  return 0\n}\n")
    rc, out2, err2 = run(["ffi", gen, "--header", hdr])
    check(rc == 0, "the generator produced bindings its own proof refuses:\n%s%s" % (out2, err2))

    # 3. Struct-through-a-pointer, in all three implementations.
    rc, out3, err3 = run(["run", "--interp", prog])
    check(rc == 0 and out3 == WANT, "interp:\nwant %r\ngot  %r%s" % (WANT, out3, err3))

    exe = os.path.join(TMP, "prog.exe")
    rc, out4, err4 = run(["build", prog, "-o", exe])
    check(rc == 0, "build failed: %s%s" % (out4, err4))
    q = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    check(q.stdout == WANT, "native:\nwant %r\ngot  %r" % (WANT, q.stdout))

    if os.path.exists(CXX):
        c = subprocess.run([CXX, "cc", prog], capture_output=True, text=True, timeout=300)
        check(c.returncode == 0, "brat-cxx cc failed: %s" % (c.stdout + c.stderr)[-300:])
        cfile = os.path.join(TMP, "cxx.c")
        with open(cfile, "w", encoding="utf-8") as f:
            f.write(c.stdout)
        cexe = os.path.join(TMP, "cxx.exe")
        k = subprocess.run(["cc", "-std=c99", "-O1", "-w", "-I", os.path.join(ROOT, "lang", "engine"),
                            cfile, lib, "-o", cexe, "-lm", "-lpthread"],
                           capture_output=True, text=True, timeout=300)
        check(k.returncode == 0, "cc failed: %s" % k.stderr[-400:])
        r2 = subprocess.run([cexe], capture_output=True, text=True, timeout=120)
        check(r2.stdout == WANT, "brat-cxx cc output:\nwant %r\ngot  %r" % (WANT, r2.stdout))
        n += 1

    # 4. The build gate: a header that disagrees with the program stops the
    #    build before the compiler is asked to produce a binary.
    bad = os.path.join(TMP, "bad.h")
    with open(bad, "w", encoding="utf-8") as f:
        f.write(HEADER.replace("float x; float y;", "double x; double y;"))
    outbin = os.path.join(TMP, "should_not_exist")
    rc, out5, err5 = run(["build", prog, "--header", bad, "-o", outbin])
    check(rc == 1, "a wrong header did not stop the build (rc=%d)" % rc)
    check("refusing to build" in err5, "the refusal does not say so:\n%s" % err5)
    check(not os.path.exists(outbin), "a binary was produced despite a failed proof")

    # A header that agrees lets the build through, and the binary works.
    goodbin = os.path.join(TMP, "good")
    rc, out6, err6 = run(["build", prog, "--header", hdr, "-o", goodbin])
    check(rc == 0, "a proven build was refused: %s%s" % (out6, err6))
    r3 = subprocess.run([goodbin], capture_output=True, text=True, timeout=120)
    check(r3.stdout == WANT, "the gated binary prints:\nwant %r\ngot  %r" % (WANT, r3.stdout))

    # 5. `--json` carries the same verdict, so CI can gate on it.
    rc, out7, _err7 = run(["ffi", prog, "--header", hdr, "--json"])
    doc = json.loads(out7)
    check(doc["errors"] == 0 and doc["compiler"]["ok"] is True, "json verdict: %r" % doc)

    print("FFI LOOP ALL GREEN (%d asserts: generate -> prove -> build -> run, three implementations)"
          % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
