"""Parse diagnostics: the two front ends must print the same line, the same way.

README says the C++ front end has "the same tree, the same diagnostics, the same
C". For ownership and type errors that was already true and covered by
`test_cxx_borrow.py` and friends. For *syntax* errors it was not:

    py : file:4: error: stuck at ')' at line 4   + "  |   print(k)"
    cxx: file: bad expression start ')' at line 4

so a tool that greps `: error: ` (the documented shape, `diag.py`) saw the
Python diagnostic and missed the C++ one, and a reader lost the source snippet.
Two line numbers also disagreed, for two independent reasons:

  * the C++ EOF token carried the line *after* the trailing newline, while the
    Python side reports `c.t[-1].line` — the last real token;
  * the Python `stuck at` message paired the offending token's text with
    `eline(c)`, the line of the *current* token, which a `Name {` lookahead may
    already have advanced past.

Each case below is one of those shapes, plus the ordinary ones that must keep
working. Output is compared byte for byte after the temp path is replaced, so
wording, line number, `: error: ` prefix and snippet are all pinned.
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-parsediag-")

CASES = {
    "eof_in_block": "let mut m = {}\nprint(1)\nfor k in m {\n  print(k)\n",
    "eof_in_call": "fn f(a: i64) -> i64 {\n  return a\n}\nprint(f(1)\n",
    "unclosed_map": 'let mut m = {\nm.set("a", 1)\nprint(1)\n}\n',
    "stray_percent": "fn h(v: vec[i64]) -> i64 {\n  let m = 0\n  return m % % 1\n}\n",
    "stray_brace_after_let": 'let mut m = }\nm.set("a", 1)\nprint(m["a"])\n',
    "stray_paren": "let mut v = [1, 2]\nv.push3)\nprint(len(v))\n",
    # `v[0](10)` used to parse as two arguments of the enclosing call, so
    # `print(v[0](10))` printed the closure and then 10, and a closure that
    # captured the vector holding it recursed until the frame budget ran out.
    # A call needs a name; the two front ends must refuse it the same way.
    "call_of_a_value": "let mut v = []\nv.push(fn(x) { return x + 1 })\n"
                       "print(v[0](10))\n",
    "bad_let": "let = 5\nprint(1)\n",
    "bad_fn": "fn (a) {\n  return 1\n}\n",
    "want_colon": 'let mut m = {"a" 1}\nprint(1)\n',
    "unterminated_string": 'print("abc\n',
    "dangling_else": "else {\n  print(1)\n}\n",
    "double_op": "print(1 ++ 2)\n",
    "too_deep_expr": "fn main() -> i64 {\n  let a = 1\n  let b = 2\n  let x = %s1%s\n  return 0\n}\n"
                     % ("(" * 300, ")" * 300),
    "too_deep_block": "fn main() -> i64 {\n%s\n  return 0\n}\n" % ("if true {\n" * 300),
    # An extern result had no body or required semicolon to end it, and the
    # collector kept reading: `-> bool` before `print(...)` became the
    # unsupported type `boolprint(...)`, hiding the real line-2 type error.
    "extern_result_before_expr": "extern fn flip(x: bool) -> bool\nprint(flip(2))\n",
    "extern_result_same_line_expr": "extern fn f(x: i64) -> i64 print(1)\n",
    "extern_result_generic_before_expr": "extern fn f() -> vec[i64]\nprint(1)\n",
    "extern_result_errno_before_expr": "extern fn f() -> i32 errno\nprint(1)\n",
    # The snippet limit is 200 characters, not bytes: clipping this line
    # by bytes cut it earlier in C++ and could split a UTF-8 character.
    "long_unicode_line": 'print("' + ("Ж" * 250) + '" ++ 1)\n',
}


def norm(text, path):
    return "\n".join(l.replace(path, "F") for l in text.replace("\r\n", "\n").splitlines()
                     if l.strip())


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600)


def main():
    cxx = _cxx.build()
    bad = 0
    for name in sorted(CASES):
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(CASES[name])
        a = run([sys.executable, CLI, "check", "--py", path])
        b = run([cxx, "check", path])
        py = norm(a.stdout + a.stderr, path)
        cx = norm(b.stdout + b.stderr, path)
        if a.returncode != b.returncode:
            print("FAIL %s: exit codes %d and %d" % (name, a.returncode, b.returncode))
            bad += 1
            continue
        if py != cx:
            print("FAIL %s: diagnostics differ\n  py :\n%s\n  cxx:\n%s" % (name, py, cx))
            bad += 1
            continue
        if a.returncode and ": error: " not in py:
            print("FAIL %s: rejected without a `file:line: error: ` line:\n%s" % (name, py))
            bad += 1
    # Invalid UTF-8 follows a different load path in Python, so the text
    # need not match there. The C++ diagnostic itself must still be valid
    # UTF-8: echoing byte 0xff into a terminal, pipe or JSON consumer just
    # moves the decoding failure into the tool reading the diagnostic.
    bad_path = os.path.join(TMP, "invalid_utf8.brat")
    with open(bad_path, "wb") as f:
        f.write(b'let x = "\xff"\n')
    raw = subprocess.run([cxx, "check", bad_path], capture_output=True, timeout=600)
    try:
        text = (raw.stdout + raw.stderr).decode("utf-8")
    except UnicodeDecodeError:
        print("FAIL invalid_utf8: C++ diagnostic is not valid UTF-8")
        bad += 1
    else:
        if raw.returncode == 0 or "source is not valid UTF-8" not in text or "\ufffd" not in text:
            print("FAIL invalid_utf8: unexpected diagnostic:\n%s" % text)
            bad += 1

    if bad:
        print("PARSE DIAG: %d case(s) failed" % bad)
        return 1
    print("PARSE DIAG OK (%d cases, both front ends print the same bytes)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
