"""`brat-cxx cc FILE` is `codegen_c.emit_file(FILE)`, byte for byte.

The emitter is the part of the port where "close enough" is not a thing: the C
it writes carries the ownership decisions (what is dropped, where, and how many
times), so a diff in one line is a program that behaves differently. The oracle
is therefore the reference emitter itself and the comparison is exact.

A program the reference refuses to emit — a private function called across a
module boundary — has to be refused here too, not emitted into bad C.
"""

import _cxx
import _scratch

import importlib.util
import os
import subprocess
import sys
import time
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
BIN = _cxx.BIN
TMP = _scratch.mkdtemp(prefix="brat-emit-")

sys.path.insert(0, ENGINE)
import brat.parser as P  # noqa: E402,F401  (the engine expects its sibling imports)

_CG = [None]


def oracle():
    """`codegen_c.py`, loaded by path so it cannot shadow a package."""
    if _CG[0] is None:
        spec = importlib.util.spec_from_file_location(
            "brat_oracle_emit", os.path.join(ENGINE, "codegen_c.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _CG[0] = mod
    return _CG[0]


def corpus():
    out = []
    for base, dirs, files in os.walk(ROOT):
        # The corpus is the programs the repository ships. A test that writes a
        # scratch .brat into the tree must not change the count for a sibling
        # test running in parallel, so generated directories are skipped.
        dirs[:] = [d for d in dirs if d not in ("vendor", "bin", ".git", "__pycache__",
                                                "scratch", "tmp", "out")]
        for f in files:
            if f.endswith(".brat") and not f.startswith((".", "_")):
                out.append(os.path.join(base, f))
    return sorted(out)


# The one program the reference emitter gives up on: `own_priv.brat` calls a
# private function of another module, and the loader refuses it before any code
# is written. The C++ front end has to refuse it with the same words.
REFUSED = {"own_priv.brat": "function `helper` is private in module `modapi`"}

# Programs the reference emitter *could* write C for, but `brat emit-c` refuses
# because the ownership checker rejects them. `brat-cxx cc` has the same gate now,
# so the property here is: the same refusal, no C on stdout, and `--no-check`
# still produces the reference bytes -- the emitter itself is unchanged, only the
# door in front of it.
OWNERSHIP_REFUSED = {"own_bad.brat", "own_move.brat", "own_paths_bad.brat", "own_sig.brat"}


def emit(path, env=None, no_check=False):
    version = tuple(int(part) for part in unicodedata.unidata_version.split("."))
    case_version = 1600 if version >= (16, 0) else 1500
    child_env = dict(os.environ, BRAT_CASE_MAP_VERSION=str(case_version))
    if env:
        child_env.update(env)
    argv = [BIN, "cc"] + (["--no-check"] if no_check else []) + [path]
    r = subprocess.run(argv, capture_output=True, text=True, timeout=180,
                       env=child_env)
    return r.stdout, r.stderr, r.returncode


def main():
    _cxx.build()
    t0 = time.time()
    files = corpus()
    bad = refused = 0
    for path in files:
        name = os.path.basename(path)
        P._seq = 0
        try:
            want = oracle().emit_file(path)
        except Exception as e:  # the reference refuses to emit this program
            got_out, got_err, rc = emit(path)
            if name not in REFUSED:
                print("FAIL %s: reference raised %s: %s" % (name, type(e).__name__, e))
                bad += 1
                continue
            if str(e) != REFUSED[name] or rc == 0:
                print("FAIL %s: refusal (reference %r, brat-cxx rc=%d)" % (name, str(e), rc))
                bad += 1
                continue
            refused += 1
            continue
        got_out, got_err, rc = emit(path)
        if name in OWNERSHIP_REFUSED:
            # The gate: a checker error refuses emission, stdout stays empty, and
            # the words are the reference emitter's.
            if rc == 0 or got_out:
                print("FAIL %s: the ownership gate let a rejected program through "
                      "(rc=%d, %d bytes)" % (name, rc, len(got_out)))
                bad += 1
                continue
            if "--no-check" not in got_err:
                print("FAIL %s: the refusal must name --no-check, got %r"
                      % (name, got_err.strip()[-200:]))
                bad += 1
                continue
            nout, nerr, nrc = emit(path, no_check=True)
            if nrc != 0 or nout != "/* BRAT: UNVERIFIED -- ownership not checked */\n" + want:
                print("FAIL %s: --no-check must emit the reference bytes (rc=%d, %d of %d)"
                      % (name, nrc, len(nout), len(want)))
                bad += 1
                continue
            refused += 1
            continue
        if rc != 0:
            print("FAIL %s: brat-cxx exited %d: %s" % (name, rc, got_err.strip()[:400]))
            bad += 1
            continue
        if got_out == want:
            continue
        want_l, got_l = want.split("\n"), got_out.split("\n")
        line = next((i for i in range(min(len(want_l), len(got_l))) if want_l[i] != got_l[i]),
                    min(len(want_l), len(got_l)))
        print("FAIL %s: line %d\n  want %r\n  got  %r"
              % (name, line + 1, want_l[line] if line < len(want_l) else "<eof>",
                 got_l[line] if line < len(got_l) else "<eof>"))
        bad += 1
    # The same bytes with the drops turned off: BRAT_MOVE_RT is the bisect knob
    # and the C++ emitter has to honour it, or a bisect would compare two
    # different emitters. The reference reads the variable once, at import.
    oracle()._MOVE_RT = False
    env = {"BRAT_MOVE_RT": "0"}
    nodrop = 0
    for path in files:
        name = os.path.basename(path)
        if name in REFUSED or name in OWNERSHIP_REFUSED:
            continue
        P._seq = 0
        want = oracle().emit_file(path)
        got_out, got_err, rc = emit(path, env)
        if rc != 0 or got_out != want:
            print("FAIL %s (BRAT_MOVE_RT=0): rc=%d, %d bytes of %d"
                  % (name, rc, len(got_out), len(want)))
            bad += 1
        else:
            nodrop += 1
    oracle()._MOVE_RT = True
    if bad:
        print("CXX EMIT FAILED (%d of %d files, %.1fs)" % (bad, len(files), time.time() - t0))
        return 1
    print("CXX EMIT ALL GREEN (%d files, %d refusal(s), %d without drops, same bytes, %.2fs)"
          % (len(files), refused, nodrop, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
