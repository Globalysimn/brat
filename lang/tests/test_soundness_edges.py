"""Optimizer soundness, runtime boundary checks, HTTP/package limits, and compiler warning cleanliness."""

import glob
import json
import os
import shutil
import stat
import subprocess

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


def build_and_run_levels(td, name, src, levels=("-O0", "-O1", "-O2"), env=None):
    p = os.path.join(td, name + ".brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)
    results = {}
    cc = shutil.which("clang") or shutil.which("gcc") or "cc"
    for lvl in levels:
        r_emit = run_cmd([CXX, "cc", lvl, "--no-check", p], env=env)
        assert r_emit.returncode == 0, f"emit {lvl} failed: {r_emit.stderr.decode()}"
        c_text = r_emit.stdout.decode("utf-8")
        # Strip banner line if any
        if c_text.startswith("/*"):
            pass
        c_path = os.path.join(td, f"{name}_{lvl[1:]}.c")
        exe_path = os.path.join(td, f"{name}_{lvl[1:]}.exe")
        with open(c_path, "w", encoding="utf-8") as cf:
            cf.write(c_text)
        r_cc = run_cmd([cc, "-O1", "-fwrapv", "-I", ENG, c_path, "-o", exe_path, "-lm", "-pthread", "-ldl"])
        assert r_cc.returncode == 0, f"cc {lvl} failed: {r_cc.stderr.decode()}"
        r_run = run_cmd([exe_path], cwd=td, env=env)
        results[lvl] = (r_run.returncode, r_run.stdout.decode("utf-8", "replace"))
    return results


def test_o2_ssa_origin_and_congruence_soundness():
    td = _scratch.mkdtemp(prefix="brat-o2-")
    # 1. Exact mutated-variable congruence SIGILL regression (`extract_mod_var` SSA origin check)
    src_mut = """fn bug(a: i64) -> i64 {
  let mut x = a
  let r = x % 4
  x = 1
  if r == 0 {
    if x % 4 == 0 {
      return 999
    }
    return 42
  }
  return 7
}

fn main() -> i64 {
  print(bug(8))
  print(bug(5))
  return 0
}
"""
    res = build_and_run_levels(td, "mut_cong", src_mut)
    assert res["-O0"] == (0, "42\n7\n"), res
    assert res["-O1"] == res["-O0"], res
    assert res["-O2"] == res["-O0"], res

    # 2. Extreme signed congruence join (`__int128` + 2^62 cap) + zero-trip `for`
    src_join = """fn big_join(flag: i64) -> i64 {
  let mut x = 0
  if flag > 0 {
    x = 4611686018427387900
  } else {
    x = -4611686018427387900
  }
  let mut k = 9
  for k in 10..3 {
    x = 0
  }
  if x > 0 {
    return (x / 4) + k
  }
  return ((0 - x) / 4) + k
}

fn main() -> i64 {
  print(big_join(1))
  print(big_join(-1))
  return 0
}
"""
    res2 = build_and_run_levels(td, "big_join", src_join)
    assert res2["-O0"] == (0, "1152921504606846984\n1152921504606846984\n"), res2
    assert res2["-O1"] == res2["-O0"], res2
    assert res2["-O2"] == res2["-O0"], res2


def test_fastpath_logical_ops_and_code_size():
    td = _scratch.mkdtemp(prefix="brat-logic-")
    p = os.path.join(td, "use_http.brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write('import std.http\nprint("ok")\n')
    r_cx = run_cmd([CXX, "emit-c", p])
    assert r_cx.returncode == 0
    # Client + server HTTP/1.1 stdlib emits about 300 KB; under the O(2^N) logical blowup it was more
    # than 4.8 MB. The bound sits between the two: it used to be 300,000 against an output of
    # 299,589 bytes, so a 941 byte change in how `&&` is written failed it without a blowup.
    assert len(r_cx.stdout) < 450_000, f"O(2^N) logical blowup regressed: {len(r_cx.stdout)} bytes"

    # Short-circuit and single-evaluation check in fast-path i64 functions
    src_sc = """fn bump(x: i64) -> i64 {
  print(x)
  return x
}

fn check_and(a: i64) -> i64 {
  if bump(a) > 0 && bump(a + 10) > 12 && bump(a + 20) > 20 {
    return 1
  }
  return 0
}

fn check_or(a: i64) -> i64 {
  if bump(a) > 0 || bump(a + 10) > 5 || bump(a + 20) > 0 {
    return 1
  }
  return 0
}

fn main() -> i64 {
  print(check_and(1))
  print(check_and(5))
  print(check_or(3))
  print(check_or(-2))
  return 0
}
"""
    res = build_and_run_levels(td, "sc", src_sc)
    want = (0, "1\n11\n0\n5\n15\n25\n1\n3\n1\n-2\n8\n1\n")
    for k, v in res.items():
        assert v == want, f"{k}: got {v}, want {want}"


def test_runtime_nul_confinement_path_regex_and_sql():
    td = _scratch.mkdtemp(prefix="brat-rt-")
    with open(os.path.join(td, "ok.txt"), "w", encoding="utf-8") as f:
        f.write("safe")

    # 1. Direct C runtime (`crt.h`) NUL-byte confinement proof
    # (`str_from_ptr` / FFI can construct length-counted `Val` strings containing embedded '\0').
    c_nul_probe = os.path.join(td, "nul_probe.c")
    c_nul_exe = os.path.join(td, "nul_probe.exe")
    with open(c_nul_probe, "w", encoding="utf-8") as f:
        f.write(r'''#include "crt.h"
static Val vcall_any(Val fn, Vec *a) { (void)fn; (void)a; return vnil(); }
int main(void) {
    const char raw_traversal[] = "ok.txt\0/../../../etc/passwd";
    Val s_trav = vstrn(raw_traversal, (bint)(sizeof(raw_traversal) - 1));
    Val r1 = vreadfile(s_trav);
    if (r1.tag != V_TAG || strcmp(r1.as.tv->tag, "err") != 0) return 11;
    Val r2 = vwritefile(s_trav, vlit("x"));
    if (r2.tag != V_TAG || strcmp(r2.as.tv->tag, "err") != 0) return 12;
    const char raw_sql[] = "db.sqlite\0evil";
    Val s_sql = vstrn(raw_sql, (bint)(sizeof(raw_sql) - 1));
    Val r3 = vsql_open(s_sql);
    if (r3.tag != V_TAG || strcmp(r3.as.tv->tag, "err") != 0) return 13;
    const char raw_addr[] = "127.0.0.1:443\0evil";
    Val s_addr = vstrn(raw_addr, (bint)(sizeof(raw_addr) - 1));
    Val r4 = vhttps_request_raw(s_addr, vlit("localhost"),
                                vlit("GET / HTTP/1.1\r\n\r\n"), vlit(""));
    if (r4.tag != V_TAG || strcmp(r4.as.tv->tag, "err") != 0) return 14;
    vrc_dec(s_trav); vrc_dec(r1); vrc_dec(r2);
    vrc_dec(s_sql); vrc_dec(r3);
    vrc_dec(s_addr); vrc_dec(r4);
    return 0;
}
''')
    cc = shutil.which("clang") or shutil.which("gcc") or "cc"
    r_cc = run_cmd([cc, "-std=gnu11", "-O1", "-fsanitize=address,undefined",
                    "-I", ENG, c_nul_probe, "-o", c_nul_exe, "-lm", "-pthread", "-ldl"])
    assert r_cc.returncode == 0, r_cc.stderr.decode()
    r_np = run_cmd([c_nul_exe], cwd=td)
    assert r_np.returncode == 0, f"crt.h NUL-byte confinement failed with exit {r_np.returncode}"


    # 2. Plant a malicious executable in a relative directory on PATH (`rel_bin/echo`)
    rel_bin = os.path.join(td, "rel_bin")
    os.makedirs(rel_bin, exist_ok=True)
    evil_echo = os.path.join(rel_bin, "echo")
    with open(evil_echo, "w", encoding="utf-8") as f:
        f.write("#!/bin/sh\necho PWNED_FROM_REL_PATH\n")
    os.chmod(evil_echo, stat.S_IRWXU)

    src = """fn main() -> i64 {
  let db = unwrap(sql_open(":memory:"))
  let _ = unwrap(sql_exec(db, "CREATE TABLE notes(txt TEXT); INSERT INTO notes VALUES ('a;b'); INSERT INTO notes VALUES ('c;d');"))
  let rows = unwrap(sql_query(db, "SELECT txt FROM notes ORDER BY txt", []))
  assert(len(rows) == 2)
  assert(rows[0].get("txt") == "a;b")
  assert(rows[1].get("txt") == "c;d")
  let _ = unwrap(sql_close(db))

  let rep = re_replace("foo=12, bar=34", "([a-z]+)=([0-9]+)", "\\\\2:\\\\1 ($$100)")
  assert(rep == "12:foo ($100), 34:bar ($100)")

  let pr = unwrap(proc_run("echo", ["SAFE_OK"]))
  print(pr.get("out").trim())
  return 0
}
"""
    p = os.path.join(td, "rt_sec.brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)

    poison_env = {
        "BRAT_EXEC": "0",
        "BRAT_EXEC_ALLOW": "echo",
        "PATH": ".:rel_bin:/usr/bin:/bin",
    }
    r_int = run_cmd(_cxx.cli_args("run", "-O0", p), cwd=td, env=poison_env)
    assert r_int.returncode == 0, r_int.stderr.decode()
    assert r_int.stdout.decode().strip() == "SAFE_OK", r_int.stdout.decode()

    exe = os.path.join(td, "rt_sec.exe")
    r_b = run_cmd([CXX, "build", p, "-o", exe])
    assert r_b.returncode == 0, r_b.stderr.decode()
    r_nat = run_cmd([exe], cwd=td, env=poison_env)
    assert r_nat.returncode == 0, r_nat.stderr.decode()
    assert r_nat.stdout.decode().strip() == "SAFE_OK", r_nat.stdout.decode()


def test_http_hardening():
    td = _scratch.mkdtemp(prefix="brat-http-")
    src = """import std.http

fn main() -> i64 {
  assert(is_err(std.http.parse_url("http://user:pass@evil.com:80/x")))
  assert(is_err(std.http.parse_url("http://evil.com:80/path\\r\\nX-Injected: 1")))
  assert(is_err(std.http.parse_url("http://evil.com\\r\\n:80/")))

  let u_v6 = unwrap(std.http.parse_url("http://[::1]:8080/v1/ping"))
  assert(u_v6.get("host") == "[::1]")
  assert(u_v6.get("port") == 8080)
  assert(u_v6.get("path") == "/v1/ping")

  let u_dbl = unwrap(std.http.parse_url("http://example.com:8080//admin?x=1"))
  assert(u_dbl.get("path") == "//admin?x=1")

  assert(is_err(std.http.request_ex("GET", "http://127.0.0.1:9/", {"X-Bad": "a\\r\\nEvil: 1"}, "")))
  assert(is_err(std.http.request_ex("GET", "http://127.0.0.1:9/", {"Host": "spoof.example"}, "")))
  assert(is_err(std.http.request_ex("POST", "http://127.0.0.1:9/", {"Content-Length": "999"}, "abc")))
  assert(is_err(std.http.request_ex("GET\\r\\n", "http://127.0.0.1:9/", {}, "")))
  print("HTTP_HARDENED_OK")
  return 0
}
"""
    p = os.path.join(td, "http_sec.brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write(src)
    r_int = run_cmd(_cxx.cli_args("run", "-O0", p), cwd=td)
    assert r_int.returncode == 0 and r_int.stdout.decode().strip() == "HTTP_HARDENED_OK", r_int.stderr.decode()
    exe = os.path.join(td, "http_sec.exe")
    r_b = run_cmd([CXX, "build", p, "-o", exe])
    assert r_b.returncode == 0, r_b.stderr.decode()
    r_nat = run_cmd([exe], cwd=td)
    assert r_nat.returncode == 0 and r_nat.stdout.decode().strip() == "HTTP_HARDENED_OK", r_nat.stderr.decode()


def test_bratbind_system_header_and_vendor_confinement():
    td = _scratch.mkdtemp(prefix="brat-pkg-")
    vpkg = os.path.join(td, "vendor", "evilpkg")
    os.makedirs(vpkg, exist_ok=True)
    with open(os.path.join(vpkg, "brat.pkg.json"), "w", encoding="utf-8") as f:
        json.dump({"name": "evilpkg", "version": "0.1.0", "c_sources": ["shim.c"]}, f)
    with open(os.path.join(vpkg, "shim.c"), "w", encoding="utf-8") as f:
        f.write("int evil_fn(void) { return 7; }\n")
    with open(os.path.join(vpkg, "mod.brat"), "w", encoding="utf-8") as f:
        f.write("pub fn val() -> i64 { return 7 }\n")
    # Attempt to shadow `<stdio.h>` via `.bratbind` staged header
    with open(os.path.join(vpkg, "mod.bratbind"), "w", encoding="utf-8") as f:
        json.dump({"header": {"name": "stdio.h", "text": "#error SHADOWED_STDIO\n"}}, f)
    app = os.path.join(td, "app.brat")
    with open(app, "w", encoding="utf-8") as f:
        f.write("import evilpkg.mod\nprint(mod.val())\n")

    r_cx = run_cmd([CXX, "build", app, "-o", os.path.join(td, "a1.exe")], cwd=td)
    assert r_cx.returncode != 0 and "stdio.h" in r_cx.stderr.decode(), r_cx.stderr.decode()


def test_lsp_oversized_header_rejection_and_zero_warnings():
    # 1. Deleted dead json.{h,cpp} stays gone
    assert not os.path.exists(os.path.join(ROOT, "lang", "cxx", "src", "json.h"))
    assert not os.path.exists(os.path.join(ROOT, "lang", "cxx", "src", "json.cpp"))

    # 2. LSP rejects oversized header lines without hanging or crashing
    bad_hdr = b"X-Spam: " + (b"A" * 16384) + b"\r\nContent-Length: 2\r\n\r\n{}"
    r_lsp = run_cmd([CXX, "lsp"], input_bytes=bad_hdr, timeout=10)
    assert r_lsp.returncode in (0, 1), r_lsp.returncode

    # 3. `crt.h` compiles cleanly under `-std=c11 -Wall -Wextra -Wunused -Werror`
    td = _scratch.mkdtemp(prefix="brat-warn-")
    probe_c = os.path.join(td, "crt_warn_probe.c")
    with open(probe_c, "w", encoding="utf-8") as f:
        f.write('#include "crt.h"\nstatic Val vcall_any(Val fn, Vec *a) { (void)fn; (void)a; return vnil(); }\nint main(void) { return 0; }\n')
    r_crt = run_cmd(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Wunused", "-Werror",
                     "-fsyntax-only", "-I", ENG, probe_c])
    assert r_crt.returncode == 0, f"crt.h warning regression:\n{r_crt.stderr.decode()}"

    # 3b. Every C++ file in `lang/cxx/src/*.cpp` compiles with 0 warnings under `-Wall -Wextra -Wunused -Werror`
    for cpp in sorted(glob.glob(os.path.join(ROOT, "lang", "cxx", "src", "*.cpp"))):
        r_cpp = run_cmd(["g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Wunused", "-Werror",
                         "-fsyntax-only", cpp])
        assert r_cpp.returncode == 0, f"C++ warning regression in {cpp}:\n{r_cpp.stderr.decode()}"


def test_emitter_crt_ownership_and_locale_regressions():
    td = _scratch.mkdtemp(prefix="brat-own-loc-")
    sub = os.path.join(td, "sub_dir")
    os.makedirs(sub, exist_ok=True)
    fpath = os.path.join(sub, "item.txt")
    with open(fpath, "w", encoding="utf-8") as fh:
        fh.write("ok")

    prog = '''import std.fs

struct Box {
  tag: str,
  n: i64,
}

impl Box {
  fn self_ret(self) -> Box {
    return self
  }
}

fn fail_idx() -> i64 {
  raise("boom_idx")
  return 0
}

fn fail_fn() -> fn(i64, i64) -> i64 {
  raise("boom_fn")
  return fn(a, b) { return a + b }
}

fn make_vec() -> list[str] {
  return ["alpha_long_heap_string_0001", "beta_long_heap_string_0002"]
}

fn make_prefix(log: list[str]) -> str {
  log.push("init")
  return "seed_" + "heap_string_value_long_0001"
}

fn make_folder(log: list[str]) -> fn(str, str) -> str {
  log.push("fn")
  return fn(acc, x) { return acc + ":" + x }
}

fn shadow_catch_unboxed(x: i64) -> str {
  let e = x + 41
  let b = Box { tag: "outer_box_heap_string_12345", n: 7 }
  let mut msg = ""
  try {
    raise("caught_payload_heap_string_98765")
  } catch e {
    msg = e
  }
  try {
    raise("caught_box_shadow_heap_54321")
  } catch b {
    msg = msg + "|" + b
  }
  return "{e}:{b.n}:{msg}"
}

fn main() -> i64 {
  // 1. std.fs.is_dir and std.fs.exists on directory, file, and missing path
  assert(std.fs.is_dir("sub_dir"))
  assert(!std.fs.exists("sub_dir"))
  assert(std.fs.exists("sub_dir/item.txt"))
  assert(!std.fs.exists("sub_dir/missing.txt"))

  // 2. Empty-key map literal and dynamic empty-key lookup
  let em = {"": 99, "k": 12}
  let ek = ""
  assert(em[""] == 99)
  assert(em[ek] == 99)

  // 3. vdrop_recv on temporary str.copy(), self-returning user method, and sort_by
  let sc = ("hello_" + "world_long_heap_str_123456").copy()
  assert(sc == "hello_world_long_heap_str_123456")
  let bx = (Box { tag: "temp_" + "box_long_heap_12345", n: 5 }).self_ret()
  assert(bx.n == 5 && bx.tag == "temp_box_long_heap_12345")
  let sb = [30, 10, 20].sort_by(fn(v) { return v })
  assert(sb == [10, 20, 30])

  // 4. EK::Idx cleanup on raising index and temporary heap map key
  let dm = {"key_long_heap_string_0001": 77}
  assert(dm["key_" + "long_heap_string_0001"] == 77)
  let mut caught_i = false
  try {
    let _v = make_vec()[fail_idx()]
  } catch _e {
    caught_i = true
  }
  assert(caught_i)

  // 5. fold(init, fn) left-to-right evaluation and cleanup if fn raises
  let order: list[str] = []
  let folded = ["a", "b"].fold(make_prefix(order), make_folder(order))
  assert(order == ["init", "fn"])
  assert(folded == "seed_heap_string_value_long_0001:a:b")
  let mut caught_f = false
  try {
    let _bad = [1, 2].fold(10, fail_fn())
  } catch _e {
    caught_f = true
  }
  assert(caught_f)

  // 6. Fused stream shadowing outer struct local + reassigning base list mid-fold
  let x = Box { tag: "outer", n: 3 }
  let mut items = [{"tag": "one_heap_string_12345"}, {"tag": "two_heap_string_12345"}]
  let joined = items.map(fn(x) { return x["tag"] }).fold("", fn(acc, s) {
    items = []
    return acc + s + ";"
  })
  assert(x.n == 3)
  assert(joined == "one_heap_string_12345;two_heap_string_12345;")

  // 7. SK::ForIter retaining collection when loop body reassigns source variable
  let mut xs = ["first_heap_str_12345", "second_heap_str_12345"]
  let mut seen = ""
  for item in xs {
    xs = []
    seen = seen + item + ","
  }
  assert(seen == "first_heap_str_12345,second_heap_str_12345,")

  // 8. SK::Try catch variable shadowing unboxed int and struct locals
  assert(shadow_catch_unboxed(1) == "42:7:caught_payload_heap_string_98765|caught_box_shadow_heap_54321")

  // 9. Float parse and format round-trip
  let fv = 1.5 * 2.25
  assert("{fv}" == "3.375")
  assert("{42}" == "42")
  assert("{1}{2}px" == "12px")
  let parsed_f = "12.5".parse_float()?
  assert(parsed_f == 12.5)
  assert("12,5".parse_float().is_err())
  print("OWN_LOC_OK")
  return 0
}
'''
    src_file = os.path.join(td, "own_loc.brat")
    with open(src_file, "w", encoding="utf-8") as fh:
        fh.write(prog)
    cc = shutil.which("gcc") or shutil.which("clang") or "cc"
    for opt in ("-O0", "-O2"):
        r_em = run_cmd([CXX, "cc", opt, "--no-check", src_file])
        assert r_em.returncode == 0, r_em.stderr.decode()
        c_file = os.path.join(td, f"own_loc_{opt[1:]}.c")
        exe_file = os.path.join(td, f"own_loc_{opt[1:]}")
        with open(c_file, "w", encoding="utf-8") as cf:
            cf.write(r_em.stdout.decode("utf-8"))
        r_cc = run_cmd([
            cc, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-I", ENG, c_file, "-o", exe_file, "-lm", "-pthread", "-ldl",
        ])
        assert r_cc.returncode == 0, f"ASan compile failed ({opt}):\n{r_cc.stderr.decode()}"
        for loc in ("C", "de_DE.UTF-8"):
            r_go = run_cmd([exe_file], cwd=td, env={"LC_ALL": loc, "ASAN_OPTIONS": "detect_leaks=1"})
            assert r_go.returncode == 0 and r_go.stdout == b"OWN_LOC_OK\n", (
                f"failed under {opt} LC_ALL={loc}: rc={r_go.returncode}\n"
                f"out={r_go.stdout!r}\nerr={r_go.stderr.decode()}"
            )

    # 10. FFI ownership under ASan+LSan: export fn returning extern struct with str field
    #     and void C callback whose untyped BRAT closure returns a heap string
    ffi_lib_c = os.path.join(td, "ffi_own_lib.c")
    ffi_lib_so = os.path.join(td, "libffi_own.so")
    with open(ffi_lib_c, "w", encoding="utf-8") as lf:
        lf.write(
            "typedef struct { long long code; const char *msg; } CReply;\n"
            "void run_void_cb(long long n, void (*cb)(long long)) {\n"
            "    for (long long i = 0; i < n; ++i) cb(i);\n"
            "}\n"
        )
    r_so = run_cmd([cc, "-shared", "-fPIC", "-O2", ffi_lib_c, "-o", ffi_lib_so])
    assert r_so.returncode == 0, r_so.stderr.decode()

    ffi_brat = os.path.join(td, "ffi_own.brat")
    with open(ffi_brat, "w", encoding="utf-8") as bf:
        bf.write(
            f'extern struct CReply {{ code: i64, msg: str }}\n'
            f'extern "{ffi_lib_so}" fn run_void_cb(n: i64, cb: fn(i64));\n'
            f'export fn build_reply(x: i64) -> CReply {{\n'
            f'  return CReply {{ code: x + 100, msg: "export_struct_str_payload_long_" + str(x) }}\n'
            f'}}\n'
            f'fn void_sink(i: i64) {{\n'
            f'  return "void_cb_heap_return_payload_long_" + str(i)\n'
            f'}}\n'
            f'fn main() -> i64 {{\n'
            f'  run_void_cb(40, "void_sink")\n'
            f'  return 0\n'
            f'}}\n'
        )
    r_fc = run_cmd([CXX, "emit-c", "--unsafe-ffi", ffi_brat])
    assert r_fc.returncode == 0, r_fc.stderr.decode()
    # Build both as normal executable (exercises void_cb) and shared library (exercises export fn build_reply)
    ffi_c = os.path.join(td, "ffi_own.c")
    ffi_exe = os.path.join(td, "ffi_own_exe")
    with open(ffi_c, "w", encoding="utf-8") as cf:
        cf.write(r_fc.stdout.decode("utf-8"))
    r_fexe = run_cmd([
        cc, "-std=gnu11", "-O1", "-g", "-fsanitize=address", "-fno-omit-frame-pointer",
        "-I", ENG, ffi_c, ffi_lib_so, "-o", ffi_exe, "-lm", "-pthread", "-ldl",
        f"-Wl,-rpath,{td}",
    ])
    assert r_fexe.returncode == 0, r_fexe.stderr.decode()
    r_frun = run_cmd([ffi_exe], cwd=td, env={"ASAN_OPTIONS": "detect_leaks=1"})
    assert r_frun.returncode == 0, r_frun.stderr.decode()

    exp_so = os.path.join(td, "libbrat_exp.so")
    r_bso = run_cmd([CXX, "build", "--shared", "--unsafe-ffi", ffi_brat, "-o", exp_so])
    assert r_bso.returncode == 0, r_bso.stderr.decode()
    drv_c = os.path.join(td, "exp_drv.c")
    drv_exe = os.path.join(td, "exp_drv")
    with open(drv_c, "w", encoding="utf-8") as df:
        df.write(
            "#include <string.h>\n"
            "typedef struct { long long code; const char *msg; } CReply;\n"
            "extern CReply build_reply(long long x);\n"
            "int main(void) {\n"
            "    for (long long i = 0; i < 40; ++i) {\n"
            "        CReply r = build_reply(i);\n"
            "        if (r.code != i + 100 || strncmp(r.msg, \"export_struct_str_payload_long_\", 31) != 0) return 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
    r_dcc = run_cmd([
        cc, "-std=gnu11", "-O1", "-g", "-fsanitize=address", "-fno-omit-frame-pointer",
        drv_c, exp_so, "-o", drv_exe, f"-Wl,-rpath,{td}",
    ])
    assert r_dcc.returncode == 0, r_dcc.stderr.decode()
    r_drun = run_cmd([drv_exe], cwd=td, env={"ASAN_OPTIONS": "detect_leaks=1"})
    assert r_drun.returncode == 0, r_drun.stderr.decode()

    # 11. Standalone BRAT_CRT_TLS header compiles cleanly with -Wall -Wextra -Werror
    tls_only_c = os.path.join(td, "tls_only.c")
    tls_only_o = os.path.join(td, "tls_only.o")
    with open(tls_only_c, "w", encoding="utf-8") as tf:
        tf.write('#define BRAT_CRT_MASK BRAT_CRT_TLS\n#include "crt.h"\nBFN Val vcall_any(Val fn, Vec *a) { (void)fn; (void)a; return vnil(); }\nint main(void) { return 0; }\n')
    r_tls = run_cmd([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", ENG, "-c", tls_only_c, "-o", tls_only_o])
    assert r_tls.returncode == 0, f"standalone BRAT_CRT_TLS failed:\n{r_tls.stderr.decode()}"

    # 12. Multi-arm match (.els chain) must not bypass arena sink-escape check or `brat dist` FFI gate
    arena_m3 = os.path.join(td, "arena_match_arm3.brat")
    with open(arena_m3, "w", encoding="utf-8") as af:
        af.write(
            "let mut g_sink: vec[vec[i64]] = []\n"
            "fn stash(v: vec[i64], k: i64) -> None {\n"
            "  match k {\n"
            "    1 => { return },\n"
            "    2 => { return },\n"
            "    _ => { push(g_sink, v) }\n"
            "  }\n"
            "}\n"
            "fn main() -> i64 {\n"
            "  arena { let tmp = [1, 2, 3]\n stash(tmp, 3) }\n"
            "  return 0\n"
            "}\n"
        )
    r_am3 = run_cmd([CXX, "check", arena_m3])
    assert r_am3.returncode != 0 and b"arena: a value made inside the block" in (r_am3.stdout + r_am3.stderr), (
        r_am3.stdout + r_am3.stderr
    )

    dist_m3 = os.path.join(td, "dist_plugin_arm3.brat")
    with open(dist_m3, "w", encoding="utf-8") as df2:
        df2.write(
            "fn f(k: i64) -> i64 {\n"
            "  match k {\n"
            "    1 => { return 1 },\n"
            "    2 => { return 2 },\n"
            "    _ => { let h = lib_open(\"libc.so.6\")\n lib_close(h)\n return 0 }\n"
            "  }\n"
            "}\n"
            "fn main() -> i64 { return 0 }\n"
        )
    r_dm3 = run_cmd([CXX, "dist", dist_m3, "-o", os.path.join(td, "dist_out")])
    assert r_dm3.returncode != 0 and b"--unsafe-ffi" in (r_dm3.stdout + r_dm3.stderr), (
        r_dm3.stdout + r_dm3.stderr
    )


if __name__ == "__main__":
    test_o2_ssa_origin_and_congruence_soundness()
    test_fastpath_logical_ops_and_code_size()
    test_runtime_nul_confinement_path_regex_and_sql()
    test_http_hardening()
    test_bratbind_system_header_and_vendor_confinement()
    test_lsp_oversized_header_rejection_and_zero_warnings()
    test_emitter_crt_ownership_and_locale_regressions()
    print("SOUNDNESS EDGES ALL GREEN")
