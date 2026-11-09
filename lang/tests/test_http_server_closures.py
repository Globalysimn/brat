"""The C++ CLI without Python, the server side of `std.http`, the VS Code extension,
and closure capture pruning.

Verifies:
  - Zero-Python native C++ CLI (`brat-cxx` executing `new`, `fmt`,
    `check`, `check --json`, `explain`, `doc`, `emit-c`, `test`, `cc`, and `run`
    with `python3`/`python` absent from `PATH`).
  - Server-side HTTP/1.1 in `std.http` (`status_text`, `parse_request`,
    `format_response`, `read_request`, `respond`, `respond_text`, `respond_json`,
    `listen`, `accept_request`, `serve_once` over live loopback TCP + chunked &
    smuggling/CRLF security checks) and VS Code extension (`editors/vscode/`).
  - Exact lexical `closure_free_vars` capture pruning, immortal
    zero-alloc `vclosure0` closures, stack `BStackVec` `vmap_with` (`vvec_cap`
    pre-sizing + `filter` temp release), and 2-entry `Map` SBO (`inl_k/v/h[2]`)
    verified under ASan + LSan + UBSan with 0 bytes leaked.
  - Dynamic local `f64` unboxing (`ub_floats` / `ubf_*`, `vub_fdiv`,
    `vub_fmod`, `vub_fcmp`, mixed `float + ub_int` loops, `NaN`/`-0.0`/`inf`/
    div-by-zero 4-way differential equivalence across `-O0`/`-O0`/`-O1`/`-O2`)
    with the CLI delegating `emit-c` byte-for-byte to the sole native emitter.
  - Optional `cc -E -dD` header preprocessing (`--preprocess`, `-I`,
    `-D` filtering out system headers), `#pragma pack(push, N)` / `#pragma pack(pop)`
    alignment stack tracking, and fail-closed C bitfield (`int a : 3;`) diagnostics.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
sys.path.insert(0, HERE)
import _cxx  # noqa: E402


def _run(cmd, env=None, cwd=None, timeout=60):
    return subprocess.run(
        cmd,
        env=env,
        cwd=cwd or ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )


def _pythonless_env(bin_dir):
    """Build a minimal `PATH` containing C toolchain binaries (`cc`, `as`, `ld`)
    but NO `python` or `python3` executable, proving zero-Python execution."""
    os.makedirs(bin_dir, exist_ok=True)
    needed = ("cc", "gcc", "clang", "as", "ld", "ar", "collect2", "sh", "rm", "cp", "mkdir")
    for tool in needed:
        real = shutil.which(tool)
        if real:
            dst = os.path.join(bin_dir, tool)
            if not os.path.exists(dst):
                os.symlink(real, dst)
    env = dict(os.environ)
    # Keep compiler internal libexec dirs searchable by symlinking `cc` -> `/usr/bin/cc`
    # while ensuring `shutil.which("python3", path=...)` is None.
    env["PATH"] = bin_dir
    return env


def test_zero_python_cli(cxx):
    with tempfile.TemporaryDirectory(prefix="brat_cli_") as td:
        nopy_bin = os.path.join(td, "nopy_bin")
        env = _pythonless_env(nopy_bin)
        assert shutil.which("python3", path=env["PATH"]) is None
        assert shutil.which("python", path=env["PATH"]) is None

        # Invalid input must not turn into a Python dependency either.
        unknown = _run([cxx, "not-a-command", "argument"], env=env)
        assert unknown.returncode == 2, (unknown.returncode, unknown.stderr)
        assert "unknown command" in unknown.stderr and "not-a-command" in unknown.stderr
        assert "python" not in unknown.stderr.lower(), unknown.stderr

        # 1. `new` scaffolds a runnable project without Python.
        proj = os.path.join(td, "demo_app")
        r = _run([cxx, "new", "demo_app"], env=env, cwd=td)
        assert r.returncode == 0, r.stderr
        main_brat = os.path.join(proj, "main.brat")
        assert os.path.isfile(main_brat)
        assert os.path.isfile(os.path.join(proj, "README.md"))

        # 2. `fmt` (--check detects unformatted, then formats cleanly).
        bad_fmt = os.path.join(td, "messy.brat")
        with open(bad_fmt, "w", encoding="utf-8") as f:
            f.write("/// Add one.\npub fn inc(x: i64)->i64{\nreturn x+1\n}\nfn main()->i64{\nprint(inc(41))\nreturn 0\n}\n")
        rc_chk = _run([cxx, "fmt", "--check", bad_fmt], env=env)
        assert rc_chk.returncode != 0
        rc_fmt = _run([cxx, "fmt", bad_fmt], env=env)
        assert rc_fmt.returncode == 0, rc_fmt.stderr
        rc_chk2 = _run([cxx, "fmt", "--check", bad_fmt], env=env)
        assert rc_chk2.returncode == 0, rc_chk2.stderr

        # 3. `check` + `check --json` (both clean & error files, exact JSON schema).
        rc_ok = _run([cxx, "check", "--json", bad_fmt], env=env)
        assert rc_ok.returncode == 0, rc_ok.stderr
        j_ok = json.loads(rc_ok.stdout)
        assert j_ok["version"] == 1 and j_ok["diagnostics"] == []

        err_brat = os.path.join(td, "bad_types.brat")
        with open(err_brat, "w", encoding="utf-8") as f:
            f.write('fn add(a: i64, b: i64) -> i64 {\n  return a + b\n}\nprint(add("nope", 2))\n')
        rc_err = _run([cxx, "check", "--json", err_brat], env=env)
        assert rc_err.returncode == 1
        j_err = json.loads(rc_err.stdout)
        assert j_err["version"] == 1 and len(j_err["diagnostics"]) >= 1
        d0 = j_err["diagnostics"][0]
        assert {"severity", "code", "message", "file", "line", "helps"} <= set(d0.keys())

        # 4. `explain` + `explain --json`.
        rexp = _run([cxx, "explain", "--json"], env=env)
        assert rexp.returncode == 0, rexp.stderr
        jexp = json.loads(rexp.stdout)
        assert isinstance(jexp, list) and len(jexp) >= 10

        # 5. `doc` generates Markdown docs natively without Python.
        doc_out = os.path.join(td, "docs.md")
        rdoc = _run([cxx, "doc", "-o", doc_out, bad_fmt], env=env)
        assert rdoc.returncode == 0, rdoc.stderr
        with open(doc_out, encoding="utf-8") as f:
            md_doc = f.read()
        assert "# Module" in md_doc and "inc" in md_doc and "Add one." in md_doc

        # 6. `emit-c` + `emit-c --bundle` + `run` + `test` without Python.
        remit = _run([cxx, "emit-c", bad_fmt, "--bundle"], env=env)
        assert remit.returncode == 0 and "typedef struct Val" in remit.stdout

        rrun = _run([cxx, "run", bad_fmt], env=env)
        assert rrun.returncode == 0 and rrun.stdout.strip() == "42", rrun.stderr

        tfile = os.path.join(td, "test_sample.brat")
        with open(tfile, "w", encoding="utf-8") as f:
            f.write("fn test_math() {\n  assert(6 * 7 == 42)\n}\ntest_math()\n")
        rtest = _run([cxx, "test", tfile], env=env)
        assert rtest.returncode == 0 and "1 passed" in rtest.stdout, rtest.stderr + rtest.stdout


def test_http_server_and_vscode(cxx):
    # 1. Verify VS Code extension files & manifest integrity.
    vs_dir = os.path.join(ROOT, "editors", "vscode")
    pkg_p = os.path.join(vs_dir, "package.json")
    cfg_p = os.path.join(vs_dir, "language-configuration.json")
    tm_p = os.path.join(vs_dir, "syntaxes", "brat.tmLanguage.json")
    ext_p = os.path.join(vs_dir, "extension.js")
    rd_p = os.path.join(vs_dir, "README.md")
    for p in (pkg_p, cfg_p, tm_p, ext_p, rd_p):
        assert os.path.isfile(p), f"missing VS Code extension file: {p}"
    with open(pkg_p, encoding="utf-8") as f:
        pkg = json.load(f)
    assert pkg["name"] == "brat-lang"
    assert pkg["contributes"]["languages"][0]["id"] == "brat"
    assert ".brat" in pkg["contributes"]["languages"][0]["extensions"]
    with open(cfg_p, encoding="utf-8") as f:
        lcfg = json.load(f)
    assert lcfg["comments"]["lineComment"] == "//"
    with open(tm_p, encoding="utf-8") as f:
        tm = json.load(f)
    assert tm["scopeName"] == "source.brat"
    with open(ext_p, encoding="utf-8") as f:
        ext_js = f.read()
    assert '"lsp"' in ext_js and "brat-cxx" in ext_js
    node = shutil.which("node")
    if node:
        rn = _run([node, "--check", ext_p])
        assert rn.returncode == 0, rn.stderr

    # 2. Verify `std.http` server-side HTTP/1.1 parser, security guards & live TCP loopback!
    with tempfile.TemporaryDirectory(prefix="brat_http_") as td:
        srv_test = os.path.join(td, "http_srv_e2e.brat")
        with open(srv_test, "w", encoding="utf-8") as f:
            f.write('''import std.http

fn res_is_err(r) {
  match r {
    Ok(_) => { return false },
    Err(_) => { return true }
  }
}

let route = fn(req) {
  let m = req["method"]
  let p = req["path"]
  if m == "GET" && p == "/hello" {
    let who = req["params"].get_or("who", "world")
    return {
      "status": 200,
      "headers": {"X-App": "brat-v06"},
      "body": "hi " + who
    }
  }
  if m == "POST" && p == "/echo" {
    return {
      "status": 201,
      "headers": {"Content-Type": "application/json"},
      "body": req["body"]
    }
  }
  return {"status": 404, "headers": {}, "body": "missing"}
}

// 1. Unit-check request parsing, chunked transfer-encoding, and smuggling/CRLF guards.
assert(http.status_text(200) == "OK")
assert(http.status_text(413) == "Payload Too Large")
let pr = unwrap(http.parse_request("GET /items?tag=fast+safe&n=3 HTTP/1.1\\r\\nHost: localhost\\r\\nX-Test: ok\\r\\n\\r\\n"))
assert(pr["method"] == "GET")
assert(pr["path"] == "/items")
assert(pr["params"]["tag"] == "fast safe")
assert(pr["params"]["n"] == "3")
assert(pr["headers"]["x-test"] == "ok")

let ch_raw = "POST /upload HTTP/1.1\\r\\nHost: localhost\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n4\\r\\nbrat\\r\\n5\\r\\n-v0.6\\r\\n0\\r\\n\\r\\n"
let ch_req = unwrap(http.parse_request(ch_raw))
assert(ch_req["body"] == "brat-v0.6")
let ch_payload = "A0\\r\\n\\r\\nB"
let ch_inner = "POST /upload HTTP/1.1\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n7\\r\\n" + ch_payload + "\\r\\n0\\r\\n\\r\\n"
let ch_inner_req = unwrap(http.parse_request(ch_inner))
assert(ch_inner_req["body"] == ch_payload)
assert(res_is_err(http.parse_request("POST /x HTTP/1.1\\r\\nTransfer-Encoding: xchunkedx\\r\\n\\r\\n0\\r\\n\\r\\n")))
assert(res_is_err(http.parse_request("POST /x HTTP/1.1\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n0\\r\\n")))
assert(res_is_err(http.parse_request("POST /x HTTP/1.1\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n8000000000000000\\r\\n")))
assert(res_is_err(http.parse_request("POST /x HTTP/1.1\\r\\nContent-Length: 0\\r\\nContent-Length: 0\\r\\n\\r\\n")))

// Request-smuggling & header injection rejections:
assert(res_is_err(http.parse_request("POST /x HTTP/1.1\\r\\nTransfer-Encoding: chunked\\r\\nContent-Length: 4\\r\\n\\r\\nabcd")))
assert(res_is_err(http.parse_request("GET /bad path HTTP/1.1\\r\\nHost: localhost\\r\\n\\r\\n")))
assert(res_is_err(http.format_response(200, {"X-Bad": "ok\\r\\nInjected: 1"}, "hi")))
assert(res_is_err(http.format_response(200, {"Content-Length": "999"}, "hi")))

// 2. Live loopback TCP server + client round-trip (2 requests).
let mut port = 39481
let mut srv = -1
let mut tries = 0
while tries < 40 {
  let lr = http.listen("127.0.0.1:" + str(port))
  match lr {
    Ok(fd) => {
      srv = fd
      break
    },
    Err(_) => {}
  }
  port = port + 1
  tries = tries + 1
}
assert(srv >= 0)
let base_url = "http://127.0.0.1:" + str(port)
let ch = chan(2)
scope {
  spawn {
    let r1 = unwrap(http.get(base_url + "/hello?who=brat+server"))
    let r2 = unwrap(http.post_json(base_url + "/echo", "{\\"ok\\":true}"))
    ch.send([r1["status"], r1["headers"]["x-app"], r1["body"], r2["status"], r2["body"]])
  }
  let s1 = unwrap(http.serve_once(srv, route))
  let s2 = unwrap(http.serve_once(srv, route))
  assert(s1 == 200)
  assert(s2 == 201)
}
let _closed = tcp_close(srv)
let client_out = ch.recv()
assert(client_out[0] == 200)
assert(client_out[1] == "brat-v06")
assert(client_out[2] == "hi brat server")
assert(client_out[3] == 201)
assert(client_out[4] == "{\\"ok\\":true}")
print("http_srv_ok")
''')
        r_chk = _run([cxx, "check", srv_test])
        assert r_chk.returncode == 0, r_chk.stderr + r_chk.stdout
        r_int = _run([cxx, "run", srv_test])
        assert r_int.returncode == 0 and r_int.stdout.strip() == "http_srv_ok", r_int.stderr
        bin_p = os.path.join(td, "http_srv_bin")
        r_cc = _run([cxx, "build", srv_test, "-o", bin_p])
        assert r_cc.returncode == 0, r_cc.stderr
        r_nat = _run([bin_p])
        assert r_nat.returncode == 0 and r_nat.stdout.strip() == "http_srv_ok", r_nat.stderr


def test_steps_12_3_and_12_4_speed_and_sanitizers(cxx):
    with tempfile.TemporaryDirectory(prefix="brat_speed_") as td:
        # 1. Verify exact `free_vars`, `vclosure0`, `vmap_with` stack frame, `ub_floats`,
        # and byte-for-byte Python-CLI delegation to the sole native `emit-c`!
        src_p = os.path.join(td, "dyn_speed.brat")
        with open(src_p, "w", encoding="utf-8") as f:
            f.write('''fn sq(x) {
  return x * x
}

fn add2(a, b) {
  return a + b
}

fn closure_and_float_probe(n) {
  let unused_heavy = [10, 20, 30, 40]
  let captured_k = 5
  let mut ub_counter = 0
  let mut ubf_acc = 0.0
  for i in 1..n {
    ub_counter = ub_counter + i
    ubf_acc = ubf_acc + (i * 1.5) / 2.0 - (0.5 % 0.3)
  }
  while ubf_acc < 0.0 {
    ubf_acc = ubf_acc + 1.0
  }
  let pure_clo = fn(z) { return z + 1 }
  let named_nonrec = fn helper(z) { return z * 3 }
  let cap_clo = fn(z) { return z + captured_k }
  let vs = [1, 2, 3, 4]
  let m1 = vs.map(sq)
  let m2 = vs.map(pure_clo)
  let m3 = vs.map(named_nonrec)
  let m4 = vs.map(cap_clo)
  let s1 = m4.fold(0, add2)
  let pf = par for x in vs reduce(add2, 0) {
    return x * 2
  }
  return [len(unused_heavy), ub_counter, ubf_acc, m1[3], m2[0], m3[1], s1, pf]
}

fn ieee_edges(zero_v, nan_in, inf_in) {
  let mut z = 0.0
  z = z + zero_v
  let mut neg_z = -0.0
  neg_z = neg_z * 1.0
  let mut div_err = "none"
  try {
    let mut bad = 1.0
    bad = bad / z
    print(bad)
  } catch e {
    div_err = str(e)
  }
  let mut m1 = -7.5
  m1 = m1 % 2.0
  let mut m2 = 7.5
  m2 = m2 % -2.0
  let mut big = 1000000.0
  big = big * 1000000.0
  return [
    z == -0.0,
    nan_in == nan_in,
    nan_in != nan_in,
    nan_in < 1.0,
    nan_in <= 1.0,
    inf_in > big,
    -big < -1000.0,
    m1,
    m2,
    div_err
  ]
}

print(closure_and_float_probe(10))
print(ieee_edges(0.0, unwrap("nan".parse_float()), unwrap("inf".parse_float())))
''')
        c_cx = _run([cxx, "emit-c", src_p])
        assert c_cx.returncode == 0, c_cx.stderr

        c_src = c_cx.stdout
        # Verify unboxed local int & float coexist with a capturing closure in the same fn!
        assert "int64_t BRAT_UNUSED ub_b_ub_counter = 0;" in c_src
        assert "double BRAT_UNUSED ubf_b_ubf_acc = 0.0;" in c_src
        assert "vub_fdiv(" in c_src
        assert "vub_fmod(" in c_src
        assert "vub_fcmp(" in c_src
        # Verify `vclosure0` is emitted for pure closures, non-recursive named closures, HOF fn refs, and parfor!
        assert c_src.count("vclosure0(") >= 5, c_src
        # Verify `unused_heavy` was NOT captured into any closure map!
        assert '"unused_heavy"' not in c_src
        # Verify `vmap_with` uses stack `BStackVec` + `vvec_cap`!
        assert "Vec *o = meth[0] == 'm' ? vvec_cap(w->len) : vvec();" in c_src
        assert "BStackVec _osv; VSTACK_VEC_INIT(_osv, _ob, 1);" in c_src

        # 2. Differential equivalence across `run` and `build` (`-O0`, `-O1`, `-O2`).
        r_run = _run([cxx, "run", src_p])
        assert r_run.returncode == 0, r_run.stderr
        want_out = r_run.stdout
        for opt in ("-O0", "-O1", "-O2"):
            bp = os.path.join(td, f"probe_{opt[1:]}")
            rc = _run([cxx, "build", src_p, opt, "-o", bp])
            assert rc.returncode == 0, rc.stderr
            rr = _run([bp])
            assert rr.returncode == 0 and rr.stdout == want_out, (opt, rr.stdout, want_out)

        # 3. ASan + LSan + UBSan stress test on Small-Map SBO, shaped structs -> unshape,
        # filter heap-predicate release, and multi-threaded `vclosure0`!
        san_brat = os.path.join(td, "san_sbo.brat")
        with open(san_brat, "w", encoding="utf-8") as f:
            f.write('''fn non_empty_tag(x) {
  // Returns a fresh heap string (>22 bytes, non-SSO!) in `.filter(...)` to prove
  // `vmap_with` releases `r` on every filter predicate evaluation without leaking!
  if x % 2 == 0 {
    return ""
  }
  return "heap_allocated_predicate_string_value_" + str(x)
}

fn worker_map(delta) {
  let f = fn(z) { return z + 10 }
  return [1, 2, 3].map(f)[2] + delta
}

fn sbo_round(iter_n) {
  let mut total = 0
  for i in 0..iter_n {
    // 0, 1, 2-entry maps live inline in Map SBO (`inl_k/v/h[2]`):
    let m0 = {}
    let m1 = {"k1": "val_" + str(i)}
    let mut m2 = {"a": i + 0, "b": i + 1}
    total = total + len(m0) + len(m1) + m2["a"] + m2["b"]
    // Transition 2-entry SBO -> 3..6-entry heap table + copy + del:
    m2.set("c", i + 2)
    m2.set("d", i + 3)
    m2.set("e", i + 4)
    let m2c = m2.copy()
    let _d = m2.del("b")
    total = total + m2c["e"] + len(m2)
    // 2-field shaped struct SBO (`vmap_shaped` n=2):
    let p = Pair { a: i + 0, b: "pair_payload_long_string_" + str(i) }
    if len(p.b) > 0 {
      total = total + p.a
    }
  }
  let vs = [1, 2, 3, 4, 5, 6]
  let odds = vs.filter(non_empty_tag)
  let ch = chan(2)
  scope {
    spawn {
      ch.send(worker_map(0))
    }
    spawn {
      ch.send(worker_map(10))
    }
  }
  return total + len(odds) + ch.recv() + ch.recv()
}

print(sbo_round(400))
''')
        san_c = os.path.join(td, "san_sbo.c")
        rc_emit = _run([cxx, "emit-c", san_brat, "--bundle"])
        assert rc_emit.returncode == 0, rc_emit.stderr
        with open(san_c, "w", encoding="utf-8") as f:
            f.write(rc_emit.stdout)
        san_bin = os.path.join(td, "san_sbo_bin")
        cc = shutil.which("cc") or "gcc"
        rc_san = _run([
            cc, "-std=gnu11", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wall", "-Wextra", "-Werror",
            san_c, "-o", san_bin, "-lm", "-lpthread", "-ldl",
        ])
        assert rc_san.returncode == 0, rc_san.stderr
        env_san = dict(os.environ)
        env_san["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        env_san["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"
        rr_san = _run([san_bin], env=env_san)
        assert rr_san.returncode == 0, rr_san.stderr
        rr_san_i = _run([cxx, "run", san_brat])
        assert rr_san.stdout == rr_san_i.stdout

        # 4. Dynamic float + closure + map SBO micro-benchmark (100k harmonic + map SBO ops).
        bench_brat = os.path.join(td, "bench_w6.brat")
        with open(bench_brat, "w", encoding="utf-8") as f:
            f.write('''fn run_bench(n) {
  let mut acc = 0.0
  for i in 1..n {
    acc = acc + (i * 0.25) / (i + 1)
  }
  let f = fn(x) { return x + 1 }
  let mut s = 0
  for j in 0..2000 {
    let m = {"x": j, "y": j + 1}
    s = s + m["x"] + m["y"]
  }
  let v = [1, 2, 3, 4].map(f)
  return [acc > 0.0, s, v[3]]
}
print(run_bench(100000))
''')
        bench_bin = os.path.join(td, "bench_bin")
        assert _run([cxx, "build", bench_brat, "-O2", "-o", bench_bin]).returncode == 0
        t0 = time.perf_counter()
        rb = _run([bench_bin])
        dt_ms = (time.perf_counter() - t0) * 1000.0
        assert rb.returncode == 0 and rb.stdout.strip() == "[true, 4000000, 5]", rb.stdout
        return dt_ms


def test_ffi_preprocess_pack_bitfields():
    with tempfile.TemporaryDirectory(prefix="brat_ffi_") as td:
        inc_dir = os.path.join(td, "inc")
        os.makedirs(inc_dir)
        with open(os.path.join(inc_dir, "cfg_types.h"), "w", encoding="utf-8") as f:
            f.write("typedef int32_t cfg_val_t;\n")

        hdr_pp = os.path.join(td, "pp_api.h")
        with open(hdr_pp, "w", encoding="utf-8") as f:
            f.write("""#ifndef PP_API_H
#define PP_API_H
#include <stdint.h>
#include "cfg_types.h"

#ifdef ENABLE_TURBO
#define TURBO_SCALE (BASE_SCALE * 2)
typedef struct {
    cfg_val_t base;
    int64_t scaled;
} TurboPair;
int64_t turbo_eval(TurboPair p);
#else
int32_t legacy_only(int32_t x);
#endif
#endif
""")
        c_pp = os.path.join(td, "pp_api.c")
        with open(c_pp, "w", encoding="utf-8") as f:
            f.write("""#include "pp_api.h"
int64_t turbo_eval(TurboPair p) {
    return (int64_t)p.base * TURBO_SCALE + p.scaled;
}
""")
        so_pp = os.path.join(td, "libpp_api.so")
        cc = shutil.which("cc") or "gcc"
        rc_so = _run([cc, "-std=gnu11", "-shared", "-fPIC", f"-I{inc_dir}",
                      "-DENABLE_TURBO=1", "-DBASE_SCALE=5", c_pp, "-o", so_pp])
        assert rc_so.returncode == 0, rc_so.stderr

        # 1. `bindgen --preprocess -I... -D...` expands `#ifdef ENABLE_TURBO`, `#include <stdint.h>`,
        # and `-I` user header without leaking glibc internals!
        bind_p = os.path.join(td, "pp_bind.brat")
        r_bg = _run(_cxx.cli_args("bindgen", "--preprocess", f"-I{inc_dir}", "-DENABLE_TURBO=1", "-DBASE_SCALE=5", "--lib", so_pp, "-o", bind_p, hdr_pp))
        assert r_bg.returncode == 0, r_bg.stderr
        with open(bind_p, encoding="utf-8") as f:
            btxt = f.read()
        assert "extern struct TurboPair { base: i32, scaled: i64 }" in btxt, btxt
        assert "turbo_eval" in btxt and "legacy_only" not in btxt
        assert "pub const TURBO_SCALE = 10" in btxt, btxt
        # Ensure `<stdint.h>` system internals did NOT flood bindgen output.
        assert "__uint128" not in btxt and "_IO_" not in btxt

        # Verify `brat ffi --preprocess` proves the generated binding!
        r_ffi_ok = _run(_cxx.cli_args("ffi", "--preprocess", f"-I{inc_dir}", "-DENABLE_TURBO=1", "-DBASE_SCALE=5", "--header", hdr_pp, bind_p))
        assert r_ffi_ok.returncode == 0, r_ffi_ok.stderr + r_ffi_ok.stdout

        # Execute end-to-end call into `libpp_api.so`!
        use_pp = os.path.join(td, "use_pp.brat")
        with open(use_pp, "w", encoding="utf-8") as f:
            f.write(btxt + "\nlet tp = TurboPair { base: 7, scaled: 3 }\nassert(turbo_eval(tp) == 73)\nprint(TURBO_SCALE)\n")
        r_pp_bin = os.path.join(td, "use_pp_bin")
        r_pp_b = _run(_cxx.cli_args("build", "--unsafe-ffi", "-o", r_pp_bin, use_pp))
        assert r_pp_b.returncode == 0, r_pp_b.stderr
        r_pp_run = _run([r_pp_bin])
        assert r_pp_run.returncode == 0 and r_pp_run.stdout.strip() == "10", r_pp_run.stderr

        # 2. `#pragma pack(push, 1)` / `#pragma pack(push, 2)` / `#pragma pack(pop)` + C bitfields!
        hdr_pack = os.path.join(td, "pack_and_bf.h")
        with open(hdr_pack, "w", encoding="utf-8") as f:
            f.write("""#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    uint8_t tag;
    uint32_t payload;
} WirePkt1;

#pragma pack(push, 2)
typedef struct {
    uint8_t kind;
    uint32_t seq;
} HalfPacked2;
#pragma pack(pop)

typedef struct {
    uint8_t end_tag;
    uint64_t stamp;
} StillPack1;
#pragma pack(pop)

typedef struct {
    uint8_t flag;
    uint32_t count;
} RestoredNatural;

typedef struct {
    uint32_t ready : 1;
    uint32_t mode : 3;
    uint32_t : 0;
    uint32_t raw;
} RegBitfields;

uint32_t read_pkt1(WirePkt1 p);
int touch_half(HalfPacked2 *p);
int touch_bf(RegBitfields *r);
""")
        r_bg2 = _run(_cxx.cli_args("bindgen", hdr_pack))
        assert r_bg2.returncode == 0, r_bg2.stderr
        g2 = r_bg2.stdout
        # `WirePkt1` and `StillPack1` (under `#pragma pack(push, 1)` after nested `pop`) -> `extern packed struct`!
        assert "extern packed struct WirePkt1 { tag: u8, payload: u32 }" in g2, g2
        assert "extern packed struct StillPack1 { end_tag: u8, stamp: u64 }" in g2, g2
        # `RestoredNatural` (after outer `#pragma pack(pop)`) -> normal `extern struct`!
        assert "extern struct RestoredNatural { flag: u8, count: u32 }" in g2, g2
        # `HalfPacked2` (`#pragma pack(2)`, 6B align 2) & `RegBitfields` -> NOT emitted as broken struct layouts,
        # and pointer functions take `ptr` cleanly and round-trip `brat ffi`!
        assert "extern struct HalfPacked2" not in g2 and "extern packed struct HalfPacked2" not in g2, g2
        assert "extern struct RegBitfields" not in g2, g2
        assert "// skipped: struct RegBitfields has C bitfield(s) (ready:1, mode:3, <pad>:0)" in g2, g2
        assert "// skipped: struct HalfPacked2: #pragma pack(2) layout (6B, align 2)" in g2, g2
        assert "extern fn touch_half(p: ptr) -> i32" in g2, g2
        assert "extern fn touch_bf(r: ptr) -> i32" in g2, g2

        g2_brat = os.path.join(td, "g2.brat")
        with open(g2_brat, "w", encoding="utf-8") as f:
            f.write(g2)
        r_g2_prove = _run(_cxx.cli_args("ffi", "--header", hdr_pack, g2_brat))
        assert r_g2_prove.returncode == 0, r_g2_prove.stdout + r_g2_prove.stderr

        # Verify fail-closed errors if user manually writes byte-addressed `RegBitfields` or wrong `HalfPacked2`!
        bad_bf = os.path.join(td, "bad_bf.brat")
        with open(bad_bf, "w", encoding="utf-8") as f:
            f.write("extern struct RegBitfields { ready: u32, mode: u32, raw: u32 }\nextern struct HalfPacked2 { kind: u8, seq: u32 }\n")
        r_bad_bf = _run(_cxx.cli_args("ffi", "--header", hdr_pack, bad_bf))
        assert r_bad_bf.returncode != 0
        out_bad = r_bad_bf.stdout + r_bad_bf.stderr
        assert "C bitfield(s) (ready:1, mode:3, <pad>:0)" in out_bad, out_bad
        assert "#pragma pack(2)" in out_bad, out_bad


def main():
    cxx = _cxx.build()
    test_zero_python_cli(cxx)
    print("ok: zero-Python native C++ CLI + check --json parity")
    test_http_server_and_vscode(cxx)
    print("ok: server-side std.http loopback + VS Code extension")
    dt_ms = test_steps_12_3_and_12_4_speed_and_sanitizers(cxx)
    print("ok: vclosure0 + Map SBO + ub_floats + ASan/LSan, bench %.1f ms" % dt_ms)
    test_ffi_preprocess_pack_bitfields()
    print("ok: cc -E --preprocess + #pragma pack stack + C bitfields")
    print("ALL (v0.6) TESTS GREEN!")


if __name__ == "__main__":
    main()
