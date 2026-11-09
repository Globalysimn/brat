"""Regression suite for security, runtime memory/bounds, 16-byte Val ABI,
LSP malformed UTF-8 resilience, FFI gate parity, and decorative-flag cleanup."""
import _cxx
import _scratch
import _toolchain as tc_mod

import hashlib
import http.server
import io
import json
import os
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-cli-diag-")
BIN = _cxx.build()


# 1. Decorative non-enforcing flags (--effects, --proto, --caps, --ownership) are gone.
ok_file = os.path.join(TMP, "ok.brat")
with open(ok_file, "w", encoding="utf-8") as f:
    f.write("fn main() -> i64 {\n  return 0\n}\n")

for flag in ("--effects", "--proto", "--caps", "--ownership"):
    rc_cxx = subprocess.run([BIN, "check", flag, ok_file],
                            capture_output=True, text=True, timeout=30)
    assert rc_cxx.returncode == 2, (flag, rc_cxx.returncode, rc_cxx.stderr)


# 2. SSRF / redirect / file:// scheme hardening in native `brat pkg` HTTP client.
secret_file = os.path.join(TMP, "secret.txt")
with open(secret_file, "w", encoding="utf-8") as f:
    f.write("pub fn leak() -> i64 { return 1 }\n")
secret_sha = hashlib.sha256(open(secret_file, "rb").read()).hexdigest()


class _RedirectHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/index-file.json":
            doc = {"packages": {"acme/sec": {"1.0": {"url": "file://" + secret_file, "sha256": secret_sha}}}}
            raw = json.dumps(doc).encode("utf-8")
            self.send_response(200)
            self.end_headers()
            self.wfile.write(raw)
        elif self.path == "/index-redir.json":
            doc = {"packages": {"acme/sec": {"1.0": {"url": "http://127.0.0.1:%d/to-file" % self.server.server_port, "sha256": secret_sha}}}}
            raw = json.dumps(doc).encode("utf-8")
            self.send_response(200)
            self.end_headers()
            self.wfile.write(raw)
        elif self.path == "/index-cred.json":
            doc = {"packages": {"acme/sec": {"1.0": {"url": "http://127.0.0.1:%d/to-cred" % self.server.server_port, "sha256": secret_sha}}}}
            raw = json.dumps(doc).encode("utf-8")
            self.send_response(200)
            self.end_headers()
            self.wfile.write(raw)
        elif self.path == "/to-file":
            self.send_response(302)
            self.send_header("Location", "file://" + secret_file)
            self.end_headers()
        elif self.path == "/to-cred":
            self.send_response(302)
            self.send_header("Location", "http://admin:hunter2@127.0.0.1:9/x")
            self.end_headers()
        else:
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"ok")

    def log_message(self, *_args):
        pass


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _RedirectHandler)
port = srv.server_address[1]
th = threading.Thread(target=srv.serve_forever, daemon=True)
th.start()
pkg_ssrf_dir = os.path.join(TMP, "pkg_ssrf")
os.makedirs(pkg_ssrf_dir, exist_ok=True)
try:
    for idx_path in ("/index-file.json", "/index-redir.json", "/index-cred.json"):
        env = dict(os.environ,
                   BRAT_INDEX="http://127.0.0.1:%d%s" % (port, idx_path),
                   BRAT_PKG_ALLOW_INTERNAL="1")
        r = subprocess.run([BIN, "pkg", "add", "acme/sec@1.0"],
                           cwd=pkg_ssrf_dir, env=env, capture_output=True, text=True, timeout=30)
        assert r.returncode != 0, "pkg add allowed unsafe remote index %s" % idx_path
finally:
    srv.shutdown()
    srv.server_close()


# 3. MCP framing (Content-Length + JSONL), header cap, and brat_build `-o` safety via native brat-cxx mcp.
init_msg = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}).encode("utf-8")
framed = b"Content-Length: %d\r\n\r\n%s" % (len(init_msg), init_msg)
r_cl = subprocess.run([BIN, "mcp"], input=framed, capture_output=True, timeout=15)
assert r_cl.returncode == 0 and b'"id":1' in r_cl.stdout, r_cl.stdout

r_jl = subprocess.run([BIN, "mcp"], input=init_msg + b"\n", capture_output=True, timeout=15)
assert r_jl.returncode == 0 and b'"id":1' in r_jl.stdout, r_jl.stdout

many_hdrs = b"".join(b"X-H%d: v\r\n" % i for i in range(100)) + b"\r\n{}"
r_hdr = subprocess.run([BIN, "mcp"], input=many_hdrs, capture_output=True, timeout=15)
assert r_hdr.returncode == 0 and b'"code":-32700' in r_hdr.stdout, r_hdr.stdout

bad_build_msg = json.dumps({
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/call",
    "params": {"name": "brat_build", "arguments": {"path": ok_file, "out": "-amalicious"}},
}).encode("utf-8")
r_bad_out = subprocess.run([BIN, "mcp"], input=init_msg + b"\n" + bad_build_msg + b"\n",
                           capture_output=True, timeout=15)
out_txt = r_bad_out.stdout.decode("utf-8", errors="replace")
assert r_bad_out.returncode == 0 and '"isError":true' in out_txt and "'-'" in out_txt, out_txt


# 4. Runtime fixes: >511B vtxt concat, HOF + arena sort_by + nested sort_by + trace.span under ASan+LSan.
rt_prog = os.path.join(TMP, "rt_audit.brat")
with open(rt_prog, "w", encoding="utf-8") as f:
    f.write('''fn main() -> i64 {
  let big = "a".repeat(700)
  let joined = big + 42
  assert(len(joined) == 702)

  let v = [3, 1, 4, 1, 5]
  let f = fn(x) { return x > 3 }
  assert(v.any(f))
  assert(!v.all(f))
  assert([10, 20, 30].fold(0, fn(a, b) { return a + b }) == 60)

  let found = ["aa", "bbb", "cc"].find(fn(s) { return len(s) == 3 })
  assert(found == "bbb")

  let mut pairs = [[2, 9], [1, 8], [3, 7]]
  arena {
    let tag = "k"
    pairs.sort_by(fn(p) {
      let mut inner = [20, 10]
      inner.sort_by(fn(z) { return z })
      return len(tag) * p[0] + inner[0]
    })
  }
  assert(pairs[0][0] == 1 && pairs[2][0] == 3)

  let sp = trace.span("op", fn() { return 7 })
  assert(sp == 7)

  try {
    arena {
      let xs = [10, 20]
      let _oom = xs[99]
    }
  } catch err {
    assert(err.contains("range"))
  }

  print("RT_OK")
  return 0
}
''')

ip = subprocess.run([BIN, "run", "-O0", rt_prog], capture_output=True, text=True, timeout=30)
assert ip.returncode == 0 and "RT_OK" in ip.stdout, (ip.returncode, ip.stdout, ip.stderr)

em = subprocess.run([BIN, "cc", rt_prog], capture_output=True, text=True, timeout=30)
assert em.returncode == 0, em.stderr
c_path = os.path.join(TMP, "rt_audit.c")
exe_path = os.path.join(TMP, "rt_audit.exe")
with open(c_path, "w", encoding="utf-8") as f:
    f.write(em.stdout)
cc = tc_mod.cc_cached(
    ["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
     "-fno-sanitize-recover=all", "-I", ENGINE, c_path, "-o", exe_path, "-lm", "-lpthread"],
    c_path, exe_path, timeout=90)
assert cc.returncode == 0, cc.stderr
rn = subprocess.run([exe_path], capture_output=True, text=True,
                    env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"), timeout=30)
assert rn.returncode == 0 and "RT_OK" in rn.stdout and rn.stderr == "", (
    rn.returncode, rn.stdout, rn.stderr)


# 5. brat-cxx FFI gate refuses unverified externs unless --unsafe-ffi (or --header via CLI).
ffi_prog = os.path.join(TMP, "unverified_ffi.brat")
with open(ffi_prog, "w", encoding="utf-8") as f:
    f.write('extern fn puts(s: str) -> i32\nfn main() -> i64 {\n  return 0\n}\n')
ffi_exe = os.path.join(TMP, "unverified_ffi.exe")
refused_build = subprocess.run([BIN, "build", ffi_prog, "-o", ffi_exe],
                               capture_output=True, text=True, timeout=30)
assert refused_build.returncode != 0 and "--header" in refused_build.stderr, (
    refused_build.returncode, refused_build.stderr)
allowed_build = subprocess.run([BIN, "build", "--unsafe-ffi", ffi_prog, "-o", ffi_exe],
                               capture_output=True, text=True, timeout=60)
assert allowed_build.returncode == 0, allowed_build.stderr


# 6. brat-cxx multi-file `check` sees signatures from the first file when checking the second.
mod_a = os.path.join(TMP, "a.brat")
mod_b = os.path.join(TMP, "b.brat")
with open(mod_a, "w", encoding="utf-8") as f:
    f.write("pub fn takes_int(x: i64) -> i64 {\n  return x + 1\n}\n")
with open(mod_b, "w", encoding="utf-8") as f:
    f.write('fn main() -> i64 {\n  return takes_int("wrong")\n}\n')
mf = subprocess.run([BIN, "check", mod_a, mod_b], capture_output=True, text=True, timeout=30)
assert mf.returncode == 1 and "type error" in mf.stdout, (mf.returncode, mf.stdout, mf.stderr)


# 7. LSP server survives truncated / malformed UTF-8 in didChange without crashing.
def _lsp_frame(obj):
    raw = json.dumps(obj).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n%s" % (len(raw), raw)


bad_utf8_doc = b"fn main() -> i64 {\n  // \x80\xf0\n  return 0\n}\n"
did_open = (
    b'{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":'
    b'{"uri":"file:///t.brat","version":1,"text":"'
    + bad_utf8_doc.replace(b'"', b'\\"').replace(b"\n", b"\\n") + b'"}}}'
)
lsp_in = b"".join([
    _lsp_frame({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}),
    f"Content-Length: {len(did_open)}\r\n\r\n".encode("ascii") + did_open,
    _lsp_frame({
        "jsonrpc": "2.0",
        "method": "textDocument/didChange",
        "params": {
            "textDocument": {"uri": "file:///t.brat", "version": 2},
            "contentChanges": [{
                "range": {"start": {"line": 1, "character": 5},
                          "end": {"line": 1, "character": 7}},
                "text": "ok",
            }],
        },
    }),
    _lsp_frame({"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None}),
    _lsp_frame({"jsonrpc": "2.0", "method": "exit", "params": None}),
])
lsp_run = subprocess.run([BIN, "lsp"], input=lsp_in, capture_output=True, timeout=30)
assert lsp_run.returncode == 0 and b'"id":2' in lsp_run.stdout, (
    lsp_run.returncode, lsp_run.stdout[:300], lsp_run.stderr[:300])

print("CLI DIAGNOSTICS ALL GREEN")
