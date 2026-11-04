"""The interpreter never shows CPython's own words for a method called with the wrong arguments.

Every method of a string, a vector and a map, on four kinds of receiver, is called with every
argument count it accepts and every combination of eleven values, wrong ones included. The
interpreter used to answer `invalid literal for int() with base 10: ''` for `"a".repeat("")`, and
the same for `chunks`, `del`, `ins` and `slice`. Whether the compiled program says the same
thing is not tested here: it does not (limits.md, "Where the backends differ").
"""
import _scratch

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
sys.path.insert(0, ENGINE)
import brat.runtime as R                                    # noqa: E402

TMP = _scratch.mkdtemp(prefix="brat-method-args-")
RECEIVERS = {
    "str_words": ('"a b,c"', R._STR_METH),
    "str_empty": ('""', R._STR_METH),
    "vec_ints": ("[3, 1, 2]", R._VEC_METH),
    "vec_mixed": ('[1, "a", 2.5]', R._VEC_METH),
    "map_two": ('{"a": 1, "b": 2}', R._MAP_METH),
}
VALUES = '[0, 1, -1, 2.5, "", "a", true, [], [1, 2], {}, 4611686018427387904]'
HOST_TEXT = re.compile(r"invalid literal|NoneType|object|unsupported|not supported|positional|"
                       r"must be|Traceback|Error:|can't|cannot|argument \d|takes \d")


def program(recv, methods):
    funcs, drivers = [], []
    for m in sorted(methods):
        lo, hi = R._METH_ARITY.get(m, (0, 2))
        body = ["fn d_%s() {" % m]
        for k in range(lo, hi + 1):
            params = ["", "a", "a, b"][k]
            funcs += ["fn f_%s_%d(%s) {" % (m, k, params), "  let mut r = " + recv,
                      "  return r.%s(%s)" % (m, params), "}"]
            args = "".join(", " + x for x in ("a", "b")[:k])
            line = 'try { print("%s/%d"%s, f_%s_%d(%s)) } catch e { print("%s/%d"%s, e) }' % (
                m, k, args, m, k, params, m, k, args)
            body += [line] if k == 0 else (["for a in V {", line, "}"] if k == 1
                                           else ["for a in V {", "for b in V {", line, "}", "}"])
        drivers += body + ["}"]
    main = ["const V = " + VALUES, "fn main() -> i64 {"] + ["d_%s()" % m for m in sorted(methods)] + [
        "return 0", "}"]
    return "\n".join(funcs + drivers + main) + "\n"


calls = 0
for name, (recv, methods) in RECEIVERS.items():
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w") as f:
        f.write(program(recv, methods))
    done = subprocess.run([sys.executable, os.path.join(ENGINE, "cli.py"), "run", "--interp", path],
                          capture_output=True, text=True, timeout=300)
    assert done.returncode == 0, "%s: %s" % (name, done.stderr[-300:])
    for line in done.stdout.splitlines():
        calls += 1
        assert not HOST_TEXT.search(line), "%s shows host text: %r" % (name, line[:200])

assert calls > 1000, calls
print("METHOD ARGUMENTS TEXT ALL GREEN (%d calls, no host text)" % calls)
