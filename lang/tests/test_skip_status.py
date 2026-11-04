"""A test that cannot run says so: exit 77 and the word SKIPPED, never 0.

A green `ok` that means "there was no compiler" is a lie the suite tells itself:
`test_cxx_emit.py` proves the native emitter handles the shipped corpus
deterministically, and on a machine without `c++` it proved nothing while
closing its block in the
summary. The contract this file pins:

- a test that skips exits 77 and prints a `SKIPPED:` line with the reason;
- `run_all.py` prints `SKIP` for it, counts it apart from `ok` and from
  `FAILED`, and the run still exits 0 when nothing failed;
- the five `test_cxx_*` files still go green on a machine that has a compiler,
  so the skip is not unconditional.

The no-compiler case is produced honestly rather than by faking a flag: the
whole `lang/` tree is copied to a scratch directory with no prebuilt binary, and
the child gets a PATH that holds only a `python3` symlink. `which("c++")` then
answers what it would answer on such a machine.
"""

import _scratch

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LANG = os.path.abspath(os.path.join(HERE, ".."))
TMP = _scratch.mkdtemp(prefix="brat-skip-")
SKIP_RC = 77

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)
        print("FAIL %s" % what)


def run(cmd, env=None, cwd=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=900,
                          env=env, cwd=cwd)


def tree(tag):
    """A copy of `lang/` with no built binary, on a path of its own."""
    dst = os.path.join(TMP, tag)
    shutil.copytree(LANG, os.path.join(dst, "lang"),
                    ignore=shutil.ignore_patterns("bin", "__pycache__"))
    return dst


def path_without_compilers(tag):
    """PATH holding `python3` and `sh` and nothing else."""
    d = os.path.join(TMP, tag)
    os.makedirs(d, exist_ok=True)
    for name in ("python3", "sh"):
        src = shutil.which(name)
        if src:
            os.symlink(src, os.path.join(d, name))
    return d


def env_with(path, tag):
    return dict(os.environ, PATH=path,
                BRAT_SCRATCH=os.path.join(TMP, tag + "-scratch"))


# 1. run_all.py counts a skip as a skip. Two stub files, one green and one
#    skipping, so the summary has to separate them.
stub = os.path.join(TMP, "runner")
os.makedirs(stub, exist_ok=True)
shutil.copy(os.path.join(HERE, "run_all.py"), os.path.join(stub, "run_all.py"))
with open(os.path.join(stub, "_stub_ok.py"), "w", encoding="utf-8") as f:
    f.write("print('stub ok')\n")
with open(os.path.join(stub, "_stub_skip.py"), "w", encoding="utf-8") as f:
    f.write("import sys\nprint('SKIPPED: no compiler')\nsys.exit(%d)\n" % SKIP_RC)
# run_all globs test_*.py, so the stubs are named that way on disk
for old, new in (("_stub_ok.py", "test_stub_ok.py"), ("_stub_skip.py", "test_stub_skip.py")):
    os.replace(os.path.join(stub, old), os.path.join(stub, new))
r = run([sys.executable, os.path.join(stub, "run_all.py"), "-j", "1"], env=env_with(
    os.environ["PATH"], "runner"))
out = r.stdout + r.stderr
check(r.returncode == 0, "run_all exits 0 when the only non-pass is a skip (got %d)" % r.returncode)
check("1 skipped" in out, "run_all summary counts the skip apart: %r" % out.strip().splitlines()[-1:])
check("SKIP" in out, "run_all prints SKIP in the per-file column")

# 2. Without a compiler the C++ frontend tests skip, and say why.
bare = tree("tree")
env = env_with(path_without_compilers("tree-bin"), "tree")
for name in ("test_cxx_frontend.py", "test_cxx_checker.py", "test_cxx_borrow.py",
             "test_cxx_modules.py", "test_cxx_emit.py", "test_cxx_build.py"):
    p = run([sys.executable, os.path.join(bare, "lang", "tests", name)], env=env)
    out = (p.stdout + p.stderr).strip()
    check(p.returncode == SKIP_RC,
          "%s exits %d without a compiler (got %d: %r)" % (name, SKIP_RC, p.returncode, out[-300:]))
    check("SKIPPED" in out, "%s says SKIPPED" % name)
    check("no c++" in out, "%s names the reason" % name)

# 3. With a compiler the same files are still green, so the skip is not a way
#    out: the emitter test checks corpus refusals and deterministic C.
#    The binary the suite builds for the real tree is copied in, so the control
#    costs one corpus walk instead of a 36 s compile of the front end.
if shutil.which("c++") or shutil.which("g++"):
    dst = os.path.join(bare, "lang", "cxx", "bin")
    os.makedirs(dst, exist_ok=True)
    shutil.copy2(os.path.join(LANG, "cxx", "bin", "brat-cxx"), os.path.join(dst, "brat-cxx"))
    p = run([sys.executable, os.path.join(bare, "lang", "tests", "test_cxx_emit.py")],
            env=env_with(os.environ["PATH"], "tree"))
    out = (p.stdout + p.stderr).strip()
    check(p.returncode == 0, "test_cxx_emit goes green with a compiler (got %d: %r)"
          % (p.returncode, out[-400:]))
    check("deterministic" in out,
          "test_cxx_emit reports deterministic corpus emission: %r" % out[-200:])
else:
    print("SKIP: no compiler on this machine, the green direction is untested")
    print("SMOKE %d check(s) failed" % len(fails))
    sys.exit(SKIP_RC if not fails else 1)

if fails:
    print("SKIP STATUS FAILED (%d)" % len(fails))
    sys.exit(1)
print("SKIP STATUS ALL GREEN (skip=77, counted, and still green with a compiler)")
