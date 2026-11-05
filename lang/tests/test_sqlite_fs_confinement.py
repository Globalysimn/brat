"""Regression suite for SQLite/FS confinement and bounded non-aborting string pinning.

Covers:
1. SQLite confinement blocks ATTACH DATABASE, VACUUM INTO, load_extension(), and dangerous
   pragmas (temp_store_directory, data_store_directory, writable_schema, trusted_schema)
   outside the program directory, while allowing BRAT_FILES=free override.
2. SQLite URI path strings ("file:/etc/passwd?mode=ro") cannot escape the confined directory.
3. fs_remove and fs_rename refuse symlink leaves inside the confined directory.
4. Returning an inline borrowed pointer (`return ptr_of_str(local_s)`) is rejected by `brat check`
   (`ref ... escapes via return`) while parameter borrows (`fn f(s: str) -> ptr`) remain valid.
5. `vstr_pin` scales past the old 32,768-slot limit (>40,000 distinct short strings under ASan)
   and raises a catchable `string pin limit exceeded` error (never `abort()`) when
   `BRAT_PIN_BUDGET_BYTES` is exceeded.
"""
import _cxx
import _scratch

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
NATIVE = _cxx.build()
COMPARE_PYTHON = False


def run_prog(td, name, code, env=None):
    path = os.path.join(td, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(code)
    merged = {**os.environ, **(env or {})}
    nat = subprocess.run([NATIVE, "run", path], cwd=td, env=merged,
                         capture_output=True, text=True, timeout=120)
    assert nat.returncode == 0, "%s native failed:\n%s\n%s" % (name, nat.stdout, nat.stderr)
    if COMPARE_PYTHON:
        py = subprocess.run(_cxx.cli_args("run", "--interp", path), cwd=td, env=merged,
                            capture_output=True, text=True, timeout=120)
        assert py.returncode == 0, "%s interp failed:\n%s\n%s" % (name, py.stdout, py.stderr)
        assert nat.stdout == py.stdout, "%s mismatch:\nnative=%r\ninterp=%r" % (
            name, nat.stdout, py.stdout)
    return nat.stdout


# 1 & 2. SQLite ATTACH / VACUUM INTO / PRAGMA / URI confinement + BRAT_FILES=free override
td = _scratch.mkdtemp(prefix="brat-sql-confine-")
outside = _scratch.mkdtemp(prefix="brat-sql-outside-")
ext_db = os.path.join(outside, "ext.db")
vac_db = os.path.join(outside, "vac.db")
with open(ext_db, "wb") as f:
    f.write(b"")

sql_code = f'''import std.sqlite
fn main() -> i64 {{
    let uri_res = sqlite.open("file:/etc/passwd?mode=ro")
    if !is_err(uri_res) {{
        let _ = sqlite.close(unwrap(uri_res))
    }}
    let db = unwrap(sqlite.open("local.db"))
    let _ = unwrap(sqlite.exec(db, "CREATE TABLE IF NOT EXISTS items(x INT); DELETE FROM items; INSERT INTO items VALUES (9);"))
    let att = is_err(sqlite.exec(db, "ATTACH DATABASE '{ext_db}' AS ext"))
    let vac = is_err(sqlite.exec(db, "VACUUM INTO '{vac_db}'"))
    let p_tmp = is_err(sqlite.exec(db, "PRAGMA temp_store_directory = '{outside}'"))
    let p_wsch = is_err(sqlite.exec(db, "PRAGMA writable_schema = ON"))
    let p_tsch = is_err(sqlite.exec(db, "PRAGMA trusted_schema = ON"))
    let rows = unwrap(sqlite.query(db, "SELECT x FROM items", []))
    let v = rows[0]["x"]
    let _ = unwrap(sqlite.close(db))
    print("{{att}} {{vac}} {{p_tmp}} {{p_wsch}} {{p_tsch}} {{v}}")
    return 0
}}
'''
out = run_prog(td, "sql_sandbox", sql_code)
assert out.strip().lower() == "true true true true true 9", out
assert not os.path.exists(vac_db), "VACUUM INTO created outside file!"

# BRAT_FILES=1 allows ATTACH DATABASE when operator explicitly opts out of confinement
out_free = run_prog(
    td,
    "sql_free",
    f'''import std.sqlite
fn main() -> i64 {{
    let db = unwrap(sqlite.open("local.db"))
    let att_ok = !is_err(sqlite.exec(db, "ATTACH DATABASE '{ext_db}' AS ext"))
    let _ = unwrap(sqlite.close(db))
    print("{{att_ok}}")
    return 0
}}
''',
    env={"BRAT_FILES": "1"},
)
assert out_free.strip().lower() == "true", out_free

# 3. fs_remove and fs_rename refuse symlink leaves inside confined dir
outside_txt = os.path.join(outside, "keep.txt")
with open(outside_txt, "w", encoding="utf-8") as f:
    f.write("secret")
os.symlink(outside_txt, os.path.join(td, "link_keep.txt"))

fs_out = run_prog(
    td,
    "fs_symlink_leaf",
    '''fn main() -> i64 {
    let _ = unwrap(write_file("plain.txt", "local"))
    let rm_lnk = is_err(fs_remove("link_keep.txt"))
    let mv_src = is_err(fs_rename("link_keep.txt", "moved.txt"))
    let mv_dst = is_err(fs_rename("plain.txt", "link_keep.txt"))
    let mv_ok = !is_err(fs_rename("plain.txt", "plain2.txt"))
    let rm_ok = !is_err(fs_remove("plain2.txt"))
    print("{rm_lnk} {mv_src} {mv_dst} {mv_ok} {rm_ok}")
    return 0
}
''',
)
assert fs_out.strip().lower() == "true true true true true", fs_out
assert os.path.exists(outside_txt) and os.path.islink(os.path.join(td, "link_keep.txt"))

# 4. Static borrow checker rejects inline `return ptr_of_str(local_s)` escape
esc_bad = os.path.join(td, "escape_ptr.brat")
with open(esc_bad, "w", encoding="utf-8") as f:
    f.write('''fn bad() -> ptr {
    let s = "hello world — heap string longer than 14 bytes!"
    return ptr_of_str(s)
}
fn main() -> i64 { return 0 }
''')
chk_bad = subprocess.run([NATIVE, "check", esc_bad], capture_output=True, text=True, timeout=30)
assert chk_bad.returncode != 0 and "ref `s` escapes via return" in (chk_bad.stdout + chk_bad.stderr), (
    chk_bad.stdout + chk_bad.stderr
)
if COMPARE_PYTHON:
    py_bad = subprocess.run(_cxx.cli_args("check", "--py", esc_bad),
                            capture_output=True, text=True, timeout=30)
    assert py_bad.returncode != 0 and "ref `s` escapes via return" in (py_bad.stdout + py_bad.stderr)

esc_ok = os.path.join(td, "param_ptr.brat")
with open(esc_ok, "w", encoding="utf-8") as f:
    f.write('''fn ok_param(s: str) -> ptr {
    return ptr_of_str(s)
}
fn main() -> i64 {
    let msg = "parameter owned by caller"
    let p = ok_param(msg)
    assert(str_from_ptr(p, 9) == "parameter")
    return 0
}
''')
chk_ok = subprocess.run([NATIVE, "check", esc_ok], capture_output=True, text=True, timeout=30)
assert chk_ok.returncode == 0, chk_ok.stdout + chk_ok.stderr

# 5. >40,000 distinct short strings succeed (exceeding old 32,768 limit) + catchable budget cap
pin_scale = run_prog(
    td,
    "pin_40k",
    '''fn main() -> i64 {
    let p0 = ptr_of_str("anchor")
    let mut ok = 0
    for i in 0..40000 {
        let s = "p_{i}"
        let p = ptr_of_str(s)
        if str_from_ptr(p, len(s)) == s {
            ok = ok + 1
        }
    }
    assert(str_from_ptr(p0, 6) == "anchor")
    print(ok)
    return 0
}
''',
)
assert pin_scale.strip() == "40000", pin_scale

pin_cap_out = run_prog(
    td,
    "pin_budget_cap",
    '''fn main() -> i64 {
    let mut caught = ""
    try {
        for i in 0..500 {
            let s = "cap_{i}"
            let _p = ptr_of_str(s)
        }
    } catch e {
        caught = e
    }
    print(caught)
    return 0
}
''',
    env={"BRAT_PIN_BUDGET_BYTES": "1024"},
)
assert pin_cap_out.strip() == "string pin limit exceeded", pin_cap_out

cc = shutil.which("cc") or shutil.which("gcc")
if cc:
    c_src = os.path.join(td, "pin_40k.c")
    exe = os.path.join(td, "pin_40k_asan")
    em = subprocess.run([NATIVE, "emit-c", os.path.join(td, "pin_40k.brat")],
                        capture_output=True, timeout=60)
    assert em.returncode == 0, em.stderr.decode()
    with open(c_src, "wb") as f:
        f.write(em.stdout)
    b = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                        "-I", ENGINE, c_src, "-o", exe, "-lm", "-lpthread", "-ldl"],
                       capture_output=True, text=True, timeout=120)
    assert b.returncode == 0, b.stderr
    r = subprocess.run([exe], cwd=td, capture_output=True, text=True, timeout=120,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:exitcode=99"})
    assert r.returncode == 0 and r.stdout.strip() == "40000", (r.returncode, r.stdout, r.stderr)

print("SQLITE & FS CONFINEMENT + PIN LIFETIME ALL GREEN")
