"""A binary says what was proved about its FFI, and can be re-proved later.

Every other toolchain forgets the proof. rustc, cgo, ctypes and bindgen compare
the declarations once, at build time, if you ask them to; the finished executable
carries no record of what was compared, so a library that drifted, or a source
that was edited after the build, is found by the bug it causes.

`brat build` writes the extern surface into the program as data -- every
declaration, every struct size and field offset, the target's widths, and the
sha256 of the header the declarations were proved against -- and
`brat ffi --bin FILE` reads it back. With the source in hand it re-proves the
recorded contract against the declarations as they are now and names each
difference; without it, it reports what the binary claims.
"""

import _scratch

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _cxx
BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-fficontract-")
LIB = os.path.join(TMP, "libprobe.so")

HEADER = """typedef struct { long id; double weight; } item_t;
int item_weight(item_t *it, double *out);
double item_total(item_t *items, int n);
"""

C_SRC = """#include "probe.h"
int item_weight(item_t *it, double *out) { *out = it->weight; return 0; }
double item_total(item_t *items, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += items[i].weight;
    return s;
}
"""

SRC = """extern "libprobe.so" struct item_t { id: i64, weight: f64 }
extern "libprobe.so" fn item_weight(it: ptr[item_t], out: ptr[f64]) -> i32;
extern "libprobe.so" fn item_total(items: ptr[item_t], n: i32) -> f64;
fn main() -> i64 {
  print("ok")
  return 0
}
"""


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600,
                          cwd=TMP, **kw)


with open(os.path.join(TMP, "probe.h"), "w", encoding="utf-8") as f:
    f.write(HEADER)
with open(os.path.join(TMP, "probe.c"), "w", encoding="utf-8") as f:
    f.write(C_SRC)
cc = run(["cc", "-shared", "-fPIC", "-O2", "probe.c", "-o", LIB])
assert cc.returncode == 0, "cc: %r" % (cc.stdout + cc.stderr)[-800:]

src = os.path.join(TMP, "prog.brat")
with open(src, "w", encoding="utf-8") as f:
    f.write(SRC)

env = dict(os.environ, LD_LIBRARY_PATH=TMP, LIBRARY_PATH=TMP)
exe = os.path.join(TMP, "prog.exe")

# the binary exists, runs, and carries its contract
built = run([BIN, "build", "--unsafe-ffi", "prog.brat", "--header", "probe.h",
             "-o", "prog.exe"], env=env)
assert built.returncode == 0, "build: %r" % (built.stdout + built.stderr)[-800:]
ran = subprocess.run([exe], capture_output=True, text=True, timeout=120)
assert ran.stdout == "ok\n", "the binary must still run: %r" % ran.stdout

with open(exe, "rb") as f:
    blob = f.read()
assert b"BRATFFI1 {" in blob, "the binary carries no contract"

q = run([BIN, "ffi", "--bin", "prog.exe", "--json"])
assert q.returncode == 0, "brat ffi --bin: %r" % (q.stdout + q.stderr)[-800:]
contract = json.loads(q.stdout)["contract"]
assert [f["name"] for f in contract["fns"]] == ["item_total", "item_weight"], contract["fns"]
assert contract["fns"][0]["params"] == ["ptr[item_t]", "i32"], contract["fns"][0]
assert contract["fns"][1]["params"] == ["ptr[item_t]", "ptr[f64]"], contract["fns"][1]
assert contract["fns"][0]["ret"] == "f64", contract["fns"][0]
item = contract["structs"][0]
assert (item["name"], item["size"], item["align"]) == ("item_t", 16, 8), item
assert [(f["name"], f["offset"]) for f in item["fields"]] == [("id", 0), ("weight", 8)], item
assert contract["proved"]["sha256"], "a build with --header records the header it proved"
assert contract["target"]["ptr"] == 8 and contract["target"]["long"] == 8, contract["target"]

same = run([BIN, "ffi", "prog.brat", "--header", "probe.h",
            "--bin", "prog.exe"], env=env)
assert same.returncode == 0, "a match must exit 0: %r" % (same.stdout + same.stderr)[-800:]
assert "contract matches" in same.stdout, same.stdout

# a different declaration in the source is named, not shrugged off
with open(os.path.join(TMP, "prog_bad.brat"), "w", encoding="utf-8") as f:
    f.write(SRC.replace("n: i32", "n: i64"))
bad = run([BIN, "ffi", "prog_bad.brat", "--header", "probe.h",
           "--bin", "prog.exe"], env=env)
assert bad.returncode == 1, "drift must exit 1: %r" % (bad.stdout + bad.stderr)[-800:]
assert "item_total: recorded f64(ptr[item_t], i32), now f64(ptr[item_t], i64)" in bad.stdout, bad.stdout

# a header that changed under the binary is named too
with open(os.path.join(TMP, "probe_bad.h"), "w", encoding="utf-8") as f:
    f.write(HEADER.replace("double weight", "float weight"))
drift = run([BIN, "ffi", "prog.brat", "--header", "probe_bad.h",
             "--bin", "prog.exe"], env=env)
assert drift.returncode == 1, "a changed header must exit 1: %r" % (drift.stdout + drift.stderr)[-800:]
assert "header sha256" in drift.stdout, drift.stdout

# a struct that would move is named per field
with open(os.path.join(TMP, "prog_moved.brat"), "w", encoding="utf-8") as f:
    f.write(SRC.replace("{ id: i64, weight: f64 }", "{ weight: f64, id: i64 }"))
moved = run([BIN, "ffi", "prog_moved.brat", "--header", "probe.h",
             "--bin", "prog.exe"], env=env)
assert moved.returncode == 1, "a moved field must exit 1: %r" % (moved.stdout + moved.stderr)[-800:]
assert "extern struct item_t.id: recorded" in moved.stdout or "item_t.weight: recorded" in moved.stdout, moved.stdout

# a binary with no extern surface carries no contract, and says so
with open(os.path.join(TMP, "plain.brat"), "w", encoding="utf-8") as f:
    f.write('fn main() -> i64 {\n  print("hi")\n  return 0\n}\n')
plain = run([BIN, "build", "--unsafe-ffi", "plain.brat", "-o", "plain.exe"], env=env)
assert plain.returncode == 0, "plain build: %r" % (plain.stdout + plain.stderr)[-800:]
empty = run([BIN, "ffi", "--bin", "plain.exe"])
assert empty.returncode == 1, "a binary without a contract must be reported, not passed"
assert "carries no FFI contract" in empty.stderr, empty.stderr

# the native driver appends exactly one contract block to the emitted C
emitted = run([BIN, "emit-c", "--unsafe-ffi", "prog.brat"])
assert emitted.returncode == 0, "emit-c: %r" % (emitted.stdout + emitted.stderr)[-800:]
assert emitted.stdout.count("BRATFFI1 {") == 1, "exactly one contract block"
_body, block = emitted.stdout.rsplit("\n/* BRAT FFI contract", 1)
assert block.count("static const char brat_ffi_contract[]") == 1, block
plain_c = run([BIN, "emit-c", "--unsafe-ffi", "plain.brat"])
assert "BRATFFI1" not in plain_c.stdout, "no extern surface, no contract block"

print("FFI CONTRACT ALL GREEN (embedded, re-proved, drift named, native emitter)")
