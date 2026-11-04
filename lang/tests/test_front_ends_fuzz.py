"""Random edits of the corpus get the same verdict from the Python and the C++ front end.

The two front ends are meant to be one language written twice, and the examples only prove they
agree on programs somebody wrote. Here every program is a corpus file with one to three random edits
(a character inserted, replaced or deleted, a slice repeated, the text cut off), mostly broken. Each
must be accepted by both or rejected by both, with the same tree when accepted and the same sentence
and line when rejected (the first line of the sentence, for a token that spans lines).

The first campaigns, 130,000 programs, found these disagreements, each pinned below: a catch-all
`match` arm that was not last, which the C++ parser accepted because it had already moved the later
arms out of the variable it then tested; in the Python lexer, a quote that went missing after a string
with a placeholder, because the regex resumed from its own end instead of the string's, and a
backslash before a newline inside a string that it refused; and parse errors that named the line
after the offending token, or printed it as a Python `repr`, or printed the pattern as a Python tuple.
"""
import _cxx
import _scratch

import os
import random
import signal
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "engine"))
import brat.parser as P                       # noqa: E402
from brat.lexer import Cur, lex               # noqa: E402
from brat.parser import parse_prog            # noqa: E402

NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-front-fuzz-")
CASES = 3000
SEED = 20261104

PINNED = [
    "impl C{fn e(){match f{d=>{}_=>{}}}}\n",
    '("{(")"")\n',
    'fn n(){("{(" }")t("")}\n',
    'fn main() -> i64 {\n  print("a\\\nb")\n  return 0\n}\n',
    'fn main() -> i64 {\n  match 1 {\n    _ => print("a"),\n    1 => print("b")\n  }\n  return 0\n}\n',
    'enum E {\n  A,\n  B(x) {\n  C\n}\n',
    'fn main() -> i64 {\n  match 1 {\n    1 => print("a"),\n    1 => print("b")\n  }\n  return 0\n}\n',
    'fn main() -> i64 {\n  for "a\\n" in 1 {\n  }\n  return 0\n}\n',
]

INSERTS = (list("\"'\\{}()[]<>=!&|+-*/%.,:;_0123456789eExXaAzZ \t\n\r#@$~^`?")
           + ["\u00e9", "\u03c0", "\u00a0", "\u2028", "\U0001f600", "\x00", "\x7f", "\ufeff", "\r\n",
              "0x", "1e", "..", "->", "=>", "//", "/*", "\\n", "{}", "{x}", "\\\n", "\"{\"", "\"{(\"",
              "{\"\"}", "(\"", "\")", "\"\""])


class Timeout(Exception):
    pass


def alarm(*_):
    raise Timeout()


signal.signal(signal.SIGALRM, alarm)


def corpus():
    out = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in ("vendor", "bin", ".git", "__pycache__")]
        for f in sorted(files):
            if f.endswith(".brat"):
                text = open(os.path.join(base, f), encoding="utf-8").read()
                if 20 < len(text) < 3000:
                    out.append(text)
    return out


def edit(rng, text):
    for _ in range(rng.choice((1, 1, 2, 3))):
        i = rng.randrange(len(text) + 1)
        kind = rng.randrange(6)
        if kind == 0:
            text = text[:i] + rng.choice(INSERTS) + text[i:]
        elif kind == 1:
            text = text[:i] + text[i + 1:]
        elif kind == 2:
            j = min(len(text), i + rng.randrange(1, 20))
            text = text[:j] + text[i:j] + text[j:]
        elif kind == 3:
            text = text[:i]
        elif kind == 4:
            text = text[:i] + rng.choice(INSERTS) + text[i + 1:]
        else:
            text = text[:i] + text[min(len(text), i + rng.randrange(1, 12)):]
        if not text:
            break
    return text


def python_verdict(text):
    P._seq = 0
    signal.alarm(10)
    try:
        return "ok", _cxx.sx(parse_prog(Cur(lex(text))))
    except SyntaxError as e:
        return "err", str(e)
    finally:
        signal.alarm(0)


def native_verdict(text, n):
    path = os.path.join(TMP, "f%d.brat" % n)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    done = subprocess.run([NATIVE, "ast", path], capture_output=True, text=True, errors="replace",
                          timeout=20)
    os.remove(path)
    if done.returncode == 0:
        return "ok", done.stdout.rstrip("\n")
    return "err", done.stderr.strip()


def disagreement(text, n):
    want, got = python_verdict(text), native_verdict(text, n)
    if want[0] != got[0]:
        return "Python says %s, C++ says %s" % (want[0], got[0])
    if want[0] == "ok" and want[1] != got[1]:
        return "the trees differ"
    if want[0] == "err" and want[1].split("\n")[0] not in got[1]:
        return "Python: %r, C++: %r" % (want[1][:200], got[1][:200])
    return None


rng = random.Random(SEED)
seeds = corpus()
assert len(seeds) > 50, len(seeds)
programs = PINNED + [edit(rng, rng.choice(seeds)) for _ in range(CASES)]
for n, text in enumerate(programs):
    why = disagreement(text, n)
    assert why is None, "%s on %s%r" % (why, "a pinned program: " if n < len(PINNED) else "a random one: ", text)

print("FRONT ENDS FUZZ ALL GREEN (%d programs, %d pinned)" % (len(programs), len(PINNED)))
