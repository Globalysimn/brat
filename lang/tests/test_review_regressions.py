"""Emitter and runtime ownership tests: retained local aliases, closure-call results, field extraction from owned receivers, and scalar-annotated returns under sanitizers."""
import _cxx

import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
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
    # The -O0 build is the oracle: it prints `True`, so the fast path may not
    # answer `1`.
    "fast_bool_prints_true": ("""fn is_pos(a: i64) -> bool { return a > 0 }
fn main() -> i64 {
  print(is_pos(1))
  print("got {is_pos(5)}")
  return 0
}
""", "true\ngot true\n", False),
    "fast_bool_f64_params": ("""fn pos(x: f64) -> bool { return x > 0.0 }
fn main() -> i64 {
  print(pos(1.0))
  print(pos(-2.0))
  return 0
}
""", "true\nfalse\n", False),
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
    # A local alias takes a reference in the emitted C and must own/release it;
    # marking the binding as borrowed leaked the vector at every call.
    "local_alias_released": ("""fn alias_len() -> i64 {
  let values = [1, 2, 3, 4]
  let alias = values
  return len(alias)
}
fn main() -> i64 {
  print(alias_len())
  return 0
}
""", "4\n", True),
    # Calling a closure through a local variable returns an owned Val even when
    # its source-level return is dynamic. Both discarded and bound results must
    # be released without freeing the closure's environment.
    "closure_call_result_released": ("""let append = fn(x) { return x + "abcdefghijklmnop" }
print(append("hi_"))
let result = append("yo_")
print(result)
""", "hi_abcdefghijklmnop\nyo_abcdefghijklmnop\n", True),
    # Returning a field out of an owned local consumes the container reference;
    # the exit epilogue must not release that already-consumed reference again.
    "field_return_consumes_receiver_once": ("""fn get_s() -> str {
  let box = Box { s: "this_is_a_long_heap_string_over_14_bytes" }
  return box.s
}
fn main() -> i64 {
  print(get_s())
  return 0
}
""", "this_is_a_long_heap_string_over_14_bytes\n", True),
    # A dynamic heap value may escape a function whose annotation says i64.
    # Return ownership must follow the value actually transferred, not just the
    # annotation, so repeated calls remain leak-free.
    "typed_return_dynamic_heap_released": ("""fn total(values) -> i64 {
  let mut sum = 0
  for value in values { sum += value }
  return sum
}
fn main() -> i64 {
  let mut i = 0
  while i < 16 {
    print(total([1, "a"]))
    i += 1
  }
  return 0
}
""", "1a\n" * 16, True),
}

WARN_FLAGS = ["-std=c99", "-Wall", "-Wextra"]


def run(cmd, env=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600,
                          env=dict(os.environ, **(env or {})))


def emit(kind, path):
    r = run(_cxx.cli_args("emit-c", path))
    assert r.returncode == 0, "%s emit: %r" % (kind, (r.stdout + r.stderr)[-800:])
    return r.stdout


bad = 0
for name in sorted(CASES):
    src, want, clean = CASES[name]
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)

    run_o0 = run(_cxx.cli_args("run", "-O0", path))
    if run_o0.stdout != want:
        print("FAIL %s: -O0 %r, want %r" % (name, run_o0.stdout, want))
        bad += 1
        continue

    for kind in ("cxx",):
        cfile = os.path.join(TMP, "%s.%s.c" % (name, kind))
        with open(cfile, "w", encoding="utf-8") as f:
            f.write(emit(kind, path))

        # The README names C11; the output is kept warning-free under the
        # stricter `-std=c99` as well. The emitted program is the only thing
        # compiled here, so any warning is this compiler's.
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
print("REVIEW REGRESSIONS ALL GREEN (%d cases, LSan clean, "
      "-O0 == native, no -Wall warnings)" % len(CASES))
