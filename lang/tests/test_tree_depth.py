"""Depth of the tree, not depth of the nesting: the front end must not fall over.

`1+1+...+1` is one nesting level and ten thousand nodes. Every pass after the
parser walks the tree recursively, so an unbounded depth was a stack overflow --
a SIGSEGV, on a file that had already tokenised and parsed -- and both front ends
fell at the same input, which is why the differential tests never saw it.

The bound is `MAX_TREE_DEPTH` in `parser.cpp` / `kMaxTreeDepth` in `parser.cpp`:
far above hand-written code, far under the stack. This file pins four things:

  * a chain past the bound is refused by both front ends, with identical
    diagnostics, and neither dies by a signal;
  * a chain just under the bound is accepted and still builds and runs on both
    backends, so the bound is not hiding a broken middle;
  * the refusal names the line the deepest chain starts on;
  * the same shape in strings (`"a"+"b"+...`) is refused the same way.
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-depth-")

CXX = None
failures = 0


def fail(name, why):
    global failures
    failures += 1
    print("FAIL %s: %s" % (name, why))


def run(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)
    except subprocess.TimeoutExpired:
        return None


def chain(n, op="+"):
    return "print(" + op.join(["1"] * n) + ")\n"


def write(name, src):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def both_checks(path):
    """(python rc, text, cxx rc, text, cxx text without the type-error tail)."""
    py = run(_cxx.cli_args("check", path))
    cx = run([CXX, "check", path])
    ptext = py.stdout + py.stderr
    ctext = cx.stdout + cx.stderr
    py_only = "\n".join(l for l in ptext.splitlines() if "type error" not in l).strip()
    cx_only = "\n".join(l for l in ctext.splitlines() if "type error" not in l).strip()
    return py.returncode, ptext, cx.returncode, ctext, py_only, cx_only


def main():
    global CXX
    CXX = _cxx.build()

    # 1. Past the bound: refused, not a crash.
    for name, src in (("chain_9000", chain(9000)),
                      ("chain_20000", chain(20000)),
                      ("string_chain_9000", 'print(' + "+".join(['"a"'] * 9000) + ')\n')):
        path = write(name, src)
        py_rc, ptext, cx_rc, ctext, py_only, cx_only = both_checks(path)
        for tag, r, text in (("py", py_rc, ptext), ("cxx", cx_rc, ctext)):
            if r is None:
                fail(name, "%s timed out" % tag)
            elif r < 0 or r >= 128:
                fail(name, "%s died by signal (rc=%d)" % (tag, r))
            elif r == 0:
                fail(name, "%s accepted a tree past the bound" % tag)
            elif "expression tree too deep" not in text:
                fail(name, "%s refused without saying why:\n%s" % (tag, text[:400]))
        if py_only != cx_only:
            fail(name, "checkers disagree\n--- python\n%s\n--- cxx\n%s" % (py_only[:400], cx_only[:400]))
        if "expression tree too deep" in ptext and ":1:" not in ptext.splitlines()[0]:
            fail(name, "the refusal does not name a line: %r" % ptext.splitlines()[:1])

    # 2. Under the bound: accepted, and the whole pipeline still works.
    ok = write("chain_900", chain(900))
    py_rc, ptext, cx_rc, ctext, py_only, cx_only = both_checks(ok)
    if py_rc != 0:
        fail("chain_900", "refused a chain under the bound:\n%s" % ptext[:400])
    if py_only != cx_only:
        fail("chain_900", "checkers disagree on an accepted program")

    exe = os.path.join(TMP, "chain_900.exe")
    b = run(_cxx.cli_args("build", ok, "-o", exe))
    if b is None or b.returncode != 0:
        fail("chain_900", "build: %s" % ((b.stdout + b.stderr)[-400:] if b else "timeout"))
    else:
        nat = run([exe])
        run_o0 = run(_cxx.run_args(ok))
        for tag, r in (("native", nat), ("-O0", run_o0)):
            if r is None or r.returncode != 0 or r.stdout.strip() != "900":
                fail("chain_900", "%s %r/%r, want rc 0 and '900'"
                     % (tag, None if r is None else r.returncode,
                        None if r is None else r.stdout))

    # 3. The snippet in a diagnostic stays readable even when the line is huge.
    path = write("wide_line", chain(9000))
    py_rc, ptext, _cx_rc, _ctext, _po, _co = both_checks(path)
    snippet = [l for l in ptext.splitlines() if l.startswith("  | ")]
    if not snippet:
        fail("wide_line", "no snippet in the diagnostic")
    elif len(snippet[0]) > 260:
        fail("wide_line", "the snippet echoes %d characters of source" % len(snippet[0]))

    if failures:
        raise SystemExit("TREE DEPTH FAILED (%d)" % failures)
    print("TREE DEPTH ALL GREEN (both front ends, both backends)")


if __name__ == "__main__":
    main()
