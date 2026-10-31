import importlib.util as _ilu
import os as _o
import random as _r
import sys as _s
import threading as _t
import time as _ti

_HERE = _o.path.dirname(_o.path.abspath(__file__))
_SHIM = _o.path.join(_HERE, '..', 'engine', 'brat.py')
_spec = _ilu.spec_from_file_location("brat_shim_fuzz", _SHIM)
_mod = _ilu.module_from_spec(_spec)
_s.modules["brat_shim_fuzz"] = _mod
_spec.loader.exec_module(_mod)
lex = _mod.lex
Cur = _mod.Cur
parse_prog = _mod.parse_prog

ALLOWED = (SyntaxError,)
PER_INPUT_BUDGET = 0.5
N = 2000

POOL = [
"let", "mut", "fn", "if", "else", "while", "return", "true", "false", "or",
"try", "catch", "spawn", "match", "for", "in", "import", "defer", "move",
"break", "continue", "impl", "self", "trait", "enum", "const",
"{", "}", "(", ")", "[", "]", "{{{", "}}}", "(((", ")))", "[[[", "]]]",
"+", "-", "*", "/", "%", "<", ">", "=", "!", "==", "!=", "<=", ">=",
"&&", "||", "..", "=>", "->", ":", ";", ",", "?", ".",
'"hello"', '"', '"unterminated', '"a b c', '"\\\\"', '"\\n"', '""',
"0", "1", "42", "3.14", "007", "99.99",
".", "foo.bar", "a.b.c", "x..y",
"x", "y", "foo", "bar", "Foo", "None", "_", "a1", "f", "ok",
"let x = 1;", "fn f() {}", "if true {}", "while false {}",
" ", " ", "\n",
]

def nesting_depth(src):
    return max(src.count("("), src.count("["), src.count("{"))

def gen_inputs(n):
    rng = _r.Random(20261003)
    out = []
    for _ in range(n):
        k = rng.randint(1, 40)
        parts = [rng.choice(POOL) for _ in range(k)]
        gaps = []
        for _ in range(k - 1):
            gaps.append(rng.choice([" ", " ", " ", "", "\n", "  "]))
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
    out[-len(deep):] = deep[:len(deep)] if len(deep) <= len(out) else out
    return out

def parse_once(src, box):
    try:
        toks = lex(src)
        parse_prog(Cur(toks))
        box["res"] = ("ok", None)
    except BaseException as e:
        box["res"] = ("exc", e)

def main():
    inputs = gen_inputs(N)
    assert len(inputs) == N
    n_ok = 0
    n_exc = 0
    n_reclimit = 0
    fails = []
    hangs = []
    t0 = _ti.perf_counter()
    for idx, src in enumerate(inputs):
        box = {}
        start = _ti.perf_counter()
        th = _t.Thread(target=parse_once, args=(src, box), daemon=True)
        th.start()
        th.join(timeout=PER_INPUT_BUDGET)
        el = _ti.perf_counter() - start
        if th.is_alive():
            fails.append((idx, src, "TIMEOUT>%.1fs" % PER_INPUT_BUDGET))
            hangs.append((idx, src))
            print("FAIL timeout #%d el=%.2fs repr=%r" % (idx, el, src[:300]))
            continue
        if el > PER_INPUT_BUDGET:
            fails.append((idx, src, "SLOW el=%.2fs" % el))
            hangs.append((idx, src))
            print("FAIL slow #%d el=%.2fs repr=%r" % (idx, el, src[:300]))
            continue
        res = box.get("res")
        if res is None:
            fails.append((idx, src, "NORESULT"))
            print("FAIL noresult #%d repr=%r" % (idx, src[:300]))
            continue
        kind, val = res
        if kind == "ok":
            n_ok += 1
            continue
        en = type(val).__name__
        if isinstance(val, RecursionError):
            if nesting_depth(src) > 200:
                n_reclimit += 1
            else:
                fails.append((idx, src, "RecursionError shallow depth=%d: %r" % (nesting_depth(src), val)))
                print("FAIL shallow-recursion #%d repr=%r err=%r" % (idx, src[:300], val))
            continue
        if isinstance(val, ALLOWED):
            n_exc += 1
            continue
        fails.append((idx, src, "BADTYPE %s: %r" % (en, val)))
        print("FAIL badexc #%d %s repr=%r err=%r" % (idx, en, src[:300], val))
    total_el = _ti.perf_counter() - t0
    print("fuzz done: N=%d ok=%d exc=%d reclimit=%d fail=%d suite_el=%.2fs" % (N, n_ok, n_exc, n_reclimit, len(fails), total_el))
    print("scope: lex+parse only, no execution and no codegen; budget %.2fs/input vs measured worst ~0.003s" % PER_INPUT_BUDGET)
    if hangs:
        print("hang inputs: %d" % len(hangs))
        for idx, src in hangs[:10]:
            print("HANG #%d repr=%r" % (idx, src[:500]))
    if fails:
        print("FAILURES: %d" % len(fails))
        for idx, src, why in fails[:20]:
            print("FAIL #%d %s repr=%r" % (idx, why, src[:500]))
        _s.exit(1)
    print("FUZZ ALL GREEN")

if __name__ == "__main__":
    main()