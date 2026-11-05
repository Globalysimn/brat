"""Full-repo security, optimizer soundness, CLI-door C identity, and warnings audit."""

import ast
import glob
import json
import os
import shutil
import stat
import subprocess
import sys

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


def build_and_run_levels(td, name, src, levels=("-O0", "-O1", "-O2"), with_interp=True, env=None):
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
    if with_interp:
        r_int = run_cmd(_cxx.cli_args("run", "--interp", p), cwd=td, env=env)
        results["--interp"] = (r_int.returncode, r_int.stdout.decode("utf-8", "replace"))
    return results


def test_o2_ssa_origin_and_congruence_soundness():
    td = _scratch.mkdtemp(prefix="audit-o2-")
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
    assert res["--interp"] == res["-O0"], res

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
    assert res2["--interp"] == res2["-O0"], res2


def test_fastpath_logical_ops_and_code_size():
    td = _scratch.mkdtemp(prefix="audit-logic-")
    p = os.path.join(td, "use_http.brat")
    with open(p, "w", encoding="utf-8") as f:
        f.write('import std.http\nprint("ok")\n')
    r_py = run_cmd(_cxx.cli_args("emit-c", p))
    r_cx = run_cmd([CXX, "emit-c", p])
    assert r_py.returncode == 0 and r_cx.returncode == 0
    assert r_py.stdout == r_cx.stdout, "Python CLI emit-c and brat-cxx emit-c diverged on std.http"
    # Client + server HTTP/1.1 stdlib emits about 300 KB; under the O(2^N) logical blowup it was more
    # than 4.8 MB. The bound sits between the two: it used to be 300,000 against an output of
    # 299,589 bytes, so a 941 byte change in how `&&` is written failed it without a blowup.
    assert len(r_py.stdout) < 450_000, f"O(2^N) logical blowup regressed: {len(r_py.stdout)} bytes"

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
    td = _scratch.mkdtemp(prefix="audit-rt-")
    with open(os.path.join(td, "ok.txt"), "w", encoding="utf-8") as f:
        f.write("safe")

    # 1. Direct C runtime (`crt.h`) + Python interpreter (`runtime.py`) NUL-byte confinement proof
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
    r_int = run_cmd(_cxx.cli_args("run", "--interp", p), cwd=td, env=poison_env)
    assert r_int.returncode == 0, r_int.stderr.decode()
    assert r_int.stdout.decode().strip() == "SAFE_OK", r_int.stdout.decode()

    exe = os.path.join(td, "rt_sec.exe")
    r_b = run_cmd([CXX, "build", p, "-o", exe])
    assert r_b.returncode == 0, r_b.stderr.decode()
    r_nat = run_cmd([exe], cwd=td, env=poison_env)
    assert r_nat.returncode == 0, r_nat.stderr.decode()
    assert r_nat.stdout.decode().strip() == "SAFE_OK", r_nat.stdout.decode()


def test_http_hardening():
    td = _scratch.mkdtemp(prefix="audit-http-")
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
    r_int = run_cmd(_cxx.cli_args("run", "--interp", p), cwd=td)
    assert r_int.returncode == 0 and r_int.stdout.decode().strip() == "HTTP_HARDENED_OK", r_int.stderr.decode()
    exe = os.path.join(td, "http_sec.exe")
    r_b = run_cmd([CXX, "build", p, "-o", exe])
    assert r_b.returncode == 0, r_b.stderr.decode()
    r_nat = run_cmd([exe], cwd=td)
    assert r_nat.returncode == 0 and r_nat.stdout.decode().strip() == "HTTP_HARDENED_OK", r_nat.stderr.decode()


def test_bratbind_system_header_and_vendor_confinement():
    td = _scratch.mkdtemp(prefix="audit-pkg-")
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

    r_py = run_cmd(_cxx.cli_args("build", app, "-o", os.path.join(td, "a1.exe")), cwd=td)
    r_cx = run_cmd([CXX, "build", app, "-o", os.path.join(td, "a2.exe")], cwd=td)
    assert r_py.returncode != 0 and "stdio.h" in r_py.stderr.decode(), r_py.stderr.decode()
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
    td = _scratch.mkdtemp(prefix="audit-warn-")
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

    # 4. Every Python file in `lang/engine/` has 0 unused local variables
    unused = []
    for path in sorted(glob.glob(os.path.join(ENG, "**", "*.py"), recursive=True)):
        with open(path, encoding="utf-8") as f:
            src = f.read()
        tree = ast.parse(src, path)
        for node in ast.walk(tree):
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                assigned = {}
                loaded = set()
                nonlocals = set()
                for sub in ast.walk(node):
                    if sub is not node and isinstance(sub, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                        for sub2 in ast.walk(sub):
                            if isinstance(sub2, ast.Name) and isinstance(sub2.ctx, ast.Load):
                                loaded.add(sub2.id)
                        continue
                    if isinstance(sub, (ast.Global, ast.Nonlocal)):
                        nonlocals.update(sub.names)
                    elif isinstance(sub, ast.Name):
                        if isinstance(sub.ctx, ast.Store) and not sub.id.startswith("_"):
                            assigned.setdefault(sub.id, sub.lineno)
                        elif isinstance(sub.ctx, (ast.Load, ast.Del)):
                            loaded.add(sub.id)
                arg_names = {a.arg for a in node.args.posonlyargs + node.args.args + node.args.kwonlyargs}
                if node.args.vararg:
                    arg_names.add(node.args.vararg.arg)
                if node.args.kwarg:
                    arg_names.add(node.args.kwarg.arg)
                for v, ln in assigned.items():
                    if v not in loaded and v not in arg_names and v not in nonlocals:
                        unused.append(f"{os.path.relpath(path, ROOT)}:{ln}:{v}")
    assert not unused, f"Unused Python locals found: {unused}"


if __name__ == "__main__":
    test_o2_ssa_origin_and_congruence_soundness()
    test_fastpath_logical_ops_and_code_size()
    test_runtime_nul_confinement_path_regex_and_sql()
    test_http_hardening()
    test_bratbind_system_header_and_vendor_confinement()
    test_lsp_oversized_header_rejection_and_zero_warnings()
    print("SECURITY & SOUNDNESS AUDIT ALL GREEN")
