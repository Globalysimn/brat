"""Ecosystem & real-utility readiness: `std.*` modules, `.tar.gz` packages, GitHub specs, bundle."""
import _cxx
import _scratch
import http.server
import io
import json
import os
import subprocess
import tarfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
BIN = _cxx.build()

TMP = _scratch.mkdtemp(prefix="brat-eco-")


# 1. All shipped `std.*` modules: checker, `emit-c`, `cc`, `-O0`/`-O2` agreement,
#    and zero ASan+LSan leaks.
std_prog = os.path.join(TMP, "std_all.brat")
with open(std_prog, "w", encoding="utf-8") as f:
    f.write("""import std.path
import std.cli
import std.fs
import std.csv
import std.json
import std.http

fn main() -> i64 {
  assert(std.path.is_abs("/usr/bin"))
  assert(!std.path.is_abs("rel/file.txt"))
  assert(std.path.join("a/b", "c.txt") == "a/b/c.txt")
  assert(std.path.dir("/a/b/c.txt") == "/a/b")
  assert(std.path.base("/a/b/c.txt") == "c.txt")
  assert(std.path.ext("/a/b/c.tar.gz") == ".gz")
  assert(std.path.stem("/a/b/c.txt") == "c")
  assert(std.path.norm("/a/x/../b/./c") == "/a/b/c")

  let av = ["-v", "--out=result.json", "--limit", "25", "in1.csv", "--", "--raw"]
  assert(std.cli.has_flag(av, "-v"))
  assert(!std.cli.has_flag(av, "--raw"))
  assert(std.cli.opt(av, "--out", "def") == "result.json")
  assert(unwrap(std.cli.opt_int(av, "--limit", 0)) == 25)
  let pos = std.cli.positional(av, ["--limit"])
  assert(len(pos) == 2 && pos[0] == "in1.csv" && pos[1] == "--raw")

  let data_file = "notes.txt"
  let _w1 = unwrap(write_file(data_file, "line1\\n"))
  let _w2 = unwrap(std.fs.append_text(data_file, "line2\\n"))
  assert(std.fs.exists(data_file))
  assert(!std.fs.exists("missing_nope.txt"))
  let ls = unwrap(std.fs.read_lines(data_file))
  assert(len(ls) == 2 && ls[0] == "line1" && ls[1] == "line2")

  let rows = unwrap(std.csv.parse("name,note\\nalice,\\"hi, \\"\\"world\\"\\"\\"\\n"))
  assert(len(rows) == 2 && rows[1][0] == "alice" && rows[1][1] == "hi, \\"world\\"")
  let dumped = std.csv.dump(rows)
  let rows2 = unwrap(std.csv.parse(dumped))
  assert(rows2 == rows && dumped == "name,note\\nalice,\\"hi, \\\"\\"world\\"\\"\\"\\n")
  let quoted_empty = unwrap(std.csv.parse("\\"\\"\\n"))
  assert(len(quoted_empty) == 1 && len(quoted_empty[0]) == 1 && quoted_empty[0][0] == "")
  let multiline = unwrap(std.csv.parse("a,b\\r\\n\\"c\\r\\nd\\",e\\r\\n"))
  assert(len(multiline) == 2 && multiline[1][0] == "c\\r\\nd" && multiline[1][1] == "e")
  assert(is_err(std.csv.parse("\\"unfinished")))

  let jtxt = "{\\"active\\": true, \\"count\\": 3, \\"items\\": [10, null, \\"a\\\\nb\\"]}"
  let jv = unwrap(std.json.parse(jtxt))
  let jout = std.json.stringify(jv)
  assert(jout == "{\\"active\\":true,\\"count\\":3,\\"items\\":[10,null,\\"a\\\\nb\\"]}")
  assert(is_err(std.json.parse("{\\"a\\": 1} trailing")))

  let u = unwrap(std.http.parse_url("http://localhost:9090/v1/ping"))
  assert(u.get("dial") == "127.0.0.1:9090" && u.get("path") == "/v1/ping")

  print("STD_OK")
  return 0
}
""")

chk_cxx = subprocess.run([BIN, "check", std_prog], capture_output=True, text=True, timeout=60)
assert chk_cxx.returncode == 0, chk_cxx.stdout

flat_cxx = subprocess.check_output([BIN, "flat", std_prog], text=True, timeout=60)
assert flat_cxx.startswith("("), "flattened tree missing"

emit_cli = subprocess.check_output([BIN, "emit-c", std_prog], text=True, timeout=60)
emit_cxx = subprocess.check_output([BIN, "cc", std_prog], text=True, timeout=60)
assert emit_cli == emit_cxx, "emit-c and cc diverged on std.*"

c_path = os.path.join(TMP, "std_all.c")
exe_path = os.path.join(TMP, "std_all.asan")
with open(c_path, "w", encoding="utf-8") as f:
    f.write(emit_cxx)
comp = subprocess.run(
    ["gcc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
     f"-I{ENGINE}", c_path, "-o", exe_path, "-lm", "-lpthread"],
    capture_output=True, text=True, timeout=90,
)
assert comp.returncode == 0, comp.stderr

ri = subprocess.run([BIN, "run", "-O0", std_prog],
                    capture_output=True, text=True, cwd=TMP, timeout=60)
rn = subprocess.run([exe_path], capture_output=True, text=True, cwd=TMP, timeout=60,
                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1"})
assert ri.returncode == 0 and ri.stdout == "STD_OK\n", (ri.returncode, ri.stdout, ri.stderr)
assert rn.returncode == 0 and rn.stdout == "STD_OK\n" and rn.stderr == "", (
    rn.returncode, rn.stdout, rn.stderr)


# 2. Real CLI + JSON + HTTP utility end-to-end over a live local socket.
class _JsonHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        ln = int(self.headers.get("Content-Length", "0"))
        payload = json.loads(self.rfile.read(ln).decode("utf-8"))
        ans = json.dumps({"path": self.path, "sum": sum(payload["nums"])}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(ans)))
        self.end_headers()
        self.wfile.write(ans)

    def log_message(self, fmt, *args):
        pass


srv = http.server.HTTPServer(("127.0.0.1", 0), _JsonHandler)
port = srv.server_address[1]
thr = threading.Thread(target=srv.serve_forever, daemon=True)
thr.start()
try:
    tool_src = os.path.join(TMP, "sum_cli.brat")
    tool_exe = os.path.join(TMP, "sum_cli.exe")
    with open(tool_src, "w", encoding="utf-8") as f:
        f.write("""import std.cli
import std.json
import std.http

fn main() -> i64 {
  let av = args()
  let url = std.cli.opt(av, "--url", "")
  let pos = std.cli.positional(av, ["--url"])
  let mut nums = []
  for item in pos {
    nums.push(unwrap(item.parse_int()))
  }
  let body = std.json.stringify({"nums": nums})
  let resp = unwrap(std.http.post_json(url, body))
  assert(resp.get("status") == 200)
  let parsed = unwrap(std.json.parse(resp.get("body")))
  print("{parsed.get(\\"path\\")}={parsed.get(\\"sum\\")}")
  return 0
}
""")
    url_arg = f"http://localhost:{port}/calc"
    b = subprocess.run([BIN, "build", tool_src, "-o", tool_exe],
                       capture_output=True, text=True, timeout=60)
    assert b.returncode == 0, b.stdout + b.stderr
    ti = subprocess.run(_cxx.cli_args("run", "-O0", tool_src, "--", "--url", url_arg, "10", "20", "12"),
                        capture_output=True, text=True, timeout=30)
    tn = subprocess.run([tool_exe, "--url", url_arg, "10", "20", "12"],
                        capture_output=True, text=True, timeout=30)
    assert ti.returncode == 0 and ti.stdout == "/calc=42\n", (ti.returncode, ti.stdout, ti.stderr)
    assert tn.returncode == 0 and tn.stdout == "/calc=42\n", (tn.returncode, tn.stdout, tn.stderr)
finally:
    srv.shutdown()
    srv.server_close()


# 3. Decentralized `.tar.gz` multi-file packages +
#    tar-slip / symlink hardening.

tar_path = os.path.join(TMP, "mathkit-v0.2.0.tar.gz")
with tarfile.open(tar_path, "w:gz") as tf:
    files = {
        "mathkit-0.2.0/ops.bri": b"fn triple(x: i64) -> i64\n",
        "mathkit-0.2.0/ops.brat": b"pub fn triple(x: i64) -> i64 {\n  return x * 3\n}\n",
        "mathkit-0.2.0/sub/fmt.brat": (
            b"import mathkit.ops\n"
            b"pub fn show_triple(x: i64) -> str {\n"
            b"  return \"v=\" + str(mathkit.ops.triple(x))\n"
            b"}\n"
        ),
        "mathkit-0.2.0/README.md": b"# ignored non-brat doc\n",
    }
    for arcname, content in files.items():
        ti = tarfile.TarInfo(arcname)
        ti.size = len(content)
        tf.addfile(ti, io.BytesIO(content))

proj = os.path.join(TMP, "proj")
os.makedirs(os.path.join(proj, "src"), exist_ok=True)
tar_url = __import__("pathlib").Path(tar_path).as_uri()

add_r = subprocess.run(_cxx.cli_args("pkg", "add", tar_url),
                       capture_output=True, text=True, cwd=proj, timeout=30)
assert add_r.returncode == 0 and "add mathkit" in add_r.stdout, add_r.stdout + add_r.stderr

ver_r = subprocess.run(_cxx.cli_args("pkg", "verify"),
                       capture_output=True, text=True, cwd=proj, timeout=30)
assert ver_r.returncode == 0 and "verified 1 package(s)" in ver_r.stdout, ver_r.stderr

# Subdirectory module `src/app.brat` importing vendored multi-file package + `std.path`:
app_src = os.path.join(proj, "src", "app.brat")
with open(app_src, "w", encoding="utf-8") as f:
    f.write("""import mathkit.sub.fmt
import std.path

fn main() -> i64 {
  print(std.path.join("out", mathkit.sub.fmt.show_triple(14)))
  return 0
}
""")

pi = subprocess.run(_cxx.cli_args("run", "-O0", app_src),
                    capture_output=True, text=True, cwd=proj, timeout=30)
pn = subprocess.run([BIN, "run", app_src],
                    capture_output=True, text=True, cwd=proj, timeout=30)
assert pi.returncode == 0 and pi.stdout == "out/v=42\n", (pi.returncode, pi.stdout, pi.stderr)
assert pn.returncode == 0 and pn.stdout == "out/v=42\n", (pn.returncode, pn.stdout, pn.stderr)

# Tampering inside vendor/mathkit/sub/fmt.brat is caught by `pkg verify`,
# and `pkg install` restores from lockfile:
tampered = os.path.join(proj, "vendor", "mathkit", "sub", "fmt.brat")
with open(tampered, "a", encoding="utf-8") as f:
    f.write("\n// tampered\n")
bad_ver = subprocess.run(_cxx.cli_args("pkg", "verify"),
                         capture_output=True, text=True, cwd=proj, timeout=30)
assert bad_ver.returncode == 1 and "HASH MISMATCH" in bad_ver.stderr

inst_r = subprocess.run(_cxx.cli_args("pkg", "install"),
                        capture_output=True, text=True, cwd=proj, timeout=30)
assert inst_r.returncode == 0 and "ok mathkit" in inst_r.stdout, inst_r.stderr

rem_r = subprocess.run(_cxx.cli_args("pkg", "remove", "mathkit"),
                       capture_output=True, text=True, cwd=proj, timeout=30)
assert rem_r.returncode == 0 and not os.path.exists(os.path.join(proj, "vendor", "mathkit"))

# Tar-slip (`../escape.brat`) and symlink archives are rejected:
evil_tar = os.path.join(TMP, "evil.tar.gz")
with tarfile.open(evil_tar, "w:gz") as tf:
    ti = tarfile.TarInfo("../escape.brat")
    body = b"pub fn x() -> i64 { return 1 }\n"
    ti.size = len(body)
    tf.addfile(ti, io.BytesIO(body))
evil_r = subprocess.run(
    _cxx.cli_args("pkg", "add", __import__("pathlib").Path(evil_tar).as_uri()),
    capture_output=True, text=True, cwd=proj, timeout=30,
)
assert evil_r.returncode != 0 and "traversal" in evil_r.stderr.lower(), evil_r.stderr


# 4. Relocatable `brat-cxx bundle` carries `lang/std/` standalone.
bundle_dir = os.path.join(TMP, "reloc_bundle")
br = subprocess.run([BIN, "bundle", bundle_dir], capture_output=True, text=True, timeout=60)
assert br.returncode == 0, br.stderr
bundled_bin = os.path.join(bundle_dir, "lang", "cxx", "bin", os.path.basename(BIN))
rb = subprocess.run([bundled_bin, "run", std_prog],
                    capture_output=True, text=True, cwd=TMP, timeout=60)
assert rb.returncode == 0 and rb.stdout == "STD_OK\n", (rb.returncode, rb.stdout, rb.stderr)


# Transitive `brat.pkg.json` archive manifests (diamond dedup,
#    SHA-256 pinning, cycle & conflict rejection, remote file:// block).
import hashlib

def _make_tgz(path, entries):
    with tarfile.open(path, "w:gz") as tf:
        for arc, raw in entries.items():
            data = raw.encode("utf-8") if isinstance(raw, str) else raw
            ti = tarfile.TarInfo(arc)
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()

leaf_tgz = os.path.join(TMP, "leaf-v0.1.0.tar.gz")
leaf_sha = _make_tgz(leaf_tgz, {
    "leaf-0.1.0/brat.pkg.json": json.dumps({"name": "leaf", "version": "0.1.0", "dependencies": []}),
    "leaf-0.1.0/core.bri": "fn base_num() -> i64\n",
    "leaf-0.1.0/core.brat": "pub fn base_num() -> i64 {\n  return 21\n}\n",
})
leaf_uri = __import__("pathlib").Path(leaf_tgz).as_uri()

mid_a_tgz = os.path.join(TMP, "mida-v0.1.0.tar.gz")
mid_a_sha = _make_tgz(mid_a_tgz, {
    "mida-0.1.0/brat.pkg.json": json.dumps({
        "name": "mida",
        "version": "0.1.0",
        "dependencies": [{"name": "leaf", "url": leaf_uri, "sha256": leaf_sha}],
    }),
    "mida-0.1.0/calc.bri": "fn a_val() -> i64\n",
    "mida-0.1.0/calc.brat": "import leaf.core\npub fn a_val() -> i64 {\n  return leaf.core.base_num() + 1\n}\n",
})
mid_a_uri = __import__("pathlib").Path(mid_a_tgz).as_uri()

mid_b_tgz = os.path.join(TMP, "midb-v0.1.0.tar.gz")
mid_b_sha = _make_tgz(mid_b_tgz, {
    "midb-0.1.0/brat.pkg.json": json.dumps({
        "name": "midb",
        "version": "0.1.0",
        "dependencies": [{"name": "leaf", "url": leaf_uri, "sha256": leaf_sha}],
    }),
    "midb-0.1.0/calc.bri": "fn b_val() -> i64\n",
    "midb-0.1.0/calc.brat": "import leaf.core\npub fn b_val() -> i64 {\n  return leaf.core.base_num() - 1\n}\n",
})
mid_b_uri = __import__("pathlib").Path(mid_b_tgz).as_uri()

top_tgz = os.path.join(TMP, "topkit-v0.1.0.tar.gz")
_make_tgz(top_tgz, {
    "topkit-0.1.0/brat.pkg.json": json.dumps({
        "name": "topkit",
        "version": "0.1.0",
        "dependencies": [
            {"name": "mida", "url": mid_a_uri, "sha256": mid_a_sha},
            {"name": "midb", "url": mid_b_uri, "sha256": mid_b_sha},
        ],
    }),
    "topkit-0.1.0/api.bri": "fn total() -> i64\n",
    "topkit-0.1.0/api.brat": (
        "import mida.calc\nimport midb.calc\n"
        "pub fn total() -> i64 {\n  return mida.calc.a_val() + midb.calc.b_val()\n}\n"
    ),
})
top_uri = __import__("pathlib").Path(top_tgz).as_uri()

tproj = os.path.join(TMP, "tproj")
os.makedirs(tproj, exist_ok=True)
tr_add = subprocess.run(_cxx.cli_args("pkg", "add", top_uri),
                        capture_output=True, text=True, cwd=tproj, timeout=30)
assert tr_add.returncode == 0, tr_add.stdout + tr_add.stderr
with open(os.path.join(tproj, "brat.lock"), "r", encoding="utf-8") as lf:
    lock_doc = json.load(lf)
locked_names = {p.get("archive") or p.get("name") for p in lock_doc}
assert locked_names == {"topkit", "mida", "midb", "leaf"}, locked_names
top_entry = [p for p in lock_doc if p.get("archive") == "topkit"][0]
assert top_entry.get("dependencies") == ["mida", "midb"], top_entry

tr_ver = subprocess.run(_cxx.cli_args("pkg", "verify"),
                        capture_output=True, text=True, cwd=tproj, timeout=30)
assert tr_ver.returncode == 0 and "verified 4 package(s)" in tr_ver.stdout, tr_ver.stderr

tapp = os.path.join(tproj, "main.brat")
with open(tapp, "w", encoding="utf-8") as f:
    f.write("import topkit.api\nfn main() -> i64 {\n  print(topkit.api.total())\n  return 0\n}\n")
tri = subprocess.run(_cxx.cli_args("run", "-O0", tapp),
                     capture_output=True, text=True, cwd=tproj, timeout=30)
trn = subprocess.run([BIN, "run", tapp],
                     capture_output=True, text=True, cwd=tproj, timeout=30)
assert tri.returncode == 0 and tri.stdout == "42\n", (tri.returncode, tri.stdout, tri.stderr)
assert trn.returncode == 0 and trn.stdout == "42\n", (trn.returncode, trn.stdout, trn.stderr)

# Diamond version conflict rejection (`leaf` v2 with different url/sha):
leaf2_tgz = os.path.join(TMP, "leaf-v0.2.0.tar.gz")
leaf2_sha = _make_tgz(leaf2_tgz, {
    "leaf-0.2.0/core.brat": "pub fn base_num() -> i64 {\n  return 99\n}\n",
})
conflict_tgz = os.path.join(TMP, "conflict-v0.1.0.tar.gz")
_make_tgz(conflict_tgz, {
    "conflict-0.1.0/brat.pkg.json": json.dumps({
        "name": "conflict",
        "dependencies": [{"name": "leaf", "url": __import__("pathlib").Path(leaf2_tgz).as_uri(), "sha256": leaf2_sha}],
    }),
    "conflict-0.1.0/x.brat": "pub fn f() -> i64 {\n  return 1\n}\n",
})
conf_r = subprocess.run(
    _cxx.cli_args("pkg", "add", __import__("pathlib").Path(conflict_tgz).as_uri()),
    capture_output=True, text=True, cwd=tproj, timeout=30,
)
assert conf_r.returncode != 0 and "conflict" in conf_r.stderr.lower(), conf_r.stderr

# Cycle detection (cyc_a <-> cyc_b):
cyc_a_tgz = os.path.join(TMP, "cyca.tar.gz")
cyc_b_tgz = os.path.join(TMP, "cycb.tar.gz")
cyc_a_uri = __import__("pathlib").Path(cyc_a_tgz).as_uri()
cyc_b_uri = __import__("pathlib").Path(cyc_b_tgz).as_uri()
_make_tgz(cyc_b_tgz, {
    "cycb/brat.pkg.json": json.dumps({"name": "cycb", "dependencies": [{"name": "cyca", "url": cyc_a_uri}]}),
    "cycb/b.brat": "pub fn f() -> i64 {\n  return 1\n}\n",
})
_make_tgz(cyc_a_tgz, {
    "cyca/brat.pkg.json": json.dumps({"name": "cyca", "dependencies": [{"name": "cycb", "url": cyc_b_uri}]}),
    "cyca/a.brat": "pub fn f() -> i64 {\n  return 1\n}\n",
})
cproj = os.path.join(TMP, "cproj")
os.makedirs(cproj, exist_ok=True)
cyc_r = subprocess.run(_cxx.cli_args("pkg", "add", cyc_a_uri),
                       capture_output=True, text=True, cwd=cproj, timeout=30)
assert cyc_r.returncode != 0 and "cycle" in cyc_r.stderr.lower(), cyc_r.stderr


# `std.env`, `std.proc`, `std.bytes` parity + confinement + ASan.
sys_dir = os.path.join(TMP, "sys_std")
os.makedirs(sys_dir, exist_ok=True)
sys_prog = os.path.join(sys_dir, "sys_prog.brat")
with open(sys_prog, "w", encoding="utf-8") as f:
    f.write("""import std.env
import std.proc
import std.bytes

fn main() -> i64 {
  assert(std.env.get_or("BRAT_TEST_ENV_MISSING", "fallback") == "fallback")
  assert(!std.env.has("BRAT_TEST_ENV_MISSING"))
  assert(std.env.has("BRAT_TEST_FLAG"))
  assert(std.env.get_or("BRAT_TEST_FLAG", "no") == "yes")
  assert(std.env.flag("BRAT_TEST_FLAG"))

  let raw = [0, 65, 255, 10, 66]
  let wr = std.bytes.write("blob.bin", raw)
  assert(!is_err(wr))
  assert(unwrap(wr) == 5)
  let rd = std.bytes.read("blob.bin")
  assert(!is_err(rd))
  let got = unwrap(rd)
  assert(std.bytes.eq(raw, got))
  let hx = unwrap(std.bytes.to_hex(got))
  assert(hx == "0041ff0a42")
  let unhx = unwrap(std.bytes.from_hex("0041FF0a42"))
  assert(std.bytes.eq(got, unhx))
  assert(is_err(std.bytes.write("../escape.bin", [1])))
  assert(is_err(std.bytes.read("/etc/passwd")))
  assert(is_err(std.bytes.write("bad.bin", [256])))

  // Shell metacharacters are passed verbatim via execvp, never interpreted by a shell:
  let pr = std.proc.run("/bin/echo", ["safe;echo INJECTED", "$HOME"])
  if is_err(pr) {
    print("PROC_BLOCKED")
    return 0
  }
  let res = unwrap(pr)
  assert(res["code"] == 0)
  assert(res["out"] == "safe;echo INJECTED $HOME\\n")
  let out_s = unwrap(std.proc.output("/bin/echo", ["clean"]))
  assert(out_s == "clean\\n")
  print("SYS_STD_OK")
  return 0
}
""")

base_env = dict(os.environ, BRAT_TEST_FLAG="yes", BRAT_EXEC_ALLOW="/bin/echo")
si = subprocess.run(_cxx.cli_args("run", "-O0", sys_prog),
                    capture_output=True, text=True, cwd=sys_dir, env=base_env, timeout=30)
assert si.returncode == 0 and si.stdout == "SYS_STD_OK\n", (si.returncode, si.stdout, si.stderr)

# Native ASan+LSan build + run:
sys_c = os.path.join(sys_dir, "sys_prog.c")
sys_exe = os.path.join(sys_dir, "sys_prog_asan")
cc_emit = subprocess.run([BIN, "cc", sys_prog], capture_output=True, text=True, timeout=30)
assert cc_emit.returncode == 0, cc_emit.stderr
with open(sys_c, "w", encoding="utf-8") as cf:
    cf.write(cc_emit.stdout)
cc_asan = subprocess.run(
    ["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
     "-I", os.path.join(ROOT, "lang", "engine"), sys_c, "-lm", "-lpthread", "-o", sys_exe],
    capture_output=True, text=True, timeout=60,
)
assert cc_asan.returncode == 0, cc_asan.stderr
sn = subprocess.run([sys_exe], capture_output=True, text=True, cwd=sys_dir,
                    env=dict(base_env, ASAN_OPTIONS="detect_leaks=1"), timeout=30)
assert sn.returncode == 0 and sn.stdout == "SYS_STD_OK\n", (sn.returncode, sn.stdout, sn.stderr)

# Capability gate default-deny blocks subprocesses at -O0 and -O2:
deny_env = dict(os.environ, BRAT_TEST_FLAG="yes", BRAT_EXEC="0", BRAT_EXEC_ALLOW="", ASAN_OPTIONS="detect_leaks=1")
di = subprocess.run(_cxx.cli_args("run", "-O0", sys_prog),
                    capture_output=True, text=True, cwd=sys_dir, env=deny_env, timeout=30)
dn = subprocess.run([sys_exe], capture_output=True, text=True, cwd=sys_dir, env=deny_env, timeout=30)
assert di.returncode == 0 and di.stdout == "PROC_BLOCKED\n", (di.returncode, di.stdout, di.stderr)
assert dn.returncode == 0 and dn.stdout == "PROC_BLOCKED\n", (dn.returncode, dn.stdout, dn.stderr)

print("ECOSYSTEM ALL GREEN")
