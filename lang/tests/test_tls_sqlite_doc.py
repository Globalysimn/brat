"""HTTPS in `std.http`, `std.sqlite`, the embedded FFI contract table with `brat doc`,
and LSP signature help and workspace symbols."""
import _cxx
import _scratch

import json
import os
import socket
import ssl
import subprocess
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-eco-")

asserts = 0


def check(cond, msg):
    global asserts
    assert cond, msg
    asserts += 1


def write(rel, content):
    p = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(content)
    return p


def run_cli(*args, cwd=None):
    return subprocess.run(_cxx.cli_args(*(list(args))),
                          capture_output=True, text=True, cwd=cwd, timeout=120)


# `https://` TLS + `std.time` + `std.crypto` (-O0, -O2, ASan)
cert_pem = os.path.join(TMP, "tls_app", "ca_cert.pem")
key_pem = os.path.join(TMP, "tls_app", "ca_key.pem")
os.makedirs(os.path.dirname(cert_pem), exist_ok=True)
subprocess.run(
    ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
     "-keyout", key_pem, "-out", cert_pem, "-days", "2",
     "-subj", "/CN=localhost",
     "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
    check=True, capture_output=True, timeout=30
)

srv_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
srv_ctx.load_cert_chain(certfile=cert_pem, keyfile=key_pem)
listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", 0))
listener.listen(8)
https_port = listener.getsockname()[1]


def _https_server():
    for _ in range(4):
        try:
            conn, _ = listener.accept()
        except OSError:
            break
        try:
            with srv_ctx.wrap_socket(conn, server_side=True) as tls_conn:
                tls_conn.recv(4096)
                body = '{"tls":"verified","wave":3}'
                resp = (
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    f"Content-Length: {len(body)}\r\n"
                    "Connection: close\r\n\r\n" + body
                )
                tls_conn.sendall(resp.encode("utf-8"))
        except Exception:
            try:
                conn.close()
            except OSError:
                pass


srv_thread = threading.Thread(target=_https_server, daemon=True)
srv_thread.start()

step71_src = write("tls_app/app.brat", f'''import std.http
import std.time
import std.crypto

fn main() -> i64 {{
  // 1. Untrusted self-signed cert without CA must be rejected by TLS verification
  let bad = http.get("https://localhost:{https_port}/ping")
  assert(is_err(bad))

  // 2. Trusted CA succeeds over real TLS 1.3 handshake + SNI + SAN check
  let ok_r = http.get_ca("https://localhost:{https_port}/ping", "ca_cert.pem")
  assert(!is_err(ok_r))
  let resp = unwrap(ok_r)
  assert(resp["status"] == 200)
  print(resp["body"])

  // 3. std.time exact Hinnant Euclidean calendar bijection & monotonic clock
  let m0 = time.now_mono_ns()
  let _sl = time.sleep(1)
  let m1 = time.now_mono_ns()
  assert(m1 >= m0)
  assert(time.days_from_civil(1970, 1, 1) == 0)
  assert(time.days_from_civil(2000, 2, 29) == 11016)
  let iso = time.format_iso8601_utc(1793491200)
  print(iso)
  assert(unwrap(time.parse_iso8601_utc(iso)) == 1793491200)

  // 4. std.crypto FIPS 180-4 SHA-256 + RFC 4231 Test Case 2 HMAC-SHA256 + CSPRNG
  print(unwrap(crypto.sha256_hex("")))
  print(unwrap(crypto.sha256_hex("abc")))
  print(unwrap(crypto.hmac_sha256_hex("Jefe", "what do ya want for nothing?")))
  assert(crypto.constant_time_eq("secret-token-123", "secret-token-123"))
  assert(!crypto.constant_time_eq("secret-token-123", "secret-token-124"))
  let rnd = unwrap(crypto.rand_bytes(16))
  assert(len(rnd) == 16)
  return 0
}}
''')

r71_i = run_cli("run", "-O0", step71_src)
check(r71_i.returncode == 0, f"run -O0 failed: {r71_i.stdout}\n{r71_i.stderr}")

c71 = run_cli("emit-c", step71_src)
check(c71.returncode == 0, f"emit-c failed: {c71.stderr}")
c71_path = os.path.join(TMP, "tls_app", "app.c")
exe71 = os.path.join(TMP, "tls_app", "app_asan.exe")
with open(c71_path, "w", encoding="utf-8") as f:
    f.write(c71.stdout)
subprocess.run(
    ["gcc", "-std=c99", "-O1", "-g", "-fsanitize=address,leak",
     "-I", ENGINE, c71_path, "-lpthread", "-ldl", "-lm", "-o", exe71],
    check=True, timeout=60
)
r71_n = subprocess.run([exe71], capture_output=True, text=True, timeout=30)
listener.close()
check(r71_n.returncode == 0, f"native ASan failed: {r71_n.stdout}\n{r71_n.stderr}")
check(r71_i.stdout == r71_n.stdout, f"parity mismatch:\nI:{r71_i.stdout}\nN:{r71_n.stdout}")
lines71 = r71_n.stdout.strip().splitlines()
check(lines71[0] == '{"tls":"verified","wave":3}', f"TLS body: {lines71}")
check(lines71[1] == "2026-11-01T00:00:00Z", f"ISO-8601: {lines71}")
check(lines71[2] == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", f"SHA256(''): {lines71}")
check(lines71[3] == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", f"SHA256('abc'): {lines71}")
check(lines71[4] == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", f"HMAC RFC4231: {lines71}")
print("ok: https:// TLS + std.time + std.crypto + ASan")


# A corrupt TLS record after valid application data is an error, not EOF.
corrupt_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
corrupt_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
corrupt_listener.bind(("127.0.0.1", 0))
corrupt_listener.listen(2)
corrupt_port = corrupt_listener.getsockname()[1]


def _corrupt_tls_server():
    for _ in range(2):
        try:
            conn, _ = corrupt_listener.accept()
        except OSError:
            break
        try:
            tls_conn = srv_ctx.wrap_socket(conn, server_side=True)
            tls_conn.recv(4096)
            tls_conn.sendall(b"HTTP/1.0 200 OK\r\n\r\npartial")
            raw_conn = socket.socket(fileno=tls_conn.detach())
            raw_conn.sendall(b"\x17\x03\x03\x00\x10" + b"X" * 16)
            raw_conn.close()
        except Exception:
            try:
                conn.close()
            except OSError:
                pass


threading.Thread(target=_corrupt_tls_server, daemon=True).start()
corrupt_src = write("tls_app/corrupt.brat", f'''fn main() -> i64 {{
  let r = https_request_raw("127.0.0.1:{corrupt_port}", "localhost", "GET / HTTP/1.0\\r\\n\\r\\n", "ca_cert.pem")
  assert(is_err(r))
  print("TLS_CORRUPT_REJECTED")
  return 0
}}
''')
r_corrupt_i = run_cli("run", "-O0", corrupt_src)
check(r_corrupt_i.returncode == 0, f"run -O0 corrupt TLS failed: {r_corrupt_i.stdout}\n{r_corrupt_i.stderr}")
check(r_corrupt_i.stdout.strip() == "TLS_CORRUPT_REJECTED", f"run -O0 corrupt TLS: {r_corrupt_i.stdout}")
c_corrupt = run_cli("emit-c", corrupt_src)
check(c_corrupt.returncode == 0, f"corrupt TLS emit-c failed: {c_corrupt.stderr}")
c_corrupt_path = os.path.join(TMP, "tls_app", "corrupt.c")
exe_corrupt = os.path.join(TMP, "tls_app", "corrupt_asan.exe")
with open(c_corrupt_path, "w", encoding="utf-8") as f:
    f.write(c_corrupt.stdout)
subprocess.run(
    ["gcc", "-std=c99", "-O1", "-g", "-fsanitize=address,leak",
     "-I", ENGINE, c_corrupt_path, "-lpthread", "-ldl", "-lm", "-o", exe_corrupt],
    check=True, timeout=60
)
r_corrupt_n = subprocess.run([exe_corrupt], capture_output=True, text=True, timeout=30)
corrupt_listener.close()
check(r_corrupt_n.returncode == 0, f"native corrupt TLS failed: {r_corrupt_n.stdout}\n{r_corrupt_n.stderr}")
check(r_corrupt_n.stdout.strip() == "TLS_CORRUPT_REJECTED", f"native corrupt TLS: {r_corrupt_n.stdout}")
print("ok: corrupt TLS record rejected after partial response")


# `std.sqlite` parameterized queries, SQLi safety & confinement
step72_src = write("sql_app/db_test.brat", '''import std.sqlite

fn main() -> i64 {
  // Confinement check: escaping program directory must be refused
  let esc = sqlite.open("../escaped.db")
  assert(is_err(esc))

  let db = unwrap(sqlite.open("local_items.db"))
  let _e1 = unwrap(sqlite.exec(db, "DROP TABLE IF EXISTS users; CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, score REAL, note TEXT)"))
  let sqli = "Robert'); DROP TABLE users;--"
  let _q1 = unwrap(sqlite.query(db, "INSERT INTO users (name, score, note) VALUES (?, ?, ?)", ["Alice", 98.5, None]))
  let _q2 = unwrap(sqlite.query(db, "INSERT INTO users (name, score, note) VALUES (?, ?, ?)", [sqli, 42.0, "safe"]))

  let cnt = unwrap(sqlite.scalar_int(db, "SELECT COUNT(*) AS c FROM users", []))
  print(cnt)
  let rows = unwrap(sqlite.query(db, "SELECT name, score, note FROM users WHERE score > ? ORDER BY id", [10.0]))
  print(len(rows))
  print(rows[0]["name"])
  print(is_none(rows[0]["note"]))
  print(rows[1]["name"])
  let _cl = unwrap(sqlite.close(db))
  assert(is_err(sqlite.close(db)))
  return 0
}
''')

r72_i = run_cli("run", "-O0", step72_src)
check(r72_i.returncode == 0, f"run -O0 failed: {r72_i.stdout}\n{r72_i.stderr}")

# Remove db file so native starts clean too
db_file = os.path.join(TMP, "sql_app", "local_items.db")
if os.path.exists(db_file):
    os.remove(db_file)

c72 = run_cli("emit-c", step72_src)
c72_path = os.path.join(TMP, "sql_app", "db_test.c")
exe72 = os.path.join(TMP, "sql_app", "db_asan.exe")
with open(c72_path, "w", encoding="utf-8") as f:
    f.write(c72.stdout)
subprocess.run(
    ["gcc", "-std=c99", "-O1", "-g", "-fsanitize=address,leak",
     "-I", ENGINE, c72_path, "-lpthread", "-ldl", "-lm", "-o", exe72],
    check=True, timeout=60
)
r72_n = subprocess.run([exe72], capture_output=True, text=True, timeout=30)
check(r72_n.returncode == 0, f"native ASan failed: {r72_n.stdout}\n{r72_n.stderr}")
check(r72_i.stdout == r72_n.stdout, f"parity:\nI:{r72_i.stdout}\nN:{r72_n.stdout}")
check("Robert'); DROP TABLE users;--" in r72_n.stdout, f"SQLi row missing: {r72_n.stdout}")
print("ok: std.sqlite + confinement + ASan")


# Standalone `brat-cxx build` `brat_ffi_contract[]` + `brat doc`
write("cxx_ffi/mylib.h", '''#pragma once
#include <stdint.h>
typedef struct Vec2i {
    int32_t x;
    int32_t y;
} Vec2i;
int32_t vec2_sum(Vec2i v);
''')
csrc73 = write("cxx_ffi/mylib.c", '''#include "mylib.h"
int32_t vec2_sum(Vec2i v) { return v.x + v.y; }
''')
obj73 = os.path.join(TMP, "cxx_ffi", "mylib.o")
subprocess.run(["gcc", "-c", "-O2", csrc73, "-o", obj73], check=True)

# Documented FFI + module file
brat73 = write("cxx_ffi/app.brat", '''// 2D integer vector shared with C.
extern struct Vec2i {
  x: i32,
  y: i32,
}

// Pure C sum over Vec2i.
extern fn vec2_sum(v: Vec2i) -> i32 pure nothrow

/// Public wrapper computing scaled vector sum.
pub fn scaled_sum(a: i32, b: i32) -> i64 {
  let v = {"x": a, "y": b}
  return vec2_sum(v) * 2
}

fn main() -> i64 {
  print(scaled_sum(10, 11))
  return 0
}
''')
exe73 = os.path.join(TMP, "cxx_ffi", "direct_cxx.exe")
b73 = subprocess.run(
    [BIN, "build", "--unsafe-ffi", "--dynamic", "--link", obj73, brat73, "-o", exe73],
    capture_output=True, text=True, timeout=60
)
check(b73.returncode == 0, f"Direct brat-cxx build failed: {b73.stdout}\n{b73.stderr}")
run73 = subprocess.run([exe73], capture_output=True, text=True, timeout=30)
check(run73.returncode == 0 and run73.stdout.strip() == "42", f"run73: {run73.stdout}")

# Verify `brat ffi --bin` reads the contract embedded directly by `brat-cxx build`!
ver73 = run_cli("ffi", brat73, "--bin", exe73)
check(ver73.returncode == 0 and "contract matches" in ver73.stdout,
      f"brat ffi --bin on brat-cxx binary failed: {ver73.stdout}\n{ver73.stderr}")

# Verify `brat doc` in both JSON and Markdown modes
doc_json = run_cli("doc", brat73, "--json")
check(doc_json.returncode == 0, f"brat doc --json failed: {doc_json.stderr}")
dj = json.loads(doc_json.stdout)
items_by_name = {it["name"]: it for it in dj["modules"][0]["items"]}
check("Vec2i" in items_by_name and "2D integer vector" in items_by_name["Vec2i"]["doc"], f"doc Vec2i: {items_by_name}")
check("vec2_sum" in items_by_name and "pure" in items_by_name["vec2_sum"]["effects"], f"doc vec2_sum: {items_by_name}")
check("scaled_sum" in items_by_name and "scaled vector sum" in items_by_name["scaled_sum"]["doc"], f"doc scaled_sum: {items_by_name}")

doc_md = run_cli("doc", os.path.join(ROOT, "lang", "std", "time.brat"))
check(doc_md.returncode == 0 and "# Module `time`" in doc_md.stdout and "days_from_civil" in doc_md.stdout,
      f"brat doc markdown failed: {doc_md.stdout[:300]}")
print("ok: standalone brat-cxx FFI contract + brat doc")


# LSP `signatureHelp` + `workspace/symbol` + MCP `brat_doc`/`brat_opt_proofs`
class RpcClient:
    def __init__(self, argv):
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.seq = 1

    def send(self, msg):
        raw = json.dumps(msg).encode("utf-8")
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(raw) + raw)
        self.p.stdin.flush()

    def recv(self):
        hdrs = {}
        while True:
            ln = self.p.stdout.readline()
            if not ln or ln in (b"\r\n", b"\n"):
                break
            k, v = ln.decode("utf-8").split(":", 1)
            hdrs[k.strip().lower()] = v.strip()
        if "content-length" not in hdrs:
            return None
        return json.loads(self.p.stdout.read(int(hdrs["content-length"])))

    def req(self, method, params=None):
        mid = self.seq
        self.seq += 1
        self.send({"jsonrpc": "2.0", "id": mid, "method": method, "params": params or {}})
        while True:
            m = self.recv()
            if m is None or m.get("id") == mid:
                return m

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


lsp = RpcClient([BIN, "lsp"])
l_init = lsp.req("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
caps = l_init["result"]["capabilities"]
check("signatureHelpProvider" in caps and caps.get("workspaceSymbolProvider") is True, f"LSP caps: {caps}")

lsp_file = write("lsp_ws/main.brat", '''import std.time

pub fn compute_metric(alpha: i64, beta: str, gamma: f64) -> i64 {
  return alpha
}

fn main() -> i64 {
  let a = compute_metric(10, "hi", 3.14)
  let d = time.days_from_civil(2026, 11, 1)
  return a + d
}
''')
lsp_uri = "file://" + lsp_file
with open(lsp_file, encoding="utf-8") as f:
    lsp_txt = f.read()
lsp.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
          "params": {"textDocument": {"uri": lsp_uri, "languageId": "brat", "version": 1, "text": lsp_txt}}})

# Cursor after second comma in `compute_metric(10, "hi", ` -> activeParameter == 2
sh1 = lsp.req("textDocument/signatureHelp",
              {"textDocument": {"uri": lsp_uri}, "position": {"line": 7, "character": 35}})
check(sh1["result"] is not None and sh1["result"]["activeParameter"] == 2, f"sigHelp local: {sh1}")
check("gamma: f64" in sh1["result"]["signatures"][0]["label"], f"sigHelp label: {sh1}")

# Cursor after first comma in `time.days_from_civil(2026, ` -> activeParameter == 1
sh2 = lsp.req("textDocument/signatureHelp",
              {"textDocument": {"uri": lsp_uri}, "position": {"line": 8, "character": 37}})
check(sh2["result"] is not None and sh2["result"]["activeParameter"] == 1, f"sigHelp std.time: {sh2}")

# workspace/symbol query across open workspace + lang/std
ws = lsp.req("workspace/symbol", {"query": "compute_metric"})
check(any(s["name"] == "compute_metric" for s in ws["result"]), f"workspace/symbol local: {ws}")
ws_std = lsp.req("workspace/symbol", {"query": "days_from_civil"})
check(any(s["name"] == "days_from_civil" for s in ws_std["result"]), f"workspace/symbol std: {ws_std}")
lsp.req("shutdown")
lsp.send({"jsonrpc": "2.0", "method": "exit"})
lsp.close()

# MCP `brat_doc` and `brat_opt_proofs`
opt_sample = write("mcp_opt/sample.brat", '''fn safe_half(n: i64) -> i64 {
  if n >= 0 {
    return n / 2
  }
  return 0
}
''')
mcp = RpcClient(_cxx.cli_args("mcp"))
mcp.req("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}})
mcp.send({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})
md_res = mcp.req("tools/call", {"name": "brat_doc", "arguments": {"path": brat73}})
check(md_res["result"]["isError"] is False and "scaled_sum" in md_res["result"]["content"][0]["text"], f"mcp doc: {md_res}")
mp_res = mcp.req("tools/call", {"name": "brat_opt_proofs", "arguments": {"path": opt_sample, "level": "O1"}})
mp_json = json.loads(mp_res["result"]["content"][0]["text"])
check(mp_res["result"]["isError"] is False and mp_json["count"] == 1, f"mcp opt proofs: {mp_json}")
mcp.close()
print("ok: LSP signatureHelp/workspaceSymbol + MCP doc/opt_proofs")

print(f"ECOSYSTEM ALL GREEN ({asserts} asserts)")
