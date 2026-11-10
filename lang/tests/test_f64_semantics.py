"""f64 semantics: a float stays a float, and `-> f64` means f64.

Two divergences, both found by differential fuzzing rather than by the corpus:

  * In an `f64` fast function the emitter printed a float literal through
    `%.17g`, which turns `1e10` into `10000000000` - an *integer* constant in C.
    `1e10 * 1e10` then multiplied two int64s and wrapped (7.76e18) while the
    -O0 multiplied two doubles (1e20). The literal now keeps its point.
    Integer literals keep theirs off: `1 / 2` inside an `f64` function is integer
    division in both backends (`0`), because the -O0 build treats an
    integer-only subexpression as an integer and SPEC §10 says so.
  * `fn f() -> f64 { return 3 }` printed `3` from the -O0 build and `3.0` from
    the C backend, whose fast-path signature is `double`. Fixed in the native
    emitter rather than by coercing values: a `f64` function whose every `return` is
    an integer stays on the tagged-union path, so the integer survives `print`.
    Coercing the -O0 build instead would have been the wrong half -- `return
    1 / 2` in an `f64` function is integer division by SPEC §10, and it still
    is. Both halves are pinned below.

    The same rule is what closes the *mixed* case (2026-10-30): a function that
    returns an integer on one branch and a float on another also stays on the
    tagged-union path, because the fast-path filter now drops an `f64` function
    that has *any* integer return, not only one whose every return is an
    integer. The annotation therefore does not coerce, in either backend --
    `return 1` prints `1`, `return -2.0` prints `-2.0` (pinned below).
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-f64-")

# name -> (source, expected stdout)
CASES = {
    "float_literal_stays_float": ("""fn g(a: f64) -> f64 {
  return 1e10 * 1e10
}
print(g(1.0))
""", "1e+20\n"),
    "float_literal_with_int_operand": ("""fn g(a: f64) -> f64 {
  return 1e10 * 2
}
print(g(1.0))
""", "20000000000.0\n"),
    "float_literal_minus_var": ("""fn g(a: f64, b: f64) -> f64 {
  return ((1e10 * 1e10) - b) * 2.5
}
print(g(3.0, 2.0))
""", "2.5e+20\n"),
    "int_literals_stay_int": ("""fn g(a: f64) -> f64 {
  return 1 / 2
}
print(g(0.0))
""", "0\n"),
    # Fast-path functions whose every subexpression is numeric do return a
    # double, and both backends agree on that today.
    "declared_f64_from_float_expr": ("""fn g(a: f64) -> f64 {
  return 2.0 * 3
}
print(g(1.0))
""", "6.0\n"),
    # A constant integer return from an `f64` function keeps the integer, on
    # both backends: the fast path needs at least one float return, otherwise
    # the C signature `double` would turn `3` into `3.0` behind `print`.
    "integer_return_stays_integer": ("""fn a(dead: f64) -> f64 {
  return 3
}
fn b(used: f64) -> f64 {
  return used
}
print(a(1.0))
print(b(2.0))
""", "3\n2.0\n"),
    "integer_return_no_params": ("""fn f() -> f64 {
  return 3
}
print(f())
""", "3\n"),
    # Mixed: one branch returns an integer, the other a float. The int must
    # survive `print` on both backends, so neither side coerces the return.
    "mixed_int_and_float_return": ("""fn f(x: f64) -> f64 {
  if x > 0.0 {
    return 1
  }
  return x
}
print(f(1.0))
print(f(-2.0))
""", "1\n-2.0\n"),
    "mixed_int_return_second": ("""fn f(x: i64) -> f64 {
  if x > 0 {
    return 2.5
  }
  return 0
}
print(f(1))
print(f(0))
""", "2.5\n0\n"),
    # Two distinct integers past 2^53 are ordered by their value, not by the
    # double they both round to. Both backends used to convert to `double` before
    # comparing, so `a < b` answered `False` for a = 2^60 and b = 2^60 + 1, and
    # `a == b` answered `True` in C and `False` in the -O0 build: the same
    # wrong answer on one side and an open disagreement on the other.
    "int_compare_past_double": ("""fn main() -> i64 {
  let a = 1152921504606846976
  let b = 1152921504606846977
  print(a < b)
  print(a == b)
  print(b > a)
  print(a <= b)
  print(b >= a)
  print([a] == [b])
  print([b, a] == [a, b])
  print(-1152921504606846977 < -1152921504606846976)
  return 0
}
""", "true\nfalse\ntrue\ntrue\ntrue\nfalse\nfalse\ntrue\n"),
    # An integer against a float compares by value, as Python does: 2^53 + 1 is
    # greater than the double 2^53, and equal to nothing. `sort` goes through the
    # same order, so the three-element vector below is the same on both sides.
    "int_against_float_exact": ("""fn main() -> i64 {
  print(9007199254740993 > 9007199254740992.0)
  print(9007199254740993 == 9007199254740992.0)
  print(1 == 1.0)
  print(0.0 == -0.0)
  print(true == 1)
  print(true < 2)
  let mut v = [1152921504606846977, 2, 1152921504606846976]
  v.sort()
  print(v)
  return 0
}
""", "true\nfalse\ntrue\ntrue\ntrue\ntrue\n[2, 1152921504606846976, 1152921504606846977]\n"),
}


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600)


def main():
    bad = 0
    for name in sorted(CASES):
        src, want = CASES[name]
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)

        run_o0 = run(_cxx.run_args(path))
        exe = os.path.join(TMP, name + ".exe")
        b = run(_cxx.cli_args("build", path, "-o", exe))
        if b.returncode != 0:
            print("FAIL %s: build: %s" % (name, (b.stdout + b.stderr)[-400:]))
            bad += 1
            continue
        native = run([exe])

        if run_o0.stdout != want:
            print("FAIL %s: -O0 %r, want %r" % (name, run_o0.stdout, want))
            bad += 1
        elif native.stdout != want:
            print("FAIL %s: native %r, want %r" % (name, native.stdout, want))
            bad += 1

    # The literal itself, not only the arithmetic: `10000000000` in the emitted
    # C is the bug, `10000000000.0` is the fix.
    path = os.path.join(TMP, "literal_shape.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write("fn g(a: f64) -> f64 {\n  return 1e10 * 1e10\n}\nprint(g(1.0))\n")
    c = run(_cxx.cli_args("emit-c", path))
    if "10000000000.0" not in c.stdout:
        print("FAIL literal_shape: the emitted C has no float literal for 1e10")
        bad += 1

    if bad:
        print("F64 SEMANTICS: %d case(s) failed" % bad)
        return 1
    print("F64 SEMANTICS OK (%d cases, both backends print the same bytes)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
