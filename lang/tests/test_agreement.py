"""The same program behaves the same on every backend: one property, one file.

Two executions of one program must agree on the exit code and on stdout:

* `brat.py FILE` -- the interpreter, which walks the tree;
* the binary built from the C written by the native emitter.

Runtime error *text* differs by contract ("RuntimeError: ... at line N" against
"unhandled: ('err', ...)"), so only rc and stdout are compared here.

The corpus is data (`corpus/agreement.json`), not code: a case either pins an
exit code and output or only requires the two backends to agree.

A `"ts": <float>` field is normalised before comparing: a log line carries a
wall clock and two runs never agree on it. A case with `asan: true` is rebuilt
with AddressSanitizer, because its subject is a value read after its owner was
released.
"""
import _cxx
import _scratch
import importlib.util
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
BRAT = os.path.join(ENGINE, "brat.py")
CORPUS = os.path.join(HERE, "corpus", "agreement.json")
TMP = _scratch.mkdtemp(prefix="brat-agreement-")

TS = re.compile(r'"ts": [\d.]+')


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TC = _load("agreement_toolchain", os.path.join(ENGINE, "toolchain.py"))
CC_ARGS = ["-O2", "-w", "-I", ENGINE, "-lm", "-lpthread"]


def norm(text):
    return TS.sub('"ts": T', text.replace("\r\n", "\n"))


def run(cmd, **kw):
    # A backend that writes invalid UTF-8 must show up as a disagreement, not
    # as a UnicodeDecodeError that takes the whole file down.
    return subprocess.run(cmd, capture_output=True, text=True,
                          errors="backslashreplace", timeout=600, **kw)


def write_case(case):
    src = case["src"]
    if case["wrap"]:
        src = ("fn main() -> i64 {\n  " + src.replace("\n", "\n  ") +
               "\n  return 0\n}\n")
    path = os.path.join(TMP, case["name"] + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def build(cfile, exe, extra=()):
    argv = TC.resolve_cc().argv(cfile, exe) + list(CC_ARGS) + list(extra)
    return TC.cc_cached(argv, cfile, exe, timeout=300)


def main():
    _cxx.build()
    with open(CORPUS, encoding="utf-8") as f:
        cases = json.load(f)["cases"]
    bad = 0
    ran = 0

    for case in cases:
        name = case["name"]
        path = write_case(case)

        interp = run([sys.executable, BRAT, path])
        got = (interp.returncode, norm(interp.stdout))

        try:
            native_c = _cxx.emit_file(path)
            if _cxx.emit_file(path) != native_c:
                print("FAIL %-16s the native emitter is not deterministic" % name)
                bad += 1
                continue
        except Exception as e:
            print("FAIL %-16s emit failed: %s: %s" % (name, type(e).__name__, e))
            bad += 1
            continue

        cfile = os.path.join(TMP, name + ".native.c")
        with open(cfile, "w", encoding="utf-8") as f:
            f.write(native_c)
        exe = os.path.join(TMP, name + ".native.exe")
        b = build(cfile, exe)
        if b.returncode != 0:
            print("FAIL %-16s cc refused the emitted C: %s"
                  % (name, (b.stdout + b.stderr)[-400:]))
            bad += 1
            continue
        native_run = run([exe])
        backends = {
            "interp": got,
            "native-C": (native_run.returncode, norm(native_run.stdout)),
        }

        ran += 1
        if len(set(backends.values())) != 1:
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
            ab = build(cfile, asan_exe,
                       extra=["-fsanitize=address", "-g", "-O1"])
            if ab.returncode != 0:
                bad += 1
                print("FAIL %-16s asan build: %s"
                      % (name, (ab.stdout + ab.stderr)[-400:]))
                continue
            r = run([asan_exe], env=dict(
                os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=0"))
            report = r.stdout + r.stderr
            if any(w in report for w in ("AddressSanitizer", "LeakSanitizer",
                                         "heap-use-after-free", "SEGV")):
                bad += 1
                print("FAIL %-16s asan report: %s" % (name, report[-600:]))
                continue

    if bad:
        print("AGREEMENT: %d case(s) failed of %d" % (bad, len(cases)))
        return 1
    print("AGREEMENT ALL GREEN (%d programs, 2 backends each)" % ran)
    return 0


if __name__ == "__main__":
    sys.exit(main())
