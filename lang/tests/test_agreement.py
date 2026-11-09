"""Native conformance against pinned corpus outputs, with ASan checks."""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys

import _cxx
import _scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CORPUS = os.path.join(HERE, "corpus", "agreement.json")
TMP = _scratch.mkdtemp(prefix="brat-agreement-")

TS = re.compile(r'"ts": [\d.]+')

CC_ARGS = ["-O2", "-w", "-I", ENGINE, "-lm"]
if os.name != "nt":
    CC_ARGS.append("-lpthread")


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
    cc = os.environ.get("BRAT_CC") or shutil.which("cc") or shutil.which("gcc") or "cc"
    argv = [cc, cfile, "-o", exe] + list(CC_ARGS) + list(extra)
    return run(argv)


def run_one_case(case):
    name = case["name"]
    path = write_case(case)

    try:
        native_c = _cxx.emit_file(path)
        if _cxx.emit_file(path) != native_c:
            return False, False, "FAIL %-16s the native emitter is not deterministic" % name
    except Exception as e:
        return False, False, "FAIL %-16s emit failed: %s: %s" % (name, type(e).__name__, e)

    cfile = os.path.join(TMP, name + ".native.c")
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(native_c)
    exe = os.path.join(TMP, name + ".native.exe")
    b = build(cfile, exe)
    if b.returncode != 0:
        return False, False, ("FAIL %-16s cc refused the emitted C: %s"
                              % (name, (b.stdout + b.stderr)[-400:]))

    native_run = run([exe])
    got = (native_run.returncode, norm(native_run.stdout))
    if case["rc"] is not None and got != (case["rc"], case["out"]):
        return True, False, ("FAIL %-16s want %r, got %r"
                             % (name, (case["rc"], case["out"]), got))

    if case["asan"]:
        asan_exe = os.path.join(TMP, name + ".asan")
        ab = build(cfile, asan_exe, extra=["-fsanitize=address", "-g", "-O1"])
        if ab.returncode != 0:
            return True, False, ("FAIL %-16s asan build: %s"
                                 % (name, (ab.stdout + ab.stderr)[-400:]))
        lsan = "0" if sys.platform == "darwin" else "1"
        r = run([asan_exe], env=dict(
            os.environ, ASAN_OPTIONS=f"detect_leaks={lsan}:exitcode=0"))
        report = r.stdout + r.stderr
        if any(w in report for w in ("AddressSanitizer", "LeakSanitizer",
                                     "heap-use-after-free", "SEGV")):
            return True, False, "FAIL %-16s asan report: %s" % (name, report[-600:])

    return True, True, None


def default_jobs():
    env_j = os.environ.get("BRAT_AGREEMENT_JOBS")
    if env_j:
        return max(1, int(env_j))
    if "BRAT_NESTED" in os.environ:
        return 2
    return max(1, min(4, os.cpu_count() or 1))


def main(jobs=None):
    _cxx.build()
    with open(CORPUS, encoding="utf-8") as f:
        cases = json.load(f)["cases"]
    pinned = [case for case in cases if case["rc"] is not None and case["out"] is not None]
    unpinned = len(cases) - len(pinned)
    cases = pinned
    if not cases:
        print("NATIVE CONFORMANCE: no independently pinned cases")
        return 1

    workers = jobs if jobs and jobs > 0 else default_jobs()
    bad = 0
    ran = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for did_run, ok, msg in pool.map(run_one_case, cases):
            if did_run:
                ran += 1
            if not ok:
                bad += 1
                if msg:
                    print(msg)

    if bad:
        print("AGREEMENT: %d case(s) failed of %d" % (bad, len(cases)))
        return 1
    print("NATIVE CONFORMANCE ALL GREEN (%d pinned programs; %d unpinned cases not run)"
          % (ran, unpinned))
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-j", "--jobs", type=int, default=0,
                        help="worker threads for C compilation/execution")
    args = parser.parse_args()
    sys.exit(main(jobs=args.jobs))
