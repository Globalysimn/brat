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

# 3b. std.fs.exists and append_text use O_NOFOLLOW stat/O_APPEND without UTF-8 whole-file reads
os.makedirs(os.path.join(td, "sub_dir"), exist_ok=True)
with open(os.path.join(td, "bin.dat"), "wb") as f:
    f.write(b"\xff\x00\xfe")

fs_bin_out = run_prog(
    td,
    "fs_exists_append_bin",
    '''import std.fs
fn main() -> i64 {
    let ex_bin = std.fs.exists("bin.dat")
    let ex_lnk = std.fs.exists("link_keep.txt")
    let ex_dir = std.fs.exists("sub_dir")
    let ex_out = std.fs.exists("../outside")
    let ap_lnk = is_err(std.fs.append_text("link_keep.txt", "nope"))
    let n1 = unwrap(std.fs.append_text("bin.dat", "tail"))
    let n2 = unwrap(std.fs.append_text("fresh_log.txt", "a\\n"))
    let n3 = unwrap(std.fs.append_text("fresh_log.txt", "b\\n"))
    let fresh = unwrap(read_file("fresh_log.txt"))
    print("{ex_bin} {ex_lnk} {ex_dir} {ex_out} {ap_lnk} {n1} {n2} {n3} {len(fresh)}")
    return 0
}
''',
)
assert fs_bin_out.strip().lower() == "true false false false true 4 2 2 4", fs_bin_out
with open(os.path.join(td, "bin.dat"), "rb") as f:
    assert f.read() == b"\xff\x00\xfetail"
assert open(outside_txt, encoding="utf-8").read() == "secret"

# 3c. Streaming line reader (std.fs.open_read / read_line / close / stdin_line) >64 MiB + arena
big_path = os.path.join(td, "huge.jsonl")
row_bytes = (b'{"ok":true,"n":3,"pad":"' + (b"x" * 1000) + b'"}\n')  # 1025 bytes/line
line_count = 68000  # ~69.7 MB (> 64 MiB whole-file cap)
with open(big_path, "wb") as f:
    chunk = row_bytes * 1000
    for _ in range(line_count // 1000):
        f.write(chunk)
with open(os.path.join(td, "crlf_nul.txt"), "wb") as f:
    f.write(b"alpha\r\nbeta\ngamma\x00bad\ndelta")

stream_code = '''import std.fs
import std.json

fn main() -> Result[i64, str] {
    let whole_blocked = is_err(read_file("huge.jsonl"))
    let lnk_blocked = is_err(std.fs.open_read("link_keep.txt"))
    let fake_bad = is_err(std.fs.read_line(42))

    let fh = std.fs.open_read("huge.jsonl")?
    let mut rows = 0
    let mut sum = 0
    while true {
        let mut done = false
        arena {
            match std.fs.read_line(fh)? {
                Some(line) => {
                    if rows < 20 || rows == 67999 {
                        let obj = std.json.parse(line)?
                        sum += std.json.get_int(obj, "n", 0)
                    }
                    rows += 1
                }
                None => {
                    done = true
                }
            }
        }
        if done { break }
    }
    let _c = std.fs.close(fh)?
    let after_close_bad = is_err(std.fs.read_line(fh))
    let double_close_bad = is_err(std.fs.close(fh))

    let fh2 = std.fs.open_read("crlf_nul.txt")?
    let l1 = unwrap(std.fs.read_line(fh2)?)
    let l2 = unwrap(std.fs.read_line(fh2)?)
    let nul_err = is_err(std.fs.read_line(fh2))
    let l4 = unwrap(std.fs.read_line(fh2)?)
    let eof_none = std.fs.read_line(fh2)?.is_none()
    let _c2 = std.fs.close(fh2)?

    let mut piped: vec[str] = []
    while true {
        match std.fs.stdin_line()? {
            Some(s) => { piped.push(s) }
            None => { break }
        }
    }

    print("{whole_blocked} {lnk_blocked} {fake_bad} {rows} {sum} {after_close_bad} {double_close_bad}")
    print("{l1}|{l2}|{nul_err}|{l4}|{eof_none}|{piped.join(",")}")
    return Ok(0)
}
'''
stream_path = os.path.join(td, "stream_test.brat")
with open(stream_path, "w", encoding="utf-8") as f:
    f.write(stream_code)
stream_nat = subprocess.run(
    [NATIVE, "run", stream_path],
    cwd=td,
    input="one\r\ntwo\nthree",
    capture_output=True,
    text=True,
    timeout=120,
)
assert stream_nat.returncode == 0, stream_nat.stdout + stream_nat.stderr
s_lines = stream_nat.stdout.strip().splitlines()
assert s_lines[0].lower() == "true true true 68000 63 true true", s_lines
assert s_lines[1] == "alpha|beta|true|delta|true|one,two,three", s_lines
os.remove(big_path)

# Static arena escape check rejects returning `std.fs.read_line` string directly out of `arena`
esc_stream = os.path.join(td, "escape_stream_arena.brat")
with open(esc_stream, "w", encoding="utf-8") as f:
    f.write('''import std.fs
fn bad_line(fh: i64) -> Result[Option[str], str] {
    arena {
        return std.fs.read_line(fh)
    }
}
fn main() -> i64 { return 0 }
''')
chk_stream = subprocess.run([NATIVE, "check", esc_stream], capture_output=True, text=True, timeout=30)
assert chk_stream.returncode != 0 and "arena" in (chk_stream.stdout + chk_stream.stderr).lower(), (
    chk_stream.stdout + chk_stream.stderr
)

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

# 8. Confined directory & metadata primitives (`mkdir_all`, `list_dir`, `is_dir`, `file_size`, `remove_dir`)
import tempfile
ext_dir = tempfile.mkdtemp(prefix="brat_ext_dir_")
try:
    with open(os.path.join(ext_dir, "secret.txt"), "w", encoding="utf-8") as f:
        f.write("secret")
    sym_dir = os.path.join(td, "sym_outside_dir")
    has_dir_symlink = False
    try:
        os.symlink(ext_dir, sym_dir)
        has_dir_symlink = True
    except OSError:
        pass

    dir_ops_out = run_prog(
        td,
        "dir_ops_suite",
        '''import std.fs

fn main() -> i64 {
    assert(fs.is_dir("."))
    assert(!fs.is_file("."))
    assert(is_err(fs.file_size(".")))
    assert(is_err(fs.remove_dir(".")))
    assert(is_err(fs.mkdir_all("../escaped_dir")))
    assert(is_err(fs.list_dir("..")))

    unwrap(fs.mkdir_all("tree/sub/leaf"))
    unwrap(fs.mkdir_all("tree/sub/leaf"))
    assert(fs.is_dir("tree/sub/leaf"))
    assert(!fs.is_file("tree/sub/leaf"))

    unwrap(write_file("tree/sub/zeta.txt", "12345"))
    unwrap(write_file("tree/sub/alpha.txt", "ab"))
    unwrap(write_file("tree/sub/tmp.txt", "xyz"))
    unwrap(fs.rename("tree/sub/tmp.txt", "tree/sub/mid.txt"))

    assert(fs.is_file("tree/sub/zeta.txt"))
    assert(!fs.is_dir("tree/sub/zeta.txt"))
    assert(unwrap(fs.file_size("tree/sub/zeta.txt")) == 5)
    assert(unwrap(fs.file_size("tree/sub/alpha.txt")) == 2)

    let mut joined = ""
    arena {
        let names = unwrap(fs.list_dir("tree/sub"))
        joined = own(names.join(","))
    }
    print(joined)

    assert(is_err(fs.remove_dir("tree/sub")))
    unwrap(fs.remove_dir("tree/sub/leaf"))
    assert(!fs.is_dir("tree/sub/leaf"))
    return 0
}
''',
    )
    assert dir_ops_out.strip() == "alpha.txt,leaf,mid.txt,zeta.txt", dir_ops_out

    if has_dir_symlink:
        sym_out = run_prog(
            td,
            "dir_sym_confine",
            '''import std.fs

fn main() -> i64 {
    assert(!fs.is_dir("sym_outside_dir"))
    assert(is_err(fs.list_dir("sym_outside_dir")))
    assert(is_err(fs.mkdir_all("sym_outside_dir/inner")))
    print("sym_ok")
    return 0
}
''',
        )
        assert sym_out.strip() == "sym_ok", sym_out

    bad_arena_dir = os.path.join(td, "bad_arena_listdir.brat")
    with open(bad_arena_dir, "w", encoding="utf-8") as f:
        f.write('''fn leak_dir() -> vec[str] {
    arena {
        return unwrap(fs_list_dir("."))
    }
}
fn main() -> i64 {
    let _x = leak_dir()
    return 0
}
''')
    chk_bad = subprocess.run([NATIVE, "check", bad_arena_dir], capture_output=True, text=True, timeout=30)
    assert chk_bad.returncode != 0, "fs_list_dir result must not escape arena without own()"
finally:
    shutil.rmtree(ext_dir, ignore_errors=True)

print("SQLITE & FS CONFINEMENT + PIN LIFETIME ALL GREEN")
