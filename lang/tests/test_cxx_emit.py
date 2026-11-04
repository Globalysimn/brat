"""The native emitter over every shipped BRAT program.

This corpus test pins the emitter's outer contract now that it is the only C
emitter:

* every accepted program emits non-empty, deterministic C;
* the one private-cross-module program is refused by name;
* ownership-rejected programs produce no C unless `--no-check` is explicit;
* `--no-check` output carries exactly the unverified banner; and
* the `BRAT_MOVE_RT=0` bisection switch changes the drops without refusing
  the corpus.
"""

import _cxx
import _scratch

import os
import subprocess
import sys
import time
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
BIN = _cxx.BIN
TMP = _scratch.mkdtemp(prefix="brat-emit-")

REFUSED = {"own_priv.brat": "function `helper` is private in module `modapi`"}
OWNERSHIP_REFUSED = {
    "own_bad.brat",
    "own_move.brat",
    "own_paths_bad.brat",
    "own_sig.brat",
}
BANNER = "/* BRAT: UNVERIFIED -- ownership not checked */\n"


def corpus():
    out = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in (
            "vendor", "bin", ".git", "__pycache__", "scratch", "tmp", "out")]
        for f in files:
            if f.endswith(".brat") and not f.startswith((".", "_")):
                out.append(os.path.join(base, f))
    return sorted(out)


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
    bad = 0
    refused = 0
    accepted = []  # (path, no-check C without the banner)

    for path in files:
        name = os.path.basename(path)
        out, err, rc = emit(path)

        if name in REFUSED:
            if rc == 0 or out or REFUSED[name] not in err:
                print("FAIL %s: expected the named private-function refusal "
                      "(rc=%d, %d bytes, err=%r)"
                      % (name, rc, len(out), err.strip()[-300:]))
                bad += 1
            else:
                refused += 1
            continue

        if name in OWNERSHIP_REFUSED:
            if rc == 0 or out or "--no-check" not in err:
                print("FAIL %s: ownership gate must refuse without C "
                      "(rc=%d, %d bytes)" % (name, rc, len(out)))
                bad += 1
                continue
            nout, nerr, nrc = emit(path, no_check=True)
            if nrc != 0 or not nout.startswith(BANNER) or len(nout) <= len(BANNER):
                print("FAIL %s: --no-check must emit bannered C (rc=%d, %r)"
                      % (name, nrc, nerr.strip()[-300:]))
                bad += 1
                continue
            accepted.append((path, nout[len(BANNER):]))
            refused += 1
            continue

        if rc != 0 or not out:
            print("FAIL %s: native emitter exited %d: %s"
                  % (name, rc, err.strip()[:400]))
            bad += 1
            continue
        out2, _err2, rc2 = emit(path)
        if rc2 != 0 or out2 != out:
            print("FAIL %s: emitter output is not deterministic" % name)
            bad += 1
            continue
        accepted.append((path, out))

    changed_without_drops = 0
    for path, want in accepted:
        out, err, rc = emit(path, env={"BRAT_MOVE_RT": "0"}, no_check=True)
        if rc != 0 or not out.startswith(BANNER):
            print("FAIL %s (BRAT_MOVE_RT=0): rc=%d, err=%r"
                  % (os.path.basename(path), rc, err.strip()[-300:]))
            bad += 1
            continue
        if out[len(BANNER):] != want:
            changed_without_drops += 1
    if accepted and changed_without_drops == 0:
        print("FAIL: BRAT_MOVE_RT=0 changed no corpus output")
        bad += 1

    if bad:
        print("CXX EMIT FAILED (%d of %d files, %.1fs)"
              % (bad, len(files), time.time() - t0))
        return 1
    print("CXX EMIT ALL GREEN (%d files, %d refusal(s), %d changed without "
          "drops, deterministic, %.2fs)"
          % (len(files), refused, changed_without_drops, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
