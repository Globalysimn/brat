"""The emitted C compiles under `cc -std=c99`.

`brat build` passes `-std=gnu11`, so no other test compiles the output with a
strict ISO flag. Two things are pinned:

- the runtime reads the clock with `clock_gettime`, not `timespec_get`, which
  `-std=c99` hides;
- `INT64_MIN` is not written as `-9223372036854775808`. C reads that as an
  unsigned constant under a unary minus, and gcc warns that the constant is so
  large that it is unsigned.

Each program is compiled, run, and compared with `run -O0`.
"""
import _cxx
import _scratch
import _skip
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
EXAMPLES = os.path.abspath(os.path.join(HERE, "..", "examples"))
TMP = _scratch.mkdtemp(prefix="brat-c99-")

PROGRAMS = {
    "hello": None,  # read from lang/examples
    "precedence": None,
    "fastvec": None,
    "int_min": ('print(-9223372036854775808)\n'
                'print(9223372036854775807 + 1)\n'
                'print(-9223372036854775807 / -1)\n'
                'print(str(-9223372036854775808))\n'),
    "int_min_fast": ('fn f(a: i64) -> i64 {\n  return a\n}\n'
                     'print(f(-9223372036854775808))\n'
                     'print(-9223372036854775808 + 0)\n'),
}


def cc():
    exe = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not exe:
        _skip.skip("no C compiler, the emitted C cannot be compiled")
    return exe


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)


TS = re.compile(r'"ts": [\d.]+')


def norm(text):
    """A log line carries a wall clock; two runs never agree on it."""
    return TS.sub('"ts": T', text.replace("\r\n", "\n"))


def main():
    compiler = cc()
    bad = 0
    for name in sorted(PROGRAMS):
        src = PROGRAMS[name]
        if src is None:
            path = os.path.join(EXAMPLES, name + ".brat")
        else:
            path = os.path.join(TMP, name + ".brat")
            with open(path, "w", encoding="utf-8") as f:
                f.write(src)

        cfile = os.path.join(TMP, name + ".c")
        e = run(_cxx.cli_args("emit-c", path), env=dict(os.environ))
        if e.returncode != 0:
            print("FAIL %s: emit-c: %s" % (name, (e.stdout + e.stderr)[-400:]))
            bad += 1
            continue
        with open(cfile, "w", encoding="utf-8") as f:
            f.write(e.stdout)

        exe = os.path.join(TMP, name + ".exe")
        b = run([compiler, "-std=c99", "-O2", "-Wall", "-I", ENGINE, cfile, "-o", exe,
                 "-lm", "-lpthread"])
        if b.returncode != 0:
            print("FAIL %s: cc -std=c99 rejected the emitted C:\n%s" % (name, b.stderr[-800:]))
            bad += 1
            continue
        if "integer constant is so large" in b.stderr:
            print("FAIL %s: INT64_MIN emitted as an unsigned literal:\n%s" % (name, b.stderr[-400:]))
            bad += 1
            continue

        got = run([exe])
        want = run(_cxx.run_args(path))
        if (got.returncode, norm(got.stdout)) != (want.returncode, norm(want.stdout)):
            print("FAIL %s: binary %r/%r, -O0 %r/%r"
                  % (name, got.returncode, got.stdout, want.returncode, want.stdout))
            bad += 1

    if bad:
        print("C99 EMIT: %d program(s) failed" % bad)
        return 1
    print("C99 EMIT OK (%d programs compile as C99 and print what the -O0 build prints)"
          % len(PROGRAMS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
