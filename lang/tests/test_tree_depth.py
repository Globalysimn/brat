"""Depth of the tree, not depth of the nesting: the front end must not fall over.

`1+1+...+1` is one nesting level and ten thousand nodes. Every pass after the
parser walks the tree recursively, so an unbounded depth is a stack overflow on
a file that has already tokenised and parsed.

The bound is `kMaxTreeDepth` in `parser.cpp`: far above hand-written code, far
under the stack. This file pins five things:

- a chain past the bound is refused by `check`, which does not die by a signal;
- the refusal names the line the chain starts on;
- the same shape in strings (`"a"+"b"+...`) is refused the same way;
- a chain under the bound is accepted and still builds and runs at -O0 and -O2;
- an `else if` chain or a nest of `if` under the bounds still builds at -O2,
  whose range analysis keeps state per branch and is recursive too.
"""
import _cxx
import _scratch
import os
import subprocess

TMP = _scratch.mkdtemp(prefix="brat-depth-")

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


def else_if_chain(n):
    src = "fn pick(c: i64) -> i64 {\n  if c == 0 {\n    return 0\n  }"
    for i in range(1, n):
        src += " else if c == %d {\n    return %d\n  }" % (i, i)
    return src + " else {\n    return -1\n  }\n}\nfn main() -> i64 {\n  print(pick(%d))\n  return 0\n}\n" % (n - 1)


def nested_ifs(n):
    return ("fn main() -> i64 {\n  let c = 7\n" + "  if c > 0 {\n" * n + "    print(c)\n"
            + "  }\n" * n + "  return 0\n}\n")


def write(name, src):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def checked(path):
    """(rc, text) of `check`; rc is None after a timeout."""
    r = run(_cxx.cli_args("check", path))
    if r is None:
        return None, ""
    return r.returncode, r.stdout + r.stderr


def main():
    # 1. Past the bound: refused, not a crash.
    for name, src in (("chain_9000", chain(9000)),
                      ("chain_20000", chain(20000)),
                      ("string_chain_9000", 'print(' + "+".join(['"a"'] * 9000) + ')\n')):
        path = write(name, src)
        rc, text = checked(path)
        if rc is None:
            fail(name, "check timed out")
        elif rc < 0 or rc >= 128:
            fail(name, "check died by signal (rc=%d)" % rc)
        elif rc == 0:
            fail(name, "check accepted a tree past the bound")
        elif "expression tree too deep" not in text:
            fail(name, "check refused without saying why:\n%s" % text[:400])
        elif ":1:" not in text.splitlines()[0]:
            fail(name, "the refusal does not name a line: %r" % text.splitlines()[:1])

    # 2. Under the bound: accepted, and the whole pipeline still works.
    ok = write("chain_900", chain(900))
    rc, text = checked(ok)
    if rc != 0:
        fail("chain_900", "refused a chain under the bound:\n%s" % text[:400])

    exe = os.path.join(TMP, "chain_900.exe")
    b = run(_cxx.cli_args("build", ok, "-o", exe))
    if b is None or b.returncode != 0:
        fail("chain_900", "build: %s" % ((b.stdout + b.stderr)[-400:] if b else "timeout"))
    else:
        nat = run([exe])
        run_o0 = run(_cxx.run_args(ok))
        for tag, r in (("-O2", nat), ("-O0", run_o0)):
            if r is None or r.returncode != 0 or r.stdout.strip() != "900":
                fail("chain_900", "%s %r/%r, want rc 0 and '900'"
                     % (tag, None if r is None else r.returncode,
                        None if r is None else r.stdout))

    # 3. The snippet in a diagnostic stays readable even when the line is huge.
    path = write("wide_line", chain(9000))
    _, text = checked(path)
    snippet = [l for l in text.splitlines() if l.startswith("  | ")]
    if not snippet:
        fail("wide_line", "no snippet in the diagnostic")
    elif len(snippet[0]) > 260:
        fail("wide_line", "the snippet echoes %d characters of source" % len(snippet[0]))

    # 4. Branches under the bounds: -O2 analyses them without dying by a signal.
    for name, src, want in (("else_if_300", else_if_chain(300), "299\n"),
                            ("nested_if_150", nested_ifs(150), "7\n")):
        path = write(name, src)
        exe = os.path.join(TMP, name + ".exe")
        b = run(_cxx.cli_args("build", path, "-O2", "-o", exe))
        if b is None or b.returncode != 0:
            fail(name, "build -O2: rc=%s %s" % (None if b is None else b.returncode,
                                                (b.stdout + b.stderr)[-200:] if b else "timeout"))
            continue
        nat = run([exe])
        if nat is None or nat.returncode != 0 or nat.stdout != want:
            fail(name, "ran %r/%r, want rc 0 and %r"
                 % (None if nat is None else nat.returncode, None if nat is None else nat.stdout, want))

    if failures:
        raise SystemExit("TREE DEPTH FAILED (%d)" % failures)
    print("TREE DEPTH ALL GREEN")


if __name__ == "__main__":
    main()
