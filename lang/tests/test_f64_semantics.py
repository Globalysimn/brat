"""f64 semantics: a float literal stays a float, and `-> f64` does not coerce.

- A float literal in an `f64` function is emitted with its point. `1e10`
  written as `10000000000` is an integer constant in C, and `1e10 * 1e10` then
  wraps as int64 at -O2 while `run -O0` prints `1e+20`.
- An integer-only subexpression is integer arithmetic (SPEC 10): `1 / 2` in an
  `f64` function is `0`.
- A function declared `-> f64` that returns an integer on any path returns
  that integer: `return 3` prints `3`, not `3.0`. Such a function is kept off
  the `double` fast path, whose C signature would convert the value.
- Integers past 2^53 compare by value, against each other and against floats.
- A float prints with enough digits to read back as the same value.

Every case is checked against its expected output at -O0 and -O2.
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
    # A fast-path function whose every subexpression is numeric returns a
    # double.
    "declared_f64_from_float_expr": ("""fn g(a: f64) -> f64 {
  return 2.0 * 3
}
print(g(1.0))
""", "6.0\n"),
    # A constant integer return from an `f64` function keeps the integer: the
    # C signature `double` of the fast path would turn `3` into `3.0`.
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
    # One branch returns an integer, the other a float. Neither is coerced.
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
    # double they both round to: a = 2^60, b = 2^60 + 1.
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
    # same order.
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
    # A float below 0.1 can need all seventeen digits, and they start after the
    # leading zeros: seventeen places after the point are two digits short.
    "small_float_reads_back": ("""fn main() -> i64 {
  let x = 0.0011111111111111111
  print(x)
  print(unwrap(str(x).parse_float()) == x)
  print(0.00012345678901234567, 0.012345678901234567, 0.1, 0.0001)
  return 0
}
""", "0.0011111111111111111\ntrue\n0.00012345678901234567 0.012345678901234567 0.1 0.0001\n"),
    # `/` and `%` by zero raise on f64 as on i64; overflow is `inf`, not an error.
    "f64_zero_divisor_raises": ("""fn g(a: f64, b: f64) -> f64 {
  return a / b
}
try {
  print(g(1.0, 0.0))
} catch e {
  print(e)
}
try {
  print(1.0 % 0.0)
} catch e {
  print(e)
}
print(1e308 * 10.0)
""", "div by zero\ndiv by zero\ninf\n"),
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

    # The literal itself, not only the arithmetic: the emitted C has
    # `10000000000.0`, not `10000000000`.
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
    print("F64 SEMANTICS OK (%d cases, at -O0 and -O2)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
