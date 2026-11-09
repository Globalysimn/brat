"""The lexer, on the cases that the single-pass rewrite could break.

Tokens are compared as `(kind, text, line)` triples against literal lists via
`brat-cxx lex` (.
legacy Python lexer).
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _cxx  # noqa: E402

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def toks(src):
    return _cxx.lex_source(src)


def rejects(src):
    """The error the lexer raises for `src`, or None when it lexes."""
    try:
        _cxx.lex_source(src)
    except SyntaxError as e:
        return str(e)
    return None


check(toks("") == [], "empty source")
check(toks("let x = 1\n") == [("KW", "let", 1), ("ID", "x", 1), ("OP", "=", 1),
                              ("NUM", "1", 1)],
      "one line: %r" % (toks("let x = 1\n"),))

check(toks("a\nb\n\nc") == [("ID", "a", 1), ("ID", "b", 2), ("ID", "c", 4)],
      "line numbers count blank lines: %r" % (toks("a\nb\n\nc"),))

check(toks("a // comment\nb") == [("ID", "a", 1), ("ID", "b", 2)],
      "comment swallows the rest of the line: %r" % (toks("a // comment\nb"),))

check(toks('s = "plain"\nt = "with {x} inside"') == [
    ("ID", "s", 1), ("OP", "=", 1), ("STR", '"plain"', 1),
    ("ID", "t", 2), ("OP", "=", 2), ("STR", '"with {x} inside"', 2)],
      "plain strings: %r" % (toks('s = "plain"\nt = "with {x} inside"'),))

q = '"v = {s.join(\'|\')}"'
check(toks(q) == [("STR", q, 1)], "placeholder with a quoted join: %r" % (toks(q),))

check(toks('x = "{a}" + "b"') == [
    ("ID", "x", 1), ("OP", "=", 1), ("STR", '"{a}"', 1), ("OP", "+", 1),
    ("STR", '"b"', 1)], "two strings after a placeholder: %r" % (toks('x = "{a}" + "b"'),))

check(toks('f(1, "a\\"b")') == [
    ("ID", "f", 1), ("OP", "(", 1), ("NUM", "1", 1), ("OP", ",", 1),
    ("STR", '"a\\"b"', 1), ("OP", ")", 1)],
      "escaped quote inside a string: %r" % (toks('f(1, "a\\"b")'),))

check(toks("a..b") == [("ID", "a", 1), ("OP", "..", 1), ("ID", "b", 1)],
      "range operator: %r" % (toks("a..b"),))
check(toks("a.b") == [("ID", "a", 1), ("DOT", ".", 1), ("ID", "b", 1)],
      "field access: %r" % (toks("a.b"),))
check(toks("m.0") == [("ID", "m", 1), ("DOT", ".", 1), ("NUM", "0", 1)],
      "tuple index: %r" % (toks("m.0"),))

check(toks("1.5 1e3 2.5e-2") == [("NUM", "1.5", 1), ("NUM", "1e3", 1),
                                 ("NUM", "2.5e-2", 1)],
      "numbers: %r" % (toks("1.5 1e3 2.5e-2"),))

check(toks('s = "привет" // мир') == [
    ("ID", "s", 1), ("OP", "=", 1), ("STR", '"привет"', 1)],
      "non-ASCII: %r" % (toks('s = "привет" // мир'),))
check(rejects('s = 1\nп') == "unexpected character U+043F at line 2",
      "a Cyrillic name: %r" % (rejects('s = 1\nп'),))
check(toks("\ufefflet x = 1") == toks("let x = 1"), "a byte order mark is not a character")

check(toks("a\r\nb") == [("ID", "a", 1), ("ID", "b", 2)],
      "CRLF: %r" % (toks("a\r\nb"),))

check(rejects('x = 1\ny = "abc\nz') == "unterminated string literal at line 2",
      "unterminated string: %r" % (rejects('x = 1\ny = "abc\nz'),))

check(toks("fn f() -> i64 { return 1 }") == [
    ("KW", "fn", 1), ("ID", "f", 1), ("OP", "(", 1), ("OP", ")", 1),
    ("OP", "->", 1), ("ID", "i64", 1), ("OP", "{", 1), ("KW", "return", 1),
    ("NUM", "1", 1), ("OP", "}", 1)],
      "declaration: %r" % (toks("fn f() -> i64 { return 1 }"),))

check(rejects("a @ b") == "unexpected character '@' at line 1",
      "unknown character: %r" % (rejects("a @ b"),))
check(toks("6 | 1 ^ ~0") == [
    ("NUM", "6", 1), ("OP", "|", 1), ("NUM", "1", 1),
    ("OP", "^", 1), ("OP", "~", 1), ("NUM", "0", 1)],
      "bitwise operators")
check(toks('"a | b ^ ~ @" // | ^ ~ @') == [("STR", '"a | b ^ ~ @"', 1)],
      "stray characters inside a string and a comment are text")

assert not any(k == "brat" or k.startswith("brat.") for k in sys.modules)

print("LEXER ALL GREEN (%d asserts)" % n)
