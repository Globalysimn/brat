"""The C++ front end: where it lives, and how to build it exactly once.

A machine without a compiler skips with `_skip.SKIP`, not with 0: the four
oracle tests below prove nothing there, and a green ok would say they did.

Tests run in parallel (run_all.py takes one file per core) and two of them need
the same binary, so the build goes to a temp name and is renamed into place: a
reader either sees no binary or a complete one. Without that, the second test
execs a half-written file and gets EACCES. Builders also take an exclusive file
lock, so a second process waits and finds the binary current instead of compiling
every object again next to the first.
"""

import _skip

from concurrent.futures import ThreadPoolExecutor
import contextlib
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unicodedata

try:
    import fcntl
except ImportError:
    fcntl = None

HERE = os.path.dirname(os.path.abspath(__file__))
CXX = os.path.abspath(os.path.join(HERE, "..", "cxx"))
BIN = os.path.join(CXX, "bin", "brat-cxx")
SRC_DIR = os.path.join(CXX, "src")
SRC = sorted(os.path.join(SRC_DIR, n) for n in os.listdir(SRC_DIR))
CPP = [p for p in SRC if p.endswith(".cpp")]
HDR = [p for p in SRC if p.endswith(".h")]


def fresh():
    return os.path.exists(BIN) and all(os.path.getmtime(s) <= os.path.getmtime(BIN) for s in SRC)


@contextlib.contextmanager
def build_lock(path):
    """Exclusive across processes until the block ends. Without `fcntl` it excludes nobody."""
    with open(path, "w") as f:
        if fcntl:
            fcntl.flock(f, fcntl.LOCK_EX)
        yield


def build():
    if fresh():
        return BIN
    cc = shutil.which("c++") or shutil.which("g++")
    if not cc:
        _skip.skip("no c++ and no g++, the C++ front end cannot be built")
    os.makedirs(os.path.dirname(BIN), exist_ok=True)
    with build_lock(BIN + ".lock"):
        if not fresh():
            compile_all(cc)
    return BIN


def compile_all(cc):
    key = hashlib.sha256((cc + "|" + CXX).encode("utf-8")).hexdigest()[:12]
    obj_dir = os.path.join(tempfile.gettempdir(), "brat-cxx-objs-" + key)
    os.makedirs(obj_dir, exist_ok=True)
    hdr_mtime = max((os.path.getmtime(h) for h in HDR), default=0.0)
    flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror=switch", "-I", SRC_DIR]

    def compile_one(src):
        obj = os.path.join(obj_dir, os.path.basename(src) + ".o")
        need = (not os.path.exists(obj)
                or os.path.getmtime(obj) < max(os.path.getmtime(src), hdr_mtime))
        if need:
            tmp_obj = "%s.tmp%d" % (obj, os.getpid())
            r = subprocess.run([cc] + flags + ["-c", src, "-o", tmp_obj],
                               capture_output=True, text=True)
            if r.returncode != 0:
                if os.path.exists(tmp_obj):
                    os.remove(tmp_obj)
                raise RuntimeError("c++ failed on %s:\n%s" % (src, r.stderr[-2000:]))
            os.replace(tmp_obj, obj)
        return obj

    workers = min(4, max(2, os.cpu_count() or 2))
    try:
        with ThreadPoolExecutor(max_workers=workers) as ex:
            objs = list(ex.map(compile_one, CPP))
    except RuntimeError as e:
        sys.exit(str(e))

    tmp = "%s.tmp%d" % (BIN, os.getpid())
    r = subprocess.run([cc, "-std=c++20", "-O2"] + objs + ["-o", tmp],
                       capture_output=True, text=True)
    if r.returncode != 0:
        if os.path.exists(tmp):
            os.remove(tmp)
        sys.exit("c++ link failed:\n" + r.stderr[-2000:])
    os.replace(tmp, BIN)


_UNVERIFIED = "/* BRAT: UNVERIFIED -- ownership not checked */\n"


def _native_text(args, env=None):
    merged = dict(os.environ)
    version = tuple(int(part) for part in unicodedata.unidata_version.split("."))
    merged.setdefault("BRAT_CASE_MAP_VERSION", "1600" if version >= (16, 0) else "1500")
    if env:
        merged.update(env)
    r = subprocess.run([build()] + list(args), capture_output=True, text=True,
                       encoding="utf-8", errors="backslashreplace", env=merged)
    if r.returncode != 0:
        raise RuntimeError(
            "brat-cxx %s failed (%d):\n%s" % (args[0], r.returncode, r.stderr[-2000:]))
    return r.stdout


def emit_file(path, flags=(), env=None, no_check=True):
    """C for one file from the native emitter.

    Tests use this instead of shelling out to the CLI for one file. The
    default is the emitter-level `--no-check` door because several runtime
    corpora intentionally exercise programs below a checker rule; product
    gate tests pass `no_check=False` or call `brat-cxx emit-c` themselves.
    """
    args = ["cc"]
    if no_check and "--no-check" not in flags:
        args.append("--no-check")
    args.extend(flags)
    args.append(os.path.abspath(path))
    text = _native_text(args, env=env)
    if no_check and text.startswith(_UNVERIFIED):
        text = text[len(_UNVERIFIED):]
    return text


def emit_source(src, base=".", flags=(), env=None, no_check=True):
    """C for BRAT source text, emitted by the native compiler."""
    fd, path = tempfile.mkstemp(dir=os.path.abspath(base), suffix=".brat")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(src)
        return emit_file(path, flags=flags, env=env, no_check=no_check)
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def emit_c_file(path, flags=(), env=None):
    """C through the product `emit-c` command, including its gates."""
    return _native_text(["emit-c"] + list(flags) + [os.path.abspath(path)], env=env)


def sx(node):
    """The reference s-expression printer, used by the two tree-oracle tests.

    Same bytes as `lang/engine/brat/parser.py`'s `dump`, rebuilt here so a test
    can compare against any Python value, not only a parsed file.
    """
    if isinstance(node, tuple):
        return "(" + " ".join(sx(x) for x in node) + ")"
    if isinstance(node, list):
        return "[" + " ".join(sx(x) for x in node) + "]"
    if isinstance(node, bool):
        return "true" if node else "false"
    if node is None:
        return "None"
    if isinstance(node, float):
        return repr(node)
    if isinstance(node, int):
        return str(node)
    if isinstance(node, str):
        out = (node.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")
               .replace("\r", "\\r").replace('"', '\\"'))
        return '"%s"' % out
    return repr(node)
