"""`s += f()` releases the value `f` handed back.

`vadd_self(a, b)` copies `b` into `a` and keeps its own reference to `a`; it does
not take `b` over. The emitter bound the right side to a temp and released that
temp only on the error path (`if (verr_pending()) { vdrop_temp(_c4); ... }`), so
every append of a call or of a `+` result leaked one allocation per iteration --
12028 bytes over 181 allocations for `lang/examples/csv.brat`.

The three shapes below are the ones that leaked: a call in a loop, a
concatenation in a loop, and a single append. Both emitters are compiled for each
one: the C++ front end (`emit-c`) and the Python one (`codegen_c.emit_file`),
because the two are separate code paths and only one of them was ever exercised.
"""

import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")
TMP = _scratch.mkdtemp(prefix="brat-appendlk-")

CASES = {
    # A call in the loop: the callee builds a string, the append copies it.
    "call_in_loop": ("""fn q(f: str) -> str {
  return "[" + f + "]"
}
fn main() -> i64 {
  let mut out = ""
  let items = ["a", "b", "c"]
  for x in items {
    out += q(x)
  }
  print(out)
  return 0
}
""", "[a][b][c]\n"),
    # A `+` result in the loop: the temp belongs to the concatenation.
    "concat_in_loop": ("""fn main() -> i64 {
  let mut out = ""
  let parts = ["x", "y", "z"]
  for seg in parts {
    out += "/" + seg
  }
  print(out)
  return 0
}
""", "/x/y/z\n"),
    # The smallest form: one append and nothing else.
    "single_append": ("""fn q(f: str) -> str {
  return "[" + f + "]"
}
fn main() -> i64 {
  let mut out = ""
  out += q("x")
  print(out)
  return 0
}
""", "[x]\n"),
}


def run(cmd, env=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600,
                          env=dict(os.environ, **(env or {})))


def emit(kind, path):
    """The emitted C for `path` from the named emitter."""
    if kind == "cxx":
        r = run([sys.executable, CLI, "emit-c", path])
        assert r.returncode == 0, "%s emit-c: %r" % (kind, (r.stdout + r.stderr)[-800:])
        return r.stdout
    code = ("import sys; sys.path.insert(0, %r); import codegen_c; "
            "sys.stdout.write(codegen_c.emit_file(%r))" % (ENGINE, path))
    r = run([sys.executable, "-c", code])
    assert r.returncode == 0, "%s emit: %r" % (kind, (r.stdout + r.stderr)[-800:])
    return r.stdout


bad = 0
for name in sorted(CASES):
    src, want = CASES[name]
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)

    interp = run([sys.executable, CLI, "run", "--interp", path])
    if interp.stdout != want:
        print("FAIL %s: interpreter %r, want %r" % (name, interp.stdout, want))
        bad += 1

    for kind in ("cxx", "py"):
        cfile = os.path.join(TMP, "%s.%s.c" % (name, kind))
        with open(cfile, "w", encoding="utf-8") as f:
            f.write(emit(kind, path))
        exe = os.path.join(TMP, "%s.%s.asan" % (name, kind))
        cc = run(["cc", "-fsanitize=address", "-g", "-I", ENGINE, cfile,
                  "-o", exe, "-lm"])
        if cc.returncode != 0:
            print("FAIL %s/%s: cc: %r" % (name, kind, (cc.stdout + cc.stderr)[-800:]))
            bad += 1
            continue
        r = run([exe], {"ASAN_OPTIONS": "detect_leaks=1:exitcode=0"})
        out = r.stdout + r.stderr
        if "AddressSanitizer" in out:
            print("FAIL %s/%s: not clean: %s" % (name, kind, out[-900:]))
            bad += 1
        elif r.stdout != want:
            print("FAIL %s/%s: native %r, want %r" % (name, kind, r.stdout, want))
            bad += 1

if bad:
    print("APPEND LEAK: %d case(s) failed" % bad)
    sys.exit(1)
print("APPEND LEAK ALL GREEN (%d shapes x 2 emitters, LSan clean, interpreter == native)"
      % len(CASES))
