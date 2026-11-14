"""Fuzz `brat-cxx ast` on random edits of the corpus across 3,000 mutated inputs.
"""
import os
import random
import signal
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

import _cxx      # noqa: E402
import _scratch  # noqa: E402

NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-front-fuzz-")
CASES = 3000
SEED = 20261104

# (source, expected_native_ok)
PINNED = [
    ("impl C{fn e(){match f{d=>{}_=>{}}}}\n", False),
    ('("{(")"")\n', False),
    ('fn n(){("{(" }")t("")}\n', True),
    ('fn main() -> i64 {\n  print("a\\\nb")\n  return 0\n}\n', True),
    ('fn main() -> i64 {\n  match 1 {\n    _ => print("a"),\n    1 => print("b")\n  }\n  return 0\n}\n', False),
    ('enum E {\n  A,\n  B(x) {\n  C\n}\n', False),
    ('fn main() -> i64 {\n  match 1 {\n    1 => print("a"),\n    1 => print("b")\n  }\n  return 0\n}\n', False),
    ('fn main() -> i64 {\n  for "a\\n" in 1 {\n  }\n  return 0\n}\n', False),
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


def native_verdict(text, n):
    path = os.path.join(TMP, "f%d.brat" % n)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    done = subprocess.run([NATIVE, "ast", path], capture_output=True, text=True, errors="replace",
                          timeout=20)
    os.remove(path)
    assert done.returncode in (0, 1), "brat-cxx ast crashed (%d) on %r" % (done.returncode, text)
    if done.returncode == 0:
        out = done.stdout.rstrip("\n")
        assert out.startswith('("prog" '), "malformed AST dump: %r" % out[:120]
        return "ok", out
    err = done.stderr.strip()
    assert ": error: " in err, "missing diagnostic format: %r" % err[:120]
    return "err", err


rng = random.Random(SEED)
seeds = corpus()
assert len(seeds) > 50, len(seeds)
mutated = [edit(rng, rng.choice(seeds)) for _ in range(CASES)]

# The mutated corpus goes through `brat-cxx ast` in chunks of 250 files, so
# 3,000 cases cost 12 processes and not 3,000.
for idx, (src, want_ok) in enumerate(PINNED):
    got_kind, _ = native_verdict(src, idx)
    assert (got_kind == "ok") == want_ok, "pinned #%d expected ok=%s, got %s" % (idx, want_ok, got_kind)

CHUNK = 250
for base_i in range(0, len(mutated), CHUNK):
    batch = mutated[base_i: base_i + CHUNK]
    paths = []
    for j, text in enumerate(batch):
        p = os.path.join(TMP, "b_%d_%d.brat" % (base_i, j))
        with open(p, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        paths.append(p)
    done = subprocess.run([NATIVE, "ast"] + paths, capture_output=True, text=True,
                          errors="replace", timeout=30)
    for p in paths:
        os.remove(p)
    assert done.returncode in (0, 1), "brat-cxx ast batch crashed (%d)" % done.returncode
assert not any(k == "brat" or k.startswith("brat.") for k in sys.modules)
print('FRONT-ENDS FUZZ ALL GREEN (%d cases)' % CASES)
