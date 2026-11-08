#!/usr/bin/env python3
"""Reproducible BRAT benchmark suite (`make bench`).

Measures wall-clock execution time (median and best of N runs) across:
  1. fib(35)              -- recursive i64 calls (29.86M calls)
  2. int_loop(100M)       -- tight 64-bit wrapping integer loop
  3. vec3_sra(50M)        -- loop-carried nested struct (Body { pos: Vec3, vel: Vec3 })
  4. str_append(200k)     -- chained string concatenation in a loop
  5. dyn_records(100k)    -- map allocation + string concat + RC drops
  6. json_roundtrip(5k)   -- std.json.parse + std.json.stringify on nested objects

When `go` is on PATH (unless `--no-go` is passed), Go equivalents are compiled
and timed alongside C, BRAT, and CPython.

Also measures single-function `-O2` compile-time scaling across 50..400 managed
string locals with the compilation cache disabled.
"""

import argparse
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CXX = os.environ.get("BRAT_CXX") or os.path.join(ROOT, "lang", "cxx", "bin", "brat-cxx")
CC = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang") or shutil.which("cc") or "cc"
GO = os.environ.get("GO") or shutil.which("go")


def cpu_model() -> str:
    if os.path.isfile("/proc/cpuinfo"):
        with open("/proc/cpuinfo", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.lower().startswith("model name"):
                    return line.split(":", 1)[1].strip()
    return platform.processor() or platform.machine()


def one_line_ver(cmd) -> str:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        out = (r.stdout or r.stderr).strip().splitlines()
        return out[0] if out else "unknown"
    except Exception:
        return "unavailable"


def bench_cmd(cmd, runs: int, expect_stdout: str):
    times = []
    for _ in range(runs):
        t0 = time.perf_counter()
        r = subprocess.run(cmd, capture_output=True, text=True, check=True)
        dt = time.perf_counter() - t0
        got = r.stdout.strip()
        if got != expect_stdout:
            raise RuntimeError(f"{cmd} printed {got!r}, expected {expect_stdout!r}")
        times.append(dt)
    return statistics.median(times), min(times)


FIB_C = r"""
#include <stdint.h>
#include <stdio.h>
__attribute__((noinline)) static int64_t fib(int64_t n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
int main(void) {
    printf("%lld\n", (long long)fib(35));
    return 0;
}
"""

FIB_GO = """\
package main
import "fmt"
//go:noinline
func fib(n int64) int64 {
\tif n < 2 {
\t\treturn n
\t}
\treturn fib(n-1) + fib(n-2)
}
func main() {
\tfmt.Println(fib(35))
}
"""

FIB_BRAT = """\
fn fib(n: i64) -> i64 {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}
fn main() -> i64 {
  print(fib(35))
  return 0
}
"""

FIB_PY = """\
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(35))
"""

LOOP_C = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
__attribute__((noinline)) static int64_t run_loop(int64_t n) {
    uint64_t acc = 1;
    for (int64_t i = 1; i <= n; ++i) {
        acc = acc * 6364136223846793005ULL + (uint64_t)i;
    }
    int64_t out;
    memcpy(&out, &acc, sizeof out);
    return out;
}
int main(void) {
    printf("%lld\n", (long long)run_loop(100000000LL));
    return 0;
}
"""

LOOP_GO = """\
package main
import "fmt"
//go:noinline
func runLoop(n int64) int64 {
\tvar acc uint64 = 1
\tfor i := int64(1); i <= n; i++ {
\t\tacc = acc*6364136223846793005 + uint64(i)
\t}
\treturn int64(acc)
}
func main() {
\tfmt.Println(runLoop(100000000))
}
"""

LOOP_BRAT = """\
fn run_loop(n: i64) -> i64 {
  let mut acc = 1
  let mut i = 1
  while i <= n {
    acc = acc * 6364136223846793005 + i
    i = i + 1
  }
  return acc
}
fn main() -> i64 {
  print(run_loop(100000000))
  return 0
}
"""

LOOP_PY = """\
MASK = (1 << 64) - 1
SIGN = 1 << 63
def run_loop(n):
    acc = 1
    i = 1
    while i <= n:
        acc = (acc * 6364136223846793005 + i) & MASK
        i += 1
    return acc - (1 << 64) if (acc & SIGN) else acc

print(run_loop(100000000))
"""

VEC3_C = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t x, y, z; } Vec3;
typedef struct { Vec3 pos, vel; } Body;
__attribute__((noinline)) static int64_t run_vec3(int64_t n) {
    uint64_t acc = 1;
    for (int64_t i = 0; i < n; ++i) {
        uint64_t u = (uint64_t)(i ^ ((int64_t)acc >> 32));
        Body b = { { u, u + 1ULL, u + 2ULL }, { 3ULL, 5ULL, 7ULL } };
        b.pos.x += b.vel.x;
        b.pos.y += b.vel.y;
        b.pos.z += b.vel.z;
        acc = acc * 2862933555777941757ULL + b.pos.x * 3ULL + b.pos.y * 5ULL + b.pos.z * 7ULL;
    }
    int64_t out;
    memcpy(&out, &acc, sizeof out);
    return out;
}
int main(void) {
    printf("%lld\n", (long long)run_vec3(50000000LL));
    return 0;
}
"""

VEC3_GO = """\
package main
import "fmt"
type Vec3 struct{ x, y, z uint64 }
type Body struct{ pos, vel Vec3 }
//go:noinline
func runVec3(n int64) int64 {
\tvar acc uint64 = 1
\tfor i := int64(0); i < n; i++ {
\t\tu := uint64(i ^ (int64(acc) >> 32))
\t\tb := Body{Vec3{u, u + 1, u + 2}, Vec3{3, 5, 7}}
\t\tb.pos.x += b.vel.x
\t\tb.pos.y += b.vel.y
\t\tb.pos.z += b.vel.z
\t\tacc = acc*2862933555777941757 + b.pos.x*3 + b.pos.y*5 + b.pos.z*7
\t}
\treturn int64(acc)
}
func main() {
\tfmt.Println(runVec3(50000000))
}
"""

VEC3_BRAT = """\
struct Vec3 {
  x: i64,
  y: i64,
  z: i64,
}
struct Body {
  pos: Vec3,
  vel: Vec3,
}
fn run_vec3(n: i64) -> i64 {
  let mut acc = 1
  let mut i = 0
  while i < n {
    let u = i ^ (acc >> 32)
    let mut b = Body {
      pos: Vec3 { x: u, y: u + 1, z: u + 2 },
      vel: Vec3 { x: 3, y: 5, z: 7 },
    }
    b.pos.x = b.pos.x + b.vel.x
    b.pos.y = b.pos.y + b.vel.y
    b.pos.z = b.pos.z + b.vel.z
    acc = acc * 2862933555777941757 + b.pos.x * 3 + b.pos.y * 5 + b.pos.z * 7
    i = i + 1
  }
  return acc
}
fn main() -> i64 {
  print(run_vec3(50000000))
  return 0
}
"""

VEC3_PY = """\
MASK = (1 << 64) - 1
SIGN = 1 << 63
def to_i64(u):
    u &= MASK
    return u - (1 << 64) if (u & SIGN) else u
class Vec3:
    __slots__ = ("x", "y", "z")
    def __init__(self, x, y, z):
        self.x = x; self.y = y; self.z = z
class Body:
    __slots__ = ("pos", "vel")
    def __init__(self, pos, vel):
        self.pos = pos; self.vel = vel
def run_vec3(n):
    acc = 1
    for i in range(n):
        u = to_i64(i ^ (acc >> 32))
        b = Body(Vec3(u, to_i64(u + 1), to_i64(u + 2)), Vec3(3, 5, 7))
        b.pos.x = to_i64(b.pos.x + b.vel.x)
        b.pos.y = to_i64(b.pos.y + b.vel.y)
        b.pos.z = to_i64(b.pos.z + b.vel.z)
        acc = to_i64(acc * 2862933555777941757 + b.pos.x * 3 + b.pos.y * 5 + b.pos.z * 7)
    return acc

print(run_vec3(50000000))
"""

STR_C = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
__attribute__((noinline)) static size_t run_str(int64_t n) {
    size_t cap = 64, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) abort();
    char num[32];
    for (int64_t i = 0; i < n; ++i) {
        int k = snprintf(num, sizeof num, "%lld", (long long)i);
        size_t need = len + 3 + (size_t)k + 1;
        if (need > cap) {
            while (cap < need) cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (!buf) abort();
        }
        buf[len++] = 'k';
        buf[len++] = '=';
        memcpy(buf + len, num, (size_t)k);
        len += (size_t)k;
        buf[len++] = ';';
    }
    buf[len] = '\0';
    free(buf);
    return len;
}
int main(void) {
    printf("%zu\n", run_str(200000));
    return 0;
}
"""

STR_GO = """\
package main
import (
\t"fmt"
\t"strconv"
\t"strings"
)
//go:noinline
func runStr(n int64) int {
\tvar b strings.Builder
\tfor i := int64(0); i < n; i++ {
\t\tb.WriteString("k")
\t\tb.WriteString("=")
\t\tb.WriteString(strconv.FormatInt(i, 10))
\t\tb.WriteString(";")
\t}
\treturn len(b.String())
}
func main() {
\tfmt.Println(runStr(200000))
}
"""

STR_BRAT = """\
fn run_str(n: i64) -> i64 {
  let mut s = ""
  let mut i = 0
  while i < n {
    s = s + "k" + "=" + str(i) + ";"
    i = i + 1
  }
  return len(s)
}
fn main() -> i64 {
  print(run_str(200000))
  return 0
}
"""

STR_PY = """\
def run_str(n):
    s = ""
    for i in range(n):
        s += "k=" + str(i) + ";"
    return len(s)

print(run_str(200000))
"""

DYN_GO = """\
package main
import (
\t"fmt"
\t"strconv"
)
//go:noinline
func runDyn(n int64) int64 {
\tvar total int64
\tfor i := int64(0); i < n; i++ {
\t\tlabel := "item_" + strconv.FormatInt(i, 10)
\t\trec := map[string]any{"id": i, "label": label}
\t\ttotal += rec["id"].(int64) + int64(len(rec["label"].(string)))
\t}
\treturn total
}
func main() {
\tfmt.Println(runDyn(100000))
}
"""

DYN_BRAT = """\
fn run_dyn(n: i64) -> i64 {
  let mut total = 0
  let mut i = 0
  while i < n {
    let label = "item_" + str(i)
    let rec = {"id": i, "label": label}
    total = total + rec["id"] + len(rec["label"])
    i = i + 1
  }
  return total
}
fn main() -> i64 {
  print(run_dyn(100000))
  return 0
}
"""

DYN_PY = """\
def run_dyn(n):
    total = 0
    for i in range(n):
        label = "item_" + str(i)
        rec = {"id": i, "label": label}
        total += rec["id"] + len(rec["label"])
    return total

print(run_dyn(100000))
"""

JSON_GO = """\
package main
import (
\t"encoding/json"
\t"fmt"
)
//go:noinline
func runJSON(n int) int64 {
\traw := []byte(`{"service":"edge-gw","port":8443,"tls":true,"tags":["eu","prod","v2"],"limits":{"rps":1200,"burst":250}}`)
\tvar total int64
\tfor i := 0; i < n; i++ {
\t\tvar doc map[string]any
\t\tif err := json.Unmarshal(raw, &doc); err != nil {
\t\t\tpanic(err)
\t\t}
\t\tout, err := json.Marshal(doc)
\t\tif err != nil {
\t\t\tpanic(err)
\t\t}
\t\ttotal += int64(doc["port"].(float64)) + int64(len(out))
\t}
\treturn total
}
func main() {
\tfmt.Println(runJSON(5000))
}
"""

JSON_BRAT = """\
import std.json
fn run_json(n: i64) -> i64 {
  let raw = "{\\"service\\":\\"edge-gw\\",\\"port\\":8443,\\"tls\\":true,\\"tags\\":[\\"eu\\",\\"prod\\",\\"v2\\"],\\"limits\\":{\\"rps\\":1200,\\"burst\\":250}}"
  let mut total = 0
  let mut i = 0
  while i < n {
    let doc = unwrap(std.json.parse(raw))
    let out = std.json.stringify(doc)
    total = total + doc["port"] + len(out)
    i = i + 1
  }
  return total
}
fn main() -> i64 {
  print(run_json(5000))
  return 0
}
"""

JSON_PY = """\
import json
def run_json(n):
    raw = '{"service":"edge-gw","port":8443,"tls":true,"tags":["eu","prod","v2"],"limits":{"rps":1200,"burst":250}}'
    total = 0
    for _ in range(n):
        doc = json.loads(raw)
        out = json.dumps(doc, separators=(",", ":"), sort_keys=True)
        total += doc["port"] + len(out)
    return total

print(run_json(5000))
"""


def fmt_cell(pair) -> str:
    m, b = pair
    return f"{m:.3f}s ({b:.3f}s)"


def build_go(td: str, name: str, src: str) -> str:
    p_src = os.path.join(td, f"{name}.go")
    p_exe = os.path.join(td, f"{name}_go")
    with open(p_src, "w", encoding="utf-8") as f:
        f.write(src)
    subprocess.run([GO, "build", "-o", p_exe, p_src], check=True)
    return p_exe


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runs", type=int, default=3, help="repetitions per runtime benchmark (default: 3)")
    ap.add_argument("--skip-python-slow", action="store_true", help="skip slow 50M/100M-step Python loops")
    ap.add_argument("--no-go", action="store_true", help="omit Go comparison even when `go` is installed")
    args = ap.parse_args()

    if not os.path.isfile(CXX):
        sys.stderr.write(f"error: {CXX} not built; run `make -j4` first\n")
        return 1

    use_go = bool(GO) and not args.no_go

    print("## Environment")
    print(f"- CPU: {cpu_model()} ({os.cpu_count() or 1} logical cores)")
    print(f"- OS: {platform.system()} {platform.release()} ({platform.machine()})")
    print(f"- C compiler: {one_line_ver([CC, '--version'])}")
    if use_go:
        print(f"- Go compiler: {one_line_ver([GO, 'version'])}")
    print(f"- BRAT compiler: {CXX}")
    print(f"- Python: {sys.version.split()[0]}")
    print()

    with tempfile.TemporaryDirectory(prefix="brat-bench-") as td:
        fib_c = os.path.join(td, "fib.c")
        fib_c_exe = os.path.join(td, "fib_c")
        fib_b = os.path.join(td, "fib.brat")
        fib_b_exe = os.path.join(td, "fib_brat")
        fib_py = os.path.join(td, "fib.py")
        open(fib_c, "w", encoding="utf-8").write(FIB_C)
        open(fib_b, "w", encoding="utf-8").write(FIB_BRAT)
        open(fib_py, "w", encoding="utf-8").write(FIB_PY)
        subprocess.run([CC, "-std=c99", "-O2", fib_c, "-o", fib_c_exe], check=True)
        subprocess.run([CXX, "build", "-O2", fib_b, "-o", fib_b_exe], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        fib_go_exe = build_go(td, "fib", FIB_GO) if use_go else None

        loop_c = os.path.join(td, "loop.c")
        loop_c_exe = os.path.join(td, "loop_c")
        loop_b = os.path.join(td, "loop.brat")
        loop_b_exe = os.path.join(td, "loop_brat")
        loop_py = os.path.join(td, "loop.py")
        open(loop_c, "w", encoding="utf-8").write(LOOP_C)
        open(loop_b, "w", encoding="utf-8").write(LOOP_BRAT)
        open(loop_py, "w", encoding="utf-8").write(LOOP_PY)
        subprocess.run([CC, "-std=c99", "-O2", loop_c, "-o", loop_c_exe], check=True)
        subprocess.run([CXX, "build", "-O2", loop_b, "-o", loop_b_exe], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        loop_expect = subprocess.run([loop_c_exe], capture_output=True, text=True, check=True).stdout.strip()
        loop_go_exe = build_go(td, "loop", LOOP_GO) if use_go else None

        vec3_c = os.path.join(td, "vec3.c")
        vec3_c_exe = os.path.join(td, "vec3_c")
        vec3_b = os.path.join(td, "vec3.brat")
        vec3_b_exe = os.path.join(td, "vec3_brat")
        vec3_py = os.path.join(td, "vec3.py")
        open(vec3_c, "w", encoding="utf-8").write(VEC3_C)
        open(vec3_b, "w", encoding="utf-8").write(VEC3_BRAT)
        open(vec3_py, "w", encoding="utf-8").write(VEC3_PY)
        subprocess.run([CC, "-std=c99", "-O2", vec3_c, "-o", vec3_c_exe], check=True)
        subprocess.run([CXX, "build", "-O2", vec3_b, "-o", vec3_b_exe], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        vec3_expect = subprocess.run([vec3_c_exe], capture_output=True, text=True, check=True).stdout.strip()
        vec3_go_exe = build_go(td, "vec3", VEC3_GO) if use_go else None

        str_c = os.path.join(td, "str_app.c")
        str_c_exe = os.path.join(td, "str_c")
        str_b = os.path.join(td, "str_app.brat")
        str_b_exe = os.path.join(td, "str_brat")
        str_py = os.path.join(td, "str_app.py")
        open(str_c, "w", encoding="utf-8").write(STR_C)
        open(str_b, "w", encoding="utf-8").write(STR_BRAT)
        open(str_py, "w", encoding="utf-8").write(STR_PY)
        subprocess.run([CC, "-std=c99", "-O2", str_c, "-o", str_c_exe], check=True)
        subprocess.run([CXX, "build", "-O2", str_b, "-o", str_b_exe], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        str_expect = subprocess.run([str_c_exe], capture_output=True, text=True, check=True).stdout.strip()
        str_go_exe = build_go(td, "str_app", STR_GO) if use_go else None

        dyn_b = os.path.join(td, "dyn.brat")
        dyn_b_exe = os.path.join(td, "dyn_brat")
        dyn_py = os.path.join(td, "dyn.py")
        open(dyn_b, "w", encoding="utf-8").write(DYN_BRAT)
        open(dyn_py, "w", encoding="utf-8").write(DYN_PY)
        subprocess.run([CXX, "build", "-O2", dyn_b, "-o", dyn_b_exe], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        dyn_expect = subprocess.run([sys.executable, dyn_py], capture_output=True, text=True, check=True).stdout.strip()
        dyn_go_exe = build_go(td, "dyn", DYN_GO) if use_go else None

        json_b = os.path.join(td, "json_rt.brat")
        json_b_exe = os.path.join(td, "json_brat")
        json_py = os.path.join(td, "json_rt.py")
        open(json_b, "w", encoding="utf-8").write(JSON_BRAT)
        open(json_py, "w", encoding="utf-8").write(JSON_PY)
        subprocess.run([CXX, "build", "-O2", json_b, "-o", json_b_exe], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        json_expect = subprocess.run([sys.executable, json_py], capture_output=True, text=True, check=True).stdout.strip()
        json_go_exe = build_go(td, "json_rt", JSON_GO) if use_go else None

        print(f"## Runtime (median / best of {args.runs} runs)")
        if use_go:
            print("| Workload | C (`-O2`) | Go (`go build`) | BRAT (`build -O2`) | CPython |")
            print("|---|---:|---:|---:|---:|")
        else:
            print("| Workload | C (`-O2`) | BRAT (`build -O2`) | CPython |")
            print("|---|---:|---:|---:|")

        def emit_row(label: str, c_cell: str, go_cell: str, brat_cell: str, py_cell: str):
            if use_go:
                print(f"| {label} | {c_cell} | {go_cell} | {brat_cell} | {py_cell} |")
            else:
                print(f"| {label} | {c_cell} | {brat_cell} | {py_cell} |")

        emit_row(
            "`fib(35)` (29.9M calls)",
            fmt_cell(bench_cmd([fib_c_exe], args.runs, "9227465")),
            fmt_cell(bench_cmd([fib_go_exe], args.runs, "9227465")) if use_go else "",
            fmt_cell(bench_cmd([fib_b_exe], args.runs, "9227465")),
            fmt_cell(bench_cmd([sys.executable, fib_py], args.runs, "9227465")),
        )

        if args.skip_python_slow:
            lp_str = "skipped"
        else:
            lp_m, _ = bench_cmd([sys.executable, loop_py], 1, loop_expect)
            lp_str = f"{lp_m:.3f}s"
        emit_row(
            "`int_loop(100M)` (wrapping `i64`)",
            fmt_cell(bench_cmd([loop_c_exe], args.runs, loop_expect)),
            fmt_cell(bench_cmd([loop_go_exe], args.runs, loop_expect)) if use_go else "",
            fmt_cell(bench_cmd([loop_b_exe], args.runs, loop_expect)),
            lp_str,
        )

        if args.skip_python_slow:
            vp_str = "skipped"
        else:
            vp_m, _ = bench_cmd([sys.executable, vec3_py], 1, vec3_expect)
            vp_str = f"{vp_m:.3f}s"
        emit_row(
            "`vec3_sra(50M)` (nested `struct`)",
            fmt_cell(bench_cmd([vec3_c_exe], args.runs, vec3_expect)),
            fmt_cell(bench_cmd([vec3_go_exe], args.runs, vec3_expect)) if use_go else "",
            fmt_cell(bench_cmd([vec3_b_exe], args.runs, vec3_expect)),
            vp_str,
        )

        emit_row(
            "`str_append(200k)` (chained `+`)",
            fmt_cell(bench_cmd([str_c_exe], args.runs, str_expect)),
            fmt_cell(bench_cmd([str_go_exe], args.runs, str_expect)) if use_go else "",
            fmt_cell(bench_cmd([str_b_exe], args.runs, str_expect)),
            fmt_cell(bench_cmd([sys.executable, str_py], args.runs, str_expect)),
        )

        emit_row(
            "`dyn_records(100k)` (`map` + `str`)",
            "n/a",
            fmt_cell(bench_cmd([dyn_go_exe], args.runs, dyn_expect)) if use_go else "",
            fmt_cell(bench_cmd([dyn_b_exe], args.runs, dyn_expect)),
            fmt_cell(bench_cmd([sys.executable, dyn_py], args.runs, dyn_expect)),
        )

        emit_row(
            "`json_roundtrip(5k)` (`std.json` vs stdlib)",
            "n/a",
            fmt_cell(bench_cmd([json_go_exe], args.runs, json_expect)) if use_go else "",
            fmt_cell(bench_cmd([json_b_exe], args.runs, json_expect)),
            fmt_cell(bench_cmd([sys.executable, json_py], args.runs, json_expect)),
        )
        print()

        print("## Compile-time scaling (`brat-cxx build -O2`, single function with N heap string locals)")
        print("| String locals | Emitted C | `brat-cxx build -O2` |")
        print("|---:|---:|---:|")
        env_no_cache = dict(os.environ, BRAT_NATIVE_CACHE_DISABLE="1")
        for n_locals in (50, 100, 200, 400):
            src_lines = ["fn work(flag: bool) -> i64 {"]
            for i in range(n_locals):
                src_lines.append(f'  let s{i} = "payload_string_item_{i}_" + str({i})')
            src_lines.append(f"  if flag {{ return len(s0) + len(s{n_locals - 1}) }}")
            src_lines.append(f"  return len(s{n_locals // 2})")
            src_lines.append("}")
            src_lines.append("fn main() -> i64 { print(work(true)); return 0 }")
            p_brat = os.path.join(td, f"many_{n_locals}.brat")
            p_exe = os.path.join(td, f"many_{n_locals}")
            open(p_brat, "w", encoding="utf-8").write("\n".join(src_lines) + "\n")
            em = subprocess.run([CXX, "emit-c", "-O2", p_brat], capture_output=True, text=True, check=True)
            kb = len(em.stdout.encode("utf-8")) / 1024.0
            t0 = time.perf_counter()
            subprocess.run([CXX, "build", "-O2", p_brat, "-o", p_exe],
                           capture_output=True, text=True, check=True, env=env_no_cache)
            dt_build = time.perf_counter() - t0
            print(f"| {n_locals} | {kb:.1f} KB | {dt_build:.2f}s |")

    return 0


if __name__ == "__main__":
    sys.exit(main())
