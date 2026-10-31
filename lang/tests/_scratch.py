"""Scratch directories whose path is the same on every run.

A test's directory ends up inside the C the engine emits (`vbase_set`), which is
part of the compiler-cache key. A fresh `mkdtemp` per run therefore makes every
compiled program a cache miss: `test_agreement.py` takes 46 s cold and 4 s warm.
The path is derived from the test file that asks for it, so it is stable and
unique per test; the contents are wiped at the start of every run.
"""

import inspect
import os
import pathlib
import re
import shutil

ROOT = os.environ.get("BRAT_SCRATCH") or os.path.join(
    os.environ.get("TMPDIR") or "/tmp", "brat-scratch")

_seen = {}


def _tag(depth):
    path = inspect.stack()[depth].filename
    return re.sub(r"[^a-z0-9]+", "-", os.path.basename(path)[:-3].lower()).strip("-")


def mkdtemp(prefix="", suffix="", dir=None):
    """Stable stand-in for `tempfile.mkdtemp`: same path, empty every run."""
    name = _tag(2) + ("-" + re.sub(r"[^a-z0-9]+", "-", prefix.lower()).strip("-")
                      if prefix else "")
    n = _seen[name] = _seen.get(name, 0) + 1
    if n > 1:
        name = "%s-%d" % (name, n)
    path = os.path.join(dir or ROOT, name + suffix)
    shutil.rmtree(path, ignore_errors=True)
    pathlib.Path(path).mkdir(parents=True, exist_ok=True)
    return path
