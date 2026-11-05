#!/usr/bin/env python3
"""Round 5 verification: imported-module dead-function tree-shaking, negative
BFieldIC + stdlib shaped maps, pre-1970 ISO-8601 timestamps, LSP %00 URI
rejection, and native Linux/macOS/Windows C preprocessor + FFI + bundle checks.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
IS_WIN = os.name == "nt"
IS_MAC = sys.platform == "darwin"


def find_brat_cxx() -> str:
    env = os.environ.get("BRAT_CXX")
    if env and os.path.isfile(env):
        return env
    exe = "brat-cxx.exe" if IS_WIN else "brat-cxx"
    cand = os.path.join(ROOT, "lang", "cxx", "bin", exe)
    if os.path.isfile(cand):
        return cand
    raise RuntimeError(f"brat-cxx binary not found at {cand}")


def find_cc() -> str:
    for cand in (os.environ.get("BRAT_CC"), "cc", "gcc", "clang"):
        if not cand:
            continue
        found = shutil.which(cand)
        if found:
            return found
    raise RuntimeError("no C compiler found on PATH")


def run_cmd(argv, cwd=None, input_bytes=None, timeout=60):
    return subprocess.run(
        argv,
        cwd=cwd or ROOT,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )


def lsp_frame(payload: dict) -> bytes:
    raw = json.dumps(payload).encode("utf-8")
    return f"Content-Length: {len(raw)}\r\n\r\n".encode("ascii") + raw


def test_import_tree_shaking(cxx: str, tmp: str) -> None:
    # 1. Importing std.http solely for parse_url must drop unused server/client functions.
    url_prog = os.path.join(tmp, "only_url.brat")
    with open(url_prog, "w", encoding="utf-8") as f:
        f.write(
            'import std.http\n'
            'fn main() -> i64 {\n'
            '  let u = unwrap(std.http.parse_url("https://example.com:8443/v1?x=1"))\n'
            '  assert(u["host"] == "example.com")\n'
            '  assert(u["port"] == 8443)\n'
            '  return 0\n'
            '}\n'
        )
    ec = run_cmd([cxx, "emit-c", url_prog])
    assert ec.returncode == 0, ec.stderr.decode("utf-8", "replace")
    c_src = ec.stdout.decode("utf-8", "replace")
    assert "parse_url" in c_src, "reachable std.http.parse_url missing"
    assert "accept_request" not in c_src, "dead std.http.accept_request was not pruned"
    assert "respond_json" not in c_src, "dead std.http.respond_json was not pruned"

    # `api` and `flat` must still preserve the full unpruned module surface.
    api_r = run_cmd([cxx, "api", "--json", url_prog])
    assert api_r.returncode == 0, api_r.stderr.decode("utf-8", "replace")
    api_text = api_r.stdout.decode("utf-8", "replace")
    assert '"serve_once"' in api_text and '"parse_url"' in api_text, "brat api lost uncalled std.http exports"

    flat_r = run_cmd([cxx, "flat", url_prog])
    assert flat_r.returncode == 0, flat_r.stderr.decode("utf-8", "replace")
    assert b"std__http__serve_once" in flat_r.stdout, "brat-cxx flat must remain unpruned"

    # 2. Reachability across EK::Call, EK::Var, EK::Str callback name, and EK::ParFor reducer.
    mod_path = os.path.join(tmp, "helpers.brat")
    with open(mod_path, "w", encoding="utf-8") as f:
        f.write(
            'pub fn used_direct(x: i64) -> i64 { return x + 1 }\n'
            'pub fn used_as_val(x: i64) -> i64 { return x * 2 }\n'
            'pub fn used_reducer(a: i64, b: i64) -> i64 { return a + b }\n'
            'pub fn dead_unused(x: i64) -> i64 { return x - 999 }\n'
        )
    app_path = os.path.join(tmp, "app_reach.brat")
    with open(app_path, "w", encoding="utf-8") as f:
        f.write(
            'import helpers\n'
            'fn apply_fn(f, v: i64) -> i64 { return f(v) }\n'
            'fn main() -> i64 {\n'
            '  let a = helpers.used_direct(9)\n'
            '  let b = apply_fn(fn(v) { return helpers.used_as_val(v) }, a)\n'
            '  let xs = [1, 2, 3, 4]\n'
            '  let c = par for x in xs reduce(used_reducer, 0) { return x * 10 }\n'
            '  assert(b + c == 120)\n'
            '  return 0\n'
            '}\n'
        )
    ec2 = run_cmd([cxx, "emit-c", app_path])
    assert ec2.returncode == 0, ec2.stderr.decode("utf-8", "replace")
    c2 = ec2.stdout.decode("utf-8", "replace")
    assert "used_direct" in c2
    assert "used_as_val" in c2
    assert "used_reducer" in c2
    assert "dead_unused" not in c2

    rr = run_cmd([cxx, "run", app_path])
    assert rr.returncode == 0, rr.stderr.decode("utf-8", "replace")


def test_stdlib_shaped_maps_and_pre1970_time(cxx: str, tmp: str) -> None:
    prog = os.path.join(tmp, "std_check.brat")
    with open(prog, "w", encoding="utf-8") as f:
        f.write(
            'import std.json\n'
            'import std.time\n'
            'fn main() -> i64 {\n'
            '  // Pre-1970 negative timestamps & weekday Euclidean remainder\n'
            '  assert(std.time.format_iso8601_utc(-1) == "1969-12-31T23:59:59Z")\n'
            '  assert(unwrap(std.time.parse_iso8601_utc("1969-12-31T23:59:59Z")) == -1)\n'
            '  assert(std.time.format_iso8601_utc(-86400) == "1969-12-31T00:00:00Z")\n'
            '  assert(unwrap(std.time.parse_iso8601_utc("1969-12-31T00:00:00Z")) == -86400)\n'
            '  assert(std.time.day_of_week(-5) == 6)\n'
            '  assert(std.time.day_of_week(-4) == 0)\n'
            '\n'
            '  // std.json shaped internal result + negative BFieldIC hits on `.has` / `.get_or`\n'
            '  let doc = unwrap(std.json.parse("{\\"items\\":[{\\"k\\":1},{\\"k\\":2,\\"opt\\":9},{\\"k\\":3}]}"))\n'
            '  let items = doc["items"]\n'
            '  let mut total = 0\n'
            '  let mut round = 0\n'
            '  while round < 500 {\n'
            '    let a = {"x": round, "y": 1}\n'
            '    let b = {"x": round, "y": 2, "z": 3}\n'
            '    if !a.has("missing") && is_none(a.get("missing")) {\n'
            '      total = total + a.get_or("missing", 1) + b.get_or("z", 0)\n'
            '    }\n'
            '    round = round + 1\n'
            '  }\n'
            '  assert(len(items) == 3)\n'
            '  assert(total == 2000)\n'
            '  return 0\n'
            '}\n'
        )
    ec = run_cmd([cxx, "emit-c", prog])
    assert ec.returncode == 0, ec.stderr.decode("utf-8", "replace")
    c_src = ec.stdout.decode("utf-8", "replace")
    assert "vmap_shaped(" in c_src, "expected vmap_shaped in std.json / shaped literal codegen"

    rr = run_cmd([cxx, "run", "-O2", prog])
    assert rr.returncode == 0, rr.stderr.decode("utf-8", "replace")


def test_lsp_nul_uri_rejection(cxx: str, tmp: str) -> None:
    good = os.path.join(tmp, "good.brat")
    with open(good, "w", encoding="utf-8") as f:
        f.write("fn add(a: i64, b: i64) -> i64 { return a + b }\n")
    raw_path = good.replace("\\", "/")
    if not raw_path.startswith("/"):
        raw_path = "/" + raw_path
    good_uri = "file://" + raw_path

    msgs = b"".join([
        lsp_frame({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}}),
        lsp_frame({"jsonrpc": "2.0", "method": "initialized", "params": {}}),
        lsp_frame({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": "file:///tmp/poison%00.brat",
                    "languageId": "brat",
                    "version": 1,
                    "text": "fn main() -> i64 { return 0 }\n",
                }
            },
        }),
        lsp_frame({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": good_uri,
                    "languageId": "brat",
                    "version": 1,
                    "text": "fn add(a: i64, b: i64) -> i64 { return a + b }\n",
                }
            },
        }),
        lsp_frame({
            "jsonrpc": "2.0",
            "id": 2,
            "method": "textDocument/hover",
            "params": {"textDocument": {"uri": good_uri}, "position": {"line": 0, "character": 4}},
        }),
        lsp_frame({"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None}),
        lsp_frame({"jsonrpc": "2.0", "method": "exit", "params": None}),
    ])
    res = run_cmd([cxx, "lsp"], input_bytes=msgs)
    assert res.returncode == 0, res.stderr.decode("utf-8", "replace")
    out = res.stdout.decode("utf-8", "replace")
    assert '"id":2' in out and "poison" not in out


def test_xplat_preprocess_ffi_and_bundle(cxx: str, tmp: str) -> None:
    cc = find_cc()
    hdr = os.path.join(tmp, "xplat_pkt.h")
    csrc = os.path.join(tmp, "xplat_pkt.c")
    obj = os.path.join(tmp, "xplat_pkt.obj" if IS_WIN else "xplat_pkt.o")
    with open(hdr, "w", encoding="utf-8") as f:
        f.write(
            "#ifndef XPLAT_PKT_H\n"
            "#define XPLAT_PKT_H\n"
            "#include <stdint.h>\n"
            "#define PKT_SCALE 3\n"
            "typedef struct Pkt {\n"
            "    int32_t tag;\n"
            "    int64_t val;\n"
            "} Pkt;\n"
            "int64_t pkt_score(Pkt p, int32_t bonus);\n"
            "#endif\n"
        )
    with open(csrc, "w", encoding="utf-8") as f:
        f.write(
            '#include "xplat_pkt.h"\n'
            "int64_t pkt_score(Pkt p, int32_t bonus) {\n"
            "    return (int64_t)p.tag * PKT_SCALE + p.val + (int64_t)bonus;\n"
            "}\n"
        )
    cc_cmd = [cc, "-c", "-O2"]
    if not IS_WIN:
        cc_cmd.append("-fPIC")
    cc_cmd.extend([csrc, "-o", obj])
    r_cc = run_cmd(cc_cmd)
    assert r_cc.returncode == 0, r_cc.stderr.decode("utf-8", "replace")

    bind_out = os.path.join(tmp, "pkt_bind.brat")
    r_bg = run_cmd([cxx, "bindgen", hdr, "--preprocess", "-o", bind_out])
    assert r_bg.returncode == 0, r_bg.stderr.decode("utf-8", "replace")
    with open(bind_out, encoding="utf-8") as f:
        btxt = f.read()
    assert "pkt_score" in btxt and "PKT_SCALE" in btxt, btxt

    app = os.path.join(tmp, "use_pkt.brat")
    with open(app, "w", encoding="utf-8") as f:
        f.write(
            btxt
            + "\nfn main() -> i64 {\n"
            + "  let p = Pkt { tag: 10, val: 7 }\n"
            + "  let s = pkt_score(p, 5)\n"
            + "  assert(s == 42)\n"
            + "  return 0\n"
            + "}\n"
        )
    r_ffi = run_cmd([cxx, "ffi", app, "--header", hdr, "--preprocess", "--object", obj])
    assert r_ffi.returncode == 0, r_ffi.stderr.decode("utf-8", "replace")

    bin_out = os.path.join(tmp, "use_pkt.exe" if IS_WIN else "use_pkt_bin")
    r_bld = run_cmd([cxx, "build", app, "--header", hdr, "--object", obj, "-o", bin_out])
    assert r_bld.returncode == 0, r_bld.stderr.decode("utf-8", "replace")
    assert os.path.isfile(bin_out), f"missing built executable {bin_out}"
    r_exe = run_cmd([bin_out])
    assert r_exe.returncode == 0, r_exe.stderr.decode("utf-8", "replace")

    # Portable bundle layout & wrapper check
    stage = os.path.join(tmp, "portable_bundle")
    r_bun = run_cmd([cxx, "bundle", stage])
    assert r_bun.returncode == 0, r_bun.stderr.decode("utf-8", "replace")
    top_brat = os.path.join(stage, "bin", "brat.cmd" if IS_WIN else "brat")
    assert os.path.exists(top_brat), f"missing top-level launcher {top_brat}"
    hello_ex = os.path.join(ROOT, "examples", "hello.brat")
    json_ex = os.path.join(ROOT, "examples", "json_arena.brat")
    if IS_WIN:
        r_run1 = run_cmd(["cmd.exe", "/c", top_brat, "run", hello_ex])
        r_run2 = run_cmd(["cmd.exe", "/c", top_brat, "run", json_ex])
    else:
        r_run1 = run_cmd([top_brat, "run", hello_ex])
        r_run2 = run_cmd([top_brat, "run", json_ex])
    assert r_run1.returncode == 0, r_run1.stderr.decode("utf-8", "replace")
    assert r_run2.returncode == 0, r_run2.stderr.decode("utf-8", "replace")
    assert not IS_MAC or os.path.islink(top_brat)


def main() -> None:
    cxx = find_brat_cxx()
    with tempfile.TemporaryDirectory(prefix="brat-r5-") as tmp:
        test_import_tree_shaking(cxx, tmp)
        test_stdlib_shaped_maps_and_pre1970_time(cxx, tmp)
        test_lsp_nul_uri_rejection(cxx, tmp)
        test_xplat_preprocess_ffi_and_bundle(cxx, tmp)
    print("ok import prune, negative BFieldIC, pre-1970 time, LSP URI guard, xplat FFI & bundle")


if __name__ == "__main__":
    main()
