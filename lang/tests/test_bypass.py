"""The ownership check must not be switchable off from inside a program.

`umeth` in borrow.py is keyed by method name, not by type, and mutate_kind
returned early on a hit -- never reaching MUTATORS. One `impl` block anywhere
in the file therefore turned `let` into "anything goes":

    impl Foo { fn push(self, x: i64) -> i64 { return 0; } ... }
    let v = [1, 2, 3]
    v.push(4)                 -> 0 error(s)

The rule: a built-in mutator always writes. A user method with that name and
without `mut self` is rejected; with `mut self` it is fine and makes the check
stricter, not weaker.
"""
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")
OWN = os.path.join(ENGINE, "borrow.py")

ENV = dict(os.environ)
ENV["PYTHONIOENCODING"] = "utf-8"
ENV["BRAT_FILES"] = "1"

n = 0
FAILED = []
TMP = _scratch.mkdtemp(prefix="brat-bypass-")

MUTATORS = ["push", "pop", "sort", "sort_by", "ins", "del", "set"]


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


def check_file(path):
    r = subprocess.run([sys.executable, "-X", "utf8", CLI, "check", path],
                       capture_output=True, text=True, encoding="utf-8",
                       timeout=180, env=ENV)
    return r.returncode, r.stdout + r.stderr


def impl_block(names, mut_self=False):
    recv = "mut self" if mut_self else "self"
    sigs = {
        "push": "%s, x: i64" % recv,
        "pop": recv,
        "sort": recv,
        "sort_by": "%s, f" % recv,
        "ins": "%s, i: i64, x: i64" % recv,
        "del": "%s, i: i64" % recv,
        "set": "%s, i: i64, x: i64" % recv,
    }
    body = []
    for nm in names:
        args = sigs[nm].split(", ")[1:]
        params = (", ".join(["%s: i64" % a for a in args])) if args else ""
        body.append("  fn %s(%s) -> i64 { return 0 }" % (nm, params))
    return "impl Foo {\n" + "\n".join(body) + "\n}\n\n"


USE = """fn main() -> i64 {
  let v = [1, 2, 3]
  v.push(4)
  v.pop()
  0
}
"""


def all_mutators_shadowed():
    src = impl_block(MUTATORS) + USE
    p = write("b1_all.brat", src)
    rc, out = check_file(p)
    check(rc != 0, "all 7 mutators: check must reject (rc=%d)" % rc)
    check("reserved" in out,
          "all 7 mutators: want the reserved diagnostic, got %r" % out[:250])
    check("cannot mutate immutable" in out,
          "all 7 mutators: ownership check must fire (out=%r)" % out[:250])


def one_mutator_shadowed():
    for nm in MUTATORS:
        src = impl_block([nm]) + USE
        p = write("b2_%s.brat" % nm, src)
        rc, out = check_file(p)
        check(rc != 0, "one mutator `%s`: check must reject (rc=%d)" % (nm, rc))
        check("cannot mutate immutable" in out or "reserved" in out,
              "one mutator `%s`: ownership check must fire" % nm)


def mut_self_method_is_allowed():
    src = ("impl Box {\n"
           "  fn set(mut self, i: i64, x: i64) -> i64 { return self.v }\n"
           "}\n\n"
           "fn main() -> i64 {\n"
           "  let mut b = Box { v: 1 }\n"
           "  b.set(0, 2)\n"
           "  print(b.v)\n"
           "  0\n"
           "}\n")
    p = write("b3_mutself.brat", src)
    rc, out = check_file(p)
    check(rc == 0, "a `mut self` method is valid code (rc=%d, out=%r)"
          % (rc, out[:300]))


def mut_self_named_push_still_checks():
    src = impl_block(["push"], mut_self=True) + USE
    p = write("b4_pushmut.brat", src)
    rc, out = check_file(p)
    check(rc != 0, "`push` with `mut self`: writing to immutable is still an error (rc=%d)" % rc)
    check("cannot mutate immutable" in out,
          "`push` with `mut self`: want an ownership error, got %r" % out[:250])


def no_impl_still_rejects():
    p = write("b5_plain.brat", USE)
    rc, out = check_file(p)
    check(rc != 0, "no impl: writing to immutable is an error (rc=%d)" % rc)
    check("cannot mutate immutable" in out,
          "no impl: want cannot mutate immutable, got %r" % out[:250])


def spawn_write_through_immutable_rejected():
    """A thread does not grant the right to write.

    walk_stmt used to force `mut = True` on every capture, and check_mutate /
    check_write returned early on spawn_frozen. Writing through a container the
    program declared immutable therefore passed check with zero errors.
    """
    cases = {
        "s1_push": '  v.push(4)',
        "s2_set": '  m.set("b", 2)',
        "s3_index": '  v[0] = 99',
    }
    for tag, stmt in cases.items():
        src = ('fn main() {\n'
               '    let v = [1, 2, 3]\n'
               '    let m = {"a": 1}\n'
               '    spawn {\n%s\n    }\n'
               '    print("{v} {m}")\n'
               '}\n') % stmt
        p = write("%s.brat" % tag, src)
        rc, out = check_file(p)
        check(rc != 0, "%s: write to immutable inside spawn must be rejected (rc=%d)"
              % (tag, rc))
        check("immutable" in out,
              "%s: want an immutability diagnostic, got %r" % (tag, out[:250]))


def spawn_write_with_mut_still_allowed():
    # A mut binding is required to write at all; the write itself is legal once a
    # `join_all()` orders it before the parent reads (see test_capability.py).
    src = ('fn main() {\n'
           '    let mut v = [1, 2, 3]\n'
           '    spawn {\n'
           '        v[0] = 99\n'
           '    }\n'
           '    join_all()\n'
           '    print("{v}")\n'
           '}\n')
    p = write("s4_mutok.brat", src)
    rc, out = check_file(p)
    check(rc == 0, "let mut + spawn must pass (rc=%d, out=%r)" % (rc, out[:250]))


def spawn_rebind_still_reported():
    src = ('fn main() {\n'
           '    let mut v = 1\n'
           '    spawn {\n'
           '        v = 2\n'
           '    }\n'
           '    print(v)\n'
           '}\n')
    p = write("s5_rebind.brat", src)
    rc, out = check_file(p)
    check(rc != 0 and "captured" in out,
          "rebind inside spawn must report a captured write (rc=%d, out=%r)"
          % (rc, out[:250]))


def main():
    for fn in (all_mutators_shadowed, one_mutator_shadowed,
               mut_self_method_is_allowed, mut_self_named_push_still_checks,
               no_impl_still_rejects, spawn_write_through_immutable_rejected,
               spawn_write_with_mut_still_allowed,
               spawn_rebind_still_reported):
        try:
            fn()
        except Exception as e:  # noqa: BLE001
            FAILED.append("%s raised %r" % (fn.__name__, e))
            print("  ERROR in %s: %r" % (fn.__name__, e))

    if FAILED:
        print("\nBYPASS FAILED — %d problem(s), %d ok:" % (len(FAILED), n))
        for f in FAILED:
            print("  - %s" % f)
        sys.exit(1)
    print("BYPASS ALL GREEN (%d asserts)" % n)


if __name__ == "__main__":
    main()