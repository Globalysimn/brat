"""A map key is str, i64, f64, bool, a tag or none, at -O0 and at -O2 alike.

The rule was already written down. What this file pins is the behaviour
around it:

* `m[[1, 2]] = 5` used to hang. `map_set_v` raised while holding
  `_brat_ctr_lock`, and `vraise` builds a `Val` (vvec, then vec_push), so the
  raise waited for the lock it was holding.
* `m.get([1, 2])` and the other map methods answer an err value that a
  `try`/`catch` can see.
* a map literal with a scalar key reads back: `{1: "a"}[1]` is `a`.
* `{[1, 2]: 3}` is that same err value. It used to be a half-built map that
  printed `{}`.
* a NaN key is one key. Hashing the bit pattern alone added a new entry on
  every store (`nan != nan`).

The shape of the refusal is the one `test_forge.py` pins for `$type`: an err
value, not a raise.
"""
import _cxx
import _scratch

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-mapkey-")

BAD = "unhashable map key"


def write(name, body):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write("fn main() -> i64 {\n%s\n  return 0\n}\n" % body)
    return path


def run(*argv, timeout=180):
    return subprocess.run(list(argv), capture_output=True, text=True, timeout=timeout)


def run_both(path, name):
    """(-O0 output, -O2 built output) for one program, both asserted equal."""
    run_o0 = run(BIN, "run", "-O0", path, timeout=60)
    assert run_o0.returncode == 0, "%s: -O0 rc=%d %r" % (
        name, run_o0.returncode, run_o0.stdout + run_o0.stderr)
    exe = os.path.join(TMP, name + ".exe")
    built = run(BIN, "build", "-O2", path, "-o", exe, timeout=300)
    assert built.returncode == 0, "%s: build %r" % (name, built.stdout + built.stderr)
    native = run(exe, timeout=60)
    assert native.returncode == 0, "%s: native rc=%d %r" % (
        name, native.returncode, native.stdout + native.stderr)
    assert native.stdout == run_o0.stdout, "%s: -O0/-O2 differ\n  -O0: %r\n  -O2: %r" % (
        name, run_o0.stdout, native.stdout)
    return run_o0.stdout


def main():
    # 1. Every way a container key can reach a map answers the same err value,
    #    in the same words at -O0 and -O2, and none of them waits for a lock it
    #    already holds.
    cases = {
        "store_vec": '  let mut m = {"a": 1}\n  m[[1, 2]] = 5\n  print(m)',
        "store_struct": '  let mut m = {"a": 1}\n  m[Point { x: 1, y: 2 }] = 5\n  print(m)',
        "get_vec": '  let m = {"a": 1}\n  print(m.get([1, 2]))',
        "has_struct": '  let m = {"a": 1}\n  print(m.has(Point { x: 1, y: 2 }))',
        "del_vec": '  let mut m = {"a": 1}\n  print(m.del([1, 2]))',
        "set_vec": '  let mut m = {"a": 1}\n  print(m.set([1, 2], 3))',
        "get_or_vec": '  let m = {"a": 1}\n  print(m.get_or([1, 2], 7))',
        "literal_vec": '  let m = {[1, 2]: 3}\n  print(m)',
        "get_tag_vec": '  let m = {"a": 1}\n  print(m.get(Ok([1, 2])))',
        "set_tag_map": '  let mut m = {"a": 1}\n  print(m.set(Some({"k": 1}), 3))',
        "get_closure": '  let m = {"a": 1}\n  let f = fn(x) { return x }\n  print(m.get(f))',
        "store_tag_vec": '  let mut m = {"a": 1}\n  m[Ok([1, 2])] = 5\n  print(m)',
    }
    # A bare `m[k] = v` throws its value away, the same shape `test_forge.py`
    # pins for `$type`, so the observable result there is an unchanged map and
    # not a message. What must not happen is a hang or a store that
    # half-happened.
    quiet = {"store_vec", "store_struct", "store_tag_vec"}
    for name, body in sorted(cases.items()):
        out = run_both(write(name + ".brat", body), name)
        if name in quiet:
            assert out == "{'a': 1}\n", "%s: map changed: %r" % (name, out)
        else:
            assert BAD in out, "%s: said %r, wanted %r" % (name, out, BAD)

    # 2. The value is catchable and the map keeps the keys it already had: the
    #    store that was refused did not half-happen.
    caught = write("caught.brat",
                   '  let mut m = {"a": 1}\n'
                   '  try { raise(m.get([1, 2])) } catch e { print("caught " + e) }\n'
                   '  print(m)\n'
                   '  print(len(m))')
    out = run_both(caught, "caught")
    # `raise` re-raises the value, so the message a `catch` binds is the tag
    # itself; the point here is that it travels as a value and that the map is
    # still whole afterwards.
    assert out == "caught Err('%s')\n{'a': 1}\n1\n" % BAD, repr(out)

    # 3. The keys a map does hold still work, a scalar literal is readable, and
    #    `1`, `1.0` and `true` stay three keys.
    good = write("good.brat",
                 '  let mut m = {1: "a", true: "b", 1.5: "c", "s": "d", Ok(1): "e"}\n'
                 '  print(m[1])\n  print(m[true])\n  print(m[1.5])\n  print(m["s"])\n'
                 '  print(m[Ok(1)])\n  print(len(m))\n'
                 '  let mut n = {}\n  n[2] = "x"\n  n[2.0] = "y"\n  n[false] = "z"\n'
                 '  print(len(n))\n  print(n[2] + n[2.0] + n[false])\n'
                 '  print(m.keys())')
    want = "a\nb\nc\nd\ne\n5\n3\nxyz\n[1, true, 1.5, 's', Ok(1)]\n"
    out = run_both(good, "good")
    assert out == want, "keys: %r wanted %r" % (out, want)

    # 4. A NaN key is one key. IEEE says NaN != NaN, so hashing the bit pattern
    #    added a new entry per store and answered `has` false a line after the
    #    store that made it; the expression `nan == nan` stays false.
    nan = write("nan.brat",
                '  let a = "nan".parse_float()?\n'
                '  let b = "nan".parse_float()?\n'
                '  let mut m = {}\n'
                '  m[a] = 1\n'
                '  print(m.has(b))\n'
                '  m[b] = 2\n'
                '  print(len(m))\n'
                '  print(a == b)\n'
                '  print(m)')
    out = run_both(nan, "nan")
    assert out == "true\n1\nfalse\n{nan: 2}\n", "nan: %r" % (out,)

    tag_hash = write("tag_hash.brat",
                     '  let a = "nan".parse_float()?\n'
                     '  let b = "nan".parse_float()?\n'
                     '  let mut m = {}\n'
                     '  m[Ok(1)] = "int"\n'
                     '  m[Ok(1.0)] = "flt"\n'
                     '  m[Ok(a)] = "nan1"\n'
                     '  m[Ok(b)] = "nan2"\n'
                     '  print(len(m), m[Ok(1)], m[Ok(1.0)], m[Ok(a)])\n'
                     '  let mut big = {}\n'
                     '  for i in 0..2000 { big[Some(i)] = i * 2 }\n'
                     '  print(len(big), big[Some(0)], big[Some(1999)])')
    out = run_both(tag_hash, "tag_hash")
    assert out == "3 int flt nan2\n2000 0 3998\n", "tag_hash: %r" % (out,)

    print("map keys: one sentence at -O0 and -O2, no hang; scalar literal keys readable")


main()
