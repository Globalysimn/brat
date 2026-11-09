"""Characters that start no valid token are rejected with an explicit diagnostic and line number."""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-lexer-strays-")

REFUSED = [
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
    "let x = (6 | 1) ^ (~5 & 0xff)\n",
    "let s = \"a | b ^ ~ @ # $ ` ' \\\\\"\n",
    "// a | b ^ ~ @ # $ `\nlet x = 1\n",
    "\ufefflet x = 1\n",
    "let s = \"caf\u00e9 \u03c0 \U0001f600\"\n",
]

n = 0


for src, message in REFUSED:
    with open(os.path.join(TMP, "example.brat"), "w", encoding="utf-8", newline="") as f:
        f.write(src)
    cxx = subprocess.run([NATIVE, "check", "example.brat"], cwd=TMP,
                         capture_output=True, text=True, timeout=120)
    shown = repr(src)
    assert cxx.returncode == 1, "%s should be refused: rc=%d" % (shown, cxx.returncode)
    assert message in cxx.stderr, "%s: want %r in %r" % (shown, message, cxx.stderr)
    n += 2

for src in ACCEPTED:
    with open(os.path.join(TMP, "example.brat"), "w", encoding="utf-8", newline="") as f:
        f.write(src)
    cxx = subprocess.run([NATIVE, "check", "example.brat"], cwd=TMP,
                         capture_output=True, text=True, timeout=120)
    assert cxx.returncode == 0, "%s should be accepted: rc=%d (%r)" % (
        repr(src), cxx.returncode, cxx.stdout[-200:])
    n += 1

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
