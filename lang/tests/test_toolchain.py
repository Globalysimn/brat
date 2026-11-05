"""Toolchain contract: system `cc` by default, `zig cc` only for --target and --static."""
import _cxx
import _scratch

import importlib.util
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
HELLO = os.path.abspath(os.path.join(HERE, "..", "examples", "hello.brat"))

_spec = importlib.util.spec_from_file_location("tc_contract", os.path.join(HERE, "_toolchain.py"))
tc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tc)

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def fake_cc(directory, name):
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write("#!/bin/sh\nexit 0\n")
    os.chmod(path, 0o755)
    return path


def refused(fn, msg):
    try:
        fn()
    except tc.CcError as e:
        check(str(e) == msg, "CcError text %r, want %r" % (str(e), msg))
        return
    check(False, "no CcError, want %r" % msg)


tmp = _scratch.mkdtemp()
cc = fake_cc(tmp, "cc")
env_cc_only = {"PATH": tmp}

spec = tc.resolve_cc(env=env_cc_only)
check(spec.exe == cc, "default compiler %r, want %r" % (spec.exe, cc))
check(spec.zig is False, "default compiler must not be zig")
check(spec.argv("p.c", "p.exe") == [cc, "p.c", "-o", "p.exe"],
      "argv %r" % (spec.argv("p.c", "p.exe"),))
check(spec.argv("p.c", "p.exe", static=True) == [cc, "p.c", "-o", "p.exe", "-static"],
      "static argv %r" % (spec.argv("p.c", "p.exe", static=True),))

refused(lambda: tc.resolve_cc(target="x86_64-linux-gnu", env=env_cc_only),
        "--target needs zig cc")
check(tc.resolve_cc(static=True, env=env_cc_only).exe == cc,
      "--static without zig falls back to cc")

zig = fake_cc(tmp, "zig")
env_both = {"PATH": tmp}

check(tc.resolve_cc(env=env_both).exe == cc, "zig on PATH must not change the default build")

spec = tc.resolve_cc(target="x86_64-linux-gnu", env=env_both)
check(spec.zig is True, "--target must select zig")
check(spec.exe == zig, "--target compiler %r, want %r" % (spec.exe, zig))
check(spec.argv("p.c", "p.exe", target="x86_64-linux-gnu")
      == [zig, "cc", "p.c", "-o", "p.exe", "-target", "x86_64-linux-gnu"],
      "cross argv %r" % (spec.argv("p.c", "p.exe", target="x86_64-linux-gnu"),))

spec = tc.resolve_cc(static=True, env=env_both)
check(spec.exe == zig, "--static must select zig when it is installed")
check(spec.argv("p.c", "p.exe", static=True) == [zig, "cc", "p.c", "-o", "p.exe", "-static"],
      "zig static argv %r" % (spec.argv("p.c", "p.exe", static=True),))

spec = tc.resolve_cc(env=dict(env_both, BRAT_CC=cc))
check(spec.exe == cc, "BRAT_CC must win over zig")
refused(lambda: tc.resolve_cc(target="aarch64-linux-gnu", env=dict(env_both, BRAT_CC=cc)),
        "--target needs zig cc")

empty = _scratch.mkdtemp()
refused(lambda: tc.resolve_cc(env={"PATH": empty}),
        "no C compiler found: install cc or set BRAT_CC")

cfile = os.path.join(tmp, "p.c")
with open(cfile, "w", encoding="utf-8") as f:
    f.write("int main(void){return 0;}\n")
key = tc.cache_key(cfile, [cc, cfile, "-o", "p.exe"], "p.exe")
check(isinstance(key, str) and len(key) == 64, "cache key %r" % (key,))
check(all(c in "0123456789abcdef" for c in key), "cache key is not hex: %r" % (key,))

ccdir = _scratch.mkdtemp()
fake_cc(ccdir, "cc")
out = os.path.join(tmp, "hello.exe")
p = subprocess.run(_cxx.cli_args("build", HELLO, "-o", out, "--target", "x86_64-linux-gnu"),
                   capture_output=True, text=True, timeout=120,
                   env={"PATH": ccdir, "LC_ALL": "C"})
check(p.returncode == 1, "build --target without zig exit=%d" % p.returncode)
check(p.stderr == "brat build: --target needs zig cc\n",
      "build --target stderr %r" % (p.stderr,))

# the compile cache: it must actually be used, and it must stay bounded
# This used to be dead code for the test harnesses: argv[0] arrived as a bare
# "cc", os.stat failed, the key was None and every test recompiled from scratch.
cache = _scratch.mkdtemp()
os.environ["BRAT_CACHE"] = cache
bare = tc.cache_key(cfile, ["cc", cfile, "-o", "p.exe"], "p.exe")
check(isinstance(bare, str) and len(bare) == 64,
      "a bare compiler name must still give a key, got %r" % (bare,))
check(bare != key, "a resolved and a bare name must not collide")

src_c = os.path.join(tmp, "cached.c")
with open(src_c, "w", encoding="utf-8") as f:
    f.write("int main(void){return 0;}\n")
exe1 = os.path.join(tmp, "cached1")
r1 = tc.cc_cached(["cc", src_c, "-o", exe1], src_c, exe1, timeout=120)
check(r1.returncode == 0, "first compile failed")
check(len(os.listdir(cache)) == 1, "the first compile was not stored: %r" % (os.listdir(cache),))
exe2 = os.path.join(tmp, "cached2")
r2 = tc.cc_cached(["cc", src_c, "-o", exe2], src_c, exe2, timeout=120)
check(r2.returncode == 0 and r2.stderr == "", "cache hit must be silent")
check(os.path.isfile(exe2), "cache hit did not produce the output binary")

# A hit whose entry vanished under it (another process pruned) is a miss, not a crash.
for name in os.listdir(cache):
    os.unlink(os.path.join(cache, name))
r3 = tc.cc_cached(["cc", src_c, "-o", exe1], src_c, exe1, timeout=120)
check(r3.returncode == 0, "a pruned entry must fall back to compiling")

# The bound: entries go oldest-first until the directory fits. Four 4096-byte
# entries against a 9000-byte limit must leave exactly the two newest.
for name in os.listdir(cache):
    os.unlink(os.path.join(cache, name))
for i in range(4):
    p_i = os.path.join(cache, "%064x" % i)
    with open(p_i, "wb") as f:
        f.write(b"x" * 4096)
    os.utime(p_i, (1000 + i, 1000 + i))
freed = tc.prune_cache(limit=9000)
check(freed == 8192, "prune freed %d, want 8192" % freed)
check(sorted(os.listdir(cache)) == ["%064x" % 2, "%064x" % 3],
      "prune must drop the oldest first, left %r" % (sorted(os.listdir(cache)),))
check(tc.prune_cache(limit=9000) == 0, "prune under the limit must free nothing")

# The cache must live somewhere that is not the temporary directory by default:
# a tmpfs of a few hundred megabytes filled up and every compile after it failed
# with "No space left on device", reported as the test that happened to run.
saved = os.environ.pop("BRAT_CACHE", None)
try:
    home_cache = tc.cache_dir()
    check(home_cache != os.path.join(tempfile.gettempdir(), "brat-cache"),
          "the default cache is still the temporary directory: %r" % (home_cache,))
    check(home_cache.startswith(os.environ.get("XDG_CACHE_HOME")
                                or os.path.expanduser("~")),
          "the default cache is not under the home cache dir: %r" % (home_cache,))
    os.environ["BRAT_CACHE"] = cache
    check(tc.cache_dir() == cache, "BRAT_CACHE must win over the default")
    check(tc.CACHE_MAX_BYTES % (1024 * 1024) == 0, "cap is not a whole number of MiB")
finally:
    if saved is None:
        os.environ.pop("BRAT_CACHE", None)
    else:
        os.environ["BRAT_CACHE"] = saved

print("TOOLCHAIN ALL GREEN (%d asserts)" % n)
