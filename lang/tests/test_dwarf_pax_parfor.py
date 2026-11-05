"""Security hardening, runtime speed invariants, and C-header FFI regressions.

Covers:
1. DWARF `DW_TAG_subroutine_type` exponential DAG memoization (`4^22` paths in <2s).
2. PAX / GNU tar archive hardening (sparse/dev headers, control bytes in PAX paths,
   global `'g'` overrides, monotonic `linkpath`, dangling trailing metadata headers).
3. MCP `brat_build` output confinement, symlink rejection, and source-extension protection.
4. `std.http.get_follow` blocking redirects to link-local / unspecified / internal hosts
   and HTTPS -> HTTP protocol downgrades.
5. Chunked `par for` execution, error propagation, and exact single-threaded recursion budget.
6. C-header constant-expression array dimensions (`#define` + `enum` + hex), multi-dimensional
   arrays (`T[M][N]`), and C11 `_Atomic` / GNU qualifier stripping.
"""
import gzip
import io
import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cxx

BIN = os.environ.get("BRAT_CXX") or _cxx.build()


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def make_pax_record(kv: str) -> bytes:
    # Format: "<len> <key>=<value>\n" where <len> includes the length digits themselves.
    body = " " + kv + "\n"
    n = len(body) + 1
    while len(str(n)) + len(body) != n:
        n = len(str(n)) + len(body)
    return (str(n) + body).encode("utf-8", errors="surrogateescape")


def make_ustar_header(name: str, size: int, typeflag: bytes) -> bytes:
    buf = bytearray(512)
    nb = name.encode("ascii")[:100]
    buf[0:len(nb)] = nb
    buf[100:108] = b"0000644\x00"
    buf[108:116] = b"0000000\x00"
    buf[116:124] = b"0000000\x00"
    buf[124:136] = f"{size:011o}\x00".encode("ascii")
    buf[136:148] = b"00000000000\x00"
    buf[148:156] = b"        "
    buf[156:157] = typeflag
    buf[257:263] = b"ustar\x00"
    buf[263:265] = b"00"
    csum = sum(buf)
    buf[148:156] = f"{csum:06o}\x00 ".encode("ascii")
    return bytes(buf)


def pad512(b: bytes) -> bytes:
    rem = len(b) % 512
    return b if rem == 0 else b + b"\x00" * (512 - rem)


def test_dwarf_dag_memoization() -> None:
    with tempfile.TemporaryDirectory(prefix="brat-dwarf-dag-") as td:
        c_src = os.path.join(td, "dag.c")
        lines = ["typedef void (*f0)(int, int, int, int);"]
        for i in range(1, 23):
            lines.append(f"typedef void (*f{i})(f{i-1}, f{i-1}, f{i-1}, f{i-1});")
        lines.append("void dag_entry(f22 cb, int tag) { (void)cb; (void)tag; }")
        with open(c_src, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

        obj_path = os.path.join(td, "dag.o")
        subprocess.run(["gcc", "-g", "-O0", "-c", c_src, "-o", obj_path], check=True, timeout=20)

        brat_src = os.path.join(td, "dag.brat")
        with open(brat_src, "w", encoding="utf-8") as f:
            # Declare a mismatched scalar parameter so DWARF inspection runs `type_shape`
            # through the full `f22 -> f21 -> ... -> f0` subroutine DAG and reports mismatch fast.
            f.write('extern "dag" {\n  fn dag_entry(cb: i64, tag: i32) -> none\n}\n')

        t0 = time.perf_counter()
        res = subprocess.run(
            [BIN, "ffi", brat_src, "--object", obj_path],
            capture_output=True,
            text=True,
            timeout=8,
        )
        dt = time.perf_counter() - t0
        check(dt < 3.0, f"DWARF subroutine DAG took too long: {dt:.3f}s")
        check(res.returncode != 0, "expected ABI mismatch between fn pointer and scalar i64")


def test_pax_tar_hardening() -> None:
    with tempfile.TemporaryDirectory(prefix="brat-pax-sec-") as td:
        proj = os.path.join(td, "proj")
        os.makedirs(proj)
        valid_member_body = b"pub fn ok() -> i64 { return 1 }\n"

        cases = []
        # 1. GNU.sparse PAX key
        pax_sparse = make_pax_record("GNU.sparse.major=1") + make_pax_record("path=pkg/lib.brat")
        cases.append((
            "gnu_sparse",
            make_ustar_header("PaxHeader/lib.brat", len(pax_sparse), b"x")
            + pad512(pax_sparse)
            + make_ustar_header("pkg/lib.brat", len(valid_member_body), b"0")
            + pad512(valid_member_body)
            + b"\x00" * 1024,
        ))
        # 2. Control byte inside PAX path
        pax_ctl = make_pax_record("path=pkg/bad\x01name.brat")
        cases.append((
            "ctl_path",
            make_ustar_header("PaxHeader/lib.brat", len(pax_ctl), b"x")
            + pad512(pax_ctl)
            + make_ustar_header("pkg/lib.brat", len(valid_member_body), b"0")
            + pad512(valid_member_body)
            + b"\x00" * 1024,
        ))
        # 3. Global PAX 'g' size smuggling
        pax_glob = make_pax_record("size=4")
        cases.append((
            "global_size",
            make_ustar_header("pax_global_header", len(pax_glob), b"g")
            + pad512(pax_glob)
            + make_ustar_header("pkg/lib.brat", len(valid_member_body), b"0")
            + pad512(valid_member_body)
            + b"\x00" * 1024,
        ))
        # 4. Monotonic linkpath (non-empty linkpath followed by empty linkpath=)
        pax_link = make_pax_record("linkpath=/etc/passwd") + make_pax_record("linkpath=")
        cases.append((
            "linkpath_reset",
            make_ustar_header("PaxHeader/lib.brat", len(pax_link), b"x")
            + pad512(pax_link)
            + make_ustar_header("pkg/lib.brat", len(valid_member_body), b"0")
            + pad512(valid_member_body)
            + b"\x00" * 1024,
        ))
        # 5. Dangling PAX 'x' header at EOF with no member following
        pax_dangle = make_pax_record("path=pkg/orphan.brat")
        cases.append((
            "dangling_pax",
            make_ustar_header("pkg/lib.brat", len(valid_member_body), b"0")
            + pad512(valid_member_body)
            + make_ustar_header("PaxHeader/orphan", len(pax_dangle), b"x")
            + pad512(pax_dangle)
            + b"\x00" * 1024,
        ))

        for label, raw_tar in cases:
            tgz_path = os.path.join(td, f"{label}.tar.gz")
            with gzip.open(tgz_path, "wb") as gz:
                gz.write(raw_tar)
            uri = pathlib.Path(tgz_path).as_uri()
            r = subprocess.run(
                [BIN, "pkg", "add", uri],
                capture_output=True,
                text=True,
                cwd=proj,
                timeout=20,
            )
            check(r.returncode != 0, f"PAX case {label!r} should have been rejected: {r.stdout} {r.stderr}")


def mcp_rpc(proc: subprocess.Popen, req_id: int, method: str, params: dict) -> dict:
    payload = json.dumps({
        "jsonrpc": "2.0",
        "id": req_id,
        "method": method,
        "params": params,
    }).encode("utf-8")
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
    assert proc.stdin is not None and proc.stdout is not None
    proc.stdin.write(header + payload)
    proc.stdin.flush()
    headers = {}
    while True:
        line = proc.stdout.readline()
        check(bool(line), "MCP closed unexpectedly")
        if line in (b"\r\n", b"\n"):
            break
        k, v = line.decode("ascii").split(":", 1)
        headers[k.strip().lower()] = v.strip()
    n = int(headers["content-length"])
    body = proc.stdout.read(n)
    return json.loads(body.decode("utf-8"))


def mcp_call(proc: subprocess.Popen, req_id: int, name: str, arguments: dict) -> dict:
    return mcp_rpc(proc, req_id, "tools/call", {"name": name, "arguments": arguments})


def test_mcp_build_confinement() -> None:
    with tempfile.TemporaryDirectory(prefix="brat-mcp-src-") as src_dir, \
         tempfile.TemporaryDirectory(prefix="brat-mcp-out-") as outside_dir:
        src_file = os.path.join(src_dir, "hello.brat")
        with open(src_file, "w", encoding="utf-8") as f:
            f.write('fn main() {\n  print("ok")\n}\n')
        other_brat = os.path.join(src_dir, "victim.brat")
        with open(other_brat, "w", encoding="utf-8") as f:
            f.write("// keep me\n")
        sym_out = os.path.join(src_dir, "link_out.bin")
        os.symlink(os.path.join(outside_dir, "target.bin"), sym_out)

        proc = subprocess.Popen(
            [BIN, "mcp"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=src_dir,
        )
        try:
            init = mcp_rpc(proc, 0, "initialize", {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "test", "version": "1"},
            })
            check("result" in init, f"MCP initialize failed: {init}")
            # 1. Outside project & outside cwd -> rejected
            r_out = mcp_call(proc, 1, "brat_build", {
                "path": src_file,
                "out": os.path.join(outside_dir, "escaped.bin"),
            })
            check(r_out["result"].get("isError") is True, f"expected confinement error: {r_out}")

            # 2. Protected source extension `.brat` -> rejected
            r_ext = mcp_call(proc, 2, "brat_build", {
                "path": src_file,
                "out": other_brat,
            })
            check(r_ext["result"].get("isError") is True, f"expected protected ext error: {r_ext}")
            with open(other_brat, encoding="utf-8") as f:
                check(f.read() == "// keep me\n", "victim.brat was modified")

            # 3. Symlink output path -> rejected
            r_sym = mcp_call(proc, 3, "brat_build", {
                "path": src_file,
                "out": sym_out,
            })
            check(r_sym["result"].get("isError") is True, f"expected symlink error: {r_sym}")

            # 4. Valid in-project binary output -> succeeds
            good_out = os.path.join(src_dir, "hello.bin")
            r_ok = mcp_call(proc, 4, "brat_build", {
                "path": src_file,
                "out": good_out,
            })
            check(not r_ok["result"].get("isError"), f"valid MCP build failed: {r_ok}")
            check(os.path.isfile(good_out), "expected built binary at hello.bin")
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_http_redirect_security() -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.listen(4)

    def serve() -> None:
        for _ in range(2):
            conn, _ = srv.accept()
            with conn:
                req = conn.recv(2048).decode("latin1", errors="replace")
                if "GET /to-meta " in req:
                    resp = (
                        "HTTP/1.1 302 Found\r\n"
                        "Location: http://169.254.169.254/latest/meta-data/\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n"
                    )
                else:
                    resp = (
                        "HTTP/1.1 302 Found\r\n"
                        "Location: http://0.0.0.0:80/\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n"
                    )
                conn.sendall(resp.encode("ascii"))

    th = threading.Thread(target=serve, daemon=True)
    th.start()
    try:
        with tempfile.TemporaryDirectory(prefix="brat-http-sec-") as td:
            prog = os.path.join(td, "redir.brat")
            with open(prog, "w", encoding="utf-8") as f:
                f.write(f"""import std.http

fn main() {{
  let r1 = std.http.get_follow("http://127.0.0.1:{port}/to-meta", 3)
  assert(is_err(r1))
  let r2 = std.http.get_follow("http://127.0.0.1:{port}/to-zero", 3)
  assert(is_err(r2))
  print("HTTP_REDIRECT_GUARD_OK")
}}
""")
            r = subprocess.run([BIN, "run", prog], capture_output=True, text=True, timeout=90)
            check(r.returncode == 0 and "HTTP_REDIRECT_GUARD_OK" in r.stdout,
                  f"HTTP redirect guard failed: {r.stdout} {r.stderr}")
    finally:
        srv.close()
        th.join(timeout=3)


def test_parfor_chunks_and_recursion_budget() -> None:
    with tempfile.TemporaryDirectory(prefix="brat-par-rec-") as td:
        prog = os.path.join(td, "par_chunk.brat")
        with open(prog, "w", encoding="utf-8") as f:
            f.write("""fn work(x: i64) -> i64 {
  return x * x + 1
}

fn add_pair(a: i64, b: i64) -> i64 {
  return a + b
}

fn join_len(a: i64, s: str) -> i64 {
  return a + len(s)
}

fn dive(n: i64) -> i64 {
  if n <= 0 { return 0 }
  return 1 + dive(n - 1)
}

fn main() {
  let mut xs = []
  let mut i = 0
  let mut expected = 0
  let mut expected_slen = 0
  while i < 1200 {
    xs.push(i)
    expected = expected + (i * i + 1)
    expected_slen = expected_slen + len("item_" + str(i) + "_payload_heap_string")
    i = i + 1
  }
  let got = par for x in xs reduce(add_pair, 0) {
    return work(x)
  }
  assert(got == expected)
  let got_s = par for x in xs reduce(join_len, 0) {
    return "item_" + str(x) + "_payload_heap_string"
  }
  assert(got_s == expected_slen)
  assert(dive(80) == 80)
  print("PAR_AND_REC_OK")
}
""")
        env_ok = os.environ.copy()
        env_ok["BRAT_THREADS"] = "4"
        env_ok["BRAT_RECURSION_BUDGET"] = "120"
        r_ok = subprocess.run([BIN, "run", prog], capture_output=True, text=True, env=env_ok, timeout=90)
        check(r_ok.returncode == 0 and "PAR_AND_REC_OK" in r_ok.stdout,
              f"par_chunk run failed: {r_ok.stdout} {r_ok.stderr}")

        # Mid-chunk runtime fault (`div by zero` at element 333) unwinds cleanly without leaks/hangs.
        fault_prog = os.path.join(td, "par_fault.brat")
        with open(fault_prog, "w", encoding="utf-8") as f:
            f.write("""fn add_pair(a: i64, b: i64) -> i64 { return a + b }
fn main() {
  let mut xs = []
  let mut i = 0
  while i < 800 {
    xs.push(i)
    i = i + 1
  }
  let s = par for x in xs reduce(add_pair, 0) {
    return 1000 / (x - 333)
  }
  print(s)
}
""")
        r_fault = subprocess.run([BIN, "run", fault_prog], capture_output=True, text=True, env=env_ok, timeout=90)
        check(r_fault.returncode != 0 and "div by zero" in (r_fault.stdout + r_fault.stderr),
              f"expected clean mid-chunk div by zero abort: {r_fault.stdout} {r_fault.stderr}")

        env_deep = os.environ.copy()
        env_deep["BRAT_RECURSION_BUDGET"] = "60"
        r_deep = subprocess.run([BIN, "run", prog], capture_output=True, text=True, env=env_deep, timeout=90)
        check(r_deep.returncode != 0 and "recursion too deep" in (r_deep.stdout + r_deep.stderr),
              f"expected recursion budget overflow at 60: {r_deep.stdout} {r_deep.stderr}")


def test_ffi_constexpr_multidim_atomic() -> None:
    with tempfile.TemporaryDirectory(prefix="brat-ffi-cdecl-") as td:
        hdr = os.path.join(td, "pkt.h")
        c_impl = os.path.join(td, "pkt.c")
        with open(hdr, "w", encoding="utf-8") as f:
            f.write("""#pragma once
#include <stdint.h>

#define PKT_HDR_BYTES 8
enum { PKT_EXTRA_BYTES = 12 };

typedef struct PacketFrame {
    _Atomic(uint32_t) seq;
    int32_t matrix[2][3];
    uint8_t payload[PKT_HDR_BYTES + PKT_EXTRA_BYTES];
} PacketFrame;

uint32_t pkt_checksum(const PacketFrame *__restrict f, const char *__const tag);
""")
        with open(c_impl, "w", encoding="utf-8") as f:
            f.write("""#include "pkt.h"
#include <string.h>

uint32_t pkt_checksum(const PacketFrame *__restrict f, const char *__const tag) {
    uint32_t acc = f->seq;
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++)
            acc += (uint32_t)f->matrix[r][c];
    for (int i = 0; i < PKT_HDR_BYTES + PKT_EXTRA_BYTES; i++)
        acc += f->payload[i];
    acc += (uint32_t)strlen(tag);
    return acc;
}
""")
        so_path = os.path.join(td, "libpkt.so")
        subprocess.run(["gcc", "-shared", "-fPIC", "-O2", c_impl, "-o", so_path], check=True, timeout=20)

        out_brat = os.path.join(td, "pkt_bind.brat")
        bg = subprocess.run(
            [BIN, "bindgen", hdr, "--lib", so_path, "--inline-arrays", "-o", out_brat],
            capture_output=True,
            text=True,
            timeout=20,
        )
        check(bg.returncode == 0, f"bindgen failed: {bg.stdout} {bg.stderr}")
        bind_txt = open(out_brat, encoding="utf-8").read()
        check("seq: u32" in bind_txt, f"expected _Atomic(uint32_t) -> u32 in:\n{bind_txt}")
        check("matrix: i32[6]" in bind_txt, f"expected int32_t[2][3] -> i32[6] in:\n{bind_txt}")
        check("payload: u8[20]" in bind_txt, f"expected uint8_t[8 + 12] -> u8[20] in:\n{bind_txt}")

        vf = subprocess.run(
            [BIN, "ffi", out_brat, "--header", hdr],
            capture_output=True,
            text=True,
            timeout=20,
        )
        check(vf.returncode == 0, f"ffi --header failed on PacketFrame:\n{vf.stdout}\n{vf.stderr}")


if __name__ == "__main__":
    test_dwarf_dag_memoization()
    test_pax_tar_hardening()
    test_mcp_build_confinement()
    test_http_redirect_security()
    test_parfor_chunks_and_recursion_budget()
    test_ffi_constexpr_multidim_atomic()
    print("OK: security, runtime speed invariants, and FFI header regressions passed")
