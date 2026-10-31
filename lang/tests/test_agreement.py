"""The same program behaves the same on every backend: one property, one file.

Three executions of one program must agree on the exit code and on stdout:

* `brat.py FILE` -- the interpreter, which walks the tree;
* the binary built from `codegen_c.emit` -- the Python emitter;
* the binary built from `brat-cxx cc` -- the C++ emitter.

Runtime error *text* differs by contract ("RuntimeError: ... at line N" against
"unhandled: ('err', ...)"), so only rc and stdout are compared here. The two
emitters are required to write byte-identical C, which `test_cxx_emit.py` checks
directly -- so the second compile is a cache hit and the third backend costs one
exec, not one build.

The corpus is data (`corpus/agreement.json`), not code: 97 programs, 31 of them
with a pinned exit code and output, 66 with the agreement requirement only. This
file used to be `test_parity.py` plus `test_diff.py`, which checked the same
property with two harnesses and two half-corpora; merging them means every
program is now checked on three backends instead of two, and the corpus can grow
without touching the harness.

A `"ts": <float>` field is normalised before comparing: a log line carries a wall
clock and two runs never agree on it. Two cases carry `asan: true` and are
rebuilt with AddressSanitizer, because their subject is a value read after its
owner was released.
"""
import _scratch
import importlib.util
import json
import os
import re
import subprocess
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
BRAT = os.path.join(ENGINE, "brat.py")
CXX = os.path.abspath(os.path.join(HERE, "..", "cxx", "bin", "brat-cxx"))
CORPUS = os.path.join(HERE, "corpus", "agreement.json")
TMP = _scratch.mkdtemp(prefix="brat-agreement-")

TS = re.compile(r'"ts": [\d.]+')


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


CC = _load("agreement_codegen", os.path.join(ENGINE, "codegen_c.py"))
TC = _load("agreement_toolchain", os.path.join(ENGINE, "toolchain.py"))

CC_ARGS = ["-O2", "-w", "-I", ENGINE, "-lm", "-lpthread"]
version = tuple(int(part) for part in unicodedata.unidata_version.split("."))
case_version = 1600 if version >= (16, 0) else 1500
CXX_ENV = dict(os.environ, BRAT_CASE_MAP_VERSION=str(case_version))

# `brat-cxx cc --no-check` writes the same banner `brat emit-c --no-check` does;
# the comparison here is between two emitters, so the banner is normalised away
# once, where the C++ output is read.
UNVERIFIED = "/* BRAT: UNVERIFIED -- ownership not checked */\n"


def norm(text):
    return TS.sub('"ts": T', text.replace("\r\n", "\n"))


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)


def write_case(case):
    src = case["src"]
    if case["wrap"]:
        src = ("fn main() -> i64 {\n  " + src.replace("\n", "\n  ") +
               "\n  return 0\n}\n")
    path = os.path.join(TMP, case["name"] + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return src, path


def build(cfile, exe, extra=()):
    argv = TC.resolve_cc().argv(cfile, exe) + list(CC_ARGS) + list(extra)
    return TC.cc_cached(argv, cfile, exe, timeout=300)


def main():
    with open(CORPUS, encoding="utf-8") as f:
        cases = json.load(f)["cases"]
    have_cxx = os.path.isfile(CXX)
    bad = 0
    ran = 0

    for case in cases:
        name = case["name"]
        src, path = write_case(case)

        # The interpreter, which is the reference for the pinned expectations.
        interp = run([sys.executable, BRAT, path])
        got = (interp.returncode, norm(interp.stdout))

        # The Python emitter, compiled in process (no CLI, no second parse).
        try:
            py_c = CC.emit(src, TMP)
        except Exception as e:                      # a refused program is a bug here
            print("FAIL %-16s emit failed: %s: %s" % (name, type(e).__name__, e))
            bad += 1
            continue
        # Emitting twice in one process must give the same bytes as emitting
        # once: `parser.fresh` used to number temporaries with a counter that
        # lived for the whole process, so the second program of a batch got
        # different names than a separate run.
        if CC.emit(src, TMP) != py_c:
            print("FAIL %-16s the emitter is not deterministic in one process" % name)
            bad += 1
            continue
        py_cfile = os.path.join(TMP, name + ".py.c")
        with open(py_cfile, "w", encoding="utf-8") as f:
            f.write(py_c)
        py_exe = os.path.join(TMP, name + ".py.exe")
        b = build(py_cfile, py_exe)
        if b.returncode != 0:
            print("FAIL %-16s cc refused the python-emitted C: %s"
                  % (name, (b.stdout + b.stderr)[-400:]))
            bad += 1
            continue
        py_run = run([py_exe])
        backends = {"interp": got, "py-C": (py_run.returncode, norm(py_run.stdout))}

        # The C++ emitter. Its C is byte-identical by test_cxx_emit.py, so this
        # compile hits the cache; only the exec is new work.
        #
        # `--no-check`: several programs here are ones the *checker* rejects on
        # purpose (`neg_index`, `temp_names`, `spawn_in_closure`) because their
        # subject is what the runtime does underneath a rule. That makes this file
        # an old program in the checker's eyes, not a broken one: the gate in
        # `cc` is for the product path and `--no-check` is the door the emitter
        # tests go through. Without it the gate refuses five corpus programs and
        # this test cannot see the C at all.
        if have_cxx:
            e = run([CXX, "cc", "--no-check", path], env=CXX_ENV)
            if e.stdout.startswith(UNVERIFIED):
                e = subprocess.CompletedProcess(e.args, e.returncode,
                                                e.stdout[len(UNVERIFIED):], e.stderr)
            if e.returncode != 0:
                print("FAIL %-16s brat-cxx cc failed: %s" % (name, (e.stdout + e.stderr)[-400:]))
                bad += 1
                continue
            if e.stdout != py_c:
                # Not a behaviour difference: the emitters must agree exactly,
                # and when they do not this file cannot attribute the divergence.
                print("FAIL %-16s emitters disagree on the C (%d vs %d bytes)"
                      % (name, len(py_c), len(e.stdout)))
                bad += 1
                continue
            cxx_cfile = os.path.join(TMP, name + ".cxx.c")
            with open(cxx_cfile, "w", encoding="utf-8") as f:
                f.write(e.stdout)
            cxx_exe = os.path.join(TMP, name + ".cxx.exe")
            b = build(cxx_cfile, cxx_exe)
            if b.returncode != 0:
                print("FAIL %-16s cc refused the cxx-emitted C: %s"
                      % (name, (b.stdout + b.stderr)[-400:]))
                bad += 1
                continue
            r = run([cxx_exe])
            backends["cxx-C"] = (r.returncode, norm(r.stdout))

        ran += 1
        values = set(backends.values())
        if len(values) != 1:
            bad += 1
            print("FAIL %-16s backends disagree: %r" % (name, backends))
            continue
        if case["rc"] is not None and got != (case["rc"], case["out"]):
            bad += 1
            print("FAIL %-16s want %r, got %r"
                  % (name, (case["rc"], case["out"]), got))
            continue
        if case["asan"]:
            asan_exe = os.path.join(TMP, name + ".asan")
            ab = build(py_cfile, asan_exe, extra=["-fsanitize=address", "-g", "-O1"])
            if ab.returncode != 0:
                bad += 1
                print("FAIL %-16s asan build: %s" % (name, (ab.stdout + ab.stderr)[-400:]))
                continue
            r = run([asan_exe], env=dict(os.environ,
                                         ASAN_OPTIONS="detect_leaks=1:exitcode=0"))
            report = r.stdout + r.stderr
            if any(w in report for w in ("AddressSanitizer", "LeakSanitizer",
                                         "heap-use-after-free", "SEGV")):
                bad += 1
                print("FAIL %-16s asan report: %s" % (name, report[-600:]))
                continue

    if bad:
        print("AGREEMENT: %d case(s) failed of %d" % (bad, len(cases)))
        return 1
    print("AGREEMENT ALL GREEN (%d programs, %s backends each)"
          % (ran, "3" if have_cxx else "2"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
