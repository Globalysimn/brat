"""Six defects found by an outside differential run, each pinned by a case.

Every case here failed on the tree of 2026-10-30 and passes after the fix. Three
of them were invisible to the suite because it compares two implementations that
were both wrong the same way, which is the blind spot `LEDGER.md` names:

* `move x` as a call argument retained an extra reference, so the value leaked
  once per call and without bound inside a loop (1000 passes: 3000 allocations,
  188890 bytes). Both emitters emitted the same wrong `vrc_inc(vmove(&b))`, so
  the byte-identity test could not see it.
* `-> bool` on a fast function was dropped on the way back: the raw C body
  answered 1/0, the wrapper built `vint(1)`, and the program printed `1` where
  the interpreter printed `True`, with `0 error(s)` from both checkers.
* an `i64v` wrapper checked the argument's tag and not its elements, so
  `total([1, "a"])` ran a raw body over a `str` and raised `vec elem not int`
  where the interpreter printed `1a`.
* `v[0].x` read a field out of a receiver nobody owned and released it never
  (4 allocations per program), and `v[0].x = 5` stored through the same kind of
  reference.
* a moved value reaching a printer printed nothing, because `V_MOVED` was not
  handled in four exhaustive switches (`-Wswitch` was the only witness).
* the emitted C carried 82-86 `-Wall -Wextra` warnings, so the README's
  "compiles as C99 without warnings" was false and untested.

Each case runs on both emitters (`brat-cxx cc` and `codegen_c.emit_file`),
compares the interpreter against the native binary, and compiles with ASan so a
leak or a use-after-free fails here.
"""

import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")
TMP = _scratch.mkdtemp(prefix="brat-review-")

# name: (source, expected stdout, must be LSan clean)
CASES = {
    "move_arg_str": ("""fn eat(s: move str) -> i64 { return len(s) }
fn main() -> i64 {
  let s = "abc" + "d"
  print(eat(move s))
  return 0
}
""", "4\n", True),
    "move_arg_vec_in_loop": ("""fn eat(v: move vec[str]) -> i64 { return len(v) }
fn main() -> i64 {
  let mut i = 0
  let mut t = 0
  while i < 200 {
    let v = ["a{i}"]
    t += eat(move v)
    i += 1
  }
  print(t)
  return 0
}
""", "200\n", True),
    "move_arg_tag": ("""fn eat(p: move P) -> i64 { return p.x }
fn main() -> i64 {
  let p = P { x: 1, y: "two" }
  print(eat(move p))
  return 0
}
""", "1\n", True),
    # The interpreter is the oracle: it prints `True`, so the fast path may not
    # answer `1`.
    "fast_bool_prints_true": ("""fn is_pos(a: i64) -> bool { return a > 0 }
fn main() -> i64 {
  print(is_pos(1))
  print("got {is_pos(5)}")
  return 0
}
""", "True\ngot True\n", False),
    "fast_bool_f64_params": ("""fn pos(x: f64) -> bool { return x > 0.0 }
fn main() -> i64 {
  print(pos(1.0))
  print(pos(-2.0))
  return 0
}
""", "True\nFalse\n", False),
    # A `vec[i64]` that holds a float is legal in a dynamic language (`mk()` is
    # unannotated, so its vector is `dyn` and the checker cannot see it): both
    # sides have to answer the same bytes, so the wrapper falls back to the
    # general path instead of raising `vec elem not int` from the raw body.
    "i64v_float_element": ("""fn total(v: vec[i64]) -> i64 {
  let mut s = 0
  for x in v { s += x }
  return s
}
fn mk() { return [1.5, 2.5] }
fn main() -> i64 {
  print(total([1, 2, 3]))
  print(total(mk()))
  return 0
}
""", "6\n4.0\n", False),
    "field_of_element": ("""fn main() -> i64 {
  let v = [P { x: 1 }]
  print(v[0].x)
  return 0
}
""", "1\n", True),
    "store_into_field_of_element": ("""fn main() -> i64 {
  let mut v = [P { x: 1 }]
  v[0].x = 5
  print(v[0].x)
  print(v[0])
  return 0
}
""", "5\n{'$type': 'P', 'x': 5}\n", True),
    "field_of_call_result": ("""fn mk() -> P { return P { x: 7 } }
fn main() -> i64 {
  print(mk().x)
  return 0
}
""", "7\n", True),
}

WARN_FLAGS = ["-std=c99", "-Wall", "-Wextra"]


def run(cmd, env=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600,
                          env=dict(os.environ, **(env or {})))


def emit(kind, path):
    if kind == "cxx":
        r = run([sys.executable, CLI, "emit-c", path])
    else:
        code = ("import sys; sys.path.insert(0, %r); import codegen_c; "
                "sys.stdout.write(codegen_c.emit_file(%r))" % (ENGINE, path))
        r = run([sys.executable, "-c", code])
    assert r.returncode == 0, "%s emit: %r" % (kind, (r.stdout + r.stderr)[-800:])
    return r.stdout


bad = 0
for name in sorted(CASES):
    src, want, clean = CASES[name]
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)

    interp = run([sys.executable, CLI, "run", "--interp", path])
    if interp.stdout != want:
        print("FAIL %s: interpreter %r, want %r" % (name, interp.stdout, want))
        bad += 1
        continue

    for kind in ("cxx", "py"):
        cfile = os.path.join(TMP, "%s.%s.c" % (name, kind))
        with open(cfile, "w", encoding="utf-8") as f:
            f.write(emit(kind, path))

        # The README promises C99 without warnings: the emitted program is the
        # only thing compiled here, so any warning is this compiler's.
        w = run(["cc"] + WARN_FLAGS + ["-I", ENGINE, "-c", cfile, "-o", os.devnull])
        if w.returncode != 0 or w.stderr.strip():
            print("FAIL %s/%s: emitted C is not warning-free:\n%s"
                  % (name, kind, w.stderr[-800:]))
            bad += 1

        exe = os.path.join(TMP, "%s.%s.bin" % (name, kind))
        cc = run(["cc", "-fsanitize=address", "-g", "-I", ENGINE, cfile,
                  "-o", exe, "-lm"])
        if cc.returncode != 0:
            print("FAIL %s/%s: cc: %r" % (name, kind, (cc.stdout + cc.stderr)[-800:]))
            bad += 1
            continue
        r = run([exe], {"ASAN_OPTIONS": "detect_leaks=1:exitcode=0"})
        out = r.stdout + r.stderr
        if "AddressSanitizer" in out:
            print("FAIL %s/%s: not memory clean:\n%s" % (name, kind, out[-900:]))
            bad += 1
            continue
        if r.stdout != want:
            print("FAIL %s/%s: native %r, want %r" % (name, kind, r.stdout, want))
            bad += 1

if bad:
    print("REVIEW REGRESSIONS: %d case(s) failed" % bad)
    sys.exit(1)
print("REVIEW REGRESSIONS ALL GREEN (%d cases x 2 emitters, LSan clean, "
      "interpreter == native, no -Wall warnings)" % len(CASES))
