"""Known leaks closed, FFI ladder L1 object, archive and DWARF proofs, `.bratbind` packages,
16-byte small strings, plugin pointer ownership, escape summaries and non-Linux platform parity."""
import _scratch

import importlib.util
import json
import os
import struct
import subprocess
import sys
import tarfile

import _cxx

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
CLI = os.path.join(ENGINE, "cli.py")
CXX = _cxx.build()

sys.path.insert(0, ENGINE)
import ffi as ffi_mod  # noqa: E402

TMP = _scratch.mkdtemp(prefix="brat-leaks-ffi-")
n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def run_py(*args, cwd=None, env=None):
    return subprocess.run([sys.executable, CLI] + list(args),
                          capture_output=True, text=True, timeout=180,
                          cwd=cwd, env=env)


def run_cxx(*args, cwd=None, env=None):
    return subprocess.run([CXX] + list(args),
                          capture_output=True, text=True, timeout=180,
                          cwd=cwd, env=env)


# KNOWN_LEAKS closed (`jsonlibtest.brat`, `newcomer.brat`) + ASan
_asan_spec = importlib.util.spec_from_file_location(
    "t_asan_ex", os.path.join(HERE, "test_asan_examples.py"))
with open(os.path.join(HERE, "test_asan_examples.py"), encoding="utf-8") as _af:
    _asan_txt = _af.read()
check('"jsonlibtest.brat"' not in _asan_txt and '"newcomer.brat"' not in _asan_txt
      and "KNOWN_LEAKS = set()" in _asan_txt,
      "KNOWN_LEAKS must be empty (0 known leaks including syslib.brat)")

leak_brat = os.path.join(TMP, "step36.brat")
with open(leak_brat, "w", encoding="utf-8") as f:
    f.write('''
fn scalar_ret_drops_heap(flag: bool) -> i64 {
  let msg = "allocated_long_heap_string_" + str(123456)
  if flag {
    return len(msg)
  }
  return 0
}

fn partial_return_alias(flag: bool) -> str {
  let m = {"alpha": "long_value_exceeding_sso_limit_12345"}
  let got = m.get("alpha")
  let local_only = "another_long_heap_string_for_branch_drop"
  if flag {
    return got or ""
  }
  return "fallback_" + str(len(local_only))
}

fn main() -> i64 {
  assert(scalar_ret_drops_heap(true) == 33)
  assert(scalar_ret_drops_heap(false) == 0)
  assert(partial_return_alias(true) == "long_value_exceeding_sso_limit_12345")
  assert(partial_return_alias(false) == "fallback_40")
  return 0
}
''')
c_cli = run_py("emit-c", leak_brat,
               env=dict(os.environ, BRAT_CXX=CXX, BRAT_NO_NATIVE_CLI="1"))
c_cx = run_cxx("cc", leak_brat)
check(c_cli.returncode == 0 and c_cx.returncode == 0 and c_cli.stdout == c_cx.stdout,
      "Python CLI/native cc output mismatch")
leak_c = os.path.join(TMP, "step36.c")
leak_exe = os.path.join(TMP, "step36_asan")
with open(leak_c, "w", encoding="utf-8") as f:
    f.write(c_cx.stdout)
cc = subprocess.run(
    ["gcc", "-std=c99", "-O2", "-g", "-fsanitize=address,undefined",
     "-I", ENGINE, leak_c, "-lm", "-pthread", "-o", leak_exe],
    capture_output=True, text=True, timeout=60)
check(cc.returncode == 0, "ASan compile failed: %s" % cc.stderr)
rn = subprocess.run([leak_exe], capture_output=True, text=True, timeout=30,
                    env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"))
check(rn.returncode == 0, "ASan/LSan detected leak: %s" % rn.stderr)


# FFI Ladder L1 — `.o`/`.a`/`.so` + DWARF symbol & ABI size proof
l1_dir = os.path.join(TMP, "l1")
os.makedirs(l1_dir, exist_ok=True)
good_h = os.path.join(l1_dir, " sensor.h".strip())
with open(good_h, "w", encoding="utf-8") as f:
    f.write('''
#include <stdint.h>
typedef struct {
    int64_t id;
    int64_t reading;
} Sensor;
int64_t sensor_sum(Sensor s);
''')
good_c = os.path.join(l1_dir, "sensor_good.c")
with open(good_c, "w", encoding="utf-8") as f:
    f.write('''
#include <stdint.h>
typedef struct { int64_t id; int64_t reading; } Sensor;
int64_t sensor_sum(Sensor s) { return s.id + s.reading; }
''')
drift_c = os.path.join(l1_dir, "sensor_drift.c")
with open(drift_c, "w", encoding="utf-8") as f:
    f.write('''
#include <stdint.h>
/* Binary compiled with an extra 8-byte field the stale header hides! */
typedef struct { int64_t id; int64_t reading; int64_t secret_pad; } Sensor;
int64_t sensor_sum(Sensor s) { return s.id + s.reading + s.secret_pad; }
''')
good_o = os.path.join(l1_dir, "sensor_good.o")
drift_o = os.path.join(l1_dir, "sensor_drift.o")
good_a = os.path.join(l1_dir, "libsensor.a")
subprocess.check_call(["gcc", "-g", "-O2", "-c", good_c, "-o", good_o])
subprocess.check_call(["gcc", "-g", "-O2", "-c", drift_c, "-o", drift_o])
subprocess.check_call(["ar", "rcs", good_a, good_o])

l1_brat = os.path.join(l1_dir, "use_sensor.brat")
with open(l1_brat, "w", encoding="utf-8") as f:
    f.write('''
extern struct Sensor { id: i64, reading: i64 }
extern "%s" fn sensor_sum(s: Sensor) -> i64
fn main() -> i64 {
  let s = Sensor { id: 19, reading: 23 }
  assert(sensor_sum(s) == 42)
  return 0
}
''' % good_o)

r_ok = run_py("ffi", l1_brat, "--header", good_h, "--object", good_o)
check(r_ok.returncode == 0, "L1 good .o rejected: %s" % (r_ok.stdout + r_ok.stderr))
r_ar = run_py("ffi", l1_brat, "--header", good_h, "--object", good_a)
check(r_ar.returncode == 0, "L1 good .a archive rejected: %s" % (r_ar.stdout + r_ar.stderr))
r_drift = run_py("ffi", l1_brat, "--header", good_h, "--object", drift_o)
check(r_drift.returncode != 0 and ("24" in r_drift.stdout + r_drift.stderr or "Sensor" in r_drift.stdout + r_drift.stderr),
      "L1 failed to catch header-vs-.o struct size drift: %s" % (r_drift.stdout + r_drift.stderr))

# Synthetic Mach-O 64-bit LE & PE/COFF x64 symbol table parser checks:
macho_sym = b"\x00_sensor_sum\x00"
macho_hdr = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x01000007, 3, 1, 1, 24, 0, 0)
symtab_cmd = struct.pack("<IIIIII", 0x2, 24, 32 + 24, 1, 32 + 24 + 16, len(macho_sym))
nlist64 = struct.pack("<IBBHQ", 1, 0x0F, 1, 0, 0x1000)  # N_SECT | N_EXT -> `_sensor_sum`
macho_info = ffi_mod.inspect_object_symbols_bytes(macho_hdr + symtab_cmd + nlist64 + macho_sym)
check(macho_info["format"] == "macho" and "sensor_sum" in macho_info["symbols"],
      "Mach-O 64 symbol reader failed: %r" % (macho_info,))

coff_hdr = struct.pack("<HHIIIHH", 0x8664, 1, 0, 20 + 40, 1, 0, 0)
coff_sec = struct.pack("<8sIIIIIIHHI", b".text\x00\x00\x00", 0, 0, 16, 0, 0, 0, 0, 0, 0x60000020)
coff_sym = struct.pack("<8sIHHBB", b" win_fn\x00".replace(b" ", b""), 0, 1, 0x20, 2, 0)
coff_str = struct.pack("<I", 4)
coff_info = ffi_mod.inspect_object_symbols_bytes(coff_hdr + coff_sec + coff_sym + coff_str)
check(coff_info["format"] == "coff" and "win_fn" in coff_info["symbols"],
      "PE/COFF x64 symbol reader failed: %r" % (coff_info,))


# `.bratbind` portable FFI packages (FFI Ladder L4)
pkg_src_dir = os.path.join(TMP, "pkg_src")
os.makedirs(pkg_src_dir, exist_ok=True)
c_math_h = os.path.join(pkg_src_dir, "fastcrc.h")
c_math_c = os.path.join(pkg_src_dir, "fastcrc.c")
c_math_o = os.path.join(pkg_src_dir, "fastcrc.o")
with open(c_math_h, "w", encoding="utf-8") as f:
    f.write("#include <stdint.h>\nint64_t fast_mix(int64_t a, int64_t b);\n")
with open(c_math_c, "w", encoding="utf-8") as f:
    f.write("#include <stdint.h>\nint64_t fast_mix(int64_t a, int64_t b) { return (a * 31) ^ b; }\n")
subprocess.check_call(["gcc", "-g", "-O2", "-fPIC", "-c", c_math_c, "-o", c_math_o])

pkg_brat = os.path.join(pkg_src_dir, "fastcrc.brat")
pkg_bri = os.path.join(pkg_src_dir, "fastcrc.bri")
pkg_bind = os.path.join(pkg_src_dir, "fastcrc.bratbind")
with open(pkg_brat, "w", encoding="utf-8") as f:
    f.write('''
extern fn fast_mix(a: i64, b: i64) -> i64;
pub fn mix(x: i64, y: i64) -> i64 {
  return fast_mix(x, y)
}
''')
with open(pkg_bri, "w", encoding="utf-8") as f:
    f.write("pub fn mix(x: i64, y: i64) -> i64\n")

eb = run_py("ffi", pkg_brat, "--header", c_math_h, "--object", c_math_o, "--emit-bind", pkg_bind)
check(eb.returncode == 0 and os.path.isfile(pkg_bind),
      "--emit-bind failed: %s" % (eb.stdout + eb.stderr))
bind_json = json.loads(open(pkg_bind, encoding="utf-8").read())
check(set(bind_json["models"].keys()) == {"lp64", "llp64", "ilp32"},
      "portable int64_t binding must prove all 3 ABI models: %r" % list(bind_json["models"].keys()))

tgz_path = os.path.join(TMP, "fastcrc.tar.gz")
with tarfile.open(tgz_path, "w:gz") as tf:
    for fn in ("fastcrc.brat", "fastcrc.bri", "fastcrc.bratbind", "fastcrc.o"):
        tf.add(os.path.join(pkg_src_dir, fn), arcname=fn)

app_dir = os.path.join(TMP, "consumer_app")
os.makedirs(app_dir, exist_ok=True)
add_r = run_py("pkg", "add", "file://" + tgz_path, cwd=app_dir)
check(add_r.returncode == 0, "brat pkg add .bratbind archive failed: %s" % (add_r.stdout + add_r.stderr))
verify_r = run_py("pkg", "verify", cwd=app_dir)
check(verify_r.returncode == 0, "brat pkg verify failed: %s" % (verify_r.stdout + verify_r.stderr))

app_brat = os.path.join(app_dir, "app.brat")
with open(app_brat, "w", encoding="utf-8") as f:
    f.write('''
import fastcrc.fastcrc
fn main() -> i64 {
  assert(fastcrc.fastcrc.mix(10, 7) == 305)
  print(fastcrc.fastcrc.mix(10, 7))
  return 0
}
''')
vendored_o = os.path.join(app_dir, "vendor", "fastcrc", "fastcrc.o")
app_exe_py = os.path.join(app_dir, "app_py")
app_exe_cx = os.path.join(app_dir, "app_cx")
# Build WITHOUT --header and WITHOUT --unsafe-ffi through both drivers!
b_py = run_py("build", app_brat, "--link", vendored_o, "-o", app_exe_py, cwd=app_dir)
check(b_py.returncode == 0, "Python build with .bratbind failed: %s" % (b_py.stdout + b_py.stderr))
b_cx = run_cxx("build", app_brat, "--link", vendored_o, "-o", app_exe_cx, cwd=app_dir)
check(b_cx.returncode == 0, "C++ build with .bratbind failed: %s" % (b_cx.stdout + b_cx.stderr))
out_py = subprocess.check_output([app_exe_py], text=True)
out_cx = subprocess.check_output([app_exe_cx], text=True)
check(out_py.strip() == "305" and out_cx.strip() == "305",
      ".bratbind binary output mismatch: %r vs %r" % (out_py, out_cx))


# 16B SSO (`<= 14B`) zero-malloc loop + FFI `vstr_pin` safety
sso_dir = os.path.join(TMP, "sso")
os.makedirs(sso_dir, exist_ok=True)
c_str_h = os.path.join(sso_dir, "cstr.h")
c_str_c = os.path.join(sso_dir, "cstr.c")
c_str_o = os.path.join(sso_dir, "cstr.o")
with open(c_str_h, "w", encoding="utf-8") as f:
    f.write('''
#include <stdint.h>
void remember_cstr(const char *s);
int64_t check_remembered(const char *expect);
''')
with open(c_str_c, "w", encoding="utf-8") as f:
    f.write('''
#include <stdint.h>
#include <string.h>
static const char *g_saved = 0;
void remember_cstr(const char *s) { g_saved = s; }
int64_t check_remembered(const char *expect) {
    return (g_saved && strcmp(g_saved, expect) == 0) ? 1 : 0;
}
''')
subprocess.check_call(["gcc", "-O2", "-c", c_str_c, "-o", c_str_o])

sso_brat = os.path.join(sso_dir, "sso_pin.brat")
with open(sso_brat, "w", encoding="utf-8") as f:
    f.write('''
extern fn remember_cstr(s: str)
extern fn check_remembered(expect: str) -> i64

fn make_and_pass() {
  // Computed short string lives inline inside 16B `Val` (SSO), passed by value;
  // `vstr_pin` interns it so `g_saved` never dangles after `make_and_pass` returns!
  let s = "sso_" + "pin" + "_14"
  remember_cstr(s)
}

fn hot_sso(n) {
  let mut i = 0
  let mut hits = 0
  while i < n {
    let a = "k_" + "99" + "_ok"
    let b = a.slice(0, 7)
    if b == "k_99_ok" && len(b) == 7 {
      hits = hits + 1
    }
    i = i + 1
  }
  return hits
}

fn main() -> i64 {
  make_and_pass()
  assert(check_remembered("sso_pin_14") == 1)
  assert(hot_sso(100000) == 100000)
  return 0
}
''')
sso_c = os.path.join(sso_dir, "sso_pin.c")
with open(sso_c, "w", encoding="utf-8") as f:
    f.write(run_cxx("cc", "--no-check", sso_brat).stdout)
wrap_c = os.path.join(sso_dir, "wrap_alloc.c")
with open(wrap_c, "w", encoding="utf-8") as f:
    f.write(r'''
#include <stdio.h>
#include <stdlib.h>
static long long g_allocs = 0;
static int g_armed = 0;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__wrap_malloc(size_t n) { if (g_armed) g_allocs++; return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { if (g_armed) g_allocs++; return __real_calloc(a, b); }
__attribute__((constructor)) static void arm(void) { g_armed = 1; }
__attribute__((destructor)) static void rep(void) { fprintf(stderr, "SSO_ALLOCS=%lld\n", g_allocs); }
''')
sso_exe = os.path.join(sso_dir, "sso_pin")
subprocess.check_call([
    "gcc", "-std=c99", "-O2", "-I", ENGINE, sso_c, c_str_o, wrap_c,
    "-Wl,--wrap=malloc,--wrap=calloc", "-lm", "-pthread", "-ldl", "-o", sso_exe])
sso_run = subprocess.run([sso_exe], capture_output=True, text=True, check=True)
alloc_line = [ln for ln in sso_run.stderr.splitlines() if ln.startswith("SSO_ALLOCS=")][0]
alloc_cnt = int(alloc_line.split("=", 1)[1])
check(alloc_cnt <= 16, "SSO hot loop allocated %d times on the heap (expected <= 16)" % alloc_cnt)


# Plugin pointer ownership (`&ptr`, `own ptr`, `out ptr`, cb) & gate
p38_dir = os.path.join(TMP, "p38")
os.makedirs(p38_dir, exist_ok=True)
plug_c = os.path.join(p38_dir, "p38.c")
plug_so = os.path.join(p38_dir, "libp38.so")
plug_h = os.path.join(p38_dir, "p38.h")
with open(plug_h, "w", encoding="utf-8") as f:
    f.write('''
#include <stdint.h>
void *mk_box(int64_t v);
int64_t rd_box(const void *p);
void free_box(void *p);
void fill_i64(int64_t *out, int64_t v);
int64_t apply_cb(int64_t (*cb)(int64_t), int64_t x);
''')
with open(plug_c, "w", encoding="utf-8") as f:
    f.write('''
#include <stdint.h>
#include <stdlib.h>
void *mk_box(int64_t v) { int64_t *p = malloc(sizeof(int64_t)); *p = v; return p; }
int64_t rd_box(const void *p) { return *(const int64_t *)p; }
void free_box(void *p) { free(p); }
void fill_i64(int64_t *out, int64_t v) { *out = v; }
int64_t apply_cb(int64_t (*cb)(int64_t), int64_t x) { return cb(x) + 1; }
''')
subprocess.check_call(["gcc", "-shared", "-fPIC", "-O2", plug_c, "-o", plug_so])

# Unowned `own ptr` leak refused identically by Python & C++ checkers:
leak_own = os.path.join(p38_dir, "leak_own.brat")
with open(leak_own, "w", encoding="utf-8") as f:
    f.write('''
fn main() -> i64 {
  let h = lib_open("%s")
  let mk = lib_sym(h, "mk_box")
  let p = call(mk, "own ptr(i64)", 9)
  lib_close(h)
  return 0
}
''' % plug_so)
r_lp = run_py("check", "--py", leak_own)
r_lc = run_cxx("check", leak_own)
check(r_lp.returncode != 0 and r_lc.returncode != 0 and r_lp.stdout == r_lc.stdout,
      "own ptr leak parity mismatch:\nPY:\n%s\nCXX:\n%s" % (r_lp.stdout, r_lc.stdout))

# Valid plugin own ptr + &ptr + out ptr + callback retention + --header proof:
ok_p38 = os.path.join(p38_dir, "ok_p38.brat")
with open(ok_p38, "w", encoding="utf-8") as f:
    f.write('''
fn triple(x: i64) -> i64 { return x * 3 }

fn main() -> i64 {
  let h = lib_open("%s")
  let mk = lib_sym(h, "mk_box")
  let rd = lib_sym(h, "rd_box")
  let fr = lib_sym(h, "free_box")
  let fl = lib_sym(h, "fill_i64")
  let ap = lib_sym(h, "apply_cb")

  let p = call(mk, "own ptr(i64)", 77)
  let v = call(rd, "i64(&ptr)", p)
  call(fr, "none(own ptr)", move p)

  let mut slot = 0
  call(fl, "none(out ptr,i64)", &slot, 25)
  let cb_res = call(ap, "i64(fn(i64)->i64,i64)", "triple", 10)
  lib_close(h)
  print(v + slot + cb_res)
  return 0
}
''' % plug_so)
# Gate refuses build without --header / --unsafe-ffi even without `extern fn`:
gate_ref = run_py("build", ok_p38, "-o", os.path.join(p38_dir, "refused"))
check(gate_ref.returncode != 0 and "--header" in gate_ref.stderr,
      "FFI gate did not catch plugin call without --header: %s" % gate_ref.stderr)
# Gate accepts with --header (proving `call(sym, sig)` against `p38.h`!):
p38_exe = os.path.join(p38_dir, "ok_p38_exe")
b_p38 = run_py("build", ok_p38, "--header", plug_h, "-o", p38_exe)
check(b_p38.returncode == 0, "proven plugin build failed: %s" % (b_p38.stdout + b_p38.stderr))
i_p38 = run_py("run", "--interp", "--header", plug_h, ok_p38)
n_p38 = subprocess.run([p38_exe], capture_output=True, text=True, check=True)
check(i_p38.returncode == 0 and i_p38.stdout.strip() == "133" and n_p38.stdout.strip() == "133",
      "interp vs native mismatch: %r vs %r" % (i_p38.stdout, n_p38.stdout))


# Cross-function escape summaries & `spawn` graph isolation
p39_escape = os.path.join(TMP, "p39_escape.brat")
with open(p39_escape, "w", encoding="utf-8") as f:
    f.write('''
fn stash(mut sink, item) {
  sink.push(item)
}

fn main() -> i64 {
  let mut outer = []
  arena {
    let temp = [1, 2, 3]
    stash(outer, temp)
  }
  print(len(outer))
  return 0
}
''')
e_py = run_py("check", "--py", p39_escape)
e_cx = run_cxx("check", p39_escape)
check(e_py.returncode != 0 and e_cx.returncode != 0 and e_py.stdout == e_cx.stdout,
      "cross-fn arena escape parity failed:\nPY:\n%s\nCXX:\n%s" % (e_py.stdout, e_cx.stdout))

p39_spawn = os.path.join(TMP, "p39_spawn.brat")
with open(p39_spawn, "w", encoding="utf-8") as f:
    f.write('''
extern fn malloc(n: i64) -> ptr
extern fn free(move p: ptr)

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    ptr_write_i64(p, 42)
  }
  free(move p)
  return 0
}
''')
s_py = run_py("check", "--py", p39_spawn)
s_cx = run_cxx("check", p39_spawn)
check(s_py.returncode != 0 and s_cx.returncode != 0 and s_py.stdout == s_cx.stdout,
      "spawn pointer race/isolation parity failed:\nPY:\n%s\nCXX:\n%s" % (s_py.stdout, s_cx.stdout))


# Non-Linux platform parity (`_WIN32`, `__APPLE__`) & L5 ABI algebra
win_inc = os.path.join(TMP, "win_sdk")
os.makedirs(win_inc, exist_ok=True)
with open(os.path.join(win_inc, "winsock2.h"), "w", encoding="utf-8") as f:
    f.write('''
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uintptr_t SOCKET;
#define INVALID_SOCKET ((SOCKET)(~0))
#define SOCKET_ERROR (-1)
#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 4
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#define INADDR_NONE 0xffffffffU
#define WSAEINTR 10004
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAEALREADY 10037
#define WSAETIMEDOUT 10060
#define SO_ERROR 0x1007
#define SD_RECEIVE 0
#define SD_SEND 1
#define SD_BOTH 2
#define FIONBIO 0x8004667e
#define MAKEWORD(a, b) ((uint16_t)(((uint8_t)(a)) | ((uint16_t)((uint8_t)(b))) << 8))
typedef unsigned long u_long;
typedef struct { unsigned int fd_count; SOCKET fd_array[64]; } fd_set;
#define FD_ZERO(set) ((set)->fd_count = 0)
#define FD_SET(fd, set) ((set)->fd_array[(set)->fd_count++] = (fd))
struct timeval { long tv_sec; long tv_usec; };
typedef struct { uint16_t wVersion; } WSADATA;
struct in_addr { uint32_t s_addr; };
struct sockaddr { uint16_t sa_family; char sa_data[14]; };
struct sockaddr_in { int16_t sin_family; uint16_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
int WSAStartup(uint16_t, WSADATA *);
int WSAGetLastError(void);
void WSASetLastError(int);
uint32_t inet_addr(const char *);
uint16_t htons(uint16_t);
SOCKET socket(int, int, int);
int setsockopt(SOCKET, int, int, const char *, int);
int bind(SOCKET, const struct sockaddr *, int);
int listen(SOCKET, int);
SOCKET accept(SOCKET, struct sockaddr *, int *);
int connect(SOCKET, const struct sockaddr *, int);
int ioctlsocket(SOCKET, long, u_long *);
int select(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
int getsockopt(SOCKET, int, int, char *, int *);
int recv(SOCKET, char *, int, int);
int send(SOCKET, const char *, int, int);
int shutdown(SOCKET, int);
int closesocket(SOCKET);
''')
with open(os.path.join(win_inc, "ws2tcpip.h"), "w", encoding="utf-8") as f:
    f.write("#pragma once\n")
with open(os.path.join(win_inc, "io.h"), "w", encoding="utf-8") as f:
    f.write('''
#pragma once
#include <stdint.h>
#include <stdio.h>
intptr_t _get_osfhandle(int fd);
int _open_osfhandle(intptr_t osfhandle, int flags);
int _close(int fd);
FILE *_fdopen(int fd, const char *mode);
int _fileno(void *);
#ifndef fileno
#define fileno _fileno
#endif
''')
with open(os.path.join(win_inc, "fcntl.h"), "w", encoding="utf-8") as f:
    f.write('''
#pragma once
#define _O_RDONLY 0x0000
#define _O_WRONLY 0x0001
#define _O_BINARY 0x8000
#define _O_NOINHERIT 0x0080
''')
with open(os.path.join(win_inc, "windows.h"), "w", encoding="utf-8") as f:
    f.write('''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef size_t SIZE_T;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HMODULE;
typedef void *PVOID;
typedef int (*FARPROC)(void);
typedef struct { PVOID Ptr; } SRWLOCK;
#define SRWLOCK_INIT {0}
typedef struct { PVOID Ptr; } CRITICAL_SECTION;
typedef struct { PVOID Ptr; } CONDITION_VARIABLE;
typedef struct { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME;
typedef union { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { DWORD nLength; void *lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION;
typedef struct { DWORD LimitFlags; } JOBOBJECT_BASIC_LIMIT_INFORMATION;
typedef struct {
    JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
    SIZE_T ProcessMemoryLimit;
    SIZE_T JobMemoryLimit;
} JOBOBJECT_EXTENDED_LIMIT_INFORMATION;
#define JobObjectExtendedLimitInformation 9
#define JOB_OBJECT_LIMIT_PROCESS_MEMORY 0x00000100
#define JOB_OBJECT_LIMIT_JOB_MEMORY 0x00000200
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000
#define WINAPI
#define FALSE 0
#define TRUE 1
#define INFINITE 0xFFFFFFFFUL
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 0x00000008
#define GENERIC_READ 0x80000000UL
#define GENERIC_WRITE 0x40000000UL
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
#define FILE_TYPE_DISK 0x0001
#define FILE_BEGIN 0
#define ERROR_FILE_NOT_FOUND 2UL
#define ERROR_PATH_NOT_FOUND 3UL
#define HANDLE_FLAG_INHERIT 0x00000001
#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOOPENFILEERRORBOX 0x8000
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00001000
void InitializeSRWLock(SRWLOCK *);
void AcquireSRWLockExclusive(SRWLOCK *);
void ReleaseSRWLockExclusive(SRWLOCK *);
void InitializeCriticalSection(CRITICAL_SECTION *);
void EnterCriticalSection(CRITICAL_SECTION *);
void LeaveCriticalSection(CRITICAL_SECTION *);
void InitializeConditionVariable(CONDITION_VARIABLE *);
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *, CRITICAL_SECTION *, DWORD);
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *, SRWLOCK *, DWORD, DWORD);
void WakeConditionVariable(CONDITION_VARIABLE *);
void WakeAllConditionVariable(CONDITION_VARIABLE *);
void GetSystemTimeAsFileTime(FILETIME *);
uint64_t GetTickCount64(void);
void Sleep(DWORD);
int MultiByteToWideChar(UINT, DWORD, const char *, int, wchar_t *, int);
int WideCharToMultiByte(UINT, DWORD, const wchar_t *, int, char *, int, const char *, BOOL *);
int _snwprintf(wchar_t *, size_t, const wchar_t *, ...);
DWORD GetFileAttributesA(const char *);
DWORD GetFileAttributesW(const wchar_t *);
HANDLE CreateFileW(const wchar_t *, DWORD, DWORD, SECURITY_ATTRIBUTES *, DWORD, DWORD, HANDLE);
DWORD GetFileType(HANDLE);
BOOL GetFileInformationByHandle(HANDLE, BY_HANDLE_FILE_INFORMATION *);
BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER, LARGE_INTEGER *, DWORD);
BOOL SetEndOfFile(HANDLE);
BOOL CloseHandle(HANDLE);
HANDLE CreateJobObjectW(SECURITY_ATTRIBUTES *, const wchar_t *);
BOOL SetInformationJobObject(HANDLE, int, const void *, DWORD);
BOOL AssignProcessToJobObject(HANDLE, HANDLE);
HANDLE GetCurrentProcess(void);
char *_fullpath(char *, const char *, size_t);
int _strnicmp(const char *, const char *, size_t);
char *_strdup(const char *);
#ifndef strdup
#define strdup _strdup
#endif
#define MOVEFILE_REPLACE_EXISTING 0x1
BOOL MoveFileExA(const char *, const char *, DWORD);
BOOL MoveFileExW(const wchar_t *, const wchar_t *, DWORD);
BOOL DeleteFileW(const wchar_t *);
int _unlink(const char *);
BOOL SetHandleInformation(HANDLE, DWORD, DWORD);
DWORD GetFinalPathNameByHandleA(HANDLE, char *, DWORD, DWORD);
DWORD GetFinalPathNameByHandleW(HANDLE, wchar_t *, DWORD, DWORD);
UINT SetErrorMode(UINT);
HMODULE LoadLibraryExA(const char *, HANDLE, DWORD);
DWORD GetLastError(void);
DWORD FormatMessageA(DWORD, const void *, DWORD, DWORD, char *, DWORD, void *);
FARPROC GetProcAddress(HMODULE, const char *);
BOOL FreeLibrary(HMODULE);
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(void *);
HANDLE CreateThread(void *, size_t, LPTHREAD_START_ROUTINE, void *, DWORD, DWORD *);
''')

plat_c = os.path.join(TMP, "plat_hello.c")
with open(plat_c, "w", encoding="utf-8") as f:
    f.write(run_cxx("cc", os.path.join(ROOT, "lang", "examples", "hello.brat")).stdout)

r_win = subprocess.run(
    ["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-U__linux__", "-D_WIN32",
     "-I", win_inc, "-I", ENGINE, "-fsyntax-only", plat_c],
    capture_output=True, text=True, timeout=30)
check(r_win.returncode == 0, "_WIN32 C99 runtime check failed:\n%s" % r_win.stderr)

r_mac = subprocess.run(
    ["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-U__linux__", "-D__APPLE__",
     "-D_DEFAULT_SOURCE=1", "-D_XOPEN_SOURCE=700", "-DF_GETPATH=50", "-DSO_NOSIGPIPE=0x1022",
     "-I", ENGINE, "-fsyntax-only", plat_c],
    capture_output=True, text=True, timeout=30)
check(r_mac.returncode == 0, "__APPLE__ C99 runtime check failed:\n%s" % r_mac.stderr)

# Plugin confinement outside program base directory is refused in both backends:
conf_dir = os.path.join(TMP, "confined_sub")
os.makedirs(conf_dir, exist_ok=True)
esc_plug = os.path.join(conf_dir, "escape_plug.brat")
with open(esc_plug, "w", encoding="utf-8") as f:
    f.write('fn main() -> i64 {\n  let h = lib_open("../p38/libp38.so")\n  return h\n}\n')
ep_i = run_py("run", "--interp", "--unsafe-ffi", esc_plug)
ep_exe = os.path.join(conf_dir, "esc_exe")
run_py("build", "--unsafe-ffi", esc_plug, "-o", ep_exe)
ep_n = subprocess.run([ep_exe], capture_output=True, text=True)
check(ep_i.returncode != 0 and "outside the program directory" in (ep_i.stdout + ep_i.stderr),
      "interp lib_open allowed path escape: %s" % (ep_i.stdout + ep_i.stderr))
check(ep_n.returncode != 0 and "outside the program directory" in (ep_n.stdout + ep_n.stderr),
      "native lib_open allowed path escape: %s" % (ep_n.stdout + ep_n.stderr))

# FFI Ladder L5: `--abi-matrix` differential sizeof/offsetof across LP64, LLP64, ILP32:
l5_h_portable = os.path.join(TMP, "l5_port.h")
with open(l5_h_portable, "w", encoding="utf-8") as f:
    f.write('''
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint8_t tag;
    int64_t ts;
    size_t len;
} Packet;
size_t packet_len(Packet p);
''')
l5_brat_portable = os.path.join(TMP, "l5_port.brat")
with open(l5_brat_portable, "w", encoding="utf-8") as f:
    f.write('''
extern struct Packet { tag: u8, ts: i64, len: usize }
extern fn packet_len(p: Packet) -> usize
fn main() -> i64 { return 0 }
''')
m_ok = run_cxx("ffi", l5_brat_portable, "--header", l5_h_portable, "--abi-matrix", "--json")
check(m_ok.returncode == 0, "portable L5 ABI matrix failed: %s" % (m_ok.stdout + m_ok.stderr))
m_data = json.loads(m_ok.stdout)
check(m_data["ok"] is True and len(m_data["targets"]) == 5, "unexpected L5 matrix: %r" % m_data)
pkt_diff = m_data["differential"]["Packet"]
# Verify differential sizes across LP64 (24B), i686 (16B: align8=4 -> offsets 0,4,12), wasm32 (24B: align8=8 -> offsets 0,8,16)!
check(pkt_diff["x86_64-unknown-linux-gnu"]["brat"]["size"] == 24, "LP64 Packet size != 24")
check(pkt_diff["i686-unknown-linux-gnu"]["brat"]["size"] == 16 and
      pkt_diff["i686-unknown-linux-gnu"]["brat"]["offsets"] == {"tag": 0, "ts": 4, "len": 12},
      "i686 ILP32 Packet layout wrong: %r" % pkt_diff["i686-unknown-linux-gnu"])
check(pkt_diff["wasm32-unknown-wasi"]["brat"]["size"] == 24 and
      pkt_diff["wasm32-unknown-wasi"]["brat"]["offsets"] == {"tag": 0, "ts": 8, "len": 16},
      "wasm32 ILP32 Packet layout wrong: %r" % pkt_diff["wasm32-unknown-wasi"])

# Non-portable C `long` bound to `i64`: passes LP64, caught by `--abi-matrix` on LLP64 & ILP32!
l5_h_bad = os.path.join(TMP, "l5_bad.h")
with open(l5_h_bad, "w", encoding="utf-8") as f:
    f.write("typedef struct { char tag; long count; } BadLong;\n")
l5_brat_bad = os.path.join(TMP, "l5_bad.brat")
with open(l5_brat_bad, "w", encoding="utf-8") as f:
    f.write("extern struct BadLong { tag: u8, count: i64 }\nfn main() -> i64 { return 0 }\n")
m_bad = run_py("ffi", l5_brat_bad, "--header", l5_h_bad, "--abi-matrix", "--json")
check(m_bad.returncode == 1, "non-portable `long` passed --abi-matrix!")
m_bad_data = json.loads(m_bad.stdout)
by_model = {t["target"]: t["errors"] for t in m_bad_data["targets"]}
check(by_model["x86_64-unknown-linux-gnu"] == 0 and
      by_model["aarch64-apple-darwin"] == 0 and
      by_model["x86_64-pc-windows-msvc"] > 0 and
      by_model["i686-unknown-linux-gnu"] > 0 and
      by_model["wasm32-unknown-wasi"] > 0,
      "L5 differential model detection wrong: %r" % by_model)

print("ROADMAP CLOSURE ALL GREEN (%d asserts)" % n)
