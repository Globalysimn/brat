"""Security and soundness regressions found during review."""
import hashlib
import http.server
import json
import os
import socket
import socketserver
import subprocess
import sys
import tarfile
import threading
import time

import _cxx
import _scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
TMP = _scratch.mkdtemp(prefix="closure-")


def zero_python_env():
    # Prepend a directory shadowing python/python3 with exit 99 to prove zero-Python execution.
    shim_dir = os.path.join(TMP, "no-py-bin")
    os.makedirs(shim_dir, exist_ok=True)
    for name in ("python", "python3"):
        p = os.path.join(shim_dir, name)
        with open(p, "w", encoding="utf-8") as f:
            f.write("#!/bin/sh\nexit 99\n")
        os.chmod(p, 0o755)
    env = dict(os.environ)
    env["PATH"] = shim_dir + os.pathsep + env.get("PATH", "")
    return env


def test_13_6b_native_ffi_and_bindgen(cxx, env):
    d = os.path.join(TMP, "ffi13_6b")
    os.makedirs(d, exist_ok=True)
    hdr = os.path.join(d, "vec2.h")
    csrc = os.path.join(d, "vec2.c")
    obj = os.path.join(d, "vec2.o")
    brat_src = os.path.join(d, "vec2.brat")
    bind = os.path.join(d, "vec2.bratbind")
    exe = os.path.join(d, "vec2_app")
    with open(hdr, "w", encoding="utf-8") as f:
        f.write("typedef struct Vec2 { double x; double y; } Vec2;\ndouble vec2_dot(Vec2 a, Vec2 b);\n")
    with open(csrc, "w", encoding="utf-8") as f:
        f.write('#include "vec2.h"\ndouble vec2_dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }\n')
    with open(brat_src, "w", encoding="utf-8") as f:
        f.write(
            "extern struct Vec2 { x: f64, y: f64 }\n"
            "extern fn vec2_dot(a: Vec2, b: Vec2) -> f64;\n"
            "export fn dot_pair(x: f64, y: f64) -> f64 {\n"
            "  let v = Vec2 { x: x, y: y }\n"
            "  return vec2_dot(v, v)\n"
            "}\n"
            "fn main() -> i64 { return 0 }\n"
        )
    subprocess.run(["gcc", "-g", "-c", csrc, "-o", obj], check=True)

    r_bg = subprocess.run([cxx, "bindgen", hdr], env=env, capture_output=True, text=True, check=True)
    assert "Vec2" in r_bg.stdout and "vec2_dot" in r_bg.stdout
    for lang, ext in (("python", ".py"), ("rust", ".rs"), ("go", ".go")):
        out = os.path.join(d, "out" + ext)
        r = subprocess.run([cxx, "bindgen", brat_src, "--lang", lang, "-o", out],
                           env=env, capture_output=True, text=True, check=True)
        assert os.path.isfile(out) and os.path.getsize(out) > 0, (lang, r.stderr)

    subprocess.run([cxx, "ffi", brat_src, "--header", hdr, "--emit-bind", bind], env=env, check=True)
    assert os.path.isfile(bind)
    vr = subprocess.run([cxx, "ffi", brat_src, "--header", hdr, "--object", obj, "--json"],
                        env=env, capture_output=True, text=True, check=True)
    vdata = json.loads(vr.stdout)
    assert vdata.get("errors") == 0 and vdata.get("proven") == 2, vdata

    mr = subprocess.run([cxx, "ffi", brat_src, "--header", hdr, "--abi-matrix", "--json"],
                        env=env, capture_output=True, text=True, check=True)
    mdata = json.loads(mr.stdout)
    assert mdata.get("ok") is True and len(mdata.get("targets", [])) == 5, mdata

    subprocess.run([cxx, "build", brat_src, "--link", obj, "--header", hdr, "-o", exe], env=env, check=True)
    br_info = subprocess.run([cxx, "ffi", "--bin", exe],
                             env=env, capture_output=True, text=True, check=True)
    assert "vec2_dot" in br_info.stdout and "Vec2" in br_info.stdout, br_info.stdout
    br_ok = subprocess.run([cxx, "ffi", brat_src, "--header", hdr, "--bin", exe],
                           env=env, capture_output=True, text=True, check=True)
    assert "contract matches" in br_ok.stdout, br_ok.stdout


def test_13_1c_native_pkg_http_targz_and_c_sources(cxx, env):
    d = os.path.join(TMP, "pkg13_1c")
    pkg_src = os.path.join(d, "src")
    srv_root = os.path.join(d, "srv")
    proj = os.path.join(d, "proj")
    os.makedirs(pkg_src, exist_ok=True)
    os.makedirs(srv_root, exist_ok=True)
    os.makedirs(proj, exist_ok=True)

    with open(os.path.join(pkg_src, "brat.pkg.json"), "w", encoding="utf-8") as f:
        json.dump({"name": "fastadd", "version": "0.1.0", "c_sources": ["fastadd.c", "fastsub.c"]}, f)
    hdr_p = os.path.join(pkg_src, "fastadd.h")
    c_p = os.path.join(pkg_src, "fastadd.c")
    c2_p = os.path.join(pkg_src, "fastsub.c")
    brat_p = os.path.join(pkg_src, "fastadd.brat")
    bri_p = os.path.join(pkg_src, "fastadd.bri")
    bind_p = os.path.join(pkg_src, "fastadd.bratbind")
    with open(hdr_p, "w", encoding="utf-8") as f:
        f.write("#include <stdint.h>\nint64_t fast_add_c(int64_t a, int64_t b);\nint64_t fast_sub_c(int64_t a, int64_t b);\n")
    with open(c_p, "w", encoding="utf-8") as f:
        f.write('#include "fastadd.h"\nint64_t fast_add_c(int64_t a, int64_t b) { return a + b + 100; }\n')
    with open(c2_p, "w", encoding="utf-8") as f:
        f.write('#include "fastadd.h"\nint64_t fast_sub_c(int64_t a, int64_t b) { return a - b; }\n')
    with open(brat_p, "w", encoding="utf-8") as f:
        f.write("extern fn fast_add_c(a: i64, b: i64) -> i64;\nextern fn fast_sub_c(a: i64, b: i64) -> i64;\npub fn calc(a: i64, b: i64) -> i64 {\n  return fast_add_c(a, b) + fast_sub_c(a, a)\n}\n")
    with open(bri_p, "w", encoding="utf-8") as f:
        f.write("fn calc(a: i64, b: i64) -> i64\n")
    subprocess.run([cxx, "ffi", brat_p, "--header", hdr_p, "--emit-bind", bind_p], env=env, check=True)

    tgz_path = os.path.join(srv_root, "fastadd-0.1.0.tar.gz")
    with tarfile.open(tgz_path, "w:gz") as tf:
        for fn in ("brat.pkg.json", "fastadd.h", "fastadd.c", "fastsub.c", "fastadd.brat", "fastadd.bri", "fastadd.bratbind"):
            tf.add(os.path.join(pkg_src, fn), arcname=fn)

    # Also build a tampered c_sources archive where .c defines double instead of int64_t.
    bad_c_p = os.path.join(d, "bad_fastadd.c")
    with open(bad_c_p, "w", encoding="utf-8") as f:
        f.write("double fast_add_c(double a, double b) { return a + b; }\n")
    bad_tgz_path = os.path.join(srv_root, "fastadd-bad.tar.gz")
    with tarfile.open(bad_tgz_path, "w:gz") as tf:
        for fn in ("brat.pkg.json", "fastadd.h", "fastsub.c", "fastadd.brat", "fastadd.bri", "fastadd.bratbind"):
            tf.add(os.path.join(pkg_src, fn), arcname=fn)
        tf.add(bad_c_p, arcname="fastadd.c")

    # Duplicate tar entry archive must also be rejected.
    dup_tgz_path = os.path.join(srv_root, "fastadd-dup.tar.gz")
    with tarfile.open(dup_tgz_path, "w:gz") as tf:
        for fn in ("brat.pkg.json", "fastadd.h", "fastadd.c", "fastsub.c", "fastadd.brat", "fastadd.bri", "fastadd.bratbind"):
            tf.add(os.path.join(pkg_src, fn), arcname=fn)
        tf.add(bad_c_p, arcname="fastadd.c")

    reg_brat = os.path.join(srv_root, "mathx.brat")
    with open(reg_brat, "w", encoding="utf-8") as f:
        f.write("pub fn scale(x: i64) -> i64 { return x * 2 }\n")
    with open(reg_brat, "rb") as f:
        reg_sha = hashlib.sha256(f.read()).hexdigest()

    class QuietHandler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=srv_root, **kw)
        def log_message(self, *args):
            pass

    class ReusableTCPServer(socketserver.TCPServer):
        allow_reuse_address = True

    httpd = ReusableTCPServer(("127.0.0.1", 0), QuietHandler)
    port = httpd.server_address[1]
    index = {"packages": {"acme/mathx": {"0.1.0": {"url": f"http://127.0.0.1:{port}/mathx.brat", "sha256": reg_sha}}}}
    with open(os.path.join(srv_root, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f)
    th = threading.Thread(target=httpd.serve_forever, daemon=True)
    th.start()
    try:
        pkg_env = dict(env)
        pkg_env["BRAT_INDEX"] = f"http://127.0.0.1:{port}/index.json"

        # Tampered c_sources ABI & duplicate tar entries must be rejected in both CXX and PY.
        for bad_url in (f"http://127.0.0.1:{port}/fastadd-bad.tar.gz",
                        f"http://127.0.0.1:{port}/fastadd-dup.tar.gz"):
            r_cxx_bad = subprocess.run([cxx, "pkg", "add", bad_url],
                                       cwd=proj, env=pkg_env, capture_output=True, text=True)
            assert r_cxx_bad.returncode != 0, (bad_url, r_cxx_bad.stderr)
            r_py_bad = subprocess.run(_cxx.cli_args("pkg", "add", bad_url),
                                      cwd=proj, capture_output=True, text=True)
            assert r_py_bad.returncode != 0, (bad_url, r_py_bad.stderr)

        subprocess.run([cxx, "pkg", "add", "acme/mathx@0.1.0"], cwd=proj, env=pkg_env, check=True)
        subprocess.run([cxx, "pkg", "add", f"http://127.0.0.1:{port}/fastadd-0.1.0.tar.gz"],
                       cwd=proj, env=pkg_env, check=True)
        subprocess.run([cxx, "pkg", "verify"], cwd=proj, env=pkg_env, check=True)
        subprocess.run(_cxx.cli_args("pkg", "verify"), cwd=proj, check=True)
        main_brat = os.path.join(proj, "main.brat")
        with open(main_brat, "w", encoding="utf-8") as f:
            f.write(
                "import mathx\nimport fastadd.fastadd\n"
                "fn main() -> i64 {\n"
                "  print(mathx.scale(fastadd.fastadd.calc(7, 8)))\n"
                "  return 0\n"
                "}\n"
            )
        run_r = subprocess.run([cxx, "run", main_brat], cwd=proj, env=pkg_env,
                               capture_output=True, text=True, check=True)
        assert run_r.stdout == "230\n", run_r.stdout

        # Tampered SHA-256 in remote index must fail closed and preserve existing lock/vendor.
        bad_index = {"packages": {"acme/badpkg": {"0.1.0": {"url": f"http://127.0.0.1:{port}/mathx.brat", "sha256": "0" * 64}}}}
        with open(os.path.join(srv_root, "bad_index.json"), "w", encoding="utf-8") as f:
            json.dump(bad_index, f)
        bad_env = dict(env)
        bad_env["BRAT_INDEX"] = f"http://127.0.0.1:{port}/bad_index.json"
        bad = subprocess.run([cxx, "pkg", "add", "acme/badpkg@0.1.0"],
                             cwd=proj, env=bad_env, capture_output=True, text=True)
        assert bad.returncode != 0
        subprocess.run([cxx, "pkg", "verify"], cwd=proj, env=pkg_env, check=True)
    finally:
        httpd.shutdown()
        httpd.server_close()


def test_13_2b2_and_compiler_leaks_under_asan(cxx):
    d = os.path.join(TMP, "asan13_2b2")
    os.makedirs(d, exist_ok=True)
    srv_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv_sock.bind(("127.0.0.1", 0))
    srv_sock.listen(4)
    ka_port = srv_sock.getsockname()[1]

    close_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    close_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    close_sock.bind(("127.0.0.1", 0))
    close_sock.listen(2)
    close_port = close_sock.getsockname()[1]

    port_file = os.path.join(d, "srv.port")
    accepted = [0]

    def ka_peer():
        try:
            c, _ = srv_sock.accept()
            accepted[0] += 1
            with c:
                c.settimeout(10)
                for idx in range(3):
                    req = bytearray()
                    while b"\r\n\r\n" not in req:
                        ch = c.recv(4096)
                        if not ch:
                            return
                        req.extend(ch)
                    if idx == 0:
                        c.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n3\r\na\xc3\xa9\r\n1\r\nb\r\n0\r\n\r\n")
                    elif idx == 1:
                        c.sendall(b"HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\n\r\n")
                    else:
                        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\ndone")
        finally:
            srv_sock.close()

    def close_delim_peer():
        try:
            c, _ = close_sock.accept()
            with c:
                c.recv(1024)
                c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\neofbody")
        finally:
            close_sock.close()

    src = f'''import std.http
import std.json
import std.csv

fn add_i(a: i64, b: i64) -> i64 {{ return a + b }}

fn sink_vec(move v: vec[i64]) -> i64 {{
  return len(v)
}}

fn catch_ret_vec() {{
  try {{
    raise([99, 88])
    return []
  }} catch e {{
    return e
  }}
}}

fn audit_runtime_regressions() -> i64 {{
  let xs = [1, 2, 0, 4]
  let mut pf_caught = false
  try {{
    scope {{
      let _out = par for x in xs reduce(add_i, 0) {{
        return 100 / x
      }}
    }}
  }} catch e {{
    pf_caught = true
  }}
  assert(pf_caught == true)

  let mut sp_caught = false
  try {{
    scope {{
      spawn {{ let _a = 1 / 0 }}
      spawn {{ let _b = 2 / 0 }}
      spawn {{ raise("boom") }}
    }}
  }} catch e {{
    sp_caught = true
  }}
  assert(sp_caught == true)

  let mut ar_msg = ""
  try {{
    arena {{
      let v = [10, 20, 30]
      raise(str(v))
    }}
  }} catch e {{
    ar_msg = str(e)
  }}
  assert(ar_msg == "[10, 20, 30]")
  assert(str(catch_ret_vec()) == "[99, 88]")

  let ch1 = chan(2)
  ch1.close()
  let ch2 = chan(2)
  ch2.send(777)
  let stale_rx = ch1.recv()
  let stale_tx = ch1.send(999)
  let ok_rx = ch2.recv()
  ch2.close()
  assert(is_none(stale_rx) && is_err(stale_tx) && ok_rx == 777)
  return 0
}}

fn leak_regressions(flag: i64) -> i64 {{
  let mut base = "a"
  for k in 0..4 {{
    base = base + "b"
  }}
  assert(base == "abbbb")
  if flag > 0 {{
    let mut mv = [1, 2, 3]
    assert(sink_vec(move mv) == 3)
  }}
  if flag > 0 {{
    let mut mv = [4, 5]
    mv = [6, 7, 8, 9]
    assert(len(mv) == 4)
  }}
  let mut acc = "p"
  let m = {{"x": "-q"}}
  for i in 0..6 {{
    if i >= 0 {{
      let mut d = Ok("")
      d = Ok("alpha")
      assert(unwrap(d) == "alpha")
    }}
    if i >= 0 {{
      let mut d = Ok("")
      d = Ok("beta")
      assert(unwrap(d) == "beta")
    }}
    acc = acc + m["x"]
  }}
  assert(len(utf8_bytes(["u", "v"].join(""))) == 2)
  assert(len(acc) == 13)
  return 0
}}

fn bind_ephemeral_server(port_path: str) -> i64 {{
  let mut p = 24100
  while p < 24300 {{
    let lr = std.http.listen("127.0.0.1:" + str(p))
    if !is_err(lr) {{
      write_file(port_path, str(p))
      return unwrap(lr)
    }}
    p = p + 1
  }}
  raise("no free port in 24100..24300")
  return -1
}}

fn main() -> i64 {{
  let _r0 = audit_runtime_regressions()
  let _r = leak_regressions(1)
  let lfd = bind_ephemeral_server("{port_file}")

  let c = unwrap(std.http.open_keep_alive("http://127.0.0.1:{ka_port}/1"))
  let r1 = unwrap(std.http.get_keep_alive(c, "http://127.0.0.1:{ka_port}/1"))
  assert(r1["body"] == "aéb" && r1["keep_alive"] == true)
  let r2 = unwrap(std.http.get_keep_alive(c, "http://127.0.0.1:{ka_port}/2"))
  assert(r2["status"] == 204 && r2["body"] == "" && r2["keep_alive"] == true)
  let r3 = unwrap(std.http.get_keep_alive(c, "http://127.0.0.1:{ka_port}/3"))
  assert(r3["body"] == "done" && r3["keep_alive"] == false)

  let reof = unwrap(std.http.get("http://127.0.0.1:{close_port}/eof"))
  assert(reof["body"] == "eofbody" && reof["keep_alive"] == false)

  let hfn = fn(req) {{ return "ok:" + req["path"] + ":" + req["body"] }}
  let served = unwrap(std.http.serve_keep_alive(lfd, 4, hfn))
  let _cl = tcp_close(lfd)
  assert(served == 2)

  assert(is_err(std.json.parse("\\"\\\\uD800\\"")))
  assert(is_err(std.json.parse("\\"\\\\u0000\\"")))
  assert(unwrap(std.json.parse("\\"\\\\uD83D\\\\uDE80\\"")) == "🚀")
  assert(is_err(std.csv.parse("\\"unterminated")))
  assert(unwrap(std.csv.parse("x,\\"y\\r\\nz\\"\\n1,2"))[0][1] == "y\\r\\nz")
  print("CLOSURE_ASAN_OK")
  return 0
}}
'''
    brat_p = os.path.join(d, "asan_case.brat")
    c_p = os.path.join(d, "asan_case.c")
    exe_p = os.path.join(d, "asan_case.asan")
    with open(brat_p, "w", encoding="utf-8") as f:
        f.write(src)

    cli_emit_env = dict(os.environ)
    cli_emit_env.update(BRAT_CXX=cxx, BRAT_NO_NATIVE_CLI="1")
    c_cli = subprocess.check_output(_cxx.cli_args("emit-c", brat_p),
                                    env=cli_emit_env, text=True)
    c_cx = subprocess.check_output([cxx, "cc", brat_p], text=True)
    assert c_cli == c_cx, "Python CLI and native cc diverged on closure program"
    with open(c_p, "w", encoding="utf-8") as f:
        f.write(c_cx)
    subprocess.run(
        ["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
         f"-I{ENGINE}", c_p, "-o", exe_p, "-lm", "-lpthread", "-ldl"],
        check=True,
    )

    th_peer = threading.Thread(target=ka_peer, daemon=True)
    th_peer.start()
    th_eof = threading.Thread(target=close_delim_peer, daemon=True)
    th_eof.start()

    def pipeline_client():
        bound_port = None
        for _ in range(200):
            if os.path.isfile(port_file):
                txt = open(port_file, encoding="utf-8").read().strip()
                if txt.isdigit():
                    bound_port = int(txt)
                    break
            time.sleep(0.02)
        assert bound_port is not None, "BRAT server did not publish bound port"
        cs = socket.create_connection(("127.0.0.1", bound_port), timeout=5)
        with cs:
            cs.settimeout(5)
            cs.sendall(
                b"GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n"
                b"POST /b HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi"
            )
            got = bytearray()
            while True:
                ch = cs.recv(4096)
                if not ch:
                    break
                got.extend(ch)
            assert b"ok:/a:" in got and b"ok:/b:hi" in got, got

    th_cli = threading.Thread(target=pipeline_client, daemon=True)
    th_cli.start()

    res = subprocess.run(
        [exe_p], capture_output=True, text=True, timeout=30,
        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1",
             "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
    )
    th_peer.join(timeout=5)
    th_eof.join(timeout=5)
    th_cli.join(timeout=5)
    assert res.returncode == 0 and res.stdout == "CLOSURE_ASAN_OK\n" and res.stderr == "", (
        res.returncode, res.stdout, res.stderr
    )
    assert accepted[0] == 1, f"expected 1 keep-alive connection, got {accepted[0]}"


def test_security_and_soundness_hardening(cxx, env):
    d = os.path.join(TMP, "audit_hardening")
    os.makedirs(d, exist_ok=True)

    # 1. C1 + C2: value-position arena escapes, dyn-fn & scope-wrapped arena escapes,
    #    multi-module --no-check refusal, and emit-c / cc type+arena gates.
    escape_cases = [
        ("mcall_val", "fn main() -> i64 {\n  let mut out = [[0]]\n  arena { let v = [1, 2]; let _r = out.push(v) }\n  return len(out)\n}\n"),
        ("chan_val", "fn main() -> i64 {\n  let ch = chan(2)\n  arena { let v = [1]; let _r = ch.send(v) }\n  return 0\n}\n"),
        ("dyn_fn", "fn bad() {\n  let mut out = []\n  arena { let v = [1]; out.push(v) }\n  return out\n}\nfn main() -> i64 { let _x = bad(); return 0 }\n"),
        ("in_scope", "fn main() -> i64 {\n  let mut out = [[0]]\n  scope { arena { let v = [1]; out.push(v) } }\n  return len(out)\n}\n"),
    ]
    for tag, code in escape_cases:
        p = os.path.join(d, f"esc_{tag}.brat")
        with open(p, "w", encoding="utf-8") as f:
            f.write(code)
        for cmd in ([cxx, "check", p], _cxx.cli_args("check", "--py", p),
                    [cxx, "emit-c", p], [cxx, "cc", p], _cxx.cli_args("emit-c", p)):
            r = subprocess.run(cmd, capture_output=True, text=True)
            assert r.returncode != 0, (tag, cmd, r.stdout, r.stderr)

    # Multi-module arena --no-check refusal.
    m_sub = os.path.join(d, "arena_helper.brat")
    m_top = os.path.join(d, "arena_top.brat")
    with open(m_sub, "w", encoding="utf-8") as f:
        f.write("pub fn f() -> i64 { arena { let v = [1, 2]; return len(v) } }\n")
    with open(m_top, "w", encoding="utf-8") as f:
        f.write("import arena_helper\nfn main() -> i64 { return arena_helper.f() }\n")
    for cmd in ([cxx, "emit-c", "--no-check", m_top], [cxx, "cc", "--no-check", m_top],
                _cxx.cli_args("emit-c", "--no-check", m_top)):
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode != 0 and "arena" in r.stderr, (cmd, r.stderr)

    # 2. C4: multi-dimensional DWARF array size + zstd debug section + corrupted DWARF fail-closed.
    md_h = os.path.join(d, "mat.h")
    md_c = os.path.join(d, "mat.c")
    md_o = os.path.join(d, "mat.o")
    md_b = os.path.join(d, "mat.brat")
    with open(md_h, "w", encoding="utf-8") as f:
        f.write("typedef struct Mat { int m[12]; } Mat;\nint mat_sum(Mat a);\n")
    with open(md_c, "w", encoding="utf-8") as f:
        f.write("typedef struct Mat { int m[2][4]; } Mat;\nint mat_sum(Mat a) { return a.m[0][0]; }\n")
    with open(md_b, "w", encoding="utf-8") as f:
        f.write("extern struct Mat { m: [i32; 12] }\nextern fn mat_sum(a: Mat) -> i32;\nfn main() -> i64 { return 0 }\n")
    subprocess.run(["gcc", "-g", "-c", md_c, "-o", md_o], check=True)
    for cmd in ([cxx, "ffi", md_b, "--header", md_h, "--object", md_o],
                _cxx.cli_args("ffi", md_b, "--header", md_h, "--object", md_o)):
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode != 0, (cmd, r.stdout, r.stderr)

    # 3. H4: bindgen --preprocess draining >200 KiB stderr without pipe deadlock.
    noisy_h = os.path.join(d, "noisy.h")
    with open(noisy_h, "w", encoding="utf-8") as f:
        for _ in range(2500):
            f.write('#warning "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"\n')
        f.write("int noisy_ok(int x);\n")
    r_bg = subprocess.run([cxx, "bindgen", noisy_h, "--preprocess"], env=env,
                          capture_output=True, text=True, timeout=15, check=True)
    assert "noisy_ok" in r_bg.stdout

    # 4. H5: URL whitespace/CTL rejection + link-local metadata SSRF guard.
    proj_h5 = os.path.join(d, "proj_h5")
    os.makedirs(proj_h5, exist_ok=True)
    with open(os.path.join(proj_h5, "brat.toml"), "w", encoding="utf-8") as f:
        f.write('[package]\nname = "app"\nversion = "0.1.0"\n')
    for bad_u in ("http://127.0.0.1:9/bad space/x.tar.gz", "http://169.254.169.254/x.tar.gz"):
        assert subprocess.run([cxx, "pkg", "add", bad_u], cwd=proj_h5, env=env,
                              capture_output=True, text=True).returncode != 0
        assert subprocess.run(_cxx.cli_args("pkg", "add", bad_u), cwd=proj_h5,
                              capture_output=True, text=True).returncode != 0

    # 5. Deep Subsystem 1–6 & stdlib audit regressions:
    # 5a. Arena UAF via raise/catch, assert message, closure-captured `own`, and
    #     runtime parity (`vis_true({})`, `vparse_int`, `vslice`/`vzip`/`vchunks`
    #     error propagation, `ptr_write_struct` temporary drop) under ASan+UBSan+LSan.
    pt_h = os.path.join(d, "pt_lib.h")
    with open(pt_h, "w", encoding="utf-8") as f:
        f.write("typedef struct Pt { int x; int y; } Pt;\nvoid *malloc(unsigned long n);\nvoid free(void *p);\n")
    rt_src = os.path.join(d, "arena_and_rt_audit.brat")
    with open(rt_src, "w", encoding="utf-8") as f:
        f.write(
            'extern struct Pt { x: i32, y: i32 }\n'
            'extern fn malloc(n: usize) -> ptr;\n'
            'extern fn free(p: ptr);\n'
            'fn hide_any(x) { return x }\n'
            'fn rec_clo() -> bool {\n'
            '  let mut holder = []\n'
            '  let f = fn() { return len(holder) }\n'
            '  holder.push(f)\n'
            '  let mut hit = false\n'
            '  try { let _copied = own(holder) } catch e { hit = str(e).contains("deep") }\n'
            '  let _dropped = holder.pop()\n'
            '  return hit\n'
            '}\n'
            'fn main() -> i64 {\n'
            '  let mut caught1 = ""\n'
            '  try {\n'
            '    arena {\n'
            '      let s = "arena-boom-" + str(1234567890)\n'
            '      raise(s)\n'
            '    }\n'
            '  } catch e {\n'
            '    caught1 = str(e)\n'
            '  }\n'
            '  assert(caught1.contains("arena-boom-1234567890"))\n'
            '  let mut caught2 = ""\n'
            '  try {\n'
            '    arena {\n'
            '      let msg = "arena-assert-" + str(9876543210)\n'
            '      assert(false, msg)\n'
            '    }\n'
            '  } catch e {\n'
            '    caught2 = str(e)\n'
            '  }\n'
            '  assert(caught2.contains("arena-assert-9876543210"))\n'
            '  assert(rec_clo() == true)\n'
            '  let m = {}\n'
            '  let mut m_truthy = false\n'
            '  if m { m_truthy = true }\n'
            '  assert(m_truthy == false)\n'
            '  assert(unwrap("-9223372036854775808".parse_int()) == -9223372036854775807 - 1)\n'
            '  let mut chunk_err = false\n'
            '  let dyn_bad = hide_any("bad")\n'
            '  try { let _c = [1, 2].chunks(dyn_bad) } catch e { chunk_err = len(str(e)) > 0 }\n'
            '  assert(chunk_err == true)\n'
            '  assert(len([1, 2, 3].chunks(9223372036854775807)) == 1)\n'
            '  let ls = "012345678901234567890123456789"\n'
            '  let ls2 = ls.copy()\n'
            '  assert(ls == ls2)\n'
            '  assert([ls.copy()].pop() == ls)\n'
            '  assert([ls.copy()].del(0) == ls)\n'
            '  let mut iv = [[30]]\n'
            '  let _ins = iv.ins(0, [10, 20])\n'
            '  assert(iv[0][1] == 20)\n'
            '  let m1 = {"k": 1}\n'
            '  assert(m1 == {"k": 1})\n'
            '  assert((Pt { x: 3, y: 4 }) == (Pt { x: 3, y: 4 }))\n'
            '  unsafe {\n'
            '    let p = malloc(8)\n'
            '    let _w = ptr_write_struct(p, Pt { x: 11, y: 22 }, "Pt")\n'
            '    let rd = ptr_read_struct(p, "Pt")\n'
            '    assert(rd.x == 11 && rd.y == 22)\n'
            '    free(p)\n'
            '  }\n'
            '  return 0\n'
            '}\n'
        )
    r_c1 = subprocess.run([cxx, "emit-c", "--header", pt_h, rt_src], capture_output=True, text=True, check=True)
    r_c2 = subprocess.run(_cxx.cli_args("emit-c", "--header", pt_h, rt_src), capture_output=True, text=True, check=True)
    assert r_c1.stdout == r_c2.stdout, "emit-c CLI doors mismatch on arena_and_rt_audit.brat"
    rt_c = os.path.join(d, "arena_and_rt_audit.c")
    rt_exe = os.path.join(d, "arena_and_rt_audit")
    with open(rt_c, "w", encoding="utf-8") as f:
        f.write(r_c1.stdout)
    subprocess.run(
        ["gcc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         "-I", os.path.join(ROOT, "lang", "engine"), "-I", d, rt_c, "-o", rt_exe, "-lm", "-lpthread"],
        check=True,
    )
    san_env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                   UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    subprocess.run([rt_exe], env=san_env, check=True)
    subprocess.run(_cxx.cli_args("run", "--interp", "--header", pt_h, rt_src), check=True)

    # 5b. Checker parity across newly hardened arena/parfor/plugin/session/capability rules.
    more_rejects = [
        ("arena_global_set", "let mut g = [[0]]\nfn main() -> i64 {\n  arena { let v = [1]; g = [v] }\n  return 0\n}\n"),
        ("arena_scalar_chan", "fn main() -> i64 {\n  let ch = chan(2)\n  arena { let v = [1, 2]; ch.send(v) }\n  return 0\n}\n"),
        ("parfor_mut_outer", "fn add(a: i64, b: i64) -> i64 { return a + b }\nfn main() -> i64 {\n  let mut out = [0]\n  let s = par for x in [1, 2] reduce(add, 0) { out.push(x); return x }\n  return s\n}\n"),
        ("session_over_cap", "fn main() -> i64 {\n  let ch = chan(1)\n  ch.send(1)\n  ch.send(2)\n  let _a = ch.recv()\n  return 0\n}\n"),
    ]
    for tag, code in more_rejects:
        p = os.path.join(d, f"rej_{tag}.brat")
        with open(p, "w", encoding="utf-8") as f:
            f.write(code)
        r1 = subprocess.run([cxx, "check", p], capture_output=True, text=True)
        r2 = subprocess.run(_cxx.cli_args("check", "--py", p), capture_output=True, text=True)
        assert r1.returncode != 0 and r2.returncode != 0, (tag, r1.returncode, r2.returncode, r1.stderr, r2.stderr)

    # 5c. Formatter preserves raw strings `r"..."` and multi-line string bodies across C++ & Python.
    fmt_p = os.path.join(d, "fmt_raw.brat")
    fmt_in = 'fn main() -> i64 {\nlet p=r"C:\\dir\\"\nlet m="line1\n    indented_line2"\nreturn 0\n}\n'
    with open(fmt_p, "w", encoding="utf-8") as f:
        f.write(fmt_in)
    r_f1 = subprocess.run([cxx, "fmt", "--stdout", fmt_p], capture_output=True, text=True, check=True)
    assert '  let p = r"C:\\dir\\"\n' in r_f1.stdout
    assert '\n    indented_line2"\n' in r_f1.stdout
    assert '  return 0\n' in r_f1.stdout

    # 5d. `brat new` rejects ASCII control characters in project names across C++ & Python.
    assert subprocess.run([cxx, "new", "bad\x07name"], cwd=d, env=env,
                          capture_output=True, text=True).returncode != 0
    assert subprocess.run(_cxx.cli_args("new", "bad\x07name"), cwd=d,
                          env=dict(os.environ, BRAT_INTERP="1"),
                          capture_output=True, text=True).returncode != 0

    # 5e. Stdlib hardening (`bytes.from_base64` pad bits, `crypto.hmac_sha256` type check,
    #     `cli.opt_int` empty value, `time.parse_iso8601_utc` `+` sign, `http` `+` port/CL).
    std_p = os.path.join(d, "std_audit.brat")
    with open(std_p, "w", encoding="utf-8") as f:
        f.write(
            'import std.bytes\nimport std.crypto\nimport std.cli\nimport std.time\nimport std.http\n'
            'fn main() -> i64 {\n'
            '  assert(is_err(std.bytes.from_base64("AB==")))\n'
            '  assert(unwrap(std.bytes.from_base64("AQ=="))[0] == 1)\n'
            '  assert(is_err(std.crypto.hmac_sha256(["not-byte"], "msg")))\n'
            '  assert(is_err(std.cli.opt_int(["--limit="], "--limit", 10)))\n'
            '  assert(unwrap(std.cli.opt_int(["--other"], "--limit", 10)) == 10)\n'
            '  assert(is_err(std.time.parse_iso8601_utc("+202-01-01T00:00:00Z")))\n'
            '  assert(is_err(std.http.get("http://127.0.0.1:+80/")))\n'
            '  return 0\n'
            '}\n'
        )
    subprocess.run([cxx, "run", std_p], env=env, check=True)
    subprocess.run(_cxx.cli_args("run", "--interp", std_p), check=True)

    # 5f. MCP rejects non-scalar/boolean JSON-RPC `id` with -32600 across C++ & Python.
    for bad_req in ('{"jsonrpc":"2.0","id":[1],"method":"ping"}\n',
                    '{"jsonrpc":"2.0","id":true,"method":"ping"}\n'):
        r_m1 = subprocess.run([cxx, "mcp"], input=bad_req, capture_output=True, text=True, check=True)
        r_m2 = subprocess.run(_cxx.cli_args("mcp"), input=bad_req,
                              env=dict(os.environ, BRAT_INTERP="1"),
                              capture_output=True, text=True, check=True)
        assert "-32600" in r_m1.stdout and "-32600" in r_m2.stdout, (r_m1.stdout, r_m2.stdout)


def test_batch_1_2_3_regressions(cxx, env):
    d = os.path.join(TMP, "b123_regress")
    os.makedirs(d, exist_ok=True)

    # 1. Runtime/stdlib/parser/checker/emitter end-to-end execution at -O0 and -O2:
    #    - `move` parameter inside closure & SRA struct
    #    - `std.bytes.from_base64` rejecting non-ASCII UTF-8 bytes
    #    - `std.http` SSRF classifier blocking CGNAT/decimal/hex IPv4 & NAT64 IPv6
    #    - Pike-VM regex escaped `\]` inside `[...]` and rejecting trailing `\`
    #    - String interpolation with nested quotes `"{m.get("{")}"`
    #    - Nested `fn` inside `else` / `else if` blocks
    prog = os.path.join(d, "b123_rt.brat")
    with open(prog, "w", encoding="utf-8") as f:
        f.write(
            'import std.bytes\n'
            'import std.http\n'
            'struct Pair { a: str, b: i64 }\n'
            'fn sink(s: move str) -> i64 { return len(s) }\n'
            'fn main() -> i64 {\n'
            '  let clo = fn(s: move str) { return sink(move s) }\n'
            '  assert(clo(move ("hello-" + "world-long-heap-string")) == 10 + 18)\n'
            '  let p = Pair { a: "alpha-beta-gamma-long", b: 21 }\n'
            '  assert(len(p.a) + p.b == 42)\n'
            '  assert(is_err(std.bytes.from_base64("é===")))\n'
            '  assert(is_err(std.http.get("http://100.64.0.1/x")))\n'
            '  assert(is_err(std.http.get("http://2130706433/x")))\n'
            '  assert(is_err(std.http.get("http://0x7f000001/x")))\n'
            '  assert(is_err(std.http.get("http://[64:ff9b::7f00:1]/x")))\n'
            '  assert(re_match("a]b", "^[a\\\\]b]+$"))\n'
            '  let mut trailing_err = false\n'
            '  try { let _m = re_match("abc", "abc\\\\") } catch _e { trailing_err = true }\n'
            '  assert(trailing_err)\n'
            '  let m = {"{": "brace-ok"}\n'
            '  assert("{m.get("{")}" == "brace-ok")\n'
            '  if false { return 1 }\n'
            '  else {\n'
            '    fn inner_inc(v: i64) -> i64 { return v + 1 }\n'
            '    assert(inner_inc(41) == 42)\n'
            '  }\n'
            '  return 0\n'
            '}\n'
        )
    assert subprocess.run([cxx, "check", "--", prog], env=env, capture_output=True, text=True).returncode == 0
    for opt_lvl in ("-O0", "-O2"):
        r = subprocess.run([cxx, "run", opt_lvl, "--mem=64M", prog], env=env, capture_output=True, text=True)
        assert r.returncode == 0, (opt_lvl, r.stdout, r.stderr)

    # 2. Multi-field `extern union` literal rejected inside `else if` chains and closures.
    u_bad = os.path.join(d, "union_bad.brat")
    with open(u_bad, "w", encoding="utf-8") as f:
        f.write(
            'extern union U { a: i32, b: f32 }\n'
            'fn untyped(k) {\n'
            '  if k == 0 { return 0 }\n'
            '  else if k == 1 { return 1 }\n'
            '  else if k == 2 { let u = U { a: 1, b: 2.0 }; return 2 }\n'
            '  let f = fn() { if true { let u2 = U { a: 1, b: 2.0 }; return u2 } return 0 }\n'
            '  return f()\n'
            '}\n'
            'fn main() -> i64 { return 0 }\n'
        )
    ru = subprocess.run([cxx, "check", u_bad], env=env, capture_output=True, text=True)
    assert ru.returncode != 0 and ru.stdout.count("initializes 2 arms") == 2, ru.stdout

    # 3. Go untagged typedef struct alias, Rust repr(isize) + INT64_MAX enum, cyclic typedef,
    #    deep ConstExpr, bindgen -o error check, and --header-only callback struct retention.
    go_h = os.path.join(d, "only_alias.h")
    with open(go_h, "w", encoding="utf-8") as f:
        f.write("typedef struct { int a; } OnlyAlias;\nOnlyAlias id_a(OnlyAlias x);\n")
    r_go = subprocess.run([cxx, "bindgen", "--lang", "go", go_h], env=env, capture_output=True, text=True, check=True)
    assert "\tOnlyAlias = C.OnlyAlias" in r_go.stdout, r_go.stdout

    rs_f = os.path.join(d, "mode.rs")
    with open(rs_f, "w", encoding="utf-8") as f:
        f.write('#[repr(isize)]\npub enum Mode { Fast = 0, Max = 9223372036854775807 }\n'
                '#[no_mangle]\npub extern "C" fn set_mode(m: Mode) -> i32 { 0 }\n')
    r_rs = subprocess.run([cxx, "bindgen", rs_f], env=env, capture_output=True, text=True, check=True)
    assert "const Fast: i64 = 0" in r_rs.stdout and "extern fn set_mode(m: i64) -> i32;" in r_rs.stdout

    cyc_h = os.path.join(d, "cyc.h")
    with open(cyc_h, "w", encoding="utf-8") as f:
        f.write("typedef b a;\ntypedef a b;\n#define DEEP " + ("(" * 4000) + "1" + (")" * 4000) +
                "\nstruct S { int x[DEEP]; };\nvoid f(const a *p);\n")
    assert subprocess.run([cxx, "bindgen", "--inline-arrays", cyc_h], env=env, capture_output=True, text=True).returncode == 0

    r_bad_o = subprocess.run([cxx, "bindgen", go_h, "-o", os.path.join(d, "missing_dir", "out.bri")],
                             env=env, capture_output=True, text=True)
    assert r_bad_o.returncode != 0 and "cannot write" in r_bad_o.stderr

    with open(os.path.join(d, "cb_sub.h"), "w", encoding="utf-8") as f:
        f.write("struct CbData { int v; };\n")
    cb_root = os.path.join(d, "cb_root.h")
    with open(cb_root, "w", encoding="utf-8") as f:
        f.write('#include "cb_sub.h"\nvoid on_ev(void (*cb)(struct CbData));\n')
    r_cb = subprocess.run([cxx, "bindgen", "--header-only", cb_root], env=env, capture_output=True, text=True, check=True)
    assert "extern struct CbData { v: i32 }" in r_cb.stdout and "extern fn on_ev(cb: fn(CbData)) -> none;" in r_cb.stdout


def test_ten_subsystem_improvements(cxx, env):
    d = os.path.join(TMP, "ten_improvements")
    os.makedirs(d, exist_ok=True)

    # #1 (pinned str + chained vadd_self), #3 (bool + vec[f64] fast), #4 (register _rem exact budget)
    src = os.path.join(d, "perf_and_pin.brat")
    with open(src, "w", encoding="utf-8") as f:
        f.write(
            'extern fn strlen(p: ptr) -> usize\n'
            'fn pick_step(x: i64, up: bool) -> i64 {\n'
            '  if up { return x + 3 }\n'
            '  return x - 1\n'
            '}\n'
            'fn both_ok(a: bool, b: bool) -> bool {\n'
            '  return a && !b\n'
            '}\n'
            'fn scale_sum(mut xs: vec[f64], factor: f64) -> f64 {\n'
            '  for i in 0..len(xs) {\n'
            '    xs[i] = xs[i] * factor\n'
            '  }\n'
            '  let mut acc = 0.0\n'
            '  for v in xs {\n'
            '    acc = acc + v\n'
            '  }\n'
            '  return acc\n'
            '}\n'
            'fn rec_exact(n: i64) -> i64 {\n'
            '  if n == 0 { return 0 }\n'
            '  return 1 + rec_exact(n - 1)\n'
            '}\n'
            'fn main() -> i64 {\n'
            '  let mut s = "0123456789abcdef"\n'
            '  unsafe {\n'
            '    let p = ptr_of_str(s)\n'
            '    assert(strlen(p) == 16)\n'
            '  }\n'
            '  for i in 0..200 {\n'
            '    s = s + "k" + "=" + str(i) + ";"\n'
            '  }\n'
            '  assert(s.starts_with("0123456789abcdefk=0;k=1;"))\n'
            '  let mut mp = {}\n'
            '  let dyn_k = "host"\n'
            '  mp.set(dyn_k, "127.0.0.1:18080")\n'
            '  assert(len(mp.get(dyn_k)) == 15)\n'
            '  let mut hdr_wire = "GET /\\r\\n"\n'
            '  hdr_wire = hdr_wire + dyn_k + ": " + mp.get(dyn_k) + "\\r\\n"\n'
            '  assert(hdr_wire == "GET /\\r\\nhost: 127.0.0.1:18080\\r\\n")\n'
            '  assert(mp.get(dyn_k) == "127.0.0.1:18080")\n'
            '  assert(pick_step(10, true) == 13)\n'
            '  assert(pick_step(10, false) == 9)\n'
            '  assert(both_ok(true, false) == true)\n'
            '  assert(both_ok(true, true) == false)\n'
            '  let mut fv: vec[f64] = [1.5, 2.5, 4.0]\n'
            '  assert(scale_sum(fv, 2.0) == 16.0)\n'
            '  assert(rec_exact(1998) == 1998)\n'
            '  let mut caught = ""\n'
            '  try { let _bad = rec_exact(1999) } catch e { caught = e }\n'
            '  assert(caught == "recursion too deep")\n'
            '  return 0\n'
            '}\n'
        )
    em = subprocess.check_output([cxx, "emit-c", "-O2", "--unsafe-ffi", src],
                                 env=env, text=True)
    assert "_r(" in em and "bf_vfat" in em and "bf_vset_f" in em, "expected fast codegen helpers missing"
    c_p = os.path.join(d, "perf_and_pin.c")
    exe_p = os.path.join(d, "perf_and_pin.asan")
    with open(c_p, "w", encoding="utf-8") as f:
        f.write(em)
    subprocess.run(
        ["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
         f"-I{ENGINE}", c_p, "-o", exe_p, "-lm", "-lpthread"],
        check=True,
    )
    asan_env = dict(env, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1")
    subprocess.run([exe_p], env=asan_env, check=True)

    # #5 PCH caching when BRAT_CC is set
    pch_home = os.path.join(d, "pch_cache")
    pch_env = dict(env, XDG_CACHE_HOME=pch_home, BRAT_CC="gcc -O2")
    pch_env.pop("BRAT_NO_PCH", None)
    pch_env.pop("BRAT_NATIVE_CACHE_DISABLE", None)
    for k in range(2):
        p_b = os.path.join(d, f"pch_{k}.brat")
        with open(p_b, "w", encoding="utf-8") as f:
            f.write(f"print({k + 10})\n")
        subprocess.run([cxx, "build", p_b, "-o", os.path.join(d, f"pch_{k}.bin")],
                       env=pch_env, check=True)
    pch_dir = os.path.join(pch_home, "brat", "pch")
    assert os.path.isdir(pch_dir) and any(fn.endswith(".gch") or fn.endswith(".pch")
                                          for fn in os.listdir(pch_dir)), "PCH not built under BRAT_CC"

    # #6 LSP dot-completion on Enum. and chained struct fields (b.pos.)
    lsp_base = (
        "enum Color {\n  Red,\n  Green(i64),\n}\n"
        "struct Vec3 {\n  x: i64,\n  y: i64,\n}\n"
        "struct Body {\n  pos: Vec3,\n}\n"
        "fn demo(b: Body) -> i64 {\n"
    )
    lsp_doc1 = lsp_base + "  let c = Color.\n  return 0\n}\n"
    lsp_doc2 = lsp_base + "  return b.pos.\n}\n"
    uri1 = "file://" + os.path.join(d, "comp1.brat")
    uri2 = "file://" + os.path.join(d, "comp2.brat")

    def lsp_frame(obj):
        raw = json.dumps(obj).encode("utf-8")
        return f"Content-Length: {len(raw)}\r\n\r\n".encode("ascii") + raw

    payload = b"".join([
        lsp_frame({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}}),
        lsp_frame({"jsonrpc": "2.0", "method": "initialized", "params": {}}),
        lsp_frame({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                   "params": {"textDocument": {"uri": uri1, "languageId": "brat", "version": 1, "text": lsp_doc1}}}),
        lsp_frame({"jsonrpc": "2.0", "id": 2, "method": "textDocument/completion",
                   "params": {"textDocument": {"uri": uri1}, "position": {"line": 12, "character": 16}}}),
        lsp_frame({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                   "params": {"textDocument": {"uri": uri2, "languageId": "brat", "version": 1, "text": lsp_doc2}}}),
        lsp_frame({"jsonrpc": "2.0", "id": 3, "method": "textDocument/completion",
                   "params": {"textDocument": {"uri": uri2}, "position": {"line": 12, "character": 15}}}),
        lsp_frame({"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None}),
        lsp_frame({"jsonrpc": "2.0", "method": "exit", "params": None}),
    ])
    lsp_out = subprocess.run([cxx, "lsp"], input=payload, capture_output=True, timeout=30, check=True).stdout
    msgs = {}
    idx = 0
    while idx < len(lsp_out):
        hdr_end = lsp_out.find(b"\r\n\r\n", idx)
        if hdr_end < 0:
            break
        clen = int(lsp_out[idx:hdr_end].decode("ascii").split(":")[1].strip())
        body = json.loads(lsp_out[hdr_end + 4:hdr_end + 4 + clen].decode("utf-8"))
        if "id" in body:
            msgs[body["id"]] = body.get("result")
        idx = hdr_end + 4 + clen
    enum_labels = {it["label"] for it in (msgs.get(2) or [])}
    chain_labels = {it["label"] for it in (msgs.get(3) or [])}
    assert {"Red", "Green"} <= enum_labels, f"Enum dot-completion missing variants: {enum_labels}"
    assert {"x", "y"} <= chain_labels, f"Chained struct dot-completion missing fields: {chain_labels}"

    # #7 Parallel `brat test -j 2 --filter`
    tdir = os.path.join(d, "tfiles")
    os.makedirs(tdir, exist_ok=True)
    for name in ("test_alpha_1.brat", "test_alpha_2.brat", "test_skipme.brat"):
        with open(os.path.join(tdir, name), "w", encoding="utf-8") as f:
            f.write("assert(1 + 1 == 2)\n")
    tr = subprocess.run([cxx, "test", "-j", "2", "--filter", "alpha", tdir],
                        env=env, capture_output=True, text=True, check=True)
    assert "ok " in tr.stdout and "test_alpha_1.brat" in tr.stdout and "test_alpha_2.brat" in tr.stdout
    assert "test_skipme.brat" not in tr.stdout and "2 passed, 0 failed" in tr.stdout


def main():
    cxx = _cxx.build()
    env = zero_python_env()
    interp_src = os.path.join(TMP, "interp_check.brat")
    with open(interp_src, "w", encoding="utf-8") as f:
        f.write(
            'import std.json\nimport std.csv\n'
            'fn check_deep_and_clo() {\n'
            '  let mut a = []\n'
            '  let f = fn() { return 1 }\n'
            '  a.push(f)\n'
            '  assert(str(a) == "[fn]")\n'
            '  let mut cur = [1]\n'
            '  for i in 0..2005 { cur = [cur] }\n'
            '  let mut raised = false\n'
            '  try { let _c = own(cur) } catch e { raised = true }\n'
            '  assert(raised == true)\n'
            '  return 0\n'
            '}\n'
            'fn main() -> i64 {\n'
            '  let _d = check_deep_and_clo()\n'
            '  let mut s = "x"\n'
            '  for i in 0..5 { s = s + "y" }\n'
            '  assert(s == "xyyyyy")\n'
            '  assert(is_err(std.json.parse("\\"\\\\uD800\\"")))\n'
            '  assert(is_err(std.json.parse("\\"\\\\u0000\\"")))\n'
            '  assert(unwrap(std.csv.parse("a,\\"b\\r\\nc\\""))[0][1] == "b\\r\\nc")\n'
            '  return 0\n'
            '}\n'
        )
    subprocess.run(_cxx.cli_args("run", "--interp", interp_src), check=True)
    test_13_6b_native_ffi_and_bindgen(cxx, env)
    test_13_1c_native_pkg_http_targz_and_c_sources(cxx, env)
    test_13_2b2_and_compiler_leaks_under_asan(cxx)
    test_security_and_soundness_hardening(cxx, env)
    test_batch_1_2_3_regressions(cxx, env)
    test_ten_subsystem_improvements(cxx, env)
    print("(v0.7) CLOSURE SUITE ALL GREEN")


if __name__ == "__main__":
    main()
