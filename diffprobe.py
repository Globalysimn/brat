#!/usr/bin/env python3
"""Three-backend differential probe: interpreter vs Python emit-c vs brat-cxx cc.

Usage: python3 diffprobe.py FILE.brat [--keep]
Prints one line per backend: rc and a hash of stdout; exits 0 if all agree.
"""
import hashlib
import re
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
CLI = [sys.executable, os.path.join(ROOT, "lang/engine/cli.py")]
CXX = os.path.join(ROOT, "lang/cxx/bin/brat-cxx")


def run(cmd, cwd=None, timeout=60, stdin=b""):
    p = subprocess.run(cmd, cwd=cwd, input=stdin, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


TS = re.compile(rb'"ts": [0-9.e+-]+')


def norm(b):
    return TS.sub(b'"ts": <ts>', b)


def probe(src_path, workdir):
    out = {}
    # 1. interpreter
    try:
        rc, o, e = run(CLI + ["run", "--interp", src_path], timeout=120)
        out["interp"] = (rc, o, e)
    except subprocess.TimeoutExpired:
        out["interp"] = ("TIMEOUT", b"", b"")
    # 2. python emit-c
    cfile = os.path.join(workdir, "py.c")
    exe = os.path.join(workdir, "py.bin")
    inc = os.path.join(ROOT, "lang/engine")
    try:
        rc, o, e = run(CLI + ["emit-c", src_path], timeout=120)
        open(cfile, "wb").write(o)
        if rc == 0:
            rc2, o2, e2 = run(["cc", "-std=c99", "-O2", "-I", inc, "-o", exe, cfile, "-lm"])
            if rc2 != 0:
                out["py-c"] = ("CCOMPILE", o2, e2)
            else:
                rc3, o3, e3 = run([exe], timeout=120)
                out["py-c"] = (rc3, o3, e3)
        else:
            out["py-c"] = ("EMIT", o, e)
    except subprocess.TimeoutExpired:
        out["py-c"] = ("TIMEOUT", b"", b"")
    # 3. cxx cc
    if os.path.exists(CXX):
        cfile2 = os.path.join(workdir, "cxx.c")
        exe2 = os.path.join(workdir, "cxx.bin")
        try:
            rc, o, e = run([CXX, "cc", src_path], timeout=120)
            open(cfile2, "wb").write(o)
            if rc == 0:
                rc2, o2, e2 = run(["cc", "-std=c99", "-O2", "-I", inc, "-o", exe2, cfile2, "-lm"])
                if rc2 != 0:
                    out["cxx"] = ("CCOMPILE", o2, e2)
                else:
                    rc3, o3, e3 = run([exe2], timeout=120)
                    out["cxx"] = (rc3, o3, e3)
            else:
                out["cxx"] = ("EMIT", o, e)
        except subprocess.TimeoutExpired:
            out["cxx"] = ("TIMEOUT", b"", b"")
    return out


def main():
    path = os.path.abspath(sys.argv[1])
    keep = "--keep" in sys.argv
    wd = tempfile.mkdtemp(prefix="brat-diff-")
    res = probe(path, wd)
    print(f"=== {os.path.basename(path)}")
    ok = True
    first = None
    for k, v in res.items():
        rc, o, e = v
        o = norm(o) if isinstance(o, bytes) else o
        h = hashlib.sha256(o).hexdigest()[:12] if isinstance(o, bytes) else "-"
        print(f"  {k:7s} rc={rc!s:10s} out#{h} bytes={len(o) if isinstance(o, bytes) else 0}")
        if k == "interp":
            first = (rc, o)
        else:
            if first is not None and (rc, o) != first:
                ok = False
    if ok:
        print("  agree")
        if keep:
            print(f"  workdir: {wd}")
        return 0
    # A program the checker refuses is not a backend divergence: `--interp` is the
    # reference implementation and is deliberately not gated (LEDGER, "run
    # --interp executes a program the type pass rejects"). Reporting those as
    # DIVERGENCE put six false alarms in every run of `run_diff_all.sh`, which is
    # how a real divergence gets ignored. Both emitters refusing the same way is
    # the signal that this is a refusal, not a bug.
    # A program with a `spawn` prints in the order its threads finish, so the
    # bytes of two runs may differ while the content agrees. That is the program,
    # not the backends: compare the same lines in any order, and say so.
    if first is not None:
        rc0, o0 = first
        others = [(k, v) for k, v in res.items() if k != "interp"]
        if all(v[0] == rc0 for _k, v in others) and all(
                sorted(norm(v[1]).splitlines()) == sorted(o0.splitlines())
                for _k, v in others):
            print("  agree up to the order of concurrent output")
            print(f"  workdir: {wd}")
            return 0

    emitters = [res.get("py-c"), res.get("cxx")]
    refused = all(v is not None and v[0] == "EMIT" for v in emitters)
    if refused:
        def text_of(v):
            # The diagnostic can land on either stream depending on the emitter.
            return (v[1] + v[2]).decode("utf-8", "replace") if v else ""

        py_text = text_of(emitters[0])
        cx_text = text_of(emitters[1])
        # A refusal is a checker diagnostic (`file:line: error: ...`), a module
        # rule (`function ... is private in module ...`) or the emit gate that
        # names what it is skipping. Anything else is a backend bug, and stays a
        # DIVERGENCE.
        marks = (": error: ", "is private in module", "refusing to emit", "load error")
        if any(m in py_text for m in marks) and any(m in cx_text for m in marks):
            print("  refused by the checker (both emitters; `--interp` is not gated)")
            print(f"  workdir: {wd}")
            return 0
    print("  !!! DIVERGENCE")
    print(f"  workdir: {wd}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
