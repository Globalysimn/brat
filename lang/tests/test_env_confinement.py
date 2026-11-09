"""Filesystem/SQLite confinement, resource budgets, arena internals, string borrow tracking, and LSP/MCP limits.

Covers:
  - ILP32 Vec/Map checked capacity multiply, 64 MiB read_file cap, 16 MiB read_bytes & utf8_bytes caps.
  - Internalized $arena_enter / $arena_leave builtins.
  - ptr_of_str temporary rejection and live borrow tracking on the owner string.
  - Stack-rooted FFI str args and bounded stable string pins.
  - openat/O_NOFOLLOW filesystem, SQLite, and sidecar confinement.
  - TLS hostname/IP verification and Win64 cross-compile link checks.
  - C compiler & preprocessor timeouts (BRAT_CC_TIMEOUT_MS) and process-group cleanup.
  - Cumulative dependency graph archive/extracted/file/index budgets.
  - MCP 2 MiB streaming output cap and timeout process-group kill.
  - SQLite generation handles, concurrent op/close safety, and sql_query row/column/blob/byte caps.
  - LSP 16 MiB frame/doc cap, 1M JSON node cap, JSON-RPC 2.0 (-32600) envelope check, and 2048 open-doc cap.
  - proc_run timeout_ms / std.proc.run_timeout / BRAT_PROC_TIMEOUT_MS and process-group kill.
  - Aggregate channel slot budget and reclamation on close.
  - Refcounted heap sbuf (>63 builders, ASan leak-free, Unicode codepoint len).
  - expanded env_get credential denylist + BRAT_ENV_DENY_ALL=1 strict default-deny.
  - Dynamic TLS/SQLite backends reject libraries missing required symbols.
"""

import json
import os
import shutil
import socket
import sqlite3
import stat
import subprocess
import sys
import threading
import time

import _cxx
import _scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENG = os.path.abspath(os.path.join(HERE, "..", "engine"))
CXX = _cxx.build()


def run_cmd(argv, cwd=None, env=None, input_bytes=None, timeout=60):
    merged = dict(os.environ)
    if env:
        merged.update(env)
    return subprocess.run(
        argv,
        cwd=cwd,
        env=merged,
        input=input_bytes,
        capture_output=True,
        timeout=timeout,
    )


def run_both_backends(td, name, src, env=None, asan=False):
    p = os.path.join(td, name + ".brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)
    r_int = run_cmd(_cxx.cli_args("run", "-O0", p), cwd=td, env=env)
    cc = shutil.which("gcc") or shutil.which("clang") or "cc"
    r_emit = run_cmd([CXX, "cc", "-O2", p], cwd=td, env=env)
    assert r_emit.returncode == 0, f"emit failed ({name}): {r_emit.stderr.decode()}"
    c_path = os.path.join(td, name + ".c")
    exe_path = os.path.join(td, name + ".exe")
    with open(c_path, "w", encoding="utf-8") as cf:
        cf.write(r_emit.stdout.decode("utf-8"))
    cc_cmd = [cc, "-std=gnu11", "-O1", "-fwrapv", "-I", ENG, c_path, "-o", exe_path, "-lm", "-pthread", "-ldl"]
    if asan:
        cc_cmd[2:2] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    r_cc = run_cmd(cc_cmd, cwd=td)
    assert r_cc.returncode == 0, f"C compile failed ({name}): {r_cc.stderr.decode()}"
    nat_env = dict(env or {})
    if asan:
        nat_env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
        nat_env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"
    r_nat = run_cmd([exe_path], cwd=td, env=nat_env)
    out_i = r_int.stdout.decode("utf-8", "replace")
    out_n = r_nat.stdout.decode("utf-8", "replace")
    assert r_int.returncode == r_nat.returncode, (
        f"{name} rc mismatch: interp={r_int.returncode} ({r_int.stderr.decode()}) "
        f"vs native={r_nat.returncode} ({r_nat.stderr.decode()})"
    )
    assert out_i == out_n, f"{name} stdout mismatch:\ninterp={out_i!r}\nnative={out_n!r}"
    return r_int.returncode, out_i


def check_both_reject(td, name, src, needle):
    p = os.path.join(td, name + ".brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)
    r_py = run_cmd(_cxx.cli_args("check", p), cwd=td)
    r_cx = run_cmd([CXX, "check", p], cwd=td)
    assert r_py.returncode != 0, f"{name}: Python check unexpectedly succeeded"
    assert r_cx.returncode != 0, f"{name}: C++ check unexpectedly succeeded"
    out_py = (r_py.stdout + r_py.stderr).decode("utf-8", "replace")
    out_cx = (r_cx.stdout + r_cx.stderr).decode("utf-8", "replace")
    assert needle in out_py, f"{name}: {needle!r} missing from Python diag: {out_py!r}"
    assert needle in out_cx, f"{name}: {needle!r} missing from C++ diag: {out_cx!r}"


def test_mem02_arena_intrinsics_locked():
    td = _scratch.mkdtemp(prefix="brat-mem02")
    # Direct user calls to __arena_enter / __arena_leave must be rejected as unknown functions
    check_both_reject(
        td,
        "raw_arena_enter",
        "fn bad() -> vec[i64] {\n  __arena_enter()\n  let v = [1, 2, 3]\n  __arena_leave()\n  return v\n}\nprint(len(bad()))\n",
        "__arena_enter",
    )
    # Direct user source with $arena_enter must fail lexing
    p_dollar = os.path.join(td, "dollar_arena.brat")
    with open(p_dollar, "w", encoding="utf-8") as f:
        f.write("$arena_enter()\n")
    assert run_cmd(_cxx.cli_args("check", p_dollar), cwd=td).returncode != 0
    assert run_cmd([CXX, "check", p_dollar], cwd=td).returncode != 0
    # Valid arena block still works and frees cleanly under ASan
    rc, out = run_both_backends(
        td,
        "valid_arena",
        "fn run_arena() -> i64 {\n"
        "  let mut total: i64 = 0\n"
        "  for i in 0..10 {\n"
        "    arena {\n"
        "      let v: vec[i64] = [i, i + 1, i + 2]\n"
        "      total = total + v[0] + v[2]\n"
        "    }\n"
        "  }\n"
        "  return total\n"
        "}\n"
        "print(run_arena())\n",
        asan=True,
    )
    assert rc == 0 and out.strip() == "110"


def test_mem03_mem04_ptr_of_str_borrow_and_pin_ring():
    td = _scratch.mkdtemp(prefix="brat-mem03-04")
    # 1. Temporary argument to ptr_of_str rejected identically by both checkers
    check_both_reject(
        td,
        "ptr_tmp",
        'let p = ptr_of_str("hel" + "lo")\nprint(str_from_ptr(p, 5))\n',
        "`ptr_of_str` wants a string binding or string literal, not a temporary",
    )
    # 2. Reassigning owner string while pointer borrow is still used is rejected
    check_both_reject(
        td,
        "ptr_mut_while_borrowed",
        'let mut s = "hello world — heap string longer than sso limit!"\n'
        'let p = ptr_of_str(s)\n'
        's = "overwritten"\n'
        'print(str_from_ptr(p, 5))\n',
        "borrowed",
    )
    # 3. Valid borrow & >4096 SSO pin iterations run leak-free under ASan and preserve pinned values
    rc, out = run_both_backends(
        td,
        "ptr_ring_stress",
        'let mut ok_cnt = 0\n'
        'for i in 0..6000 {\n'
        '  let s = "k{i}"\n'
        '  let p = ptr_of_str(s)\n'
        '  let back = str_from_ptr(p, len(s))\n'
        '  if back == s { ok_cnt = ok_cnt + 1 }\n'
        '}\n'
        'print(ok_cnt)\n',
        asan=True,
    )
    assert rc == 0 and out.strip() == "6000"


def test_mem01_f07_size_caps_and_ilp32_mul():
    td = _scratch.mkdtemp(prefix="brat-mem01-f07")
    # Sparse 65 MiB file and 17 MiB file take 0 disk blocks on tmpfs/ext4
    big65 = os.path.join(td, "sparse65.txt")
    with open(big65, "wb") as f:
        f.truncate(65 * 1024 * 1024)
    big17 = os.path.join(td, "sparse17.bin")
    with open(big17, "wb") as f:
        f.truncate(17 * 1024 * 1024)

    rc, out = run_both_backends(
        td,
        "file_and_utf8_caps",
        'match read_file("sparse65.txt") {\n'
        '  Ok(_) => { print("unexpected-ok-65") },\n'
        '  Err(e) => { print("rf:{e}") }\n'
        '}\n'
        'match read_bytes("sparse17.bin") {\n'
        '  Ok(_) => { print("unexpected-ok-17") },\n'
        '  Err(e) => { print("rb:{e}") }\n'
        '}\n'
        'let mut s = "abcdefgh"\n'
        'for _ in 0..22 { s = s + s }\n'
        'try {\n'
        '  let b = utf8_bytes(s)\n'
        '  print("unexpected-bytes:{len(b)}")\n'
        '} catch e {\n'
        '  print("ub:{e}")\n'
        '}\n',
    )
    assert rc == 0
    lines = out.strip().splitlines()
    assert lines == [
        "rf:file exceeds 64 MiB limit",
        "rb:file exceeds 16 MiB byte vector limit",
        "ub:utf8_bytes: string exceeds 16 MiB byte vector limit",
    ], f"unexpected cap outputs: {lines}"

    # Verify checked capacity multiply (host 64-bit + ILP32 -m32 when multilib is present)
    gcc = shutil.which("gcc") or shutil.which("clang")
    if gcc:
        c_ovf = os.path.join(td, "mul_overflow.c")
        exe_64 = os.path.join(td, "mul_overflow64")
        with open(c_ovf, "w", encoding="utf-8") as f:
            f.write(
                '#include "crt.h"\n'
                'BFN Val vcall_any(Val fn, Vec *a) { (void)fn; (void)a; return vnil(); }\n'
                'int main(void) {\n'
                '    size_t ok = bmul_size(1024, sizeof(Val));\n'
                '    if (ok != 1024 * sizeof(Val)) return 2;\n'
                '#if SIZE_MAX <= 0xffffffffU\n'
                '    volatile size_t boom = bmul_size(0x10000001LL, sizeof(Val));\n'
                '#else\n'
                '    volatile size_t boom = bmul_size(INT64_MAX / 4, sizeof(Val));\n'
                '#endif\n'
                '    (void)boom;\n'
                '    return 0;\n'
                '}\n'
            )
        r_64 = run_cmd([gcc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-unused-function", "-Werror",
                        "-I", ENG, c_ovf, "-o", exe_64, "-lm", "-lpthread", "-ldl"])
        assert r_64.returncode == 0, f"host bmul_size build failed: {r_64.stderr.decode()}"
        r_exec64 = run_cmd([exe_64])
        assert r_exec64.returncode != 0 and b"out of memory" in r_exec64.stderr

        probe_c = os.path.join(td, "m32_probe.c")
        probe_exe = os.path.join(td, "m32_probe")
        with open(probe_c, "w", encoding="utf-8") as f:
            f.write("#include <stdio.h>\nint main(void) { return 0; }\n")
        if run_cmd([gcc, "-m32", probe_c, "-o", probe_exe]).returncode == 0:
            exe_m32 = os.path.join(td, "mul_overflow32")
            r_m32 = run_cmd([gcc, "-m32", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-unused-function",
                             "-Werror", "-I", ENG, c_ovf, "-o", exe_m32, "-lm", "-lpthread", "-ldl"])
            assert r_m32.returncode == 0, f"gcc -m32 build failed: {r_m32.stderr.decode()}"
            r_exec32 = run_cmd([exe_m32])
            assert r_exec32.returncode != 0 and b"out of memory" in r_exec32.stderr, (
                f"ILP32 bmul_size overflow was not caught: rc={r_exec32.returncode}, stderr={r_exec32.stderr!r}"
            )


def test_f06_sqlite_and_fs_symlink_confinement():
    td = _scratch.mkdtemp(prefix="brat-f06-sql")
    outside = _scratch.mkdtemp(prefix="brat-f06-outside")
    outside_db = os.path.join(outside, "secret.db")
    with open(outside_db, "wb") as f:
        f.write(b"")
    os.symlink(outside_db, os.path.join(td, "escape.db"))
    # Sidecar symlink escape: local.db is inside td, but local.db-wal points outside td
    outside_wal = os.path.join(outside, "hijack.wal")
    with open(outside_wal, "wb") as f:
        f.write(b"")
    os.symlink(outside_wal, os.path.join(td, "sidecar.db-wal"))

    rc, out = run_both_backends(
        td,
        "sql_symlink_escape",
        'import std.sqlite\n'
        'print("esc={is_err(sqlite.open(\\"escape.db\\"))}")\n'
        'print("wal={is_err(sqlite.open(\\"sidecar.db\\"))}")\n'
        'match sqlite.open("safe.db") {\n'
        '  Ok(db) => {\n'
        '    let _ = sqlite.exec(db, "CREATE TABLE t(v INT); INSERT INTO t VALUES (7);")\n'
        '    let closed_err = is_err(sqlite.close(db))\n'
        '    print("closed_err={closed_err}")\n'
        '  },\n'
        '  Err(e) => { print("unexpected:{e}") }\n'
        '}\n',
        asan=True,
    )
    assert rc == 0
    assert [ln.lower() for ln in out.strip().splitlines()] == ["esc=true", "wal=true", "closed_err=false"], f"got: {out!r}"


def test_r301_r302_r309_sqlite_generations_concurrency_and_caps():
    td = _scratch.mkdtemp(prefix="brat-r3-sql")
    # (handle generation: stale handle rejected after slot reuse) + (100,000 row cap)
    rc, out = run_both_backends(
        td,
        "sql_gen_and_cap",
        'import std.sqlite\n'
        'fn main() -> i64 {\n'
        '  let db1 = unwrap(sqlite.open(":memory:"))\n'
        '  let _e1 = unwrap(sqlite.exec(db1, "CREATE TABLE a(x INT); INSERT INTO a VALUES (11);"))\n'
        '  let c1 = !is_err(sqlite.close(db1))\n'
        '  let db2 = unwrap(sqlite.open(":memory:"))\n'
        '  let _e2 = unwrap(sqlite.exec(db2, "CREATE TABLE a(x INT); INSERT INTO a VALUES (22);"))\n'
        '  let diff = db1 != db2\n'
        '  let stale_q = is_err(sqlite.query(db1, "SELECT x FROM a", []))\n'
        '  let stale_c = is_err(sqlite.close(db1))\n'
        '  let rows = unwrap(sqlite.query(db2, "SELECT x FROM a", []))\n'
        '  let live_v = rows[0]["x"]\n'
        '  let cap_err = is_err(sqlite.query(db2, '
        '"WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 105000) SELECT x FROM cnt", []))\n'
        '  let multi_err = is_err(sqlite.query(db2, "SELECT x FROM a; INSERT INTO a VALUES (33)", []))\n'
        '  let after = unwrap(sqlite.query(db2, "SELECT COUNT(*) AS c FROM a", []))\n'
        '  let after_c = after[0]["c"]\n'
        '  let c2 = !is_err(sqlite.close(db2))\n'
        '  print("{c1} {diff} {stale_q} {stale_c} {live_v} {cap_err} {multi_err} {after_c} {c2}")\n'
        '  return 0\n'
        '}\n',
        asan=True,
    )
    assert rc == 0
    assert out.strip().lower() == "true true true true 22 true true 1 true", f"got: {out!r}"

    # Concurrent sql_query + sql_close race test under ThreadSanitizer (TSan) and -O0
    race_src = (
        'import std.sqlite\n'
        'fn main() -> i64 {\n'
        '  for _ in 0..8 {\n'
        '    let db = unwrap(sqlite.open(":memory:"))\n'
        '    let _e = unwrap(sqlite.exec(db, "CREATE TABLE t(v INT); INSERT INTO t VALUES (1),(2),(3);"))\n'
        '    scope {\n'
        '      spawn {\n'
        '        for _ in 0..40 {\n'
        '          let _q = sqlite.query(db, "SELECT v FROM t", [])\n'
        '        }\n'
        '      }\n'
        '      spawn {\n'
        '        let _c = sqlite.close(db)\n'
        '      }\n'
        '    }\n'
        '  }\n'
        '  print("race-clean")\n'
        '  return 0\n'
        '}\n'
    )
    rc_r, out_r = run_both_backends(td, "sql_close_race", race_src, asan=False)
    assert rc_r == 0 and out_r.strip() == "race-clean"

    cc = shutil.which("gcc") or shutil.which("clang")
    if cc:
        c_race = os.path.join(td, "sql_close_race.c")
        exe_tsan = os.path.join(td, "sql_close_race_tsan")
        r_tsan_cc = run_cmd([cc, "-std=gnu11", "-O1", "-fsanitize=thread", "-fPIE", "-pie",
                             "-fwrapv", "-I", ENG, c_race, "-o", exe_tsan, "-lm", "-pthread", "-ldl"])
        if r_tsan_cc.returncode == 0:
            import platform
            tsan_env = {"TSAN_OPTIONS": "halt_on_error=1:second_deadlock_stack=1"}
            sa = shutil.which("setarch") if sys.platform.startswith("linux") else None
            r_tsan = run_cmd([sa, platform.machine(), "-R", exe_tsan] if sa else [exe_tsan],
                             cwd=td, env=tsan_env)
            if sa and r_tsan.returncode != 0 and b"setarch" in r_tsan.stderr:
                r_tsan = run_cmd([exe_tsan], cwd=td, env=tsan_env)
            assert r_tsan.returncode == 0 and b"race-clean" in r_tsan.stdout, (
                f"TSan detected SQL race: rc={r_tsan.returncode}, stderr={r_tsan.stderr.decode()}"
            )

    busy_src = (
        'import std.sqlite\n'
        'fn main() -> i64 {\n'
        '  let db = unwrap(sqlite.open("busy.db"))\n'
        '  print(sqlite.exec(db, "INSERT INTO t VALUES (7)"))\n'
        '  let rows = unwrap(sqlite.query(db, "SELECT x FROM t", []))\n'
        '  print(rows[0]["x"])\n'
        '  let _c = unwrap(sqlite.close(db))\n'
        '  return 0\n'
        '}\n'
    )
    busy_path = os.path.join(td, "sql_busy.brat")
    with open(busy_path, "w", encoding="utf-8") as f:
        f.write(busy_src)
    busy_exe = os.path.join(td, "sql_busy.exe")
    r_busy_build = run_cmd(_cxx.cli_args("build", busy_path, "-o", busy_exe), cwd=td)
    assert r_busy_build.returncode == 0, r_busy_build.stderr.decode()
    busy_db = os.path.join(td, "busy.db")
    setup = sqlite3.connect(busy_db)
    setup.execute("CREATE TABLE t(x INTEGER)")
    setup.commit()
    setup.close()

    def run_locked(argv):
        lock = sqlite3.connect(busy_db, isolation_level=None, check_same_thread=False)
        lock.execute("DELETE FROM t")
        lock.execute("BEGIN EXCLUSIVE")

        def release():
            try:
                lock.execute("ROLLBACK")
            except sqlite3.Error:
                pass
            lock.close()

        timer = threading.Timer(0.5, release)
        started = time.monotonic()
        proc = subprocess.Popen(argv, cwd=td, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        timer.start()
        out, err = proc.communicate(timeout=10)
        elapsed = time.monotonic() - started
        timer.join()
        return proc.returncode, out, err, elapsed

    want_busy = "Ok(1)\n7\n"
    for argv in (_cxx.cli_args("run", "-O0", busy_path), [busy_exe]):
        rc_b, out_b, err_b, elapsed_b = run_locked(argv)
        assert rc_b == 0 and out_b == want_busy and elapsed_b >= 0.4, (
            f"busy lock: rc={rc_b} elapsed={elapsed_b} out={out_b!r} err={err_b!r}"
        )


def test_r305_proc_run_timeout_and_pgid_kill():
    td = _scratch.mkdtemp(prefix="brat-r305")
    t0 = time.monotonic()
    rc, out = run_both_backends(
        td,
        "proc_timeout",
        'import std.proc\n'
        'match proc.run_timeout("/bin/sh", ["-c", "sleep 30"], 150) {\n'
        '  Ok(_) => { print("unexpected-ok") },\n'
        '  Err(e) => { print("err:{e}") }\n'
        '}\n'
        'match proc.run_timeout("/bin/sh", ["-c", "exec 1>&- 2>&-; sleep 30"], 150) {\n'
        '  Ok(_) => { print("unexpected-closed-ok") },\n'
        '  Err(e) => { print("err:{e}") }\n'
        '}\n'
        'match proc.run("/bin/echo", ["hi"]) {\n'
        '  Ok(m) => { print("ok:{m[\\"code\\"]}") },\n'
        '  Err(e) => { print("unexpected:{e}") }\n'
        '}\n',
        env={"BRAT_EXEC": "1"},
        asan=True,
    )
    elapsed = time.monotonic() - t0
    assert rc == 0
    assert out.strip().splitlines() == ["err:proc_run timed out", "err:proc_run timed out", "ok:0"], f"got: {out!r}"
    assert elapsed < 10.0, f"proc_run timeout did not kill child promptly ({elapsed:.2f}s)"


def test_r308_r310_r311_chan_budget_sbuf_and_env():
    td = _scratch.mkdtemp(prefix="brat-r3-misc")
    # aggregate channel budget enforced (>4,194,304 refused) and reclaimed on chan_close
    # >63 concurrent & sequential sbuf instances + Unicode codepoint len parity
    # expanded env_get denylist + BRAT_ENV_DENY_ALL=1
    rc, out = run_both_backends(
        td,
        "chan_sbuf_env",
        'fn main() -> i64 {\n'
        '  // 1. Channel budget enforcement + reclamation\n'
        '  let c1 = chan(1000000)\n'
        '  let c2 = chan(1000000)\n'
        '  let c3 = chan(1000000)\n'
        '  let c4 = chan(1000000)\n'
        '  let over = is_err(chan(500000))\n'
        '  c1.close()\n'
        '  c2.close()\n'
        '  c3.close()\n'
        '  c4.close()\n'
        '  let c5 = chan(500000)\n'
        '  let reclaimed = !is_err(c5)\n'
        '  if reclaimed { c5.close() }\n'
        '  print("chan:{over} {reclaimed}")\n'
        '  // 2. >63 live sbuf builders + Unicode codepoint length parity\n'
        '  let mut builders = []\n'
        '  for i in 0..120 {\n'
        '    let b = sbuf()\n'
        '    sbuf_add(b, "x{i}")\n'
        '    builders.push(b)\n'
        '  }\n'
        '  let ub = sbuf()\n'
        '  let n1 = sbuf_add(ub, "ab")\n'
        '  let n2 = sbuf_len(ub)\n'
        '  let last_s = unwrap(sbuf_str(builders[119]))\n'
        '  print("sbuf:{len(builders)} {last_s} {n1} {n2}")\n'
        '  // 3. Expanded env_get denylist\n'
        '  let gh = is_err(env_get("GITHUB_TOKEN"))\n'
        '  let db = is_err(env_get("DATABASE_URL"))\n'
        '  let pub_ok = !is_err(env_get("MY_PUBLIC_VAR"))\n'
        '  print("gh={gh} db={db} pub={pub_ok}")\n'
        '  return 0\n'
        '}\n',
        env={
            "GITHUB_TOKEN": "ghp_secret",
            "DATABASE_URL": "postgres://app:pw@localhost/db",
            "MY_PUBLIC_VAR": "hello",
        },
        asan=True,
    )
    assert rc == 0
    assert [ln.lower() for ln in out.strip().splitlines()] == [
        "chan:true true",
        "sbuf:120 x119 2 2",
        "gh=true db=true pub=true",
    ], f"got: {out!r}"

    # Verify BRAT_ENV_DENY_ALL=1 blocks even non-sensitive vars
    rc2, out2 = run_both_backends(
        td,
        "env_deny_all",
        'let d = is_err(env_get("MY_PUBLIC_VAR"))\nprint("pub_denied={d}")\n',
        env={"MY_PUBLIC_VAR": "hello", "BRAT_ENV_DENY_ALL": "1"},
    )
    assert rc2 == 0 and out2.strip().lower() == "pub_denied=true"


def _lsp_frame(obj):
    raw = json.dumps(obj).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n" % len(raw) + raw


def test_r304_lsp_caps_and_jsonrpc_envelope():
    # 1. Invalid JSON-RPC envelope (jsonrpc != "2.0") returns -32600 Invalid Request
    bad_req = _lsp_frame({"jsonrpc": "1.0", "id": 1, "method": "initialize", "params": {}})
    shut = (
        _lsp_frame({"jsonrpc": "2.0", "id": 2, "method": "initialize", "params": {"capabilities": {}}})
        + _lsp_frame({"jsonrpc": "2.0", "id": 3, "method": "shutdown"})
        + _lsp_frame({"jsonrpc": "2.0", "method": "exit"})
    )
    r = run_cmd([CXX, "lsp"], input_bytes=bad_req + shut, timeout=15)
    assert r.returncode == 0
    assert b"-32600" in r.stdout, f"expected -32600 Invalid Request in LSP stdout: {r.stdout[:400]!r}"

    # 2. >16 MiB Content-Length header cleanly closes/rejects without allocating >16 MiB
    over_hdr = b"Content-Length: 17000000\r\n\r\n{}"
    r2 = run_cmd([CXX, "lsp"], input_bytes=over_hdr, timeout=10)
    assert r2.returncode != 0 or b"-32600" in r2.stdout or r2.stdout == b""

    # 3. JSON token bomb (>1,048,576 nodes inside <3 MiB frame) rejected cleanly without OOM
    bomb_body = b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"x":[' + b"0," * 1050000 + b"0]}}"
    bomb_frame = (b"Content-Length: %d\r\n\r\n" % len(bomb_body)) + bomb_body
    r3 = run_cmd([CXX, "lsp"], input_bytes=bomb_frame, timeout=15)
    assert b"-32700" in r3.stdout or r3.returncode != 0

    # 4. Incremental didChange growing document past 16 MiB is rejected cleanly
    big_chunk = "a" * (9 * 1024 * 1024)
    inc_stream = (
        _lsp_frame({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
        + _lsp_frame({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {"textDocument": {"uri": "file:///tmp/inc16.brat", "languageId": "brat", "version": 1, "text": big_chunk}},
        })
        + _lsp_frame({
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": "file:///tmp/inc16.brat", "version": 2},
                "contentChanges": [{
                    "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}},
                    "text": big_chunk,
                }],
            },
        })
        + _lsp_frame({"jsonrpc": "2.0", "id": 2, "method": "shutdown"})
        + _lsp_frame({"jsonrpc": "2.0", "method": "exit"})
    )
    r4 = run_cmd([CXX, "lsp"], input_bytes=inc_stream, timeout=15)
    assert r4.returncode == 0
    assert b"document exceeds 16 MiB limit" in r4.stdout, f"missing 16 MiB log error: {r4.stdout[:400]!r}"


def test_r306_r307_f03_f04_mcp_cc_timeout_and_pkg_budgets():
    td = _scratch.mkdtemp(prefix="brat-subproc-pkg")
    # 1. Native `brat-cxx mcp` caps oversized tool stdout (>2 MiB) cleanly
    big_brat = os.path.join(td, "big_out.brat")
    with open(big_brat, "w", encoding="utf-8") as f:
        f.write('fn main() -> i64 {\n  print("A".repeat(3 * 1024 * 1024))\n  return 0\n}\n')
    mcp_reqs = (
        '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}\n'
        + json.dumps({
            "jsonrpc": "2.0", "id": 2, "method": "tools/call",
            "params": {"name": "brat_run", "arguments": {"path": big_brat}},
        }) + "\n"
    )
    r_mcp = subprocess.run([CXX, "mcp"], input=mcp_reqs.encode("utf-8"),
                           capture_output=True, timeout=30)
    assert r_mcp.returncode == 0 and b"output exceeds limit" in r_mcp.stdout and b'"isError":true' in r_mcp.stdout, (
        f"native mcp did not enforce 2 MiB output cap: {r_mcp.stdout[:300]!r} {r_mcp.stderr[:300]!r}"
    )

    # 2. Hang C compiler killed by BRAT_CC_TIMEOUT_MS across brat-cxx ffi & build
    fake_cc = os.path.join(td, "hang_cc.sh")
    fake_cc_calls = os.path.join(td, "hang_cc.calls")
    with open(fake_cc, "w", encoding="utf-8") as f:
        f.write("#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" + fake_cc_calls + "'\nsleep 30\n")
    os.chmod(fake_cc, stat.S_IRWXU)
    hdr = os.path.join(td, "mini.h")
    with open(hdr, "w", encoding="utf-8") as f:
        f.write("int add1(int x);\n")
    prog = os.path.join(td, "use_mini.brat")
    with open(prog, "w", encoding="utf-8") as f:
        f.write('extern "C" fn add1(x: i32) -> i32\nprint(1)\n')

    t_hdr = time.monotonic()
    r_hdr_tmo = run_cmd(
        [CXX, "bindgen", "--preprocess", hdr],
        cwd=td,
        env={"BRAT_CC": fake_cc, "BRAT_CC_TIMEOUT_MS": "150"},
        timeout=10,
    )
    assert r_hdr_tmo.returncode != 0 and (time.monotonic() - t_hdr) < 5.0
    assert b"timed out" in r_hdr_tmo.stderr, f"missing preprocessor timeout: {r_hdr_tmo.stderr!r}"


    if os.path.exists(fake_cc_calls):
        os.remove(fake_cc_calls)
    t1 = time.monotonic()
    r_cxx_tmo = run_cmd(
        [CXX, "build", "--unsafe-ffi", prog, "-o", os.path.join(td, "out.bin")],
        cwd=td,
        env={"BRAT_CC": fake_cc, "BRAT_CC_TIMEOUT_MS": "150"},
        timeout=10,
    )
    assert r_cxx_tmo.returncode != 0 and (time.monotonic() - t1) < 5.0, (
        f"hanging C compiler was not stopped promptly: rc={r_cxx_tmo.returncode}, "
        f"stderr={r_cxx_tmo.stderr!r}"
    )
    assert b"C compiler timed out after 150 ms" in r_cxx_tmo.stderr, (
        f"missing compiler timeout diagnostic: {r_cxx_tmo.stderr!r}"
    )
    with open(fake_cc_calls, encoding="utf-8") as f:
        calls = f.read()
    assert "program.c" in calls and "-o" in calls, f"fake compiler was not invoked as the build compiler: {calls!r}"

    # 3. Dependency graph >64 resolved packages rejected by brat-cxx pkg
    idx_path = os.path.join(td, "huge_index.json")
    pkgs = {
        f"acme/pkg{i}": {"0.1.0": {"url": f"file://{td}/none.tar.gz", "sha256": "0" * 64}}
        for i in range(1, 70)
    }
    pkgs["acme/pkg0"] = {
        "0.1.0": {
            "url": f"file://{td}/none.tar.gz",
            "sha256": "0" * 64,
            "dependencies": {f"acme/pkg{i}": "0.1.0" for i in range(1, 70)},
        }
    }
    with open(idx_path, "w", encoding="utf-8") as f:
        json.dump({"packages": pkgs}, f)
    proj = os.path.join(td, "pkg_proj")
    os.makedirs(os.path.join(proj, "src"), exist_ok=True)
    with open(os.path.join(proj, "src", "main.brat"), "w", encoding="utf-8") as f:
        f.write("print(1)\n")
    r_pkg_cxx = run_cmd([CXX, "pkg", "add", "acme/pkg0@0.1.0"], cwd=proj, env={"BRAT_INDEX": idx_path})
    r_pkg_py = run_cmd(_cxx.cli_args("pkg", "add", "acme/pkg0@0.1.0"), cwd=proj,
                       env={"BRAT_INDEX": idx_path, "BRAT_NO_NATIVE": "1"})
    assert r_pkg_cxx.returncode != 0 and b"exceeds" in r_pkg_cxx.stderr, f"cxx pkg stderr: {r_pkg_cxx.stderr!r}"
    assert r_pkg_py.returncode != 0 and b"exceeds" in r_pkg_py.stderr, f"py pkg stderr: {r_pkg_py.stderr!r}"

    # 4. Verify MinGW x86_64 Windows cross-compile of crt.h and brat-cxx succeeds with -Werror when available
    mingw_gxx = shutil.which("x86_64-w64-mingw32-g++")
    mingw_gcc = shutil.which("x86_64-w64-mingw32-gcc")
    if mingw_gcc and mingw_gxx:
        crt_probe = os.path.join(td, "win_crt_probe.c")
        with open(crt_probe, "w", encoding="utf-8") as f:
            f.write(
                '#define BRAT_SQL 1\n#define BRAT_NET 1\n#define BRAT_CHAN 1\n#define BRAT_PROC 1\n'
                '#include "crt.h"\n'
                'BFN Val vcall_any(Val fn, Vec *a) { (void)fn; (void)a; return vnil(); }\n'
                'int main(void) { return 0; }\n'
            )
        rc_mc = run_cmd([mingw_gcc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-unused-function",
                         "-Werror", "-I", ENG, "-fsyntax-only", crt_probe])
        assert rc_mc.returncode == 0, f"MinGW crt.h failed: {rc_mc.stderr.decode()}"


def test_dynamic_backend_missing_symbols_fail_closed():
    if not sys.platform.startswith("linux"):
        return
    cc = shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")
    assert cc, "C compiler required for dynamic backend symbol test"
    td = _scratch.mkdtemp(prefix="brat-dlsym-")

    def write(name, content):
        path = os.path.join(td, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
        return path

    def build_shared(source_name, source, output_name):
        src = write(source_name, source)
        out = os.path.join(td, output_name)
        result = run_cmd([cc, "-shared", "-fPIC", src, "-o", out], cwd=td)
        assert result.returncode == 0, result.stderr.decode()
        return out

    def build_program(source_name, source, output_name):
        src = write(source_name, source)
        out = os.path.join(td, output_name)
        result = run_cmd([CXX, "build", src, "-o", out], cwd=td)
        assert result.returncode == 0, result.stderr.decode()
        return out

    build_shared(
        "fake_sqlite.c",
        """
int sqlite3_open(const char *path, void **db) { (void)path; *db = (void *)1; return 0; }
int sqlite3_close_v2(void *db) { (void)db; return 0; }
int sqlite3_prepare_v2(void *db, const char *sql, int n, void **stmt, const char **tail) {
    (void)db; (void)sql; (void)n; (void)tail; *stmt = (void *)2; return 0;
}
int sqlite3_step(void *stmt) { (void)stmt; return 101; }
int sqlite3_finalize(void *stmt) { (void)stmt; return 0; }
int sqlite3_exec(void *db, const char *sql, void *cb, void *arg, char **err) {
    (void)db; (void)sql; (void)cb; (void)arg; if (err) *err = 0; return 0;
}
const char *sqlite3_errmsg(void *db) { (void)db; return "fake sqlite"; }
""",
        "libsqlite3.so.0",
    )
    sqlite_exe = build_program(
        "sqlite_probe.brat",
        """fn main() -> i64 {
  let opened = sql_open(":memory:")
  if is_err(opened) {
    print("SQLITE_UNAVAILABLE")
    return 0
  }
  let db = unwrap(opened)
  let _rows = unwrap(sql_query(db, "SELECT ?", [42]))
  print("SQLITE_ACCEPTED")
  return 0
}
""",
        "sqlite_probe.exe",
    )
    r_sqlite = run_cmd([sqlite_exe], cwd=td, env={"LD_LIBRARY_PATH": td})
    assert r_sqlite.returncode == 0, r_sqlite.stderr.decode()
    assert r_sqlite.stdout.decode().strip() == "SQLITE_UNAVAILABLE", r_sqlite.stdout.decode()

    build_shared(
        "fake_ssl.c",
        """
#include <stdint.h>
const void *TLS_client_method(void) { return (const void *)1; }
void *SSL_CTX_new(const void *method) { (void)method; return (void *)2; }
void SSL_CTX_free(void *ctx) { (void)ctx; }
int SSL_CTX_set_default_verify_paths(void *ctx) { (void)ctx; return 1; }
void SSL_CTX_set_verify(void *ctx, int mode, void *cb) { (void)ctx; (void)mode; (void)cb; }
void *SSL_new(void *ctx) { (void)ctx; return (void *)3; }
void SSL_free(void *ssl) { (void)ssl; }
int SSL_set_fd(void *ssl, int fd) { (void)ssl; (void)fd; return 1; }
intptr_t SSL_ctrl(void *ssl, int cmd, intptr_t larg, void *parg) {
    (void)ssl; (void)cmd; (void)larg; (void)parg; return 1;
}
int SSL_set1_host(void *ssl, const char *name) { (void)ssl; (void)name; return 1; }
void *SSL_get0_param(void *ssl) { (void)ssl; return (void *)4; }
int SSL_connect(void *ssl) { (void)ssl; return 1; }
intptr_t SSL_get_verify_result(const void *ssl) { (void)ssl; return 0; }
int SSL_write(void *ssl, const void *buf, int num) { (void)ssl; (void)buf; return num; }
int SSL_read(void *ssl, void *buf, int num) { (void)ssl; (void)buf; (void)num; return 0; }
int SSL_shutdown(void *ssl) { (void)ssl; return 1; }
""",
        "libssl.so.3",
    )
    build_shared(
        "fake_crypto.c",
        'int X509_VERIFY_PARAM_set1_ip_asc(void *param, const char *ip) { (void)param; (void)ip; return 1; }\n',
        "libcrypto.so.3",
    )
    write("ca.pem", "not a real CA\n")

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(2)
    tls_port = listener.getsockname()[1]

    def accept_once():
        try:
            conn, _addr = listener.accept()
            conn.close()
        except OSError:
            pass

    server = threading.Thread(target=accept_once, daemon=True)
    server.start()
    try:
        tls_exe = build_program(
            "tls_probe.brat",
            f"""fn main() -> i64 {{
  let r = https_request_raw("127.0.0.1:{tls_port}", "localhost", "GET / HTTP/1.0\\r\\n\\r\\n", "ca.pem")
  assert(is_err(r))
  print("TLS_UNAVAILABLE")
  return 0
}}
""",
            "tls_probe.exe",
        )
        r_tls = run_cmd([tls_exe], cwd=td, env={"LD_LIBRARY_PATH": td})
    finally:
        listener.close()
        server.join(timeout=1)
    assert r_tls.returncode == 0, r_tls.stderr.decode()
    assert r_tls.stdout.decode().strip() == "TLS_UNAVAILABLE", r_tls.stdout.decode()


def main():
    test_mem02_arena_intrinsics_locked()
    print("ok: ($arena_enter/$arena_leave lockdown + ASan)")
    test_mem03_mem04_ptr_of_str_borrow_and_pin_ring()
    print("ok: & (ptr_of_str borrow tracking + bounded stable SSO pins)")
    test_mem01_f07_size_caps_and_ilp32_mul()
    print("ok: & (64 MiB/16 MiB caps + ILP32 -m32 checked multiply)")
    test_f06_sqlite_and_fs_symlink_confinement()
    print("ok: (openat/O_NOFOLLOW + SQLite & sidecar confinement)")
    test_r301_r302_r309_sqlite_generations_concurrency_and_caps()
    print("ok: (SQLite generations + TSan close race + 100k row cap)")
    test_r305_proc_run_timeout_and_pgid_kill()
    print("ok: (proc_run timeout_ms + process-group tree kill)")
    test_r308_r310_r311_chan_budget_sbuf_and_env()
    print("ok: (channel budget + refcounted sbuf + env_get policy)")
    test_r304_lsp_caps_and_jsonrpc_envelope()
    print("ok: (LSP 16 MiB cap + 1M JSON token cap + -32600 JSON-RPC envelope)")
    test_r306_r307_f03_f04_mcp_cc_timeout_and_pkg_budgets()
    print("ok: (MCP 2 MiB cap + CC timeout + pkg graph cap + MinGW)")
    test_dynamic_backend_missing_symbols_fail_closed()
    print("ok: dynamic TLS/SQLite libraries with missing symbols fail closed")
    print("CONFINEMENT & RESOURCE SUITE ALL GREEN")


if __name__ == "__main__":
    main()
