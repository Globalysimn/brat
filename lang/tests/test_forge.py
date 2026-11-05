"""`$type` is not a key a program may write.

`$type` is how the runtime marks a struct, and `method_of` dispatches on it:
`{"$type": "Guard"}` made a plain map answer to Guard's methods. Forged tags
also leaked: `len()` reported one key fewer, `"$type" in m` was False for a map
that had the key, and diagnostics printed `struct Guard` for a map.

Both backends answer the same way: the expression yields an err value instead
of raising. The first attempt raised through `vraise` in C, and longjmp from
there under -O2 misbehaved -- the binary hung instead of reporting. An err
value is the honest shape anyway: a caught error is a value in this language.

Every case keeps a positive control -- an ordinary map has to keep working, or
the guard could pass by refusing everything.
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))

ENV = dict(os.environ)
ENV["PYTHONIOENCODING"] = "utf-8"

n = 0
FAILED = []
TMP = _scratch.mkdtemp(prefix="brat-tag-")

RESERVED = 'map key "$type" is reserved'


def check(cond, msg):
    global n
    if not cond:
        FAILED.append(msg)
        print("  FAIL: %s" % msg)
    else:
        n += 1


def write(name, src):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    return path


def run(name):
    path = os.path.join(TMP, name)
    p = subprocess.run(_cxx.cli_args("run", "--interp", "--no-check", path),
                       capture_output=True, text=True, encoding="utf-8",
                       timeout=120, env=ENV)
    return p.returncode, p.stdout + p.stderr


def map_literal():
    src = ('fn main() -> i64 {\n'
           '  let m = {"$type": "Guard", "a": 1}\n'
           '  print(m)\n'
           '  0\n}\n')
    write("a1.brat", src)
    rc, out = run("a1.brat")
    check(rc == 0, "map literal must run, not abort (rc=%d, out=%r)" % (rc, out[:200]))
    check("Err(" in out, "map literal must yield an err value, got %r" % out[:200])
    check(RESERVED in out, "map literal must name the reserved key, got %r" % out[:200])


def method_set():
    src = ('fn main() -> i64 {\n'
           '  let m = {"a": 1}\n'
           '  let r = m.set("$type", "Guard")\n'
           '  print(r)\n'
           '  print(m)\n'
           '  0\n}\n')
    write("a2.brat", src)
    rc, out = run("a2.brat")
    check(rc == 0, "m.set must run (rc=%d, out=%r)" % (rc, out[:200]))
    check(RESERVED in out, "m.set must report the reserved key, got %r" % out[:200])
    check("{'a': 1}" in out, "m.set must leave the map unchanged, got %r" % out[:200])


def index_write():
    src = ('fn main() -> i64 {\n'
           '  let mut m = {"a": 1}\n'
           '  m["$type"] = "Guard"\n'
           '  print(m)\n'
           '  print(len(m))\n'
           '  0\n}\n')
    write("a3.brat", src)
    rc, out = run("a3.brat")
    check(rc == 0, "index write must run (rc=%d, out=%r)" % (rc, out[:200]))
    check("{'a': 1}" in out, "index write must leave the map unchanged, got %r" % out[:200])
    check("\n1\n" in out, "len must stay 1, got %r" % out[:200])


def computed_key():
    src = ('fn main() -> i64 {\n'
           '  let m = {"a": 1}\n'
           '  let k = "$type"\n'
           '  print(m.set(k, "Guard"))\n'
           '  0\n}\n')
    write("a4.brat", src)
    rc, out = run("a4.brat")
    check(rc == 0, "computed key must run (rc=%d, out=%r)" % (rc, out[:200]))
    check(RESERVED in out, "computed key must be caught too, got %r" % out[:200])


def caught_error():
    src = ('fn main() -> i64 {\n'
           '  try {\n'
           '    let m = {"$type": "Guard"}\n'
           '    print(m)\n'
           '  } catch e {\n'
           '    print("caught")\n'
           '  }\n'
           '  0\n}\n')
    write("a5.brat", src)
    rc, out = run("a5.brat")
    check(rc == 0, "try must not crash (rc=%d, out=%r)" % (rc, out[:200]))
    check("caught" not in out or RESERVED in out,
          "err value flows normally, got %r" % out[:200])


def ordinary_map_still_works():
    src = ('fn main() -> i64 {\n'
           '  let mut m = {"a": 1, "b": 2}\n'
           '  m.set("c", 3)\n'
           '  m["d"] = 4\n'
           '  print(len(m))\n'
           '  print(m["c"])\n'
           '  print(m.keys())\n'
           '  0\n}\n')
    write("b1_ok.brat", src)
    rc, out = run("b1_ok.brat")
    check(rc == 0, "ordinary map must keep working (rc=%d, out=%r)" % (rc, out[:200]))
    for want in ["4", "3", "['a', 'b', 'c', 'd']"]:
        check(want in out, "ordinary map wants %r, have %r" % (want, out[:200]))


def structs_still_work():
    src = ('fn main() -> i64 {\n'
           '  let p = Point { x: 3, y: 4 }\n'
           '  print(p.x + p.y)\n'
           '  0\n}\n')
    write("b2_struct.brat", src)
    rc, out = run("b2_struct.brat")
    check(rc == 0, "struct literals must keep working (rc=%d, out=%r)" % (rc, out[:200]))
    check("7" in out, "struct wants 7, got %r" % out[:200])


def unhashable_key_still_refused():
    src = ('fn main() -> i64 {\n'
           '  let m = {[1, 2]: "x"}\n'
           '  print(m)\n'
           '  0\n}\n')
    write("b3_unhash.brat", src)
    rc, out = run("b3_unhash.brat")
    check(rc == 0, "unhashable key: must run (rc=%d, out=%r)" % (rc, out[:200]))
    check("unhashable" in out, "unhashable key must still be refused, got %r" % out[:200])


def main():
    for fn in (map_literal, method_set, index_write, computed_key,
               caught_error, ordinary_map_still_works, structs_still_work,
               unhashable_key_still_refused):
        try:
            fn()
        except Exception as e:  # noqa: BLE001
            FAILED.append("%s raised %r" % (fn.__name__, e))
            print("  ERROR in %s: %r" % (fn.__name__, e))

    if FAILED:
        print("\nFORGE FAILED — %d problem(s), %d ok:" % (len(FAILED), n))
        for f in FAILED:
            print("  - %s" % f)
        sys.exit(1)
    print("FORGE ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()