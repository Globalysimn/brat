"""C compiler resolution for `brat build`.

Default: `$BRAT_CC`, else `cc`. `zig cc` is used only where it earns its size:
`--target` (cross-compiling) and `--static`, and only when zig is installed.
"""

import hashlib
import os
import shutil
import stat
import subprocess
import tempfile

_FALLBACK_CACHE = None


class CcError(Exception):
    """No usable C compiler, or a request the resolved compiler cannot fulfil."""


class CcSpec:
    """One resolved compiler: argv head, and whether it is `zig cc`."""

    def __init__(self, exe, zig):
        self.exe = exe
        self.zig = zig

    def argv(self, cfile, out, target=None, static=False):
        """Command line that compiles `cfile` into `out`."""
        args = [self.exe] + (["cc"] if self.zig else []) + [cfile, "-o", out]
        if target is not None:
            if not self.zig:
                raise CcError("--target needs zig cc")
            args += ["-target", target]
        if static:
            args.append("-static")
        return args


def _is_zig(exe):
    return os.path.basename(exe).lower().startswith("zig")


def resolve_cc(target=None, static=False, env=None):
    """Pick the compiler for one build.

    `target` and `static` mean the user asked for them explicitly; a plain
    build never looks at zig, so an installed zig cannot change its output.
    """
    env = os.environ if env is None else env
    path = env.get("PATH")

    def which(name):
        return shutil.which(name, path=path)

    override = env.get("BRAT_CC")
    if override:
        exe = which(override)
        if exe is None:
            raise CcError("BRAT_CC=%r is not an executable" % override)
        if target is not None and not _is_zig(exe):
            raise CcError("--target needs zig cc")
        return CcSpec(exe, _is_zig(exe))
    zig = which("zig")
    if zig is not None and (target is not None or static):
        return CcSpec(zig, True)
    if target is not None:
        raise CcError("--target needs zig cc")
    exe = which("cc") or which("gcc")
    if exe is None:
        raise CcError("no C compiler found: install cc or set BRAT_CC")
    return CcSpec(exe, False)


# Compiled-binary cache: `brat build` and the test harnesses compile the same C
# repeatedly. A binary is reused when the generated C, crt.h, the exact command
# and the compiler binary match. Set BRAT_CACHE to move the cache.

_HERE = os.path.dirname(os.path.abspath(__file__))


def cache_dir():
    """Where compiled binaries live: `$BRAT_CACHE`, else `~/.cache/brat`.

    It used to default to the temporary directory, which is a tmpfs of a few
    hundred megabytes on many machines. The cache would then fill it and every
    later compile would fail with "No space left on device" -- reported as a
    build failure in whatever test happened to be running, not as the cache
    problem it was. A home directory is the right place for a cache; the
    temporary directory is the fallback only when `~` is not writable.
    """
    global _FALLBACK_CACHE
    d = os.environ.get("BRAT_CACHE")
    if d:
        os.makedirs(d, exist_ok=True)
        return d
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(
        os.path.expanduser("~"), ".cache")
    candidate = os.path.join(base, "brat")
    try:
        os.makedirs(candidate, exist_ok=True)
        return candidate
    except OSError:
        pass
    uid = os.getuid() if hasattr(os, "getuid") else None
    fb = os.path.join(tempfile.gettempdir(),
                      "brat-cache-%d" % uid if uid is not None else "brat-cache")
    try:
        os.makedirs(fb, mode=0o700, exist_ok=True)
        st = os.lstat(fb)
        if not stat.S_ISLNK(st.st_mode) and stat.S_ISDIR(st.st_mode) and (
                uid is None or st.st_uid == uid):
            if uid is not None:
                os.chmod(fb, 0o700)
            return fb
    except OSError:
        pass
    if _FALLBACK_CACHE is None or not os.path.isdir(_FALLBACK_CACHE):
        _FALLBACK_CACHE = tempfile.mkdtemp(prefix="brat-cache-")
    return _FALLBACK_CACHE


# A cache that never shrinks is a leak with a nicer name: the ASan corpus alone
# is 64 debug binaries, and a developer who edits crt.h invalidates all of them
# at once. The cap is enforced on store, oldest-first by mtime, and only for
# entries this module wrote (a plain file in the cache directory).
try:
    _MAX_MB = max(1, int(os.environ.get("BRAT_CACHE_MAX_MB", "1024") or "1024"))
except ValueError:
    _MAX_MB = 1024
CACHE_MAX_BYTES = _MAX_MB * 1024 * 1024

# Below this much free space the cache gives way to the work that needs the disk.
LOW_SPACE_BYTES = 128 * 1024 * 1024


def prune_cache(limit=None):
    """Drop the oldest entries until the cache fits `limit`. Returns bytes freed."""
    limit = CACHE_MAX_BYTES if limit is None else limit
    d = cache_dir()
    try:
        # A full filesystem turns a compile into a fake test failure. When free
        # space is short, shrink to half of what is there instead of waiting for
        # the cap to be reached.
        try:
            usage = shutil.disk_usage(d)
            if usage.free < LOW_SPACE_BYTES:
                limit = min(limit, max(0, usage.total // 4))
        except OSError:
            pass
        entries = []
        total = 0
        with os.scandir(d) as it:
            for e in it:
                try:
                    if not e.is_file():
                        continue
                    st = e.stat()
                except OSError:
                    continue
                entries.append((st.st_mtime_ns, e.path, st.st_size))
                total += st.st_size
    except OSError:
        return 0
    if total <= limit:
        return 0
    freed = 0
    for _, path, size in sorted(entries):
        if total - freed <= limit:
            break
        try:
            os.unlink(path)
            freed += size
        except OSError:
            continue
    return freed


def cache_key(cfile, argv, out):
    """Key on everything that decides the binary; None when it cannot be computed.

    The compiler is identified by its path, mtime and size, so an upgrade
    invalidates the cache without running `--version`.
    """
    try:
        # argv[0] arrives as a bare name from the test harnesses ("cc") and as a
        # full path from `brat build`. Resolve it first, or os.stat fails, the
        # key is None, and the cache is silently never used.
        prog = argv[0]
        if not os.path.isfile(prog):
            prog = shutil.which(prog) or prog
        h = hashlib.sha256()
        for p in (cfile, os.path.join(_HERE, "crt.h")):
            with open(p, "rb") as f:
                h.update(f.read())
        st = os.stat(prog)
        h.update(repr([os.path.realpath(prog), st.st_mtime_ns, st.st_size,
                       ["<out>" if a == out else "<src>" if a == cfile else a
                        for a in argv]]).encode("utf-8"))
        return h.hexdigest()
    except OSError:
        return None


def cc_cached(argv, cfile, out, timeout=300, env=None):
    """Run the compiler on `cfile` -> `out`, reusing a cached binary when possible.

    Returns a CompletedProcess. A cache hit reports returncode 0 with empty output.
    """
    key = cache_key(cfile, argv, out)
    hit = os.path.join(cache_dir(), key) if key else None
    if hit and os.path.isfile(hit):
        try:
            shutil.copy2(hit, out)
            return subprocess.CompletedProcess(argv, 0, "", "")
        except OSError:
            # prune_cache in another process removed the entry between the check
            # and the copy. A miss is the correct answer, not a crash.
            pass
    raw_tmo = os.environ.get("BRAT_CC_TIMEOUT_MS", "").strip()
    if raw_tmo:
        try:
            ms_v = int(raw_tmo)
            if 0 < ms_v <= 3600000:
                timeout = ms_v / 1000.0
        except ValueError:
            pass
    pkw = {"stdin": subprocess.DEVNULL, "stdout": subprocess.PIPE, "stderr": subprocess.PIPE,
           "text": True, "env": env}
    if os.name == "nt":
        pkw["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        pkw["start_new_session"] = True
    proc = subprocess.Popen(argv, **pkw)
    try:
        out_s, err_s = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name != "nt":
            try:
                import signal
                os.killpg(proc.pid, signal.SIGKILL)
            except OSError:
                proc.kill()
        else:
            proc.kill()
        proc.communicate()
        raise
    r = subprocess.CompletedProcess(argv, proc.returncode, out_s, err_s)
    if r.returncode == 0 and hit:
        try:
            tmp = hit + ".tmp%d" % os.getpid()
            shutil.copy2(out, tmp)
            os.replace(tmp, hit)
        except OSError:
            pass   # a failed store only costs the next build its cache hit
        prune_cache()
    return r
