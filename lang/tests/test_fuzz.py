import os
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _cxx      # noqa: E402
import _scratch  # noqa: E402

BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-fuzz-")

BATCH_BUDGET = 10.0
N = 2000

POOL = [
    "let", "mut", "fn", "if", "else", "while", "return", "true", "false", "or",
    "try", "catch", "spawn", "match", "for", "in", "import", "defer", "move",
    "break", "continue", "impl", "self", "trait", "enum", "const",
    "{", "}", "(", ")", "[", "]", "{{{", "}}}", "(((", ")))", "[[[", "]]]",
    "+", "-", "*", "/", "%", "<", ">", "=", "!", "==", "!=", "<=", ">=",
    "&&", "||", "&", "|", "^", "~", "<<", ">>", "..", "=>", "->", ":", ";", ",", "?", ".",
    '"hello"', '"', '"unterminated', '"a b c', '"\\\\"', '"\\n"', '""',
    "0", "1", "42", "0xff", "0b1010", "0o755", "3.14", "007", "99.99",
    ".", "foo.bar", "a.b.c", "x..y",
    "x", "y", "foo", "bar", "Foo", "None", "_", "a1", "f", "ok",
    "let x = 1;", "fn f() {}", "if true {}", "while false {}",
    " ", " ", "\n",
]


def gen_inputs(n):
    rng = random.Random(20261003)
    out = []
    for _ in range(n):
        k = rng.randint(1, 40)
        parts = [rng.choice(POOL) for _ in range(k)]
        gaps = [rng.choice([" ", " ", " ", "", "\n", "  "]) for _ in range(k - 1)]
        s = parts[0]
        for p, g in zip(parts[1:], gaps):
            s += g + p
        if rng.random() < 0.08:
            d = rng.randint(200, 300)
            s = rng.choice(["{", "(", "["]) * d + s
        out.append(s)
    deep = []
    for d in (50, 100, 150, 200, 210, 250, 280, 300):
        deep.append("{" * d)
        deep.append("(" * d)
        deep.append("[" * d)
        deep.append("{" * d + "}" * d)
        deep.append("(" * d + ")" * d)
        deep.append("[" * d + "]" * d)
        deep.append('"unterminated' + "x" * d)
        deep.append("let x = " + "(" * d)
        deep.append('print("' + "{" * d + "1" + "}" * d + '")')
        deep.append('print("' + "{1+" * d + '1}")')
    out[-len(deep):] = deep[:len(deep)] if len(deep) <= len(out) else out
    return out


def main():
    inputs = gen_inputs(N)
    assert len(inputs) == N
    paths = []
    for idx, src in enumerate(inputs):
        p = os.path.join(TMP, "case_%04d.brat" % idx)
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(src)
        paths.append(p)

    t0 = time.perf_counter()
    fails = []
    for start_idx in range(0, N, 250):
        batch = paths[start_idx : start_idx + 250]
        bt0 = time.perf_counter()
        r = subprocess.run([BIN, "ast", *batch], capture_output=True, text=True, timeout=BATCH_BUDGET)
        dt = time.perf_counter() - bt0
        if r.returncode not in (0, 1):
            fails.append((start_idx, "crash rc=%d stderr=%r" % (r.returncode, r.stderr[:300])))
        if dt > BATCH_BUDGET:
            fails.append((start_idx, "slow batch %.2fs" % dt))

    total = time.perf_counter() - t0
    if fails:
        for idx, msg in fails:
            print("FAIL batch %d: %s" % (idx, msg))
        sys.exit(1)
    print("FUZZ ALL GREEN (%d inputs parsed cleanly in %.2fs)" % (N, total))


if __name__ == "__main__":
    main()
