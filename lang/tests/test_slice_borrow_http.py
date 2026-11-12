"""Regression tests for slice safety, ownership/exclusivity edges, and HTTP/IPv6 hardening."""

import json
import os
import shutil
import socket
import subprocess
import sys
import threading

import _cxx
import _scratch
import _skip

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
TMP = _scratch.mkdtemp(prefix="brat-sbh-")


def run(cmd, env=None, timeout=90):
    return subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout)


def write_file(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return path


def compile_and_run_asan(cc, cxx, name, src, extra_flags=()):
    p = write_file(name + ".brat", src)
    c_out = write_file(name + ".c", "")
    exe = os.path.join(TMP, name + ".bin")
    r_emit = run([cxx, "emit-c", *extra_flags, p, "-o", c_out])
    assert r_emit.returncode == 0, (name, r_emit.stdout, r_emit.stderr)
    r_cc = run([
        cc, "-std=gnu11", "-O1", "-g", "-fwrapv",
        "-fsanitize=address,undefined",
        "-I", ENGINE, c_out, "-o", exe, "-lm", "-pthread", "-ldl",
    ])
    assert r_cc.returncode == 0, (name, r_cc.stderr)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
               UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    r_run = run([exe], env=env)
    return r_run


def test_slice_soundness_and_rc(cxx, cc):
    # 1. Forged vec passed to slice_len / slice_get / slice_set is rejected statically.
    for idx, call_expr in enumerate([
        "slice_len(forged)",
        "slice_get(forged, 0)",
        "slice_set(forged, 0, 99)",
        "slice_of(42, 0, 1)",
    ]):
        p = write_file(f"forged_slice_{idx}.brat", f"""fn main() -> i64 {{
  let forged = [41414141, 0, 1, 1]
  let _x = {call_expr}
  return 0
}}
""")
        r = run([cxx, "check", p])
        assert r.returncode != 0, (call_expr, r.stdout, r.stderr)

    # 2. Even under --no-check / untyped parameters, runtime tag check blocks forged vecs.
    rt_guard = """fn leak_attempt(x) -> i64 {
  return slice_get(x, 0)
}
fn write_attempt(x) -> i64 {
  slice_set(x, 0, 12345)
  return 0
}
fn len_attempt(x) -> i64 {
  return slice_len(x)
}
fn main() -> i64 {
  let forged = [41414141, 0, 1, 1]
  let mut caught = 0
  try { let _a = leak_attempt(forged) } catch e {
    assert(str(e).contains("not a slice"))
    caught = caught + 1
  }
  try { let _b = write_attempt(forged) } catch e {
    assert(str(e).contains("not a slice"))
    caught = caught + 1
  }
  try { let _c = len_attempt(forged) } catch e {
    assert(str(e).contains("not a slice"))
    caught = caught + 1
  }
  assert(caught == 3)
  return 0
}
"""
    for opt in ("-O0", "-O2"):
        r = compile_and_run_asan(cc, cxx, f"slice_rt_{opt[1:]}", rt_guard, (opt,))
        assert r.returncode == 0, (opt, r.stdout, r.stderr)

    # 3. Heap strings and structs through slice_of, slice_get, slice_set, slice_struct_of:
    #    zero leaks and zero UAF under ASan + LSan.
    rc_src = """struct Item { label: str, code: i64 }
fn main() -> i64 {
  let mut words = [
    "alpha-heap-string-longer-than-sso",
    "bravo-heap-string-longer-than-sso",
    "charlie-heap-string-longer-than-sso"
  ]
  let win = slice_of(words, 0, 3)
  let picked = slice_get(win, 1)
  let replacement = "delta-heap-string-longer-than-sso"
  slice_set(win, 1, replacement)
  assert(picked == "bravo-heap-string-longer-than-sso")
  assert(replacement == "delta-heap-string-longer-than-sso")
  assert(slice_get(win, 1) == "delta-heap-string-longer-than-sso")

  let mut oob_caught = false
  let extra = "echo-heap-string-longer-than-sso-" + str(42)
  try { slice_set(win, 9, extra) } catch _e { oob_caught = true }
  assert(oob_caught)

  let a = Item { label: "first-struct-heap-string-value", code: 10 }
  let b = Item { label: "second-struct-heap-string-value", code: 20 }
  let sw = slice_struct_of([a, b])
  assert(slice_get(sw, 0).label == "first-struct-heap-string-value")
  slice_set(sw, 1, Item { label: "updated-struct-heap-string-value", code: 30 })
  assert(slice_get(sw, 1).code == 30)
  assert(a.label == "first-struct-heap-string-value")

  assert(slice_len(slice_of(["temp-one-heap-string", "temp-two-heap-string"], 0, 1)) == 1)
  return 0
}
"""
    for opt in ("-O0", "-O2"):
        r = compile_and_run_asan(cc, cxx, f"slice_rc_{opt[1:]}", rc_src, (opt,))
        assert r.returncode == 0, (opt, r.stdout, r.stderr)


def test_borrow_loe_closures_and_unsafe(cxx, cc):
    # 1. Single-call Law of Exclusivity (Ref + Mut / Move / sibling mutation) -> E0499
    bad_cases = {
        "loe_mut_ref": """fn boom(mut a: vec[i64], b: vec[i64]) -> i64 {
  a = []
  return len(b)
}
fn main() -> i64 {
  let mut v = [1, 2, 3]
  return boom(v, v)
}
""",
        "loe_ref_mut": """fn boom2(a: vec[i64], mut b: vec[i64]) -> i64 {
  b = []
  return len(a)
}
fn main() -> i64 {
  let mut v = [1, 2, 3]
  return boom2(v, v)
}
""",
        "loe_move_ref": """fn consume_and_read(a: move vec[i64], b: vec[i64]) -> i64 {
  return len(a) + len(b)
}
fn main() -> i64 {
  let v = [1, 2, 3]
  return consume_and_read(v, v)
}
""",
        "loe_bin_move": """fn consume(s: move str) -> str { return s + "!" }
fn main() -> i64 {
  let s = "heap-string-longer-than-sso"
  let r = s + consume(move s)
  print(r)
  return 0
}
""",
        "clo_outer_move": """fn eat(s: move vec[i64]) -> i64 { return len(s) }
fn main() -> i64 {
  let v = [1, 2, 3]
  let f = fn() { return eat(v) }
  return f() + f()
}
""",
        "clo_outer_move_expr": """fn main() -> i64 {
  let s = "heap-string-owned-by-outer-frame"
  let f = fn() {
    let t = move s
    return len(t)
  }
  return f() + f()
}
""",
        "clo_for_iter_mutate": """fn main() -> i64 {
  let mut v = ["alpha-long-heap-str", "bravo-long-heap-str"]
  let wipe = fn() { v = [] }
  for x in v {
    wipe()
    print(x)
  }
  return 0
}
""",
        "clo_for_nested_hof_mutate": """fn main() -> i64 {
  let mut v = ["alpha-long-heap-str", "bravo-long-heap-str"]
  for x in v {
    let _w = [1].map(fn(n) {
      v.pop()
      return n
    })
    print(x)
  }
  return 0
}
""",
        "clo_callback_loe": """fn inspect_with(a: vec[str], cb) -> i64 {
  let s = a[0]
  cb()
  return len(s)
}
fn main() -> i64 {
  let mut v = ["alpha-long-heap-string-value"]
  return inspect_with(v, fn() { v.pop() })
}
""",
        "clo_uncalled_ptr_leak": """extern fn my_alloc() -> move ptr
extern fn my_free(p: move ptr)
fn main() -> i64 {
  let p = my_alloc()
  let _unused = fn() { my_free(p) }
  return 0
}
""",
        "unsafe_move_uaf": """fn main() -> i64 {
  let s = "heap-string-moved-inside-unsafe"
  unsafe {
    let t = move s
    print(t)
  }
  print(s)
  return 0
}
""",
        "unsafe_double_free": """extern fn my_alloc() -> move ptr
extern fn my_free(p: move ptr)
fn main() -> i64 {
  let p = my_alloc()
  unsafe { my_free(p) }
  my_free(p)
  return 0
}
""",
    }
    for name, src in bad_cases.items():
        p = write_file(name + ".brat", src)
        r = run([cxx, "check", "--json", p])
        assert r.returncode != 0, f"{name} unexpectedly passed check"
        payload = json.loads(r.stdout)
        dlist = payload["diagnostics"] if isinstance(payload, dict) else payload
        if name.startswith("loe_") or name == "clo_callback_loe":
            assert any(d.get("code") == "E0499" for d in dlist), (name, payload)

    # 2. Valid patterns must continue to check and run cleanly under ASan:
    valid_src = """fn same_ref(a: vec[i64], b: vec[i64]) -> i64 {
  return len(a) + len(b)
}
fn vec_len(a: vec[i64]) -> i64 {
  return len(a)
}
fn take_str(s: move str) -> i64 {
  return len(s)
}
fn main() -> i64 {
  let mut v = [10, 20]
  assert(same_ref(v, v) == 4)
  v.push(len(v))
  v.push(vec_len(v))
  assert(len(v) == 4)
  assert(v.pop() == 3 && len(v) == 3)

  let push1 = fn(x: i64) { v.push(x) }
  push1(40)
  assert(len(v) == 4)
  push1(50)
  assert(len(v) == 5)

  let own_param_clo = fn(s: move str) { return take_str(move s) }
  assert(own_param_clo(move ("owned-" + "closure-argument-heap-str")) == 31)

  let ch = chan(2)
  let moved_vec = [7, 8, 9]
  spawn {
    let local_v = move moved_vec
    ch.send(len(local_v))
  }
  join_all()
  assert(ch.recv() == 3)
  return 0
}
"""
    p_ok = write_file("valid_borrow_patterns.brat", valid_src)
    assert run([cxx, "check", p_ok]).returncode == 0
    r = compile_and_run_asan(cc, cxx, "valid_borrow_patterns_rt", valid_src, ("-O2",))
    assert r.returncode == 0, (r.stdout, r.stderr)


def test_http_and_net_hardening(cxx, cc):
    # Local TCP server returning a crafted redirect for get_follow SSRF verification
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.listen(32)

    targets = [
        "https://[0:0:0:0:0:0:0:0]/",
        "https://[0:0:0:0:0:ffff:169.254.169.254]/latest",
        "https://[::ffff:a9fe:a9fe]/latest",
        "https://[0fe80::1]/",
        "https://[fe80::1%25lo0]/",
        "https://[::ffff:2130706433]/",
        "https://[0:0:0:0:0:0:0:1]/",
        "https://[0000::0001]/",
        "https://[0::1]/",
        "https://[0fc00::1]/",
        "https://[0:0:0:0:0:ffff:127.0.0.1]/",
        "https://[::ffff:7f00:1]/",
        "https://[64:ff9b:0:0:0:0:7f00:1]/",
    ]
    stop = threading.Event()

    def serve_redirects():
        while not stop.is_set():
            srv.settimeout(0.2)
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with conn:
                data = conn.recv(4096).decode("latin1", "replace")
                idx = 0
                first = data.splitlines()[0] if data else ""
                parts = first.split(" ")
                if len(parts) >= 2 and parts[1].startswith("/r/"):
                    idx = int(parts[1][3:])
                loc = targets[idx]
                resp = (
                    f"HTTP/1.1 302 Found\r\n"
                    f"Location: {loc}\r\n"
                    f"Content-Length: 0\r\n"
                    f"Connection: close\r\n\r\n"
                )
                conn.sendall(resp.encode("ascii"))

    th = threading.Thread(target=serve_redirects, daemon=True)
    th.start()
    try:
        http_src = f"""import std.http

fn main() -> i64 {{
  // 1. RFC 9112 request framing checks
  assert(is_err(std.http.parse_request("GET / HTTP/1.1\r\nHost: a\r\n X-Fold: 1\r\n\r\n")))
  assert(is_err(std.http.parse_request("GET / HTTP/1.1\r\nHost: a\r\n\tX-Fold: 1\r\n\r\n")))
  assert(is_err(std.http.parse_request("GET / HTTP/1.1\r\nHost : a\r\n\r\n")))
  assert(is_err(std.http.parse_request("GET / HTTP/1.1\r\nContent-Length : 4\r\n\r\nbody")))
  assert(is_err(std.http.parse_request("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n")))
  assert(is_err(std.http.parse_request(" GET / HTTP/1.1\r\nHost: a\r\n\r\n")))

  // Connection: close + Connection: keep-alive must not stay keep-alive
  let req1 = unwrap(std.http.parse_request("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\nConnection: keep-alive\r\n\r\n"))
  assert(req1["keep_alive"] == false)
  let req0 = unwrap(std.http.parse_request("GET / HTTP/1.0\r\nHost: a\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n"))
  assert(req0["keep_alive"] == false)
  let req_ok = unwrap(std.http.parse_request("GET /ok HTTP/1.1\r\nHost: example.com\r\n\r\n"))
  assert(req_ok["keep_alive"] == true)

  // 2. Link-local / unspecified / malformed IPv6 redirects blocked from loopback origin
  for i in 0..6 {{
    let gr = std.http.get_follow("http://127.0.0.1:{port}/r/" + str(i), 2)
    assert(is_err(gr))
    assert(str(gr).contains("refusing redirect to internal or link-local host"))
  }}

  // 3. Strict canonical IPv4 in tcp_listen / tcp_connect
  for bad_ip in ["2130706433:80", "0x7f000001:80", "0177.0.0.1:80", "127.1:80", "127.0.0.256:80"] {{
    assert(is_err(tcp_connect(bad_ip)))
    assert(is_err(tcp_listen(bad_ip)))
  }}
  return 0
}}
"""
        r = compile_and_run_asan(cc, cxx, "http_net_hardening", http_src, ("-O2",))
        assert r.returncode == 0, (r.stdout, r.stderr)

        # External-origin SSRF redirect check (rerouting 198.51.100.1 to local loopback in C)
        ext_src = f"""import std.http
fn main() -> i64 {{
  for i in 0..{len(targets)} {{
    let gr = std.http.get_follow("http://198.51.100.1:{port}/r/" + str(i), 2)
    assert(is_err(gr))
    assert(str(gr).contains("refusing redirect to internal or link-local host"))
  }}
  return 0
}}
"""
        p_ext = write_file("http_ext_ssrf.brat", ext_src)
        c_raw = write_file("http_ext_ssrf_raw.c", "")
        c_patched = write_file("http_ext_ssrf.c", "")
        exe_ext = os.path.join(TMP, "http_ext_ssrf.bin")
        assert run([cxx, "emit-c", "-O2", p_ext, "-o", c_raw]).returncode == 0
        emitted_c = open(c_raw, "r", encoding="utf-8").read()
        shim = """#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static int brat_ssrf_test_connect(int fd, const struct sockaddr *addr, socklen_t alen) {
    if (addr && addr->sa_family == AF_INET) {
        struct sockaddr_in copy = *(const struct sockaddr_in *)addr;
        copy.sin_addr.s_addr = htonl(0x7f000001u);
        return connect(fd, (const struct sockaddr *)&copy, sizeof(copy));
    }
    return connect(fd, addr, alen);
}
#define connect brat_ssrf_test_connect
"""
        with open(c_patched, "w", encoding="utf-8", newline="\n") as f:
            f.write(shim + emitted_c)
        r_cc = run([
            cc, "-std=gnu11", "-O1", "-g", "-fwrapv",
            "-fsanitize=address,undefined",
            "-I", ENGINE, c_patched, "-o", exe_ext, "-lm", "-pthread", "-ldl",
        ])
        assert r_cc.returncode == 0, r_cc.stderr
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1")
        r_ext = run([exe_ext], env=env)
        assert r_ext.returncode == 0, (r_ext.stdout, r_ext.stderr)
    finally:
        stop.set()
        srv.close()
        th.join(timeout=2)


def test_crt_vpos_and_tsan(cxx, cc):
    crt = os.path.abspath(os.path.join(HERE, "..", "engine", "crt.h")).replace("\\", "/")
    c_src = f"""#include "{crt}"
Val vcall_any(Val fn, Vec *a) {{ (void)fn; (void)a; return vnil(); }}
int main(void) {{
    Val r1 = vpos(vint(42), vint(1));
    if (!verr_pending() || r1.tag != vunwind.tag) return 1;
    vdrop(verr_take());

    vraise("prior error");
    Val v = vvecv(vvec());
    Val r2 = vpos(v, vint(1));
    if (!verr_pending() || r2.tag != vunwind.tag) return 2;
    vdrop(verr_take());
    vdrop(v);
    return 0;
}}
"""
    cpath = write_file("crt_vpos.c", c_src)
    exe = os.path.join(TMP, "crt_vpos")
    san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"] if sys.platform != "win32" else []
    libs = [] if sys.platform == "win32" else ["-lm", "-lpthread", "-ldl"]
    b = run([cc, "-std=gnu11", "-O1", "-g", "-fwrapv", *san, cpath, "-o", exe, *libs])
    assert b.returncode == 0, b.stderr
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
    r = run([exe], env=env)
    assert r.returncode == 0, (r.stdout, r.stderr)

    conc_src = """fn main() -> i64 {
  let shared = ["item-alpha-heap-string", "item-bravo-heap-string", "item-charlie-heap-string"]
  scope {
    for _k in 0..4 {
      spawn {
        let mut s = 0
        for _i in 0..200 {
          s = s + shared.pos("item-bravo-heap-string") + len(shared[0])
        }
        assert(s == 200 * (1 + 22))
      }
    }
  }
  return 0
}
"""
    p = write_file("conc_pos.brat", conc_src)
    em = run([cxx, "emit-c", "-O2", p])
    assert em.returncode == 0, em.stderr
    conc_c = write_file("conc_pos.c", em.stdout)
    conc_exe = os.path.join(TMP, "conc_pos")
    if sys.platform.startswith("linux"):
        tb = run([cc, "-std=gnu11", "-O1", "-g", "-fwrapv", "-fsanitize=thread",
                  "-I", ENGINE, conc_c, "-o", conc_exe, "-lm", "-lpthread", "-ldl"])
        if tb.returncode == 0:
            tr = run([conc_exe])
            assert tr.returncode == 0, (tr.stdout, tr.stderr)


def main():
    cxx = _cxx.build()
    cc = shutil.which("clang-18") or shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        _skip.skip("no C compiler available")
    test_slice_soundness_and_rc(cxx, cc)
    test_borrow_loe_closures_and_unsafe(cxx, cc)
    test_http_and_net_hardening(cxx, cc)
    test_crt_vpos_and_tsan(cxx, cc)
    print("SLICE, BORROW, AND HTTP HARDENING ALL GREEN")


if __name__ == "__main__":
    main()
