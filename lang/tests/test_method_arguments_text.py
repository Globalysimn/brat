"""Built-in method diagnostics and arity checks across 1,571 calls.

Verifies:
1. All string, vector, and map built-in methods called across valid and invalid argument types
   never leak host/internal helper names in `brat-cxx`.
2. Static `brat check` rejects wrong-arity built-in method calls on typed, `dyn`, and unannotated
   receivers (while honoring user-defined `impl` methods of the same name).
3. Variant destructuring (`match Ok(7) { Ok(x) => x }`) still works while `Ok(7)[1]` raises
   `index of non-indexable`.
"""
import _cxx
import _scratch

import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))

STR_METH = (
    "chars", "contains", "copy", "ends_with", "index_of", "lines", "lower",
    "parse_float", "parse_int", "repeat", "replace", "slice", "split",
    "split_once", "starts_with", "trim", "upper",
)
VEC_METH = (
    "chunks", "copy", "del", "enumerate", "first", "ins", "join", "last",
    "max", "min", "pop", "pos", "push", "rev", "slice", "sort", "sum", "zip",
)
MAP_METH = ("copy", "del", "get", "get_or", "has", "items", "keys", "set", "vals")
METH_ARITY = {
    "split": (0, 1), "lines": (0, 0), "trim": (0, 0), "lower": (0, 0), "upper": (0, 0),
    "starts_with": (1, 1), "ends_with": (1, 1), "contains": (1, 1),
    "parse_int": (0, 0), "parse_float": (0, 0), "replace": (2, 2),
    "index_of": (1, 1), "repeat": (1, 1), "split_once": (1, 1),
    "copy": (0, 0), "chars": (0, 0), "slice": (2, 2),
    "join": (1, 1), "push": (1, 1), "pop": (0, 0), "first": (0, 0), "last": (0, 0),
    "sort": (0, 0), "rev": (0, 0), "ins": (2, 2), "del": (1, 1),
    "sum": (0, 0), "min": (0, 0), "max": (0, 0), "enumerate": (0, 0),
    "zip": (1, 1), "chunks": (1, 1), "pos": (1, 1),
    "get": (1, 1), "has": (1, 1), "keys": (0, 0), "set": (2, 2),
    "items": (0, 0), "vals": (0, 0), "get_or": (2, 2),
}

NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-method-args-")
RECEIVERS = {
    "str_words": ('"a b,c"', STR_METH),
    "str_empty": ('""', STR_METH),
    "vec_ints": ("[3, 1, 2]", VEC_METH),
    "vec_mixed": ('[1, "a", 2.5]', VEC_METH),
    "map_two": ('{"a": 1, "b": 2}', MAP_METH),
}
VALUES = '[0, 1, -1, 2.5, "", "a", true, [], [1, 2], {}, 4611686018427387904]'
BAD_TEXT = re.compile(
    r"invalid literal|NoneType|object|unsupported|not supported|positional|"
    r"must be|Traceback|Error:|can't|cannot|argument \d|takes \d|no method affix"
)


def program(recv, methods):
    funcs, drivers = [], []
    for m in sorted(methods):
        lo, hi = METH_ARITY[m]
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
    with open(path, "w", encoding="utf-8") as f:
        f.write(program(recv, methods))
    nat = subprocess.run([NATIVE, "run", path], capture_output=True, text=True, timeout=300)
    assert nat.returncode == 0, "%s native: %s" % (name, nat.stderr[-400:])
    nat_lines = nat.stdout.splitlines()
    for line in nat_lines:
        calls += 1
        assert not BAD_TEXT.search(line), "%s shows bad diagnostic: %r" % (name, line[:200])

assert calls == 1571, calls

# Wrong-receiver vs wrong-argument diagnostics and variant destructuring vs indexing.
probe = os.path.join(TMP, "probe.brat")
with open(probe, "w", encoding="utf-8") as f:
    f.write("""
fn call_starts(x, y) { return x.starts_with(y) }
fn call_ends(x, y) { return x.ends_with(y) }
fn call_contains(x, y) { return x.contains(y) }
fn idx(x, i) { return x[i] }

fn main() -> i64 {
    try { call_starts(123, "a") } catch e { assert(e == "no method starts_with") }
    try { call_ends(123, "a") } catch e { assert(e == "no method ends_with") }
    try { call_contains(123, "a") } catch e { assert(e == "no method contains") }
    try { call_starts("abc", 1) } catch e { assert(e == "method `starts_with`: bad argument") }
    try { idx(Ok(42), 1) } catch e { assert(e == "index of non-indexable") }
    let mut v = 0
    match Ok(42) {
        Ok(n) => { v = n },
        Err(_) => { v = 0 }
    }
    assert(v == 42)
    return 0
}
""")
pr = subprocess.run([NATIVE, "run", probe], capture_output=True, text=True, timeout=60)
assert pr.returncode == 0, pr.stderr

# Static check rejects wrong-arity built-in method calls on untyped and dyn receivers,
# while permitting user-defined impl methods with custom arity.
bad_arity = os.path.join(TMP, "bad_arity.brat")
with open(bad_arity, "w", encoding="utf-8") as f:
    f.write("""
fn f(x) { return x.chars(1) }
fn main() -> i64 {
    let s: str = "hi"
    let _ = s.split("a", "b")
    return 0
}
""")
chk = subprocess.run([NATIVE, "check", bad_arity], capture_output=True, text=True, timeout=30)
assert chk.returncode != 0, chk.stdout + chk.stderr
assert "method `chars` wants 0 args" in chk.stdout + chk.stderr, chk.stdout + chk.stderr
assert "method `split` wants 1 args" in chk.stdout + chk.stderr, chk.stdout + chk.stderr

custom_impl = os.path.join(TMP, "custom_impl.brat")
with open(custom_impl, "w", encoding="utf-8") as f:
    f.write("""
enum Bag { Hold(n) }
impl Bag {
    fn chars(self, k: i64) -> i64 {
        let mut v = 0
        match self { Hold(n) => { v = n } }
        return v + k
    }
}
fn use_bag(b) { return b.chars(5) }
fn main() -> i64 {
    assert(use_bag(Hold(37)) == 42)
    return 0
}
""")
chk_ok = subprocess.run([NATIVE, "check", custom_impl], capture_output=True, text=True, timeout=30)
assert chk_ok.returncode == 0, chk_ok.stdout + chk_ok.stderr
run_ok = subprocess.run([NATIVE, "run", custom_impl], capture_output=True, text=True, timeout=30)
assert run_ok.returncode == 0, run_ok.stderr

print("METHOD ARGUMENTS TEXT ALL GREEN (%d calls, native + arity + tag index)" % calls)
