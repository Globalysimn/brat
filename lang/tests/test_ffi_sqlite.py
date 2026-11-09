"""A direct libsqlite3 binding, proven against the system header.

The program in `examples/sqlite_ffi.brat` declares the C API itself: opaque
handles through `ptr[ptr]`, prepared statements, typed binds (with
`SQLITE_TRANSIENT` passed as the raw destructor bits), reset/reuse, and column
name/type/value reads. This file proves those declarations with both FFI front
ends and runs the same add/list scenario on the -O0 build, on a native build,
and under ASan/UBSan/LSan.
"""
import _cxx
import _scratch
import _skip

import ctypes
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
EXAMPLE = os.path.join(ROOT, "examples", "sqlite_ffi.brat")
TMP = _scratch.mkdtemp(prefix="brat-sqlite-ffi-")

DEMO = ("id\tname\tprice\tqty\tnote\n"
        "1\tapple\t2.5\t12\tcrisp\n"
        "2\tpear\t4.75\t7\tNULL\n"
        "3\tplum\t3.25\t20\tdark\n")
LIST = ("id\tname\tprice\tqty\tnote\n"
        "1\tkiwi\t1.5\t30\tsour\n"
        "2\tfig\t6.25\t3\tNULL\n")

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=300, **kw)


def header_path():
    cand = [os.environ.get("SQLITE3_HEADER", ""),
            "/usr/include/sqlite3.h",
            "/usr/local/include/sqlite3.h",
            "/opt/homebrew/include/sqlite3.h",
            "/usr/local/opt/sqlite/include/sqlite3.h"]
    for p in cand:
        if p and os.path.isfile(p):
            return p
    return None


def sqlite_ready(header):
    if not header:
        return False
    try:
        ctypes.CDLL("libsqlite3.so.0")
    except OSError:
        return False
    cc = shutil.which(os.environ.get("BRAT_CC", "cc"))
    if cc is None:
        return False
    src = os.path.join(TMP, "probe.c")
    exe = os.path.join(TMP, "probe")
    with open(src, "w", encoding="utf-8") as f:
        f.write("#include <sqlite3.h>\nint main(void) { return sqlite3_libversion() == 0; }\n")
    r = run([cc, src, "-o", exe, "-lsqlite3"])
    return r.returncode == 0


def scenario(label, invoke):
    r = invoke([])
    check(r.returncode == 0, "%s demo rc=%d err=%s" % (label, r.returncode, r.stderr[-400:]))
    check(r.stdout == DEMO, "%s demo:\n%r\nwant %r" % (label, r.stdout, DEMO))
    db = os.path.join(TMP, label + ".db")
    r = invoke([db, "add", "kiwi", "1.5", "30", "sour"])
    check(r.returncode == 0, "%s add rc=%d err=%s" % (label, r.returncode, r.stderr[-400:]))
    check(r.stdout == "added kiwi\n", "%s add stdout %r" % (label, r.stdout))
    r = invoke([db, "add", "fig", "6.25", "3"])
    check(r.returncode == 0, "%s add2 rc=%d err=%s" % (label, r.returncode, r.stderr[-400:]))
    check(r.stdout == "added fig\n", "%s add2 stdout %r" % (label, r.stdout))
    r = invoke([db, "list"])
    check(r.returncode == 0, "%s list rc=%d err=%s" % (label, r.returncode, r.stderr[-400:]))
    check(r.stdout == LIST, "%s list:\n%r\nwant %r" % (label, r.stdout, LIST))


def main():
    header = header_path()
    if not sqlite_ready(header):
        _skip.skip("no system sqlite3.h/libsqlite3 development files")
    cxx = _cxx.build()

    r = run([cxx, "ffi", EXAMPLE, "--header", header, "--preprocess"])
    check(r.returncode == 0, "native ffi rc=%d out=%s err=%s" % (r.returncode, r.stdout[-400:], r.stderr[-400:]))
    check("18 binding(s) proven, 0 error(s)" in r.stdout, r.stdout)

    def run_o0(args):
        cmd = _cxx.cli_args("run", "-O0", EXAMPLE, "--header", header)
        if args:
            cmd += ["--"] + args
        return run(cmd, cwd=TMP)

    scenario("run_o0", run_o0)

    exe = os.path.join(TMP, "sqlite_ffi")
    r = run([cxx, "build", EXAMPLE, "-o", exe, "--header", header], cwd=TMP)
    check(r.returncode == 0, "native build rc=%d out=%s err=%s" % (r.returncode, r.stdout[-400:], r.stderr[-400:]))
    scenario("native", lambda args: run([exe] + args, cwd=TMP))

    cpath = os.path.join(TMP, "sqlite_ffi.c")
    r = run([cxx, "cc", EXAMPLE])
    check(r.returncode == 0, "emit for asan rc=%d err=%s" % (r.returncode, r.stderr[-400:]))
    with open(cpath, "w", encoding="utf-8") as f:
        f.write(r.stdout)
    asan = os.path.join(TMP, "sqlite_ffi.asan")
    cc = shutil.which(os.environ.get("BRAT_CC", "cc"))
    r = run([cc, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
             "-I", ENGINE, cpath, "-o", asan, "-lsqlite3", "-lm", "-lpthread", "-ldl"])
    check(r.returncode == 0, "asan cc rc=%d err=%s" % (r.returncode, r.stderr[-600:]))
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=99")
    r = run([asan], cwd=TMP, env=env)
    check(r.returncode == 0, "asan run rc=%d err=%s" % (r.returncode, r.stderr[-800:]))
    check(r.stdout == DEMO, "asan demo:\n%r\nwant %r" % (r.stdout, DEMO))
    check(r.stderr == "", "asan stderr %r" % r.stderr[-800:])

    print("FFI SQLITE ALL GREEN (%d asserts)" % n)


main()
