"""Parse diagnostics: the first error of each case, with its line number."""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
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
    # A call needs a name.
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
    # A jump with no loop of its own around it passed `check` and stopped in the
    # C compiler: "break statement not within loop or switch". A function body,
    # a closure and a task start outside any loop, wherever they are written.
    "jump_in_fn": "fn f() -> i64 {\n  break\n  return 1\n}\nprint(f())\n",
    "jump_at_top": "continue\nprint(1)\n",
    "jump_in_closure": "for i in 0..3 {\n  let f = fn(x) {\n    continue\n    return x\n  }\n"
                       "  print(f(i))\n}\n",
    "jump_in_spawn": "for i in 0..3 {\n  spawn {\n    break\n  }\n}\njoin_all()\n",
}


# name -> the first `error:` line with the path cut off, or None when the
# case is accepted.
WANT = {
    "eof_in_block": "4: error: unexpected end of file at line 4",
    "eof_in_call": "4: error: unexpected end of file at line 4",
    "unclosed_map": "3: error: want ':' have 'print' at line 3",
    "stray_percent": "3: error: stuck at '%' at line 3",
    "stray_brace_after_let": "1: error: stuck at '}' at line 1",
    "stray_paren": "2: error: stuck at ')' at line 2",
    "call_of_a_value": "3: error: a call needs a name, a function value is not first class at line 3",
    "bad_let": "1: error: want '=' have '5' at line 1",
    "bad_fn": "1: error: want '(' have 'a' at line 1",
    "want_colon": "1: error: want ':' have '1' at line 1",
    "unterminated_string": "1: error: unterminated string literal at line 1",
    "dangling_else": "3: error: want ':' have '}' at line 3",
    "double_op": "1: error: stuck at '+' at line 1",
    "too_deep_expr": "4: error: expression nested too deeply at line 4",
    "too_deep_block": "202: error: block nested too deeply at line 202",
    "extern_result_before_expr": "2: error: arg 1 of `flip`: want bool, got i64",
    "extern_result_same_line_expr": None,
    "extern_result_generic_before_expr": "1: error: unsupported FFI type `vec[i64]` as the result of extern fn `f`",
    "extern_result_errno_before_expr": None,
    "long_unicode_line": "1: error: stuck at '+' at line 1",
    "jump_in_fn": "2: error: `break` outside a loop at line 2",
    "jump_at_top": "1: error: `continue` outside a loop at line 1",
    "jump_in_closure": "3: error: `continue` outside a loop at line 3",
    "jump_in_spawn": "3: error: `break` outside a loop at line 3",
}


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600)


def main():
    cxx = _cxx.build()
    bad = 0
    for name in sorted(CASES):
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(CASES[name])
        r = run([cxx, "check", path])
        errors = [l for l in (r.stdout + r.stderr).splitlines() if ": error: " in l]
        want = WANT[name]
        if want is None:
            ok = r.returncode == 0 and not errors
        else:
            ok = r.returncode == 1 and errors and errors[0].startswith(path + ":" + want)
        if not ok:
            print("FAIL %s: rc=%d, want %r:\n%s" % (name, r.returncode, want, r.stdout + r.stderr))
            bad += 1
    # The diagnostic for a file that is not UTF-8 must itself be valid UTF-8:
    # echoing byte 0xff into a terminal, pipe or JSON consumer just moves the
    # decoding failure into the tool reading the diagnostic.
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
    print("PARSE DIAG OK (%d cases)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
