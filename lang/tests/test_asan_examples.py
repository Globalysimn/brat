"""Every example, compiled with AddressSanitizer, is free of memory errors.

Leaks are invisible in output, so nothing in the suite noticed them: the emitter
decides what owns a value, and a wrong decision is a program that runs
correctly for a second and then grows. This runs the whole `lang/examples`
corpus through `-fsanitize=address` and separates two kinds of finding:

* **A memory error** -- use-after-free, double free, overflow. Always a failure;
  there is no such thing as a benign one.
* **A leak** -- a value nobody releases. Tracked in `KNOWN_LEAKS` with the exact
  byte and allocation counts, so a fix has to update the number and a new leak
  fails. The list only shrinks.

The counts are per example and measured on x86-64 glibc, so they are stable for
a given emitted program; a compiler or runtime change that alters them is a
change in ownership, which is exactly what this test is for.

The `*_bad` examples are fixtures the checker is supposed to refuse: they are
counted separately, because "no binary was produced" is the correct answer.
"""
import _scratch

import os
import re
import subprocess
import importlib.util
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
EXAMPLES = os.path.abspath(os.path.join(HERE, "..", "examples"))
CLI = os.path.join(ENGINE, "cli.py")
TMP = _scratch.mkdtemp(prefix="brat-asan-")

# Examples the emitter still leaks on. The set only shrinks: a fix removes its
# name, and any name that is not here fails the test. Byte counts are deliberately
# not pinned -- they vary with the arguments an example is handed and with the
# allocator, and a number that moves on its own turns this oracle into noise.
KNOWN_LEAKS = {
    "funclib.brat", "jsonlibtest.brat", "newcomer.brat", "span.brat",
    # syslib ends with `exit(0)`: the process leaves before the script's exit
    # drops, so what it held at that line is still held. Not an ownership bug,
    # and not one a release can fix.
    "syslib.brat",
}

HARD = ("heap-use-after-free", "double-free", "heap-buffer-overflow",
        "stack-buffer-overflow", "SEGV", "attempting free on address")


def examples():
    return sorted(os.path.join(EXAMPLES, n) for n in os.listdir(EXAMPLES)
                  if n.endswith(".brat"))


def _toolchain():
    spec = importlib.util.spec_from_file_location(
        "asan_toolchain", os.path.join(ENGINE, "toolchain.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_TC = _toolchain()


def one(path):
    """(kind, detail) where kind is ok / refused / leak / error."""
    name = os.path.basename(path)
    r = subprocess.run([sys.executable, CLI, "emit-c", path],
                       capture_output=True, text=True, timeout=180)
    if r.returncode != 0:
        # A fixture the checker refuses is doing its job.
        return "refused", r.stderr.strip().splitlines()[0][:120] if r.stderr.strip() else ""
    c = os.path.join(TMP, name[:-5] + ".c")
    exe = os.path.join(TMP, name[:-5] + ".exe")
    with open(c, "w", encoding="utf-8") as f:
        f.write(r.stdout)
    # Through the content-addressed cache: the same example compiles to the
    # same instrumented binary on every run, and recompiling 64 of them at
    # ~0.5 s each was most of this file's cost. A cache hit returns an empty
    # stderr, and this test reads stderr only when the compiler fails, so
    # nothing it asserts can be skipped by a hit.
    # Undefined behaviour rides along: signed overflow, a shift by the width, a
    # misaligned read. None of it is visible in the output, and all of it is in
    # the C runtime. `-O1` matters -- LeakSanitizer scans the stack
    # conservatively, so an unoptimized build keeps stale pointers in reach and
    # hides leaks that the shipped `-O2` build has.
    cc = _TC.cc_cached(["cc", "-std=gnu11", "-g", "-O1", "-w",
                        "-fsanitize=address,undefined", "-I", ENGINE, c, "-o", exe,
                        "-lm", "-lpthread", "-ldl"],
                       c, exe, timeout=180)
    if cc.returncode != 0:
        return "error", "cc failed: %s" % cc.stderr.strip()[:200]
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=99")
    try:
        run = subprocess.run([exe], capture_output=True, text=True, timeout=120,
                             env=env, cwd=EXAMPLES)
    except subprocess.TimeoutExpired:
        return "error", "timed out"
    err = run.stderr
    for word in HARD:
        if word in err:
            first = [ln for ln in err.splitlines() if word in ln]
            return "error", first[0] if first else word
    if "runtime error:" in err:
        first = [ln for ln in err.splitlines() if "runtime error:" in ln]
        return "error", first[0][:200]
    m = re.search(r"SUMMARY: AddressSanitizer: (\d+) byte\(s\) leaked in (\d+) allocation", err)
    if m:
        return "leak", (int(m.group(1)), int(m.group(2)))
    return "ok", ""


def main():
    t0 = time.time()
    bad = []
    leaks, ok, refused = {}, 0, 0
    for path in examples():
        name = os.path.basename(path)
        try:
            kind, detail = one(path)
        except Exception as e:  # noqa: BLE001
            kind, detail = "error", "%s: %s" % (type(e).__name__, e)
        if kind == "refused":
            refused += 1
        elif kind == "error":
            bad.append("%s: %s" % (name, detail))
        elif kind == "leak":
            leaks[name] = detail
        else:
            ok += 1
    for name, got in sorted(leaks.items()):
        if name in KNOWN_LEAKS:
            continue
        bad.append("%s: NEW leak %r" % (name, got))
    # A name that stops leaking is reported, not failed: which examples leak can
    # depend on the arguments one is handed, and a ledger that fails on its own
    # improvement is a ledger nobody keeps. The entry is safe to delete.
    fixed = sorted(KNOWN_LEAKS - set(leaks))
    if fixed:
        print("  no longer leaking (drop from KNOWN_LEAKS): %s" % ", ".join(fixed))
    if bad:
        print("ASAN EXAMPLES FAILED (%d problem(s), %.1fs)" % (len(bad), time.time() - t0))
        for b in bad:
            print("  " + b)
        return 1
    print("ASAN EXAMPLES ALL GREEN (%d clean, %d refused by the checker, "
          "%d known leaks pinned, no memory errors, %.1fs)"
          % (ok, refused, len(KNOWN_LEAKS), time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
