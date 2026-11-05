"""Standard-library modules, packages with C sources, the LSP and MCP servers, and the regex engine."""
import _cxx
import _scratch
import http.server
import json
import os
import subprocess
import sys
import tarfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-eco-")

asserts = 0


def check(cond, msg):
    global asserts
    if not cond:
        raise AssertionError(msg)
    asserts += 1


# 1. Upgraded stdlib + {n,m} regex & captures/replace
#    (interpreter == ASan/LSan native, byte-identical C through both CLI doors)
class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path == "/old":
            self.send_response(302)
            self.send_header("Location", "/chunked")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/chunked"):
            hdr_val = self.headers.get("X-Client", "none")
            c1 = ("hdr=" + hdr_val + ";").encode("utf-8")
            c2 = "привет!".encode("utf-8")
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("X-Echo", "ok")
            self.end_headers()
            for part in (c1, c2):
                self.wfile.write(("%x\r\n" % len(part)).encode("ascii") + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, fmt, *args):
        pass


srv = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()

try:
    std_src = os.path.join(TMP, "std_regex.brat")
    with open(std_src, "w", encoding="utf-8") as f:
        f.write(f"""import std.bytes
import std.path
import std.fs
import std.cli
import std.json
import std.http

fn main() -> i64 {{
  // std.bytes: UTF-8, slice, index_of, Base64
  let bs = std.bytes.from_str("Hello, мир!")
  assert(unwrap(std.bytes.to_str(bs)) == "Hello, мир!")
  assert(std.bytes.index_of(bs, std.bytes.from_str("мир")) == 7)
  assert(unwrap(std.bytes.to_str(std.bytes.slice(bs, 0, 5))) == "Hello")
  let b64 = unwrap(std.bytes.to_base64(bs))
  assert(b64 == "SGVsbG8sINC80LjRgCE=")
  assert(unwrap(std.bytes.to_str(unwrap(std.bytes.from_base64(b64)))) == "Hello, мир!")
  assert(is_err(std.bytes.from_base64("bad===")))

  // std.path: with_ext, rel
  assert(std.path.with_ext("a/b/mod.brat", "bri") == "a/b/mod.bri")
  assert(std.path.with_ext("mod.brat", ".c") == "mod.c")
  assert(std.path.rel("/work/a/b", "/work/a/c/d.txt") == "../c/d.txt")
  assert(std.path.rel("src/sub", "src/sub") == ".")

  // std.fs: write_lines, write_atomic, remove
  let _wl = unwrap(std.fs.write_lines("lines.txt", ["alpha", "beta"]))
  assert(unwrap(std.fs.read_lines("lines.txt")) == ["alpha", "beta"])
  let _wa = unwrap(std.fs.write_atomic("atomic.txt", "v1"))
  let _wa2 = unwrap(std.fs.write_atomic("atomic.txt", "v2-atomic"))
  assert(std.fs.read_or("atomic.txt", "") == "v2-atomic")
  assert(!std.fs.exists("atomic.txt.brat.tmp"))
  let _rm = unwrap(std.fs.remove("lines.txt"))
  assert(!std.fs.exists("lines.txt"))

  // std.cli: --flag=bool, subcommand, subcommand_args
  let av = ["--color=false", "--jobs=4", "-v", "deploy", "--env=prod", "svc"]
  assert(!std.cli.has_flag(av, "--color"))
  assert(std.cli.has_flag(av, "--jobs"))
  assert(std.cli.has_flag(av, "-v"))
  assert(std.cli.subcommand(av, []) == "deploy")
  assert(std.cli.subcommand_args(av, []) == ["--env=prod", "svc"])

  // std.json: \\uXXXX + surrogate pair, stringify_indent, get_path, typed getters
  let raw_j = "{{\\"rocket\\": \\"\\\\uD83D\\\\uDE80\\", \\"euro\\": \\"\\\\u20AC\\", \\"cfg\\": {{\\"port\\": 8080, \\"tls\\": true}}, \\"tags\\": [\\"x\\", \\"y\\"]}}"
  let doc = unwrap(std.json.parse(raw_j))
  assert(std.json.get_str(doc, "rocket", "") == "🚀")
  assert(std.json.get_str(doc, "euro", "") == "€")
  assert(unwrap(std.json.get_path(doc, "cfg.port")) == 8080)
  assert(unwrap(std.json.get_path(doc, "tags.1")) == "y")
  assert(is_none(std.json.get_path(doc, "tags.9")))
  assert(std.json.get_int(doc.get("cfg"), "port", 0) == 8080)
  assert(std.json.get_bool(doc.get("cfg"), "tls", false))
  assert(is_err(std.json.parse("\\"\\\\uD800\\"")))
  let pretty = std.json.stringify_indent({{"a": [1, 2]}}, 2)
  assert(pretty.contains("\\n  \\"a\\": [\\n    1,"))

  // std.http: query encode/parse, request_ex custom headers + chunked, get_follow redirects
  let qs = std.http.encode_query({{"q": "hello мир", "p": "1"}})
  let pq = unwrap(std.http.parse_query("?" + qs))
  assert(pq.get("q") == "hello мир" && pq.get("p") == "1")
  let rx = unwrap(std.http.request_ex("GET", "http://127.0.0.1:{port}/chunked", {{"X-Client": "brat02"}}, ""))
  assert(rx.get("status") == 200)
  assert(rx.get("headers").get("x-echo") == "ok")
  assert(rx.get("body") == "hdr=brat02;привет!")
  let rf = unwrap(std.http.get_follow("http://127.0.0.1:{port}/old", 3))
  assert(rf.get("status") == 200 && rf.get("body") == "hdr=none;привет!")

  // bounded regex {{n,m}}, re_find, re_captures, re_replace
  assert(re_match("2026-11-01", "^\\\\d\\{{4\\}}-\\\\d\\{{2\\}}-\\\\d\\{{2\\}}$"))
  assert(!re_match("26-11-01", "^\\\\d\\{{4\\}}-\\\\d\\{{2\\}}-\\\\d\\{{2\\}}$"))
  assert(re_match("aaaa", "^a{{2,4}}$") && !re_match("aaaaa", "^a{{2,4}}$"))
  assert(re_match("aaaaa", "^a{{3,}}$") && !re_match("aa", "^a{{3,}}$"))
  assert(unwrap(re_find("id=abc-987!", "[a-z]+-\\\\d\\{{3\\}}")) == "abc-987")
  assert(is_none(re_find("nope", "\\\\d\\{{4\\}}")))
  let caps = unwrap(re_captures("2026-11-01", "(\\\\d\\{{4\\}})-(\\\\d\\{{2\\}})-(\\\\d\\{{2\\}})"))
  assert(caps == ["2026-11-01", "2026", "11", "01"])
  let rep = re_replace("2026-11-01 & 2027-01-05", "(\\\\d\\{{4\\}})-(\\\\d\\{{2\\}})-(\\\\d\\{{2\\}})", "$3/$2/$1 ($$)")
  assert(rep == "01/11/2026 ($) & 05/01/2027 ($)")

  print("W2_STD_REGEX_OK")
  return 0
}}
""")

    emit_py = subprocess.check_output(_cxx.cli_args("emit-c", std_src), text=True, timeout=60)
    emit_cx = subprocess.check_output([BIN, "cc", std_src], text=True, timeout=60)
    check(emit_py == emit_cx, "Python CLI/native cc drift on stdlib/regex")

    c_path = os.path.join(TMP, "std_regex.c")
    exe_path = os.path.join(TMP, "std_regex.asan")
    with open(c_path, "w", encoding="utf-8") as f:
        f.write(emit_cx)
    comp = subprocess.run(
        ["gcc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
         f"-I{ENGINE}", c_path, "-o", exe_path, "-lm", "-lpthread"],
        capture_output=True, text=True, timeout=90,
    )
    check(comp.returncode == 0, "ASan compile failed: " + comp.stderr)

    ri = subprocess.run(_cxx.cli_args("run", "--interp", std_src),
                        capture_output=True, text=True, cwd=TMP, timeout=60)
    rn = subprocess.run([exe_path], capture_output=True, text=True, cwd=TMP, timeout=60,
                        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1"})
    check(ri.returncode == 0 and ri.stdout == "W2_STD_REGEX_OK\n",
          f"interp failed: {ri.returncode} {ri.stdout} {ri.stderr}")
    check(rn.returncode == 0 and rn.stdout == "W2_STD_REGEX_OK\n" and rn.stderr == "",
          f"native ASan failed: {rn.returncode} {rn.stdout} {rn.stderr}")
finally:
    srv.shutdown()
    srv.server_close()


# Zero-flag `c_sources` compilation in `brat pkg` + `.bratbind`
pkg_build_dir = os.path.join(TMP, "csrc_pkg_src")
os.makedirs(pkg_build_dir, exist_ok=True)
hdr_p = os.path.join(pkg_build_dir, "hashx.h")
c_p = os.path.join(pkg_build_dir, "hashx.c")
brat_p = os.path.join(pkg_build_dir, "hashx.brat")
bri_p = os.path.join(pkg_build_dir, "hashx.bri")
bind_p = os.path.join(pkg_build_dir, "hashx.bratbind")
manifest_p = os.path.join(pkg_build_dir, "brat.pkg.json")

with open(hdr_p, "w", encoding="utf-8") as f:
    f.write("#include <stdint.h>\nint64_t hashx_fold(int64_t a, int64_t b);\n")
with open(c_p, "w", encoding="utf-8") as f:
    f.write('#include "hashx.h"\nint64_t hashx_fold(int64_t a, int64_t b) { return (a * 31) + (b * 17) + 9; }\n')
with open(brat_p, "w", encoding="utf-8") as f:
    f.write("extern fn hashx_fold(a: i64, b: i64) -> i64;\npub fn fold(x: i64, y: i64) -> i64 {\n  return hashx_fold(x, y)\n}\n")
with open(bri_p, "w", encoding="utf-8") as f:
    f.write("fn fold(x: i64, y: i64) -> i64\n")
with open(manifest_p, "w", encoding="utf-8") as f:
    json.dump({"name": "hashx", "version": "0.2.0", "c_sources": ["hashx.c"]}, f)

eb = subprocess.run(_cxx.cli_args("ffi", brat_p, "--header", hdr_p, "--emit-bind", bind_p),
                    capture_output=True, text=True, timeout=30)
check(eb.returncode == 0 and os.path.isfile(bind_p), "emit-bind failed: " + eb.stderr)

# Package contains ONLY portable source + .bratbind — NO precompiled .o!
tgz_p = os.path.join(TMP, "hashx-0.2.0.tar.gz")
with tarfile.open(tgz_p, "w:gz") as tf:
    for fn in ("brat.pkg.json", "hashx.h", "hashx.c", "hashx.brat", "hashx.bri", "hashx.bratbind"):
        tf.add(os.path.join(pkg_build_dir, fn), arcname=fn)

proj = os.path.join(TMP, "csrc_consumer")
os.makedirs(proj, exist_ok=True)
r_add = subprocess.run(_cxx.cli_args("pkg", "add", "file://" + tgz_p),
                       capture_output=True, text=True, cwd=proj, timeout=60)
check(r_add.returncode == 0, "pkg add c_sources failed: " + r_add.stdout + r_add.stderr)

lock_data = json.loads(open(os.path.join(proj, "brat.lock"), encoding="utf-8").read())
check(lock_data[0].get("c_sources") == ["hashx/hashx.c"], f"c_sources not locked: {lock_data}")

app_p = os.path.join(proj, "app.brat")
with open(app_p, "w", encoding="utf-8") as f:
    f.write("import hashx.hashx\nfn main() -> i64 {\n  print(hashx.hashx.fold(10, 5))\n  return 0\n}\n")

exe_py = os.path.join(proj, "app_py")
exe_cx = os.path.join(proj, "app_cx")
# Zero --link, zero --header, zero --unsafe-ffi!
bpy = subprocess.run(_cxx.cli_args("build", app_p, "-o", exe_py),
                     capture_output=True, text=True, cwd=proj, timeout=60)
bcx = subprocess.run([BIN, "build", app_p, "-o", exe_cx],
                     capture_output=True, text=True, cwd=proj, timeout=60)
rcx = subprocess.run([BIN, "run", app_p],
                     capture_output=True, text=True, cwd=proj, timeout=60)
check(bpy.returncode == 0, "brat build zero-flag c_sources failed: " + bpy.stderr)
check(bcx.returncode == 0, "brat-cxx build zero-flag c_sources failed: " + bcx.stderr)
check(rcx.returncode == 0 and rcx.stdout == "404\n", f"brat-cxx run c_sources failed: {rcx.stdout} {rcx.stderr}")
check(subprocess.check_output([exe_py], text=True) == "404\n", "exe_py output mismatch")
check(subprocess.check_output([exe_cx], text=True) == "404\n", "exe_cx output mismatch")

# Tampering with vendored C file must fail both `pkg verify` and `build`!
vendored_c = os.path.join(proj, "vendor", "hashx", "hashx.c")
orig_c = open(vendored_c, "rb").read()
with open(vendored_c, "ab") as f:
    f.write(b"\n/* tampered */\n")
tamper_py = subprocess.run(_cxx.cli_args("build", app_p, "-o", exe_py),
                           capture_output=True, text=True, cwd=proj, timeout=30)
tamper_cx = subprocess.run([BIN, "build", app_p, "-o", exe_cx],
                           capture_output=True, text=True, cwd=proj, timeout=30)
check(tamper_py.returncode != 0 and "HASH MISMATCH" in tamper_py.stderr,
      f"Python build missed tampered C source: {tamper_py.stderr}")
check(tamper_cx.returncode != 0 and "HASH MISMATCH" in tamper_cx.stderr,
      f"C++ build missed tampered C source: {tamper_cx.stderr}")
open(vendored_c, "wb").write(orig_c)


# Full LSP (hover, completion, references, rename, std def) & MCP
lsp_file = os.path.join(proj, "lsp_demo.brat")
lsp_src = """import std.json
import hashx.hashx

extern fn c_tick(n: c_long) -> i64;

fn helper(val: i64) -> i64 {
  // helper in comment must not count as reference
  let msg = "helper in string"
  let _m = len(msg)
  return val + 1
}

fn main() -> i64 {
  let _j = std.json.parse("{}")
  let out = helper(10) + helper(20)
  return out + hashx.hashx.fold(1, 2)
}
"""
with open(lsp_file, "w", encoding="utf-8") as f:
    f.write(lsp_src)

lsp = subprocess.Popen(_cxx.cli_args("lsp"), stdin=subprocess.PIPE,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def lsp_send(payload):
    b = json.dumps(payload).encode("utf-8")
    lsp.stdin.write(b"Content-Length: %d\r\n\r\n" % len(b) + b)
    lsp.stdin.flush()


def lsp_read():
    hdrs = {}
    while True:
        ln = lsp.stdout.readline()
        if not ln or ln in (b"\r\n", b"\n"):
            break
        k, v = ln.decode("ascii").split(":", 1)
        hdrs[k.strip().lower()] = v.strip()
    if "content-length" not in hdrs:
        return None
    return json.loads(lsp.stdout.read(int(hdrs["content-length"])).decode("utf-8"))


def lsp_req(mid, method, params):
    lsp_send({"jsonrpc": "2.0", "id": mid, "method": method, "params": params})
    while True:
        m = lsp_read()
        if m and m.get("id") == mid:
            return m


try:
    init = lsp_req(1, "initialize", {"processId": None, "rootUri": "file://" + proj, "capabilities": {}})
    caps = init["result"]["capabilities"]
    check(caps.get("hoverProvider") is True and "completionProvider" in caps
          and caps.get("referencesProvider") is True and caps.get("renameProvider") is True,
          f"missing LSP capabilities: {caps}")
    lsp_send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    uri = "file://" + lsp_file
    lsp_send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
              "params": {"textDocument": {"uri": uri, "languageId": "brat", "version": 1, "text": lsp_src}}})
    # Cross-module definition into std.json.parse (`line 13`, `parse` col 21)
    d_std = lsp_req(2, "textDocument/definition",
                    {"textDocument": {"uri": uri}, "position": {"line": 13, "character": 21}})
    check(d_std["result"] and d_std["result"]["uri"].endswith("lang/std/json.brat"),
          f"std.json.parse definition failed: {d_std}")
    # Cross-module definition into vendor/hashx/hashx.brat (`line 15`, `fold` col 28)
    d_ven = lsp_req(3, "textDocument/definition",
                    {"textDocument": {"uri": uri}, "position": {"line": 15, "character": 28}})
    check(d_ven["result"] and d_ven["result"]["uri"].endswith("vendor/hashx/hashx.brat"),
          f"vendored definition failed: {d_ven}")
    # Hover on `std.json.parse` includes .bri contract note
    h_std = lsp_req(4, "textDocument/hover",
                    {"textDocument": {"uri": uri}, "position": {"line": 13, "character": 21}})
    h_val = h_std["result"]["contents"]["value"]
    check("pub fn parse(s: str)" in h_val and "json.bri" in h_val, f"std hover: {h_val}")
    # Hover on `extern fn c_tick` includes C ABI layout note for c_long
    h_ext = lsp_req(5, "textDocument/hover",
                    {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 12}})
    check("LP64" in h_ext["result"]["contents"]["value"], f"extern hover: {h_ext}")
    # Completion on `std.json.`
    comp_doc = lsp_src + "\n// std.json."
    lsp_send({"jsonrpc": "2.0", "method": "textDocument/didChange",
              "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": comp_doc}]}})
    comp = lsp_req(6, "textDocument/completion",
                   {"textDocument": {"uri": uri}, "position": {"line": 18, "character": 12}})
    comp_labels = {it["label"] for it in comp["result"]}
    check({"parse", "stringify", "stringify_indent", "get_path"} <= comp_labels,
          f"std.json completions missing items: {comp_labels}")
    # References on `helper` (1 decl + 2 calls = 3; comment & string ignored!)
    refs = lsp_req(7, "textDocument/references",
                   {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 4},
                    "context": {"includeDeclaration": True}})
    check(len(refs["result"]) == 3, f"expected 3 token refs for helper, got: {refs}")
    refs_nodecl = lsp_req(8, "textDocument/references",
                          {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 4},
                           "context": {"includeDeclaration": False}})
    check(len(refs_nodecl["result"]) == 2, f"expected 2 non-decl refs, got: {refs_nodecl}")
    # Rename `helper` -> `compute_step`
    ren = lsp_req(9, "textDocument/rename",
                  {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 4},
                   "newName": "compute_step"})
    check(len(ren["result"]["changes"][uri]) == 3, f"rename edits: {ren}")
    bad_ren = lsp_req(10, "textDocument/rename",
                      {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 4},
                       "newName": "return"})
    check("error" in bad_ren, f"keyword rename must fail: {bad_ren}")
    lsp_req(11, "shutdown", {})
    lsp_send({"jsonrpc": "2.0", "method": "exit", "params": {}})
finally:
    lsp.kill()
    lsp.wait()

# MCP new tools: brat_abi_matrix & brat_pkg_verify
mcp = subprocess.Popen(_cxx.cli_args("mcp"), stdin=subprocess.PIPE,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    for req in (
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
         "params": {"name": "brat_abi_matrix", "arguments": {"path": brat_p, "header": hdr_p}}},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
         "params": {"name": "brat_pkg_verify", "arguments": {"dir": proj}}},
    ):
        mcp.stdin.write((json.dumps(req) + "\n").encode("utf-8"))
    mcp.stdin.flush()
    r1 = json.loads(mcp.stdout.readline().decode("utf-8"))
    r2 = json.loads(mcp.stdout.readline().decode("utf-8"))
    r3 = json.loads(mcp.stdout.readline().decode("utf-8"))
    check(r2["result"]["isError"] is False, f"MCP abi_matrix failed: {r2}")
    mat = json.loads(r2["result"]["content"][0]["text"])
    check(mat.get("ok") is True and len(mat.get("targets", [])) == 5, f"abi_matrix payload: {mat}")
    check(r3["result"]["isError"] is False, f"MCP pkg_verify failed: {r3}")
    ver_body = json.loads(r3["result"]["content"][0]["text"])
    check("verified 1 package(s)" in ver_body["stdout"], f"pkg_verify payload: {ver_body}")
finally:
    mcp.kill()
    mcp.wait()

print(f"ECOSYSTEM ALL GREEN ({asserts} asserts)")
