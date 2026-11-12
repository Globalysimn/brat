"""Names from a .brat file must never reach C source as code.

Before the fix: a struct-literal key, enum/impl type names and a post-dot
field or method name were taken as `tok.v` with no check on token kind, and
the emitter interpolated them into C string literals via `%s`. A file with a
string in a name position passed `brat check` with zero errors and produced
a C literal that closed early.

Both layers are covered: the parser rejects non-`ID`/`KW` tokens in name
position, and the emitter escapes whatever it is handed anyway.

Every negative case carries a positive control. Without one, "check rejected
the file" proves nothing: the first draft of this test passed every case
because the payload broke parsing on a comma, long before any name check.
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

ENV = dict(os.environ)

n = 0
FAILED = []
TMP = _scratch.mkdtemp(prefix="brat-inject-")

PAYLOAD = ",".join(str(b) for b in b"touch /tmp/brat-inject-marker")
BREAK_KEY = '"' + PAYLOAD + '("'
STR_NAME = '"zz"'


def check(cond, msg):
    global n
    if not cond:
        FAILED.append(msg)
        print("  FAIL: %s" % msg)
    else:
        n += 1


def write(name, src):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    return path


def cli(args, path, timeout=120):
    return subprocess.run([BIN] + args + [path],
                          capture_output=True, text=True, encoding="utf-8",
                          timeout=timeout, env=ENV)


def pair(name, evil_src, good_src, label):
    """Evil is rejected, good passes. Otherwise the case proves nothing."""
    evil = write(name + "_evil.brat", evil_src)
    good = write(name + "_good.brat", good_src)

    r_bad = cli(["check"], evil)
    out_bad = r_bad.stdout + r_bad.stderr
    check(r_bad.returncode != 0, "%s: name position must reject non-identifier "
          "(out=%r)" % (label, out_bad[:250]))
    check("identifier" in out_bad,
          "%s: want a diagnostic about the name, got %r" % (label, out_bad[:250]))

    r_good = cli(["check"], good)
    out_good = r_good.stdout + r_good.stderr
    check(r_good.returncode == 0, "%s: positive control must pass "
          "(rc=%d, out=%r)" % (label, r_good.returncode, out_good[:250]))


def struct_literal_key():
    pair("a1",
         'fn main() -> i64 {\n  let s = P{x: 1, %s: 2}\n  print(s)\n  0\n}\n' % BREAK_KEY,
         'fn main() -> i64 {\n  let s = P{x: 1, y: 2}\n  print(s)\n  0\n}\n',
         "struct-literal key")


def field_access_name():
    pair("a2",
         'fn main() -> i64 {\n  let v = [1, 2]\n  print(v[0].%s)\n  0\n}\n' % STR_NAME,
         'fn main() -> i64 {\n  let v = [1, 2]\n  print(v[0])\n  0\n}\n',
         "field access")


def method_call_name():
    pair("a3",
         'fn main() -> i64 {\n  let v = [1, 2]\n  print(v[0].%s())\n  0\n}\n' % STR_NAME,
         'fn main() -> i64 {\n  let s = "ab"\n  print(s.upper())\n  0\n}\n',
         "method call")


def impl_type_name():
    pair("a4",
         'impl %s {\n  fn f(self) -> i64 { 0 }\n}\n\nfn main() -> i64 { 0 }\n' % STR_NAME,
         'impl Q {\n  fn f(self) -> i64 { 0 }\n}\n\nfn main() -> i64 { 0 }\n',
         "impl type name")


def enum_type_name():
    pair("a5",
         'enum %s { A, B }\n\nfn main() -> i64 { 0 }\n' % STR_NAME,
         'enum Color { A, B }\n\nfn main() -> i64 { 0 }\n',
         "enum type name")


def struct_type_name():
    evil = write("a6_evil.brat",
                 'fn main() -> i64 {\n  let s = %s { x: 1 }\n  print(s)\n  0\n}\n'
                 % STR_NAME)
    r = cli(["check"], evil)
    check(r.returncode != 0, "struct type name: a string where a type name "
          "belongs must be rejected (rc=%d)" % r.returncode)
    r2 = cli(["emit-c"], evil)
    check(r2.returncode != 0, "struct type name: emitter must not accept a "
          "string as a type name (rc=%d)" % r2.returncode)


ESCAPE_CASES = [
    ('a"b', 'a\\"b'),
    ("a\\b", "a\\\\b"),
    ("a\nb", "a\\nb"),
    ("a\rb", "a\\rb"),
    (PAYLOAD, PAYLOAD),
]


def emitted_string_escapes_exactly():
    for i, (raw, want) in enumerate(ESCAPE_CASES):
        literal = json.dumps(raw, ensure_ascii=False)
        src = write("b0_escape_%d.brat" % i,
                    'fn main() -> i64 {\n  let s = %s\n  print(len(s))\n  0\n}\n'
                    % literal)
        r = cli(["emit-c"], src)
        check(r.returncode == 0, "native emit-c rejected a valid string literal "
              "for %r (out=%r)" % (raw, (r.stdout + r.stderr)[:250]))
        if r.returncode == 0:
            check('"%s"' % want in r.stdout,
                  "native C escape for %r did not contain %r" % (raw, want))

def emitted_names_are_escaped():
    src = write("b1_ok.brat", 'fn main() -> i64 {\n  print("ab".upper())\n  0\n}\n')
    r = cli(["emit-c"], src)
    check(r.returncode == 0, "emit-c on a valid program (rc=%d, out=%r)"
          % (r.returncode, (r.stdout + r.stderr)[:250]))
    if r.returncode == 0:
        for line in r.stdout.splitlines():
            if "map_set(" in line and line.count('"') % 2 != 0:
                check(False, "odd number of quotes in %r" % line)


def payload_never_reaches_c():
    src = write("b2_payload.brat",
                'fn main() -> i64 {\n  let s = P{x: 1, %s: 2}\n  print(s)\n  0\n}\n'
                % BREAK_KEY)
    r = cli(["emit-c"], src)
    check(r.returncode != 0, "emit-c must refuse a file with a name that is "
          "not a name (rc=%d)" % r.returncode)


def build_never_produces_binary():
    src = write("b3_build.brat",
                'fn main() -> i64 {\n  let s = P{x: 1, %s: 2}\n  print(s)\n  0\n}\n'
                % BREAK_KEY)
    out_bin = os.path.join(TMP, "b3.exe")
    r = cli(["build", "-o", out_bin], src, timeout=300)
    check(r.returncode != 0 or not os.path.exists(out_bin),
          "build must not produce a binary from that file (rc=%d)" % r.returncode)


def valid_programs_pass():
    cases = {
        "c1_struct": 'fn main() -> i64 {\n  let s = P{x: 1, y: 2}\n  print(s)\n  0\n}\n',
        "c2_method": 'fn main() -> i64 {\n  print("ab".upper())\n  0\n}\n',
        "c3_impl": ('trait T {\n  fn f(self) -> i64\n}\n\n'
                    'impl T for Q {\n  fn f(self) -> i64 { 7 }\n}\n\n'
                    'fn main() -> i64 { 0 }\n'),
        "c4_enum": 'enum Color { Red, Green }\n\nfn main() -> i64 { 0 }\n',
        "c5_underscore": ('fn main() -> i64 {\n  let my_value_2 = 1\n'
                          '  print(my_value_2)\n  0\n}\n'),
    }
    for name, src in cases.items():
        p = write(name + ".brat", src)
        r = cli(["check"], p)
        check(r.returncode == 0, "%s: valid program must pass check "
              "(rc=%d, out=%r)" % (name, r.returncode, (r.stdout + r.stderr)[:250]))
        r2 = cli(["run"], p)
        check(r2.returncode == 0, "%s: valid program must run "
              "(rc=%d, out=%r)" % (name, r2.returncode, (r2.stdout + r2.stderr)[:250]))


def main():
    for fn in (struct_literal_key, field_access_name, method_call_name,
               impl_type_name, enum_type_name, struct_type_name,
               emitted_string_escapes_exactly, emitted_names_are_escaped,
               payload_never_reaches_c, build_never_produces_binary,
               valid_programs_pass):
        try:
            fn()
        except Exception as e:  # noqa: BLE001
            FAILED.append("%s raised %r" % (fn.__name__, e))
            print("  ERROR in %s: %r" % (fn.__name__, e))

    if FAILED:
        print("\nINJECT FAILED - %d problem(s), %d ok:" % (len(FAILED), n))
        for f in FAILED:
            print("  - %s" % f)
        sys.exit(1)
    print("INJECT ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()