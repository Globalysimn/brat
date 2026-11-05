"""C compiler resolution for `brat build`.

Default: `$BRAT_CC`, else `cc`. `zig cc` is used only where it earns its size:
`--target` (cross-compiling) and `--static`, and only when zig is installed.
"""

import hashlib
import os
import re
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


# The test harnesses reuse binaries after preprocessing their C inputs.
# Foreign link inputs and unsupported compiler flags bypass this cache.

_HERE = os.path.dirname(os.path.abspath(__file__))
_ENGINE = os.path.abspath(os.path.join(_HERE, "..", "engine"))


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
                    # BRAT_CACHE may point at a directory with unrelated files.
                    # Only completed entries written by cc_cached belong to us.
                    if (len(e.name) != 64
                            or any(c not in "0123456789abcdef" for c in e.name)
                            or not e.is_file(follow_symlinks=False)):
                        continue
                    st = e.stat(follow_symlinks=False)
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


def _cacheable_args(argv, cfile, out):
    if os.path.splitext(cfile)[1] != ".c" or os.path.abspath(cfile) == os.path.abspath(out):
        return False
    try:
        if os.path.samefile(cfile, out):
            return False
    except OSError:
        pass
    takes_value = {"-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-D", "-U"}
    simple = {"-w", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
              "-fwrapv", "-fPIC", "-fpic", "-fno-omit-frame-pointer",
              "-fno-sanitize-recover=all", "-lm", "-lpthread", "-ldl"}
    i, sources, outputs = 1, 0, 0
    while i < len(argv):
        arg = argv[i]
        if arg == cfile:
            sources += 1
        elif arg == "-o":
            i += 1
            if i >= len(argv) or argv[i] != out:
                return False
            outputs += 1
        elif arg in takes_value:
            i += 1
            if i >= len(argv):
                return False
        elif arg in simple or re.fullmatch(r"-O[0123sg]|-g[0123]?|-std=[a-z0-9+]+", arg):
            pass
        elif any(arg.startswith(prefix) and len(arg) > len(prefix) for prefix in ("-I", "-D", "-U")):
            pass
        elif arg.startswith("-Wno-") or arg.startswith("-Werror="):
            pass
        elif arg.startswith("-fsanitize=") and set(arg.split("=", 1)[1].split(",")) <= {
                "address", "undefined", "leak", "thread"}:
            pass
        else:
            return False
        i += 1
    return sources == 1 and outputs == 1


def _run_cc(argv, timeout, env, binary=False):
    options = dict(stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                   env=env)
    if not binary:
        options.update(text=True, encoding="utf-8", errors="backslashreplace")
    if os.name == "nt":
        options["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        options["start_new_session"] = True
    proc = subprocess.Popen(argv, **options)
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name != "nt":
            import signal
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        else:
            proc.kill()
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.stdout.close()
            proc.stderr.close()
            proc.wait()
        raise
    return subprocess.CompletedProcess(argv, proc.returncode, stdout, stderr)


def _cache_input(cfile, argv, out, env, timeout):
    effective = dict(os.environ if env is None else env)
    if effective.get("BRAT_CC_CACHE_DISABLE") == "1" or not _cacheable_args(argv, cfile, out):
        return None, None
    prog = shutil.which(argv[0], path=effective.get("PATH", os.defpath))
    if not prog:
        return None, None
    try:
        output_index = argv.index("-o")
        preprocessor = [prog, *argv[1:output_index], *argv[output_index + 2:],
                        "-E", "-o", "-"]
        result = _run_cc(preprocessor, timeout, effective, binary=True)
        if result.returncode or b".incbin" in result.stdout:
            return None, None
        h = hashlib.sha256(b"brat-cc-cache-v3\0")
        crt_parts_dir = os.path.join(_ENGINE, "crt")
        crt_paths = [os.path.join(_ENGINE, "crt.h")]
        if os.path.isdir(crt_parts_dir):
            crt_paths.extend(
                os.path.join(crt_parts_dir, fn)
                for fn in sorted(os.listdir(crt_parts_dir))
                if fn.endswith(".h")
            )
        for path in (prog, cfile, *crt_paths):
            with open(path, "rb") as source:
                h.update(hashlib.sha256(source.read()).digest())
        h.update(repr([os.path.realpath(prog), os.path.abspath(cfile), os.getcwd(),
                       ["<out>" if a == out else a for a in argv],
                       sorted(effective.items())]).encode("utf-8"))
        h.update(hashlib.sha256(result.stdout).digest())
        h.update(hashlib.sha256(result.stderr).digest())
        return h.hexdigest(), result
    except (OSError, subprocess.TimeoutExpired):
        return None, None


def cache_key(cfile, argv, out, env=None, timeout=10):
    """Hash supported C inputs; foreign link inputs and opaque flags bypass.

    The installed system toolchain and its runtime libraries must remain stable
    during a build. A miss compiles the preprocessed bytes used for the key.
    """
    return _cache_input(cfile, argv, out, env, timeout)[0]


def cc_cached(argv, cfile, out, timeout=300, env=None):
    """Compile C, reusing binaries only for supported, fingerprinted inputs."""
    raw_tmo = os.environ.get("BRAT_CC_TIMEOUT_MS", "").strip()
    try:
        milliseconds = int(raw_tmo)
        if 0 < milliseconds <= 3600000:
            timeout = milliseconds / 1000.0
    except ValueError:
        pass
    key, preprocessed = _cache_input(cfile, argv, out, env, min(timeout, 10))
    try:
        hit = os.path.join(cache_dir(), key) if key else None
    except OSError:
        hit = None
    if hit and os.path.isfile(hit):
        try:
            shutil.copy2(hit, out)
            return subprocess.CompletedProcess(argv, 0, "", "")
        except OSError:
            pass
    if preprocessed is None:
        result = _run_cc(argv, timeout, env)
    else:
        # Compile the same bytes we hashed, even if a header changes meanwhile.
        with tempfile.TemporaryDirectory(prefix="brat-cc-") as stage:
            source = os.path.join(stage, "input.i")
            with open(source, "wb") as target:
                target.write(preprocessed.stdout)
            command = [source if arg == cfile else arg for arg in argv]
            result = _run_cc(command, timeout, env)
            result.stderr = preprocessed.stderr.decode("utf-8", "backslashreplace") + result.stderr
            result.args = argv
    if result.returncode == 0 and hit:
        temporary = None
        try:
            fd, temporary = tempfile.mkstemp(prefix=key + ".tmp-", dir=os.path.dirname(hit))
            os.close(fd)
            shutil.copy2(out, temporary)
            os.replace(temporary, hit)
        except OSError:
            pass
        finally:
            if temporary is not None:
                try:
                    os.unlink(temporary)
                except FileNotFoundError:
                    pass
        try:
            prune_cache()
        except OSError:
            pass
    return result
