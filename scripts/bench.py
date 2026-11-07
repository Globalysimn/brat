#!/usr/bin/env python3
"""Reproducible BRAT benchmark suite (`make bench`).

Measures wall-clock execution time (median and best of N runs) across:
  1. fib(35)              -- recursive i64 calls (29.86M calls)
  2. int_loop(100M)       -- tight 64-bit wrapping integer loop
  3. dyn_records(100k)    -- map allocation + string concat + RC drops
  4. json_roundtrip(5k)   -- std.json.parse + std.json.stringify on nested objects

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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runs", type=int, default=3, help="repetitions per runtime benchmark (default: 3)")
    ap.add_argument("--skip-python-slow", action="store_true", help="skip slow 100M-step Python loop")
    args = ap.parse_args()

    if not os.path.isfile(CXX):
        sys.stderr.write(f"error: {CXX} not built; run `make -j4` first\n")
        return 1

    print("## Environment")
    print(f"- CPU: {cpu_model()} ({os.cpu_count() or 1} logical cores)")
    print(f"- OS: {platform.system()} {platform.release()} ({platform.machine()})")
    print(f"- C compiler: {one_line_ver([CC, '--version'])}")
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

        dyn_b = os.path.join(td, "dyn.brat")
        dyn_b_exe = os.path.join(td, "dyn_brat")
        dyn_py = os.path.join(td, "dyn.py")
        open(dyn_b, "w", encoding="utf-8").write(DYN_BRAT)
        open(dyn_py, "w", encoding="utf-8").write(DYN_PY)
        subprocess.run([CXX, "build", "-O2", dyn_b, "-o", dyn_b_exe], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        dyn_expect = subprocess.run([sys.executable, dyn_py], capture_output=True, text=True, check=True).stdout.strip()

        json_b = os.path.join(td, "json_rt.brat")
        json_b_exe = os.path.join(td, "json_brat")
        json_py = os.path.join(td, "json_rt.py")
        open(json_b, "w", encoding="utf-8").write(JSON_BRAT)
        open(json_py, "w", encoding="utf-8").write(JSON_PY)
        subprocess.run([CXX, "build", "-O2", json_b, "-o", json_b_exe], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        json_expect = subprocess.run([sys.executable, json_py], capture_output=True, text=True, check=True).stdout.strip()

        print(f"## Runtime (median / best of {args.runs} runs)")
        print("| Workload | C (`-O2`) | BRAT (`build -O2`) | CPython |")
        print("|---|---:|---:|---:|")

        fc_m, fc_b = bench_cmd([fib_c_exe], args.runs, "9227465")
        fb_m, fb_b = bench_cmd([fib_b_exe], args.runs, "9227465")
        fp_m, fp_b = bench_cmd([sys.executable, fib_py], args.runs, "9227465")
        print(f"| `fib(35)` (29.9M calls) | {fc_m:.3f}s ({fc_b:.3f}s) | {fb_m:.3f}s ({fb_b:.3f}s) | {fp_m:.3f}s ({fp_b:.3f}s) |")

        lc_m, lc_b = bench_cmd([loop_c_exe], args.runs, loop_expect)
        lb_m, lb_b = bench_cmd([loop_b_exe], args.runs, loop_expect)
        if args.skip_python_slow:
            lp_str = "skipped"
        else:
            lp_m, _ = bench_cmd([sys.executable, loop_py], 1, loop_expect)
            lp_str = f"{lp_m:.3f}s"
        print(f"| `int_loop(100M)` (wrapping `i64`) | {lc_m:.3f}s ({lc_b:.3f}s) | {lb_m:.3f}s ({lb_b:.3f}s) | {lp_str} |")

        db_m, db_b = bench_cmd([dyn_b_exe], args.runs, dyn_expect)
        dp_m, dp_b = bench_cmd([sys.executable, dyn_py], args.runs, dyn_expect)
        print(f"| `dyn_records(100k)` (`map` + `str`) | n/a | {db_m:.3f}s ({db_b:.3f}s) | {dp_m:.3f}s ({dp_b:.3f}s) |")

        jb_m, jb_b = bench_cmd([json_b_exe], args.runs, json_expect)
        jp_m, jp_b = bench_cmd([sys.executable, json_py], args.runs, json_expect)
        print(f"| `json_roundtrip(5k)` (`std.json` vs `json`) | n/a | {jb_m:.3f}s ({jb_b:.3f}s) | {jp_m:.3f}s ({jp_b:.3f}s) |")
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
