"""Verification of `examples/`, multi-CU DWARF v5 `strx`, `-O2` leaf-guard recursion, and Valgrind.

Covers:
1. Every program in `examples/` passes `check`, `run`, and `build -O2`.
2. Multi-CU shared library compiled with `clang -gdwarf-5` (where `DW_AT_name` precedes
   `DW_AT_str_offsets_base` in `DW_TAG_compile_unit`) passes `brat ffi --object` across CUs.
3. `-O2` recursive leaf-guard optimization preserves exact `BRAT_RECURSION_BUDGET` bounds
   even when the budget is hit on a pure leaf call (`n < 2`).
4. When `valgrind` is installed on the host, runs `valgrind --leak-check=full --error-exitcode=99`
   on both `brat-cxx` (`check`, `emit-c`, `ffi`) and compiled BRAT binaries exercising heap
   strings, maps, vectors, `defer`, `arena`, `std.sqlite`, and C FFI.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import _cxx

BIN = os.environ.get("BRAT_CXX") or _cxx.build()
IS_ASAN_BIN = "asan" in os.path.basename(BIN).lower()


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def test_examples_directory() -> None:
    ex_dir = os.path.join(ROOT, "examples")
    cases = [
        ("hello.brat", [], "hello from brat\n"),
        ("json_arena.brat", [], "auth:45ms\n"),
        ("sqlite_kv.brat", [], "42\n"),
        ("parallel_sum.brat", [], "338350\n"),
        ("c_ffi_math.brat", ["--header", os.path.join(ex_dir, "c_ffi_math.h")], "5.0 40.0\n"),
        ("code_stats.brat", [], "code_stats: self-test ok (3 languages, 5 code lines)\n"),
    ]
    with tempfile.TemporaryDirectory(prefix="brat-examples-") as td:
        for fname, extra_flags, expected_out in cases:
            fpath = os.path.join(ex_dir, fname)
            check(os.path.isfile(fpath), f"missing example {fpath}")

            rc = subprocess.run([BIN, "check", fpath], capture_output=True, text=True, timeout=60)
            check(rc.returncode == 0, f"check failed on {fname}: {rc.stdout} {rc.stderr}")

            rr = subprocess.run([BIN, "run", fpath] + extra_flags, capture_output=True, text=True, timeout=90)
            check(rr.returncode == 0 and rr.stdout == expected_out,
                  f"run failed on {fname}: got {rr.stdout!r}, err {rr.stderr!r}")

            out_bin = os.path.join(td, fname.replace(".brat", ".bin"))
            rb = subprocess.run(
                [BIN, "build", "-O2", fpath, "-o", out_bin] + extra_flags,
                capture_output=True,
                text=True,
                timeout=90,
            )
            check(rb.returncode == 0, f"build -O2 failed on {fname}: {rb.stdout} {rb.stderr}")
            rx = subprocess.run([out_bin], capture_output=True, text=True, timeout=30)
            check(rx.returncode == 0 and rx.stdout == expected_out,
                  f"compiled {fname} output mismatch: {rx.stdout!r}")


def test_multi_cu_dwarf5_strx() -> None:
    clang = shutil.which("clang")
    if not clang:
        return
    with tempfile.TemporaryDirectory(prefix="brat-dw5-mcu-") as td:
        c1 = os.path.join(td, "cu_one.c")
        c2 = os.path.join(td, "cu_two.c")
        with open(c1, "w", encoding="utf-8") as f:
            f.write("""#include <stdint.h>
typedef struct AlphaPair { int64_t a; int64_t b; } AlphaPair;
int64_t alpha_sum(AlphaPair p) { return p.a + p.b; }
""")
        with open(c2, "w", encoding="utf-8") as f:
            f.write("""#include <stdint.h>
typedef struct BetaBox { double x; int32_t tag; int32_t pad; } BetaBox;
double beta_scale(BetaBox b, double k) { return b.x * k + (double)b.tag; }
""")
        so_path = os.path.join(td, "libmcu.so")
        subprocess.run(
            [clang, "-shared", "-fPIC", "-gdwarf-5", "-O1", c1, c2, "-o", so_path],
            check=True,
            timeout=30,
        )
        good_brat = os.path.join(td, "good.brat")
        with open(good_brat, "w", encoding="utf-8") as f:
            f.write(f"""extern struct AlphaPair {{ a: i64, b: i64 }}
extern struct BetaBox {{ x: f64, tag: i32, pad: i32 }}
extern "{so_path}" fn alpha_sum(p: AlphaPair) -> i64
extern "{so_path}" fn beta_scale(b: BetaBox, k: f64) -> f64
""")
        ok = subprocess.run([BIN, "ffi", good_brat, "--object", so_path], capture_output=True, text=True, timeout=30)
        check(ok.returncode == 0, f"multi-CU clang -gdwarf-5 verification failed:\n{ok.stdout}\n{ok.stderr}")

        bad_brat = os.path.join(td, "bad.brat")
        with open(bad_brat, "w", encoding="utf-8") as f:
            f.write(f"""extern struct BetaBox {{ x: i64, tag: i32, pad: i32 }}
extern "{so_path}" fn beta_scale(b: BetaBox, k: f64) -> f64
""")
        bad = subprocess.run([BIN, "ffi", bad_brat, "--object", so_path], capture_output=True, text=True, timeout=30)
        check(bad.returncode != 0, "multi-CU DWARF v5 should catch float-vs-int drift in second CU")


def test_o2_leaf_guard_exact_budget() -> None:
    with tempfile.TemporaryDirectory(prefix="brat-leaf-budget-") as td:
        src = os.path.join(td, "fib_budget.brat")
        with open(src, "w", encoding="utf-8") as f:
            f.write("""fn fib(n: i64) -> i64 {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}

fn main() {
  print(fib(10))
}
""")
        exe_o0 = os.path.join(td, "fib_o0")
        exe_o2 = os.path.join(td, "fib_o2")
        subprocess.run([BIN, "build", "-O0", src, "-o", exe_o0], check=True, timeout=60)
        subprocess.run([BIN, "build", "-O2", src, "-o", exe_o2], check=True, timeout=60)

        # `fib(10)` reaches exact leaf depth across budgets 8..15: `-O0` and `-O2` must agree at every budget.
        for budget in range(8, 16):
            env = dict(os.environ, BRAT_RECURSION_BUDGET=str(budget))
            r0 = subprocess.run([exe_o0], capture_output=True, text=True, env=env, timeout=15)
            r2 = subprocess.run([exe_o2], capture_output=True, text=True, env=env, timeout=15)
            check((r0.returncode == 0) == (r2.returncode == 0),
                  f"O0 vs O2 recursion budget mismatch at budget={budget}: "
                  f"O0={r0.returncode} ({r0.stderr.strip()}), O2={r2.returncode} ({r2.stderr.strip()})")
            if r0.returncode == 0:
                check(r2.stdout == "55\n", f"unexpected fib(10) output: {r2.stdout!r}")


def test_valgrind_audit() -> None:
    vg = shutil.which("valgrind")
    if not vg or IS_ASAN_BIN:
        return
    cc = shutil.which("gcc") or shutil.which("cc")
    check(bool(cc), "missing C compiler for dynamic Valgrind binary")

    with tempfile.TemporaryDirectory(prefix="brat-valgrind-") as td:
        src = os.path.join(td, "vg_target.brat")
        with open(src, "w", encoding="utf-8") as f:
            f.write("""import std.sqlite

fn build_summary(n: i64) -> Result[str, str] {
  let db = std.sqlite.open_memory()?
  defer std.sqlite.close(db)
  std.sqlite.exec(db, "CREATE TABLE t (id INT, label TEXT);")?

  let mut m = {}
  let mut i = 0
  while i < n {
    let label = "row_" + str(i) + "_heap_allocated_value"
    m.set(str(i), label)
    let _ins = std.sqlite.query(db, "INSERT INTO t VALUES (?1, ?2);", [i, label])?
    i = i + 1
  }
  arena {
    let mut tmp = "scratch"
    let mut k = 0
    while k < 20 {
      tmp = tmp + "_" + str(k)
      k = k + 1
    }
    assert(len(tmp) > 10)
  }
  let cnt = std.sqlite.scalar_int(db, "SELECT COUNT(*) FROM t;", [])?
  return Ok(m["0"] + ":" + str(cnt))
}

fn main() {
  let s = unwrap(build_summary(25))
  assert(s == "row_0_heap_allocated_value:25")
  print(s)
}
""")
        # 1. Valgrind on `brat-cxx check` and `brat-cxx emit-c -O2`
        vg_base = [
            vg,
            "--quiet",
            "--error-exitcode=99",
            "--leak-check=full",
            "--errors-for-leak-kinds=definite,indirect",
        ]
        r_chk = subprocess.run(vg_base + [BIN, "check", src], capture_output=True, text=True, timeout=120)
        check(r_chk.returncode == 0, f"Valgrind error in `brat-cxx check`:\n{r_chk.stderr}")

        c_out_res = subprocess.run(vg_base + [BIN, "emit-c", "-O2", src], capture_output=True, text=True, timeout=120)
        check(c_out_res.returncode == 0, f"Valgrind error in `brat-cxx emit-c`:\n{c_out_res.stderr}")

        # 2. Compile emitted C dynamically (Valgrind requires non-static glibc binaries on Linux)
        #    and run the resulting native BRAT binary under Valgrind Memcheck + leak-check=full.
        c_file = os.path.join(td, "vg_target.c")
        with open(c_file, "w", encoding="utf-8") as f:
            f.write(c_out_res.stdout)
        dyn_exe = os.path.join(td, "vg_target_dyn")
        engine_dir = os.path.join(ROOT, "lang", "engine")
        subprocess.run(
            [cc, "-std=gnu11", "-O1", "-g", "-fwrapv", f"-I{engine_dir}", c_file, "-lm", "-ldl", "-lpthread", "-o", dyn_exe],
            check=True,
            timeout=60,
        )
        r_run = subprocess.run(vg_base + [dyn_exe], capture_output=True, text=True, timeout=120)
        check(r_run.returncode == 0 and r_run.stdout == "row_0_heap_allocated_value:25\n",
              f"Valgrind error in compiled BRAT binary (rc={r_run.returncode}):\nstdout={r_run.stdout!r}\nstderr={r_run.stderr}")

        # 3. Multithreaded `par for` worker pool clean join on exit (zero `possibly lost` TLS DTV).
        par_src = os.path.join(ROOT, "examples", "parallel_sum.brat")
        par_c = os.path.join(td, "vg_par.c")
        par_exe = os.path.join(td, "vg_par_dyn")
        ep = subprocess.run([BIN, "emit-c", "-O2", par_src], capture_output=True, text=True, timeout=60)
        check(ep.returncode == 0, f"emit-c parallel_sum failed:\n{ep.stderr}")
        with open(par_c, "w", encoding="utf-8") as f:
            f.write(ep.stdout)
        subprocess.run(
            [cc, "-std=gnu11", "-O1", "-g", "-fwrapv", f"-I{engine_dir}", par_c, "-lm", "-ldl", "-lpthread", "-o", par_exe],
            check=True,
            timeout=60,
        )
        env_mt = dict(os.environ, BRAT_THREADS="4")
        vg_strict = [
            vg,
            "--quiet",
            "--error-exitcode=99",
            "--leak-check=full",
            "--errors-for-leak-kinds=definite,indirect,possible",
        ]
        r_par = subprocess.run(vg_strict + [par_exe], env=env_mt, capture_output=True, text=True, timeout=120)
        check(r_par.returncode == 0 and r_par.stdout == "338350\n" and r_par.stderr == "",
              f"Valgrind error/leak in multithreaded worker pool (rc={r_par.returncode}):\nstdout={r_par.stdout!r}\nstderr={r_par.stderr}")


def test_soundness_and_dogfood_regressions() -> None:
    ex_cs = os.path.join(ROOT, "examples", "code_stats.brat")
    r_json = subprocess.run(
        [BIN, "run", ex_cs, "--", "--json", "--ext", ".brat,.h", "."],
        capture_output=True, text=True, timeout=60,
    )
    check(r_json.returncode == 0 and '"languages":' in r_json.stdout and '"manifest_sha256":' in r_json.stdout,
          f"code_stats --json failed: {r_json.stdout!r} {r_json.stderr!r}")

    with tempfile.TemporaryDirectory(prefix="brat-soundness-reg-") as td:
        prog = os.path.join(td, "reg.brat")
        with open(prog, "w", encoding="utf-8") as f:
            f.write("""enum Packet {
  Empty,
  Data(str, vec[i64]),
}

struct Pair {
  a: i64,
  b: i64,
}

fn sra_else_fn(cond: bool) -> i64 {
  let p = Pair { a: 17, b: 25 }
  if cond {
    return p.a
  } else {
    fn read_p(q: Pair) -> i64 { return q.a + q.b }
    return read_p(p)
  }
}

fn inspect_with_outer(s: str, cfg: Pair, flag: bool) -> i64 {
  if flag {
    return len(s) + cfg.a
  }
  return len(s) + cfg.b
}

fn arena_enum_promote(tag_id: i64) -> Packet {
  let cfg = Pair { a: 3, b: 4 }
  arena {
    let s = "arena_heap_payload_" + str(tag_id)
    let n = inspect_with_outer(s, cfg, true)
    let v = [10, 20, n]
    return own(Data(s, v))
  }
}

fn loop_for_iter_typed(xs: vec[i64]) -> i64 {
  let mut round = 0
  let mut acc = 0
  while round < 3 {
    for x in xs {
      acc = acc + x
    }
    for i in 0..len(xs) {
      acc = acc + i
    }
    round = round + 1
  }
  return acc
}

fn self_assign_i64(x: i64) -> i64 {
  let mut a = x
  a = a
  if a == 0 {
    while a == 0 {
      a = a + 1
    }
  }
  return a
}

fn self_assign_f64(y: f64) -> f64 {
  let mut fy = y
  fy = fy
  if fy == 5.0 {
    fy = fy + 1.0
  }
  return fy
}

fn main() {
  assert(sra_else_fn(false) == 42)
  assert(sra_else_fn(true) == 17)
  match arena_enum_promote(99) {
    Data(s, v) => {
      assert(s == "arena_heap_payload_99")
      assert(len(v) == 3 && v[2] == 24)
    },
    Empty => assert(false),
  }
  let hv = own([100, 200, 300])
  let hs = own("heap_prefix_over_14_" + str(77))
  assert(len(hv) == 3 && len(hs) == 22)
  assert(loop_for_iter_typed([10, 20, 30]) == 189)
  assert(self_assign_i64(0) == 1)
  assert(self_assign_f64(5.0) == 6.0)
  print("reg_ok")
}
""")
        clang = shutil.which("clang-18") or shutil.which("clang")
        engine_dir = os.path.join(ROOT, "lang", "engine")
        for opt in ["-O0", "-O1", "-O2"]:
            ec = subprocess.run([BIN, "emit-c", opt, prog], capture_output=True, text=True, timeout=60)
            check(ec.returncode == 0, f"emit-c {opt} failed: {ec.stderr}")
            c_path = os.path.join(td, f"reg_{opt[1:]}.c")
            exe_path = os.path.join(td, f"reg_{opt[1:]}.bin")
            with open(c_path, "w", encoding="utf-8") as cf:
                cf.write(ec.stdout)
            if clang:
                cc_cmd = [
                    clang, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter",
                    "-Wno-unused-but-set-variable",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    f"-I{engine_dir}", c_path, "-lm", "-lpthread", "-o", exe_path,
                ]
                rc = subprocess.run(cc_cmd, capture_output=True, text=True, timeout=60)
                check(rc.returncode == 0, f"clang -Werror {opt} failed: {rc.stderr}")
                rr = subprocess.run(
                    [exe_path],
                    env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"),
                    capture_output=True, text=True, timeout=30,
                )
                check(rr.returncode == 0 and rr.stdout == "reg_ok\n",
                      f"ASAN/LSAN {opt} failed: out={rr.stdout!r} err={rr.stderr!r}")

        # Ensure mutable outer container passed alongside arena value is still rejected.
        neg = os.path.join(td, "neg_mut.brat")
        with open(neg, "w", encoding="utf-8") as nf:
            nf.write("""fn stash(mut dst: vec[str], s: str) { dst.push(s) }
fn main() {
  let mut out: vec[str] = []
  arena {
    let s = "arena_str_" + str(123)
    stash(out, s)
  }
}
""")
        rn = subprocess.run([BIN, "check", neg], capture_output=True, text=True, timeout=30)
        check(rn.returncode != 0 and "next to an outer binding" in (rn.stdout + rn.stderr),
              f"expected arena mut-outer rejection, got: {rn.stdout} {rn.stderr}")


if __name__ == "__main__":
    test_examples_directory()
    test_soundness_and_dogfood_regressions()
    test_multi_cu_dwarf5_strx()
    test_o2_leaf_guard_exact_budget()
    test_valgrind_audit()
    print("OK: examples, multi-CU DWARF v5, leaf-guard recursion, and Valgrind checks passed")
