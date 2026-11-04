"""One program, two executions, one line each — the tool behind the audit notes.

    python3 lang/tests/_diffprobe.py FILE.brat [FILE...]

Prints the exit code and an output hash for the interpreter and for the C
built from `brat-cxx cc`, and says `agree` when both are the same
`(rc, stdout)`. `"ts": <float>` is normalised, because a log line carries a
wall clock. Exits 1 if any file diverges, so it can drive a sweep:

    for f in lang/examples/*.brat; do python3 lang/tests/_diffprobe.py $f; done

It is a debugging tool, not a test: it needs `cc` and
`lang/cxx/bin/brat-cxx`. `test_agreement.py` is the property; this is the
microscope. Runtime error *text* is not compared here either — stderr differs
by contract (`RuntimeError: ... at line N` against `unhandled: ('err', ...)`),
so a message that a program prints from `catch e` is what shows up, and that
is the half of the contract that has to be identical.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
BRAT = os.path.join(ENGINE, "brat.py")
CXX = os.path.abspath(os.path.join(HERE, "..", "cxx", "bin", "brat-cxx"))

TS = re.compile(rb'"ts": [0-9.e+-]+')


def norm(b):
    return TS.sub(b'"ts": <ts>', b)


def run(cmd, cwd=None, timeout=180):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def build_and_run(cfile, exe):
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        return ("NO-CC", b"", b"")
    rc, o, e = run([cc, "-std=gnu11", "-O2", "-w", "-I", ENGINE, "-o", exe, cfile,
                    "-lm", "-lpthread"])
    if rc != 0:
        return ("C-REFUSED", o, e)
    return run([exe])


def probe(path, workdir):
    out = {}
    try:
        rc, o, e = run([sys.executable, BRAT, path])
        out["interp"] = (rc, norm(o), e)
    except subprocess.TimeoutExpired:
        out["interp"] = ("TIMEOUT", b"", b"")

    # `--no-check`: this probe compares what the executions print, and a
    # program the checker rejects is exactly an interesting case (the runtime
    # rule underneath the static one). The gate itself is tested by
    # `test_cxx_emit.py`, not here.
    try:
        rc, o, e = run([CXX, "cc", "--no-check", path])
    except subprocess.TimeoutExpired:
        out["native"] = ("TIMEOUT", b"", b"")
        return out
    if rc != 0:
        out["native"] = ("EMIT-REFUSED", o, e)
        return out

    cfile = os.path.join(workdir, "native.c")
    with open(cfile, "wb") as f:
        f.write(o)
    r = build_and_run(cfile, os.path.join(workdir, "native.exe"))
    out["native"] = (r[0], norm(r[1]) if isinstance(r[1], bytes) else r[1], r[2])
    return out


def main(argv):
    if not os.path.isfile(CXX):
        sys.stderr.write("brat-cxx not built at %s\n" % CXX)
        return 2

    bad = 0
    for path in argv:
        path = os.path.abspath(path)
        workdir = tempfile.mkdtemp(prefix="brat-diff-")
        res = probe(path, workdir)
        print("=== %s" % os.path.basename(path))
        first = res["interp"][:2]
        ok = True
        for name, (rc, o, _e) in res.items():
            h = hashlib.sha256(o).hexdigest()[:12] if isinstance(o, bytes) else "-"
            n = len(o) if isinstance(o, bytes) else 0
            print("  %-7s rc=%-12s out#%s bytes=%d" % (name, rc, h, n))
            if name != "interp" and (rc, o) != first:
                ok = False
        print("  %s" % ("agree" if ok else "!!! DIVERGENCE (%s)" % workdir))
        if not ok:
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
