"""`brat-cxx check` says what `brat check` says, byte for byte.

The reference runs here, in this process: `borrow.py` and then `typer.py`, the
same two passes the CLI runs, so a whole corpus costs one Python pass plus one
`brat-cxx check` per file. Imported files are part of the comparison because the
loader resolves them on the C++ side too.

The fixture set is small and each line of its output is asserted literally: the
corpus alone would pass even if both checkers reported nothing, because a corpus
file is supposed to check clean. The ownership rules have their own fixture file,
test_cxx_borrow.py; these fixtures are the type pass and the lines where the two
passes meet.
"""

import _cxx
import _scratch

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, "..", "engine")
ROOT = os.path.abspath(os.path.join(HERE, ".."))
BIN = _cxx.BIN
TMP = _scratch.mkdtemp(prefix="brat-check-")

sys.path.insert(0, ENGINE)
import brat.parser  # noqa: E402,F401  (the engine expects its sibling imports)
import diag  # noqa: E402
import typer as TP  # noqa: E402
import borrow as OWN  # noqa: E402

_LOADER = [None]


def oracle():
    """`brat.py` loaded by path: the package of the same name is the engine."""
    if _LOADER[0] is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "brat_oracle_check", os.path.join(ENGINE, "brat.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _LOADER[0] = mod
    return _LOADER[0]


OWN.load_brat = lambda path=None: oracle()


def pub_sigs(prog):
    """What one file exports: `pub fn` modes and `pub enum` variants."""
    out = {}
    for s in prog[1]:
        if isinstance(s, tuple) and s and s[0] == "pub":
            inner = s[1]
            if isinstance(inner, tuple) and inner:
                if inner[0] == "fn":
                    out[inner[1]] = OWN.fn_modes(inner[2])
                elif inner[0] == "enum":
                    for vname, _fields in inner[2]:
                        out[vname] = []
    return out


def reference(path):
    """What `brat check` prints, computed without a subprocess."""
    loaded = oracle().load_file(path)
    progs = [(s.path, s.prog) for s in loaded.files]
    srcs = {s.path: s.src for s in loaded.files}
    out = []
    per_file = [pub_sigs(prog) for _, prog in progs]
    for idx, (f, prog) in enumerate(progs):
        merged = {}
        for j, sigs in enumerate(per_file):
            if j != idx:
                merged.update(sigs)
        errs, warns = OWN.check(prog, merged)
        out.append("%s: %d error(s), %d warning(s)" % (f, len(errs), len(warns)))
        out.append(diag.to_human(f, srcs[f],
                                 diag._entries_to_diags(f, errs, "error")).rstrip("\n"))
        out.append(diag.to_human(f, srcs[f],
                                 diag._entries_to_diags(f, warns, "warning")).rstrip("\n"))
    merged = {}
    for i, (_, prog) in enumerate(progs):
        if i:
            TP.collect_sigs(prog, merged)
    for f, prog in progs:
        errs = TP.check(prog, merged)
        out.append("%s: %d type error(s)" % (f, len(errs)))
        out.append(diag.to_human(f, srcs[f],
                                 diag._entries_to_diags(f, errs, "error")).rstrip("\n"))
    return "\n".join(x for x in out if x) + "\n"


def cxx(path):
    r = subprocess.run([BIN, "check", path], capture_output=True, text=True, timeout=120)
    # 2 is a load error: the program never reaches a checker on either side.
    assert r.returncode in (0, 1, 2), "brat-cxx check rc=%d %s" % (r.returncode, r.stderr[-300:])
    return r.stdout, r.returncode


# Programs the loader refuses before any checker runs, and the words it uses.
# They are in the corpus on purpose: a refusal is part of the contract.
REFUSED = {"own_priv.brat": "function `helper` is private in module `modapi`"}


FIXTURES = {
    'ret.brat': 'fn f(a: i64) -> i64 {\n  return "x"\n}\n',
    'arg.brat': 'fn f(a: i64) -> i64 {\n  return a\n}\nlet x = f("s")\n',
    'arity.brat': 'fn f(a: i64) -> i64 {\n  return a\n}\nlet x = f()\n',
    'ann.brat': 'let x: i64 = "s"\n',
    'meth.brat': 'let v = [1, 2]\nprint(v.nope())\n',
    'field.brat': 'let s = Point { x: 1 }\nprint(s.y)\n',
    'setfield.brat': 'let s = Point { x: 1 }\ns.y = 2\n',
    'push.brat': 'let mut v = [1]\nv.push("s")\n',
    'assign.brat': 'let mut x: i64 = 1\nx = "s"\n',
    'slice.brat': 'print(slice_len(1, 2))\n',
    'sliceptr.brat': 'print(slice_of_ptr(1, 2, "f16"))\n',
    'libopen.brat': 'let p = lib_open(1)\n',
    'call.brat': 'print(call(1, "i64(i64)", "s"))\n',
    'forrange.brat': 'for i in 0.5..3 {\n  print(i)\n}\n',
    'own.brat': 'let x = own(1, 2)\n',
    'strmeth.brat': 'let s = str(1)\nprint(s.nope())\n',
    'mapget.brat': 'let m = {"a": 1}\nprint(m.get())\n',
    'arena.brat': 'fn f() -> i64 {\n  arena {\n    let v = [1]\n    return v\n  }\n}\n',
    'fnvalue.brat': 'fn double_it(x: i64) -> i64 {\n  return x * 2\n}\nlet g = double_it\n',
    'fnarg.brat': 'fn double_it(x: i64) -> i64 {\n  return x * 2\n}\nlet v = [1]\nprint(v.map(double_it))\n',
    'cb_ok.brat': 'fn cmp(a: i64, b: i64) -> i64 {\n  return a - b\n}\nfn run(f: fn(i64, i64) -> i64) -> i64 {\n  return f(1, 2)\n}\nlet x = run("cmp")\n',
    'cb_arity.brat': 'fn one(a: i64) -> i64 {\n  return a\n}\nfn run(f: fn(i64, i64) -> i64) -> i64 {\n  return 1\n}\nlet x = run("one")\n',
    'cb_default_arity.brat': 'fn one(a: i64, b: i64 = 0) -> i64 {\n  return a + b\n}\nfn run(f: fn(i64) -> i64) -> i64 {\n  return 1\n}\nlet x = run("one")\n',
    'cb_param_abi.brat': 'fn wrong(a: f64) -> i64 {\n  return 1\n}\nfn run(f: fn(i64) -> i64) -> i64 {\n  return 1\n}\nlet x = run("wrong")\n',
    'cb_ret_abi.brat': 'fn wrong(a: i64) -> f64 {\n  return 1.0\n}\nfn run(f: fn(i64) -> i64) -> i64 {\n  return 1\n}\nlet x = run("wrong")\n',
    'cb_width_abi.brat': 'fn wrong(a: i32) -> i64 {\n  return a\n}\nfn run(f: fn(i64) -> i64) -> i64 {\n  return 1\n}\nlet x = run("wrong")\n',
    'cb_unknown.brat': 'fn run(f: fn(i64) -> i64) -> i64 {\n  return 1\n}\nlet x = run("nope")\n',
    'multi.brat': 'fn f(a: i64) -> i64 {\n  return "x"\n}\nfn g(a: i64) -> i64 {\n  return a\n}\nlet x = g(f(1), g(2))\nlet y: str = 3\nlet v = [1]\nv.push("s")\nv.push(2, 3)\n',
}

# What the reference prints for these, message by message. A diff against the
# reference catches drift; these catch a reference and a port being wrong
# together, which is the one thing a differential test cannot see.
EXPECT = {
    "ret.brat": ["ret.brat:2: error: return: want i64, got str"],
    "arg.brat": ["arg.brat:4: error: arg 1 of `f`: want i64, got str"],
    "arity.brat": ["arity.brat:4: error: `f` wants 1 args, got 0"],
    "ann.brat": ["ann.brat:1: error: let `x`: annotated i64, got str"],
    "meth.brat": ["meth.brat:2: error: no method `nope` for vec[i64]"],
    "field.brat": ["field.brat:2: error: no field `y` on struct (has x)"],
    "setfield.brat": [
        "setfield.brat:2: error: assign to immutable `s.y` (want `let mut` or mut param)",
        "setfield.brat:2: error: no field `y` on struct (has x)",
    ],
    "push.brat": ["push.brat:2: error: method `push` arg 1: want i64, got str"],
    "assign.brat": ["assign.brat:2: error: assign to `x`: want i64, got str"],
    "slice.brat": ["slice.brat:1: error: `slice_len` wants 1 args, got 2"],
    "sliceptr.brat": ["sliceptr.brat:1: error: slice_of_ptr: kind must be the literal \"i64\", \"i32\", \"f64\", \"f32\" or \"u8\""],
    "libopen.brat": ["libopen.brat:1: error: lib_open: want str, got i64"],
    "call.brat": ["call.brat:1: error: call: arg 1 wants i64, got str"],
    "forrange.brat": ["forrange.brat:1: error: forrange bound: want i64, got f64"],
    "own.brat": ["own.brat:1: error: `own` wants 1 arg, got 2"],
    "strmeth.brat": ["strmeth.brat:2: error: no method `nope` for str"],
    "mapget.brat": ["mapget.brat:2: error: method `get` wants 1 args"],
    "arena.brat": [
        "arena.brat:4: error: arena: a value made inside the block is returned out of it  (hint: hand out a heap copy with `own(...)`, e.g. `return Err(own(msg))`)",
        "arena.brat:4: error: return: want i64, got vec[i64]",
    ],
    "fnvalue.brat": ["fnvalue.brat:4: error: use of `double_it` as a value: a fn name is a callback, not a value (hint: call it: `double_it()`)"],
    "fnarg.brat": [],
    "cb_ok.brat": [],
    "cb_arity.brat": ["cb_arity.brat:7: error: callback `one` takes 1 parameter(s), fn(i64,i64)->i64 wants 2"],
    "cb_default_arity.brat": ["cb_default_arity.brat:7: error: callback `one` takes 2 parameter(s), fn(i64)->i64 wants 1"],
    "cb_param_abi.brat": ["cb_param_abi.brat:7: error: callback `wrong` has signature fn(f64)->i64, expected fn(i64)->i64"],
    "cb_ret_abi.brat": ["cb_ret_abi.brat:7: error: callback `wrong` has signature fn(i64)->f64, expected fn(i64)->i64"],
    "cb_width_abi.brat": ["cb_width_abi.brat:7: error: callback `wrong` has signature fn(i32)->i64, expected fn(i64)->i64"],
    "cb_unknown.brat": ["cb_unknown.brat:4: error: unknown callback `nope`"],
    "multi.brat": [
        "multi.brat:10: error: cannot mutate immutable `v`: `v.push` writes through it",
        "multi.brat:11: error: cannot mutate immutable `v`: `v.push` writes through it",
        "multi.brat:2: error: return: want i64, got str",
        "multi.brat:7: error: `g` wants 1 args, got 2",
        "multi.brat:8: error: let `y`: annotated str, got i64",
        "multi.brat:10: error: method `push` arg 1: want i64, got str",
        "multi.brat:11: error: method `push` wants 1 args",
    ],
}


def main():
    _cxx.build()
    t0 = time.perf_counter()
    bad = 0

    fixtures = {}
    for name, src in FIXTURES.items():
        path = os.path.join(TMP, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        fixtures[name] = path

    corpus = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in ("vendor", "bin", ".git", "__pycache__")]
        for f in sorted(files):
            if f.endswith(".brat"):
                corpus.append(os.path.join(base, f))
    corpus.sort()

    refused = 0
    for path in corpus + [fixtures[n] for n in sorted(fixtures)]:
        name = os.path.basename(path)
        try:
            want = reference(path)
        except Exception as e:  # the loader refuses this program outright
            if name not in REFUSED or str(e) != REFUSED[name]:
                bad += 1
                print("LOAD %s: %s: %s" % (name, type(e).__name__, e))
                continue
            got, rc = cxx(path)
            if rc == 0 or REFUSED[name] not in got:
                bad += 1
                print("REFUSED %s: rc=%d got %r" % (name, rc, got[:200]))
            else:
                refused += 1
            continue
        got, rc = cxx(path)
        if got != want:
            bad += 1
            if bad <= 3:
                print("DIFF %s" % os.path.relpath(path, ROOT))
                wl, gl = want.split("\n"), got.split("\n")
                for i in range(max(len(wl), len(gl))):
                    a = wl[i] if i < len(wl) else "<none>"
                    b = gl[i] if i < len(gl) else "<none>"
                    if a != b:
                        print("  python: %r\n  c++   : %r" % (a, b))
                        break
        if rc != (1 if want.count(": error: ") else 0):
            bad += 1
            print("RC %s: rc=%d for %d errors"
                  % (os.path.relpath(path, ROOT), rc, want.count(": error: ")))

    # The fixture set is the reason this test can fail: every message is pinned.
    if refused != len(REFUSED):
        bad += 1
        print("REFUSED: %d file(s) refused, the list has %d" % (refused, len(REFUSED)))

    for name in sorted(EXPECT):
        got, _ = cxx(fixtures[name])
        lines = [l for l in got.split("\n") if ": error: " in l]
        lines = [l.replace(fixtures[name], name) for l in lines]
        if lines != EXPECT[name]:
            bad += 1
            print("EXPECT %s\n  want %r\n  got  %r" % (name, EXPECT[name], lines))

    # A module that does not exist is a load error, not a crash.
    r = subprocess.run([BIN, "check", os.path.join(TMP, "nope.brat")],
                       capture_output=True, text=True, timeout=60)
    if r.returncode != 2 or "load error" not in r.stdout:
        bad += 1
        print("MISSING rc=%d out=%r" % (r.returncode, r.stdout[:200]))

    dt = time.perf_counter() - t0
    assert bad == 0, "%d differences" % bad
    print("CXX CHECKER ALL GREEN (%d corpus + %d fixtures, identical diagnostics, %.2fs)"
          % (len(corpus), len(fixtures), dt))


main()
