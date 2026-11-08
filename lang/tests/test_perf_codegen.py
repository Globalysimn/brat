"""Regression suite for recursive numeric fast-path codegen and large-frame SSA lowering."""

import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "lang", "tests"))
import _cxx  # noqa: E402

CXX = os.environ.get("BRAT_CXX") or _cxx.build()
CXX_ENV = dict(os.environ)
ENGINE = os.path.join(ROOT, "lang", "engine")


def build_and_run(src: str, opt: str = "-O2", extra_env=None):
    with tempfile.TemporaryDirectory() as td:
        bp = os.path.join(td, "prog.brat")
        exe = os.path.join(td, "prog")
        with open(bp, "w", encoding="utf-8") as f:
            f.write(src)
        b = subprocess.run([CXX, "build", opt, bp, "-o", exe],
                           capture_output=True, text=True, env=CXX_ENV)
        assert b.returncode == 0, f"build failed ({opt}):\n{b.stdout}\n{b.stderr}"
        env = dict(CXX_ENV)
        if extra_env:
            env.update(extra_env)
        r = subprocess.run([exe], capture_output=True, text=True, env=env)
        return r.returncode, r.stdout, r.stderr


def emit_c(src: str, opt: str = "-O2") -> str:
    with tempfile.TemporaryDirectory() as td:
        bp = os.path.join(td, "prog.brat")
        with open(bp, "w", encoding="utf-8") as f:
            f.write(src)
        r = subprocess.run([CXX, "emit-c", opt, bp],
                           capture_output=True, text=True, env=CXX_ENV)
        assert r.returncode == 0, f"emit-c failed:\n{r.stderr}"
        return r.stdout


def test_recursive_fast_path_i64_and_f64():
    src = """
fn fib(n: i64) -> i64 {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}

fn is_even(n: i64) -> i64 {
    if n == 0 { return 1 }
    return is_odd(n - 1)
}

fn is_odd(n: i64) -> i64 {
    if n == 0 { return 0 }
    return is_even(n - 1)
}

fn boom_rec(n: i64) -> i64 {
    if n == 0 { return 10 / 0 }
    return 1 + boom_rec(n - 1)
}

fn short_rec(n: i64) -> i64 {
    if (n <= 0) || (boom_rec(0) > 0) {
        return 7
    }
    return 0
}

fn pow_f(x: f64, n: f64) -> f64 {
    if n <= 0.0 { return 1.0 }
    return x * pow_f(x, n - 1.0)
}

fn main() -> i64 {
    print(fib(20))
    print(is_even(24))
    print(is_odd(24))
    print(short_rec(0))
    let mut caught = 0
    try {
        print(boom_rec(5))
    } catch e {
        caught = 1
    }
    print(caught)
    // Verify _bdepth was cleanly unwound after mid-recursion error:
    print(fib(15))
    print(pow_f(2.0, 10.0))
    return 0
}
"""
    c_code = emit_c(src, "-O2")
    assert "static int64_t bf_f0(" in c_code, "fib must emit unboxed int64_t fast function"
    assert "static double bf_f" in c_code, "pow_f must emit unboxed double fast function"

    for opt in ("-O0", "-O1", "-O2"):
        rc, out, err = build_and_run(src, opt=opt)
        assert rc == 0, f"unexpected rc={rc} under {opt}: {err}"
        got = [ln.strip() for ln in out.strip().splitlines()]
        assert got == ["6765", "1", "0", "7", "1", "610", "1024.0"], f"{opt} got {got}"


def test_recursive_fast_budget_recovery():
    src = """
fn deep(n: i64) -> i64 {
    if n <= 0 { return 0 }
    return 1 + deep(n - 1)
}

fn main() -> i64 {
    let mut trips = 0
    let mut i = 0
    while i < 3 {
        try {
            print(deep(500))
        } catch e {
            trips = trips + 1
        }
        i = i + 1
    }
    print(trips)
    print(deep(40))
    return 0
}
"""
    rc, out, err = build_and_run(src, opt="-O2", extra_env={"BRAT_RECURSION_BUDGET": "100"})
    assert rc == 0, err
    assert [ln.strip() for ln in out.strip().splitlines()] == ["3", "40"]


def test_large_frame_compile_speed_and_leak_freedom():
    lines = ["fn heavy(flag: bool) -> i64 {"]
    for i in range(200):
        lines.append(f'    let s{i} = "heap_allocated_string_payload_{i}_" + str({i})')
    lines += [
        "    let mut acc = len(s0) + len(s199)",
        "    let mut k = 0",
        "    while k < 3 {",
        '        let loop_s = "iter_" + str(k)',
        "        acc = acc + len(loop_s)",
        "        k = k + 1",
        "    }",
        "    try {",
        '        let inner_s = "inside_try_" + s10',
        "        if !flag { raise(\"bail\") }",
        "        acc = acc + len(inner_s)",
        "    } catch e {",
        "        acc = acc + len(e)",
        "    }",
        "    let clos = fn(x) { return x + len(s50) }",
        "    if flag { return clos(acc) }",
        "    return clos(acc + len(s100))",
        "}",
        "fn main() -> i64 {",
        "    print(heavy(true))",
        "    print(heavy(false))",
        "    return 0",
        "}",
    ]
    src = "\n".join(lines) + "\n"
    c_code = emit_c(src, "-O2")
    assert "Val __fv[" in c_code, "expected compact frame array lowering for 200+ Val locals"

    with tempfile.TemporaryDirectory() as td:
        cp = os.path.join(td, "heavy.c")
        exe = os.path.join(td, "heavy.exe" if sys.platform == "win32" else "heavy")
        with open(cp, "w", encoding="utf-8") as f:
            f.write(c_code)
        cc = os.environ.get("CC", "gcc")
        if sys.platform == "win32":
            link_libs = ["-lws2_32"]
        elif sys.platform == "darwin":
            link_libs = ["-lm", "-lpthread"]
        else:
            link_libs = ["-lm", "-lpthread", "-ldl"]
        t0 = time.perf_counter()
        comp = subprocess.run(
            [cc, "-std=c99", "-O2", "-I", ENGINE, cp, *link_libs, "-o", exe],
            capture_output=True, text=True,
        )
        dt = time.perf_counter() - t0
        assert comp.returncode == 0, comp.stderr
        assert dt < 15.0, f"200-local -O2 C compile regressed to {dt:.2f}s (SSA phi blowup)"
        run = subprocess.run([exe], capture_output=True, text=True)
        assert run.returncode == 0, run.stderr
        expected_out = run.stdout

        if sys.platform != "win32":
            asan_exe = os.path.join(td, "heavy_asan")
            asan_comp = subprocess.run(
                [cc, "-std=c99", "-O1", "-g", "-fsanitize=address,undefined",
                 "-I", ENGINE, cp, *link_libs, "-o", asan_exe],
                capture_output=True, text=True,
            )
            if asan_comp.returncode == 0:
                asan_opts = "halt_on_error=1" if sys.platform == "darwin" else "detect_leaks=1:halt_on_error=1"
                env = dict(os.environ, ASAN_OPTIONS=asan_opts)
                asan_run = subprocess.run([asan_exe], capture_output=True, text=True, env=env)
                assert asan_run.returncode == 0, f"ASan/LSan failure:\n{asan_run.stderr}"
                assert asan_run.stdout == expected_out


def test_f64_fast_int_subexprs_and_ub_freedom():
    src = """
fn mul_lit(x: f64) -> f64 {
    return 10000000000 * 10000000000 + x
}
fn mul_lit_boxed(x: f64, s: str) -> f64 {
    return 10000000000 * 10000000000 + x
}
fn mul_var(x: f64) -> f64 {
    let a = 10000000000
    return a * a + x
}
fn mul_var_boxed(x: f64, s: str) -> f64 {
    let a = 10000000000
    return a * a + x
}
fn idx_wrap(v: vec[f64]) -> f64 {
    let i = 4294967296
    let j = i * i
    return v[j]
}
fn idx_wrap_boxed(v: vec[f64], s: str) -> f64 {
    let i = 4294967296
    let j = i * i
    return v[j]
}
fn cmp_exact(x: f64) -> bool {
    let big = 9007199254740993
    return x == big
}
fn range_max(v: vec[f64]) -> f64 {
    let mut acc = 10.0
    for i in 0..9223372036854775807 {
        print("iter={i}")
        acc = acc + v[i]
        break
    }
    return acc
}
fn main() -> i64 {
    assert(mul_lit(0.5) == mul_lit_boxed(0.5, ""))
    assert(mul_var(0.5) == mul_var_boxed(0.5, ""))
    let v: vec[f64] = [42.5]
    assert(idx_wrap(v) == idx_wrap_boxed(v, ""))
    assert(idx_wrap(v) == 42.5)
    assert(cmp_exact(9007199254740992.0) == false)
    assert(range_max(v) == 52.5)
    return 0
}
"""
    for opt in ("-O0", "-O1", "-O2"):
        c_code = emit_c(src, opt)
        rc, out, err = build_and_run(src, opt=opt)
        assert rc == 0, f"{opt} failed: {err}"
        assert out.strip() == "iter=0", f"{opt} unexpected output: {out!r}"
        if sys.platform != "win32":
            with tempfile.TemporaryDirectory() as td:
                cp = os.path.join(td, "f64_ub.c")
                exe = os.path.join(td, "f64_ub")
                with open(cp, "w", encoding="utf-8") as f:
                    f.write(c_code)
                cc = os.environ.get("CC", "gcc")
                link_libs = ["-lm", "-lpthread"] if sys.platform == "darwin" else ["-lm", "-lpthread", "-ldl"]
                comp = subprocess.run(
                    [cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-fsanitize=address,undefined,float-cast-overflow",
                     "-fno-sanitize-recover=all",
                     "-I", ENGINE, cp, *link_libs, "-o", exe],
                    capture_output=True, text=True,
                )
                assert comp.returncode == 0, f"{opt} -Werror/UBSan compile failed:\n{comp.stderr}"
                asan_opts = "halt_on_error=1" if sys.platform == "darwin" else "detect_leaks=1:halt_on_error=1"
                run = subprocess.run([exe], capture_output=True, text=True,
                                     env=dict(os.environ, ASAN_OPTIONS=asan_opts))
                assert run.returncode == 0, f"{opt} UBSan/ASan runtime failure:\n{run.stderr}"
                assert run.stdout.strip() == "iter=0"


if __name__ == "__main__":
    test_recursive_fast_path_i64_and_f64()
    test_recursive_fast_budget_recovery()
    test_large_frame_compile_speed_and_leak_freedom()
    test_f64_fast_int_subexprs_and_ub_freedom()
    print("PERF CODEGEN ALL GREEN")
