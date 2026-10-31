"""Deep values: the recursive walkers stop with an error, not with a signal.

`print`, `str()` and comparison walk the object graph recursively. A program
that builds a deep value cheaply --

    let mut v = []
    for i in 0..200000 { v = [v] }

-- used to make the C backend walk the machine stack once per level and die with
SIGSEGV (observed at ~100 000 levels, exit 139, no output). The interpreter
refused the same program with "recursion too deep" because it has a frame budget
of 2000, so the two backends disagreed twice over: one gave a clean error, the
other a signal.

`crt.h` now carries the same budget (`BRAT_MAX_VALUE_DEPTH`) in `vsprint_q`,
`vsprint_len_q`, `vprint_f` and `vcmp`: past it the error channel gets
"recursion too deep" and the walker stops. This test pins three things:

  * a deep value prints correctly while it is inside the budget;
  * past the budget both backends exit 1 with no signal, and stdout matches;
  * the same holds for `str()`, comparison and the log path.
"""
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")
BRAT = os.path.join(ENGINE, "brat.py")
TMP = _scratch.mkdtemp(prefix="brat-depth-")


def depth_program(n, tail):
    return ("let mut v = []\n"
            "for i in 0..%d {\n"
            "  v = [v]\n"
            "}\n"
            "print(\"built\")\n"
            "%s\n"
            "print(\"done\")\n" % (n, tail))


CASES = {
    # inside the budget: both backends print the same bytes
    "print_within": (depth_program(900, "print(v)"), 0,
                     # `v = [v]` 900 times around the initial `[]` is 901 levels
                     "built\n" + "[" * 901 + "]" * 901 + "\ndone\n"),
    "str_within": (depth_program(900, "print(len(str(v)))"), 0, "built\n1802\ndone\n"),
    # two separate values, so the comparison actually walks the graph
    "compare_within": (depth_program(900, "let mut w = []\nfor i in 0..900 {\n  w = [w]\n}\n"
                                          "print(v == w)"), 0, "built\nTrue\ndone\n"),
    # past the budget: a clean error, never a signal
    "print_over": (depth_program(5000, "print(v)"), 1, "built\n"),
    "str_over": (depth_program(5000, "print(len(str(v)))"), 1, "built\n"),
    "compare_over": (depth_program(5000, "let mut w = []\nfor i in 0..5000 {\n  w = [w]\n}\n"
                                        "print(v == w)"), 1, "built\n"),
    "log_over": (depth_program(5000, 'log.info("x {len(str(v))}")'), 1, "built\n"),
    # the shape that produced the segfault
    "huge": (depth_program(120000, "print(len(str(v)))"), 1, "built\n"),
}


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600)


def main():
    bad = 0
    for name in sorted(CASES):
        src, want_rc, want_out = CASES[name]
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)

        exe = os.path.join(TMP, name + ".exe")
        b = run([sys.executable, CLI, "build", path, "-o", exe])
        if b.returncode != 0:
            print("FAIL %s: build: %s" % (name, (b.stdout + b.stderr)[-400:]))
            bad += 1
            continue
        nat = run([exe])
        interp = run([sys.executable, BRAT, path])

        if nat.returncode < 0 or nat.returncode >= 128:
            print("FAIL %s: native died by signal (rc=%d)" % (name, nat.returncode))
            bad += 1
            continue
        if nat.returncode != want_rc:
            print("FAIL %s: native rc=%d, want %d" % (name, nat.returncode, want_rc))
            bad += 1
        if nat.stdout != want_out:
            print("FAIL %s: native stdout %r, want %r" % (name, nat.stdout, want_out))
            bad += 1
        if (interp.returncode, interp.stdout) != (want_rc, want_out):
            print("FAIL %s: interpreter %r/%r, want %r/%r"
                  % (name, interp.returncode, interp.stdout, want_rc, want_out))
            bad += 1

    if bad:
        print("DEPTH GUARD: %d case(s) failed" % bad)
        return 1
    print("DEPTH GUARD OK (%d cases, no signal, both backends agree)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
