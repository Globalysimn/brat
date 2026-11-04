"""`brat run` says so when it runs the interpreter because there is no usable native compiler.

`cli.py run` runs the compiled program when `cxx/bin/brat-cxx` exists, can be executed and is not
older than any file in `cxx/src`; otherwise it runs the interpreter. It used to do that without a
word, so after a `git pull` that touched the sources a program ran an order of magnitude slower, and
where the two backends differ it could print another answer, with nothing to say why.

A native build that fails is not a reason to fall back: the exit status of `brat-cxx run` is passed
on. Each tree below is a copy of the engine with a stand-in for the native compiler, so no real
compiler is built or run except in the last case.
"""
import _cxx
import _scratch

import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
REAL_CLI = os.path.join(ENGINE, "cli.py")
_cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-backend-notice-")
NOTICE = "so this runs in the interpreter"

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def make_tree(name, stub=None, stale=False):
    """A copy of the engine whose native compiler is the shell script `stub`, or absent."""
    root = os.path.join(TMP, name)
    shutil.copytree(ENGINE, os.path.join(root, "engine"), ignore=shutil.ignore_patterns("__pycache__"))
    src = os.path.join(root, "cxx", "src")
    os.makedirs(src)
    unit = os.path.join(src, "unit.cpp")
    open(unit, "w").close()
    if stub is not None:
        os.makedirs(os.path.join(root, "cxx", "bin"))
        binary = os.path.join(root, "cxx", "bin", "brat-cxx")
        with open(binary, "w") as f:
            f.write("#!/bin/sh\n" + stub + "\n")
        os.chmod(binary, 0o755)
        now = time.time()
        older, newer = now - 100, now
        os.utime(unit, (newer, newer) if stale else (older, older))
        os.utime(binary, (older, older) if stale else (newer, newer))
    return os.path.join(root, "engine", "cli.py")


def run(cli, *flags, **env):
    done = subprocess.run([sys.executable, cli, "run"] + list(flags) + [HELLO], capture_output=True,
                          text=True, timeout=120, env=dict(os.environ, **env))
    return done


HELLO = os.path.join(TMP, "hello.brat")
with open(HELLO, "w") as f:
    f.write("print(40 + 2)\n")

for label, cli in (("no binary", make_tree("missing")),
                   ("stale binary", make_tree("stale", stub="exit 99", stale=True))):
    ran = run(cli)
    check(ran.returncode == 0 and ran.stdout == "42\n", "%s: exit %d, %r" % (label, ran.returncode, ran.stdout))
    check(NOTICE in ran.stderr, "%s: no notice on stderr: %r" % (label, ran.stderr))

    chosen = run(cli, "--interp")
    check(chosen.returncode == 0 and chosen.stdout == "42\n" and chosen.stderr == "",
          "%s with --interp: %r %r" % (label, chosen.stdout, chosen.stderr))
    for variable in ("BRAT_INTERP", "BRAT_NO_NATIVE_CLI"):
        chosen = run(cli, **{variable: "1"})
        check(chosen.returncode == 0 and chosen.stdout == "42\n" and chosen.stderr == "",
              "%s with %s: %r %r" % (label, variable, chosen.stdout, chosen.stderr))

failing = run(make_tree("failing", stub="echo native-ran; exit 3"))
check(failing.returncode == 3 and failing.stdout == "native-ran\n" and NOTICE not in failing.stderr,
      "a failing native run fell back: exit %d, %r, %r" % (failing.returncode, failing.stdout, failing.stderr))

real = run(REAL_CLI)
check(real.returncode == 0 and real.stdout == "42\n" and real.stderr == "",
      "the real tree: exit %d, %r, %r" % (real.returncode, real.stdout, real.stderr))

print("RUN BACKEND NOTICE ALL GREEN (%d asserts)" % n)
