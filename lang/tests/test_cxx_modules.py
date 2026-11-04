"""The C++ loader flattens a program the way `brat/modules.py` does.

`brat-cxx flat FILE` prints the tree the C emitter will work on: imports spliced
in where they are written, every name qualified by its module, `pub use`
resolved and then dropped. The reference value is `brat.load_file(path).prog`,
which is the flattened form the native emitter consumes, so this test is the
contract that emitter is written against.

A program the reference refuses to link — a private function called from another
module, an ambiguous unqualified name — has to be refused with the same words.
"""

import _cxx
import _scratch

import importlib.util
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
BIN = _cxx.BIN
TMP = _scratch.mkdtemp(prefix="brat-modules-")

sys.path.insert(0, ENGINE)
import brat.parser as P  # noqa: E402,F401  (the engine expects its sibling imports)

_LOADER = [None]


def oracle():
    if _LOADER[0] is None:
        spec = importlib.util.spec_from_file_location(
            "brat_oracle_modules", os.path.join(ENGINE, "brat.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _LOADER[0] = mod
    return _LOADER[0]


def corpus():
    out = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in ("vendor", "bin", ".git", "__pycache__")]
        for f in files:
            if f.endswith(".brat"):
                out.append(os.path.join(base, f))
    return sorted(out)


def flat(path):
    r = subprocess.run([BIN, "flat", path], capture_output=True, text=True, timeout=120)
    return r.stdout, r.stderr, r.returncode


# Programs the reference refuses to link, and the line it refuses with. All of
# them are in the corpus on purpose: a refusal is part of the contract.
REFUSED = {
    "own_priv.brat": "function `helper` is private in module `modapi`",
}


def main():
    _cxx.build()
    t0 = time.perf_counter()
    bad = 0
    files = corpus()
    checked = 0

    for path in files:
        name = os.path.basename(path)
        try:
            # The parser's temp counter is a module global: without a reset the
            # `@mN` names drift with the order files are visited and nothing
            # would ever match.
            P._seq = 0
            want = _cxx.sx(oracle().load_file(path).prog) + "\n"
            failure = None
        except Exception as e:                                   # noqa: BLE001
            want = None
            failure = str(e)
        got, err, rc = flat(path)
        checked += 1
        if failure is not None:
            if rc == 0:
                bad += 1
                print("ACCEPTED %s: reference refuses it (%s)" % (name, failure))
            elif REFUSED.get(name) != failure or failure not in (got + err):
                bad += 1
                print("MESSAGE %s\n  want %r\n  got  %r"
                      % (name, REFUSED.get(name, "<not a known refusal>"), (got + err)[:200]))
            continue
        if rc != 0:
            bad += 1
            print("REFUSED %s: %s" % (name, (got + err).strip()[:200]))
            continue
        if got != want:
            bad += 1
            if bad <= 3:
                i = next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), None)
                print("DIFF %s" % os.path.relpath(path, ROOT))
                if i is None:
                    print("  lengths %d and %d" % (len(want), len(got)))
                else:
                    print("  python: %r" % want[max(0, i - 70):i + 70])
                    print("  c++   : %r" % got[max(0, i - 70):i + 70])

    # The pinned refusal has to be exercised: a corpus file that quietly started
    # loading would leave the message below untested.
    for name in REFUSED:
        if not any(os.path.basename(p) == name for p in files):
            bad += 1
            print("MISSING fixture %s" % name)

    dt = time.perf_counter() - t0
    assert bad == 0, "%d differences" % bad
    print("CXX MODULES ALL GREEN (%d files, same flattened tree, %d refusal(s), %.2fs)"
          % (checked, len(REFUSED), dt))


main()
