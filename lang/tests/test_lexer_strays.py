"""A character that starts no token is an error, and both front ends say the same thing.

The scanners used to skip such a character. `let flags = 6 | 1` was read as `let flags = 6`
followed by the expression `1`, `~x` as `x`, and `a ^ b` as two arguments, so a program that
reached for a bitwise operator the language does not have compiled and ran with the wrong
meaning. The Python lexer and `lexer.cpp` now refuse the character, name it, and give the line.
"""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-lexer-strays-")

REFUSED = [
    ("let x = 6 | 1\n", "unexpected character '|' at line 1"),
    ("let x = 6 ^ 3\n", "unexpected character '^' at line 1"),
    ("let x = ~5\n", "unexpected character '~' at line 1"),
    ("let x = 1\n@inline\n", "unexpected character '@' at line 2"),
    ("let x = 1 # not a comment\n", "unexpected character '#' at line 1"),
    ("let x = $y\n", "unexpected character '$' at line 1"),
    ("let x = 1 \\\n", "unexpected character '\\' at line 1"),
    ("let x = `y`\n", "unexpected character '`' at line 1"),
    ("let s = 'single'\n", "unexpected character ''' at line 1"),
    ("let x\u00a0= 1\n", "unexpected character U+00A0 at line 1"),
    ("let x = \u0663\n", "unexpected character U+0663 at line 1"),
    ("let \u03c0 = 3\n", "unexpected character U+03C0 at line 1"),
    ("let x = 1\n\n\U0001f600\n", "unexpected character U+1F600 at line 3"),
    ("let x = \x01\n", "unexpected character U+0001 at line 1"),
    ("let s = \"abc\nlet t = 1\n", "unterminated string literal at line 1"),
    ("let s = \"{x\nlet t = 1\n", "unterminated string literal at line 1"),
]

ACCEPTED = [
    "let s = \"a | b ^ ~ @ # $ ` ' \\\\\"\n",
    "// a | b ^ ~ @ # $ `\nlet x = 1\n",
    "\ufefflet x = 1\n",
    "let s = \"caf\u00e9 \u03c0 \U0001f600\"\n",
]

n = 0


def front_ends(path):
    env = dict(os.environ, BRAT_NO_NATIVE_CLI="1")
    py = subprocess.run([sys.executable, CLI, "check", "example.brat"], cwd=TMP, env=env,
                        capture_output=True, text=True, timeout=120)
    cxx = subprocess.run([NATIVE, "check", "example.brat"], cwd=TMP,
                         capture_output=True, text=True, timeout=120)
    return py, cxx


for src, message in REFUSED:
    with open(os.path.join(TMP, "example.brat"), "w", encoding="utf-8", newline="") as f:
        f.write(src)
    py, cxx = front_ends(src)
    shown = repr(src)
    assert py.returncode == 1 and cxx.returncode == 1, \
        "%s should be refused: python rc=%d native rc=%d" % (shown, py.returncode, cxx.returncode)
    assert message in py.stderr, "python front end, %s: want %r in %r" % (shown, message, py.stderr)
    assert py.stderr == cxx.stderr, \
        "front ends disagree on %s:\npython: %r\nnative: %r" % (shown, py.stderr, cxx.stderr)
    n += 3

for src in ACCEPTED:
    with open(os.path.join(TMP, "example.brat"), "w", encoding="utf-8", newline="") as f:
        f.write(src)
    py, cxx = front_ends(src)
    assert py.returncode == 0 and cxx.returncode == 0, \
        "%s should be accepted: python rc=%d (%r) native rc=%d (%r)" % (
            repr(src), py.returncode, py.stdout[-200:], cxx.returncode, cxx.stdout[-200:])
    n += 2

with open(os.path.join(TMP, "example.brat"), "w", encoding="utf-8", newline="") as f:
    f.write(REFUSED[0][0])
for command in ("lex", "ast"):
    done = subprocess.run([NATIVE, command, "example.brat"], cwd=TMP, capture_output=True, text=True,
                          timeout=60)
    assert done.returncode == 1, "brat-cxx %s: exit %d, %r" % (command, done.returncode,
                                                               done.stderr[-200:])
    assert REFUSED[0][1] in done.stderr, "brat-cxx %s: %r" % (command, done.stderr)
    n += 2

print("LEXER STRAYS ALL GREEN (%d asserts)" % n)
