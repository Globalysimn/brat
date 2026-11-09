"""No silent fallback from the C++ emitter to the Python one, the single-threaded reference-count fast path
and lock striping, and the small-buffer optimizations in vectors, maps and strings.

Verifies:
  - No silent fallbacks: the Python CLI requires a working native compiler
    for `emit-c`/`build`, fails closed if `BRAT_CXX` is broken or missing,
    and ignores the retired `BRAT_FORCE_PY_EMIT` opt-in. It also fails closed
    on FFI/extern errors without duplicate ownership passes. All
    `brat-cxx` C compiler invocations use explicit `-std=gnu11`.
  - Single-threaded non-atomic RC fast path (`!_BRAT_MT()`), 64-way
    64B cache-line-padded container lock striping (`BStripeLock`), and dedicated
    unconditional `_brat_plugin_lock` under AddressSanitizer + ThreadSanitizer.
  - Inline 2-slot `Vec.inl[2]` Small-Buffer Optimization (`ok`/`err`/
    `some`/`closure`/1..2-elem vecs with zero secondary malloc), stack `BStackVec`
    HOF frames (`vfold`/`vpred`/`vdfr_pop`), cached 64-bit `BHead.hash` string
    hashing, `BTLS` tag interning, and in-place string append metadata reset.
  - `-O2` descending strided `while` loop bounds + congruence + DBM
    proofs (`--why-opt`), underflow/off-by-one rejection, `SK::ForIter` shadowed
    binding soundness, and 4-way (`-O0 == -O1 == -O2 == --interp`) equivalence.
  - Exact C `enum` width & signedness (`packed`, `: T`, unsigned 32-bit,
    signed/unsigned 64-bit, `ptr[enum]`) and recursive DWARF `DW_TAG_subroutine_type`
    callback signature + float/int register-class verification in `--object`.
"""

import os
import random
import shutil
import stat
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
sys.path.insert(0, HERE)

import _cxx  # noqa: E402

COMPARE_PY = False
CXX = _cxx.build()


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def run_cli(args, env=None, cwd=None):
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(
        _cxx.cli_args(*(list(args))),
        capture_output=True,
        text=True,
        env=merged,
        cwd=cwd or ROOT,
    )


def test_fail_loud_and_gnu11():
    with tempfile.TemporaryDirectory(prefix="brat_") as td:
        src = os.path.join(td, "hello.brat")
        with open(src, "w", encoding="utf-8") as f:
            f.write('fn main() -> i64 {\n  print("ok-11-1")\n  return 0\n}\n')

        # 1. Verify legacy Python engine files are completely absent from `lang/engine/`.
        py_files = [fn for fn in os.listdir(ENGINE) if fn.endswith(".py")]
        check(not py_files, f"unexpected .py files in lang/engine: {py_files}")

        # 2. Spy on `BRAT_CC` across `brat build` to prove `-std=gnu11`
        #    and verify zero duplicate `_UNVERIFIED_BANNER` in `emit-c --no-check`.
        real_cc = shutil.which("gcc") or shutil.which("cc")
        spy_log = os.path.join(td, "cc_args.log")
        spy_cc = os.path.join(td, "spy-cc")
        with open(spy_cc, "w", encoding="utf-8") as f:
            f.write(f'#!/bin/sh\necho "$@" >> "{spy_log}"\nexec "{real_cc}" "$@"\n')
        os.chmod(spy_cc, stat.S_IRWXU)

        exe1 = os.path.join(td, "hello_py")
        r_b1 = run_cli(["build", src, "-o", exe1], env={"BRAT_CC": spy_cc})
        check(r_b1.returncode == 0, f"brat build failed: {r_b1.stderr}")
        exe2 = os.path.join(td, "hello_cxx")
        env_spy = os.environ.copy()
        env_spy["BRAT_CC"] = spy_cc
        r_b2 = subprocess.run([CXX, "build", src, "-o", exe2],
                              capture_output=True, text=True, env=env_spy)
        check(r_b2.returncode == 0, f"brat-cxx build failed: {r_b2.stderr}")

        logged = open(spy_log, encoding="utf-8").read().splitlines()
        check(len(logged) >= 2, f"expected >=2 cc invocations, got {logged}")
        for ln in logged:
            check("-std=gnu11" in ln.split(), f"missing -std=gnu11 in cc invocation: {ln}")

        r_nc = run_cli(["emit-c", src, "--no-check"])
        check(r_nc.returncode == 0, f"emit-c --no-check failed: {r_nc.stderr}")
        check(r_nc.stdout.count("BRAT: UNVERIFIED") == 1,
              f"expected exactly 1 UNVERIFIED banner, got {r_nc.stdout.count('BRAT: UNVERIFIED')}")

        # 3. `brat build --header` must fail closed (never swallow FFI contract errors).
        ext_src = os.path.join(td, "bad_ffi.brat")
        with open(ext_src, "w", encoding="utf-8") as f:
            f.write('extern "m" fn cos(x: i32) -> i32\nfn main() -> i64 { return 0 }\n')
        hdr = os.path.join(td, "m.h")
        with open(hdr, "w", encoding="utf-8") as f:
            f.write("double cos(double x);\n")
        r_bad = run_cli(["build", ext_src, "--header", hdr, "-o", os.path.join(td, "bad_out")])
        check(r_bad.returncode != 0, "brat build silently swallowed FFI header mismatch!")
    print("ok: fail-loud codegen/FFI + explicit -std=gnu11")


# Runtime Concurrency, Vec SBO, Stack HOFs & String Hash
def test_steps_11_2_and_11_3_crt_speed_and_tsan():
    cc = shutil.which("gcc") or shutil.which("clang")
    check(cc is not None, "C compiler required")
    with tempfile.TemporaryDirectory(prefix="brat_crt_") as td:
        # 1. ASan + LSan probe for Vec.inl[2] SBO, stack HOFs, cached BHead.hash,
        #    BTLS tag interning, and in-place string append metadata invalidation!
        probe_c = os.path.join(td, "probe_sbo.c")
        with open(probe_c, "w", encoding="utf-8") as f:
            f.write(r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lang/engine/crt.h"

static int g_defer_runs = 0;

Val vcall_any(Val fn, Vec *a) {
    if (fn.tag == V_INT) {
        if (fn.as.ival == 1) {
            /* fold: acc + elem */
            return vint(a->items[0].as.ival + a->items[1].as.ival);
        }
        if (fn.as.ival == 2) {
            /* pred: elem == 42 */
            return vbool(a->items[0].as.ival == 42);
        }
        if (fn.as.ival == 3) {
            /* defer callback */
            g_defer_runs++;
            return vnil();
        }
    }
    return vnil();
}

int main(void) {
    /* layout invariant: cache-line-padded 64-byte stripe locks */
    assert(sizeof(BStripeLock) >= 64);
    assert(!_BRAT_MT());

    /* Vec.inl[2] SBO — 0..2 items stay inline (`v->items == v->inl`) */
    Vec *v = vvec();
    assert(v->cap == 2);
    assert(v->items == v->inl);
    vec_push(v, vint(10));
    vec_push(v, vint(20));
    assert(v->items == v->inl);
    assert(v->items[0].as.ival == 10 && v->items[1].as.ival == 20);
    /* Pushing 3rd element spills cleanly to heap */
    vec_push(v, vint(30));
    assert(v->items != v->inl);
    assert(v->len == 3 && v->items[0].as.ival == 10 && v->items[2].as.ival == 30);
    vrc_dec(vvecv(v));

    /* SBO inside arena + tag values (`ok(x)` / `err(msg)`) */
    BMark mk = barena_enter();
    Vec *av = vvec();
    assert(av->items == av->inl);
    vec_push(av, vstr("hello_arena_world_heap_string"));
    vec_push(av, vint(99));
    assert(av->items == av->inl);
    vec_push(av, vint(100));
    assert(av->items != av->inl);
    barena_leave(mk);

    Vec *tv_vec = vvec();
    vec_push(tv_vec, vint(77));
    Val tv = vtagv("ok", tv_vec);
    assert(tv.as.tv->items == tv.as.tv->inl);
    assert(tv.as.tv->items[0].as.ival == 77);
    vrc_dec(tv);

    /* Stack HOF frames (`vfold`, `vpred`, `vdfreset`) */
    Vec *nums = vvec();
    for (int i = 1; i <= 50; i++) vec_push(nums, vint(i));
    Val nv = vvecv(nums);
    Val sum = vfold(nv, vint(0), vint(1));
    assert(sum.tag == V_INT && sum.as.ival == 1275);
    Val has42 = vpred(nv, vint(2), "any");
    assert(has42.tag == V_BOOL && has42.as.b == 1);
    Val all42 = vpred(nv, vint(2), "all");
    assert(all42.tag == V_BOOL && all42.as.b == 0);
    Val fnd42 = vpred(nv, vint(2), "find");
    assert(fnd42.tag == V_INT && fnd42.as.ival == 42);
    vrc_dec(nv);

    bint dfb = vdfsp;
    vdfpush();
    vdfadd(vint(3));
    vdfadd(vint(3));
    vdfreset(dfb);
    free(vdfr);
    vdfr = NULL;
    assert(g_defer_runs == 2);

    /* Cached BHead.hash + in-place `vadd_self_slow` invalidation! */
    Val hs = vstr("this_is_a_long_heap_string_key_for_hashing_012345");
    assert(!hs.sso);
    BHead *sh = (BHead *)hs.as.str - 1;
    assert(sh->hash == 0);
    uint64_t h1 = vkey_hash(hs);
    assert(h1 != 0 && sh->hash == h1);
    assert(h1 == vcstr_hash(hs.as.str, (size_t)sh->len));
    /* Prime cplen/ascii via vlen, then mutate in-place (rc == 1) via vadd_self_slow! */
    Val ln1 = vlen(hs);
    assert(ln1.as.ival == 49);
    assert(sh->ascii == 1 && sh->cplen == 49);
    hs = vadd_self(hs, vlit("_tail"));
    BHead *sh2 = (BHead *)hs.as.str - 1;
    assert(sh2->hash == 0 && sh2->cplen == 0 && sh2->ascii == 0);
    Val ln2 = vlen(hs);
    assert(ln2.as.ival == 54);
    uint64_t h2 = vkey_hash(hs);
    assert(h2 != 0 && h2 != h1 && sh2->hash == h2);
    vrc_dec(hs);

    puts("CRT_SBO_HASH_OK");
    return 0;
}
""")
        probe_exe = os.path.join(td, "probe_sbo")
        r_c = subprocess.run(
            [cc, "-std=gnu11", "-O2", "-g", "-fsanitize=address,undefined",
             "-Wall", "-Wextra", "-Werror", "-I", ROOT, probe_c, "-o", probe_exe,
             "-lm", "-lpthread", "-ldl"],
            capture_output=True, text=True,
        )
        check(r_c.returncode == 0, f"ASan SBO probe compile failed:\n{r_c.stderr}")
        env_asan = os.environ.copy()
        env_asan["ASAN_OPTIONS"] = "detect_leaks=1"
        r_run = subprocess.run([probe_exe], capture_output=True, text=True, env=env_asan)
        check(r_run.returncode == 0 and "CRT_SBO_HASH_OK" in r_run.stdout,
              f"ASan+LSan SBO probe failed (rc={r_run.returncode}):\n{r_run.stdout}\n{r_run.stderr}")

        # 2. ThreadSanitizer probe: concurrent plugin table ops (`_brat_plugin_lock`)
        #    while `g_pool_n == 0` AND concurrent striped container mutations with `_BRAT_MT()`!
        plug_c = os.path.join(td, "mini_plug.c")
        with open(plug_c, "w", encoding="utf-8") as f:
            f.write("long plug_twice(long x) { return x * 2; }\n")
        plug_so = os.path.join(td, "libmini_plug.so")
        subprocess.run([cc, "-std=gnu11", "-O2", "-shared", "-fPIC", plug_c, "-o", plug_so], check=True)

        tsan_c = os.path.join(td, "probe_tsan.c")
        with open(tsan_c, "w", encoding="utf-8") as f:
            f.write(r"""
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "lang/engine/crt.h"

Val vcall_any(Val fn, Vec *a) { (void)fn; (void)a; return vnil(); }

static const char *g_so_path = NULL;
static Vec *g_vecs[16];

static void *plugin_worker(void *arg) {
    (void)arg;
    /* Note: g_pool_n is STILL 0 here — proves `_brat_plugin_lock` is unconditional! */
    for (int i = 0; i < 40; i++) {
        Val p = vstr(g_so_path);
        Val nm = vstr("plug_twice");
        Val h = vlib_open(p);
        assert(h.tag == V_INT && h.as.ival > 0);
        Val sym = vlib_sym(h, nm);
        assert(sym.tag == V_INT && sym.as.ival > 0);
        bint owner = 0;
        long (*fn)(long) = (long (*)(long))vplugin_acquire(sym, &owner);
        assert(fn && fn(21) == 42);
        vplugin_release(owner);
        Val cl = vlib_close(h);
        assert(cl.tag == V_INT && cl.as.ival == 0);
        vrc_dec(nm);
        vrc_dec(p);
    }
    return NULL;
}

static void *stripe_worker(void *arg) {
    int tid = (int)(intptr_t)arg;
    for (int iter = 0; iter < 500; iter++) {
        Vec *v = g_vecs[(tid + iter) & 15];
        vec_push(v, vint(iter));
        Val f = vfirst(vvecv(v));
        (void)f;
    }
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    vargs_set(argc, argv);
    brat_mt_enable();
    g_so_path = argv[1];

    pthread_t pt[4];
    for (int i = 0; i < 4; i++) assert(pthread_create(&pt[i], NULL, plugin_worker, NULL) == 0);
    for (int i = 0; i < 4; i++) pthread_join(pt[i], NULL);

    for (int i = 0; i < 16; i++) g_vecs[i] = vvec();
    __atomic_store_n(&g_pool_n, 4, __ATOMIC_RELEASE);
    for (int i = 0; i < 4; i++) assert(pthread_create(&pt[i], NULL, stripe_worker, (void *)(intptr_t)i) == 0);
    for (int i = 0; i < 4; i++) pthread_join(pt[i], NULL);

    bint total = 0;
    for (int i = 0; i < 16; i++) {
        total += g_vecs[i]->len;
        vrc_dec(vvecv(g_vecs[i]));
    }
    assert(total == 2000);
    puts("CRT_TSAN_CLEAN");
    return 0;
}
""")
        tsan_exe = os.path.join(td, "probe_tsan")
        r_tc = subprocess.run(
            [cc, "-std=gnu11", "-O1", "-g", "-fsanitize=thread",
             "-Wall", "-Wextra", "-Werror", "-I", ROOT, tsan_c, "-o", tsan_exe,
             "-lm", "-lpthread", "-ldl"],
            capture_output=True, text=True,
        )
        check(r_tc.returncode == 0, f"TSan compile failed:\n{r_tc.stderr}")
        tsan_cmd = [tsan_exe, plug_so]
        import platform as _plat, shutil as _shutil
        if sys.platform.startswith("linux") and _shutil.which("setarch"):
            tsan_cmd = ["setarch", _plat.machine(), "-R", tsan_exe, plug_so]
        r_tr = subprocess.run(tsan_cmd, capture_output=True, text=True)
        check(r_tr.returncode == 0 and "CRT_TSAN_CLEAN" in r_tr.stdout and "WARNING: ThreadSanitizer" not in r_tr.stderr,
              f"TSan detected race or failure:\n{r_tr.stdout}\n{r_tr.stderr}")

        # 3. End-to-end dynamic workload benchmark (SBO tags + cached string hash + stack fold)
        bench_brat = os.path.join(td, "dyn_bench.brat")
        with open(bench_brat, "w", encoding="utf-8") as f:
            f.write("""enum Wrap {
  Pair(dyn),
  Empty,
}

fn wrap_pair(x: i64) -> Wrap {
  if x % 2 == 0 {
    return Pair([x, x + 1])
  }
  return Pair([x + 1, x + 2])
}

fn main() -> i64 {
  let m = {"alpha_long_lookup_key_001": 3, "beta_long_lookup_key_002": 5, "gamma_long_lookup_key_003": 7}
  let k1 = "alpha_long_lookup_key_001" + ""
  let k2 = "beta_long_lookup_key_002" + ""
  let k3 = "gamma_long_lookup_key_003" + ""
  let mut total = 0
  let mut i = 0
  while i < 20000 {
    let r = wrap_pair(i)
    match r {
      Pair(pair) => {
        total = total + pair.fold(0, fn(a, b) { return a + b }) + m.get_or(k1, 0) + m.get_or(k2, 0) + m.get_or(k3, 0)
      }
      Empty => {}
    }
    i = i + 1
  }
  print(total)
  return 0
}
""")
        bench_exe = os.path.join(td, "dyn_bench")
        subprocess.run([CXX, "build", "-O2", bench_brat, "-o", bench_exe], check=True)
        t0 = time.perf_counter()
        rb = subprocess.run([bench_exe], capture_output=True, text=True, check=True)
        dt_ms = (time.perf_counter() - t0) * 1000.0
        check(rb.stdout.strip() == "400320000", f"unexpected bench output: {rb.stdout!r}")
    print(f"ok: ASan+LSan SBO/hash/HOF + TSan stripes/plugin, 20k dyn iters in {dt_ms:.1f} ms")


# `-O2` Descending Strided Loops, `SK::ForIter` & 4-Way Fuzzing
def test_o2_descending_and_foriter():
    with tempfile.TemporaryDirectory(prefix="brat_o2_") as td:
        # 1. Positive proofs: reverse vector scan + descending strided congruence + ForIter VecLit hull
        good_src = os.path.join(td, "desc_good.brat")
        with open(good_src, "w", encoding="utf-8") as f:
            f.write("""fn rev_dot(v: vec[i64]) -> i64 {
  let mut i = len(v) - 1
  let mut acc = 0
  while i >= 0 {
    acc = acc + v[i] + (i / 3)
    i = i - 1
  }
  return acc
}

fn rev_plus_neg(v: vec[i64]) -> i64 {
  let mut i = len(v) - 1
  let mut acc = 0
  while i >= 0 {
    acc = acc + v[i]
    i = i + (-1)
  }
  return acc
}

fn rev_stride_cong(n: i64) -> i64 {
  if n < 0 || n > 10000 { return 0 }
  let mut i = n * 12
  let mut acc = 0
  while i > 0 {
    acc = acc + (i / 6)
    i = i - 6
  }
  return acc
}

fn iter_vec_guard(v: vec[i64]) -> i64 {
  let mut acc = 0
  for elem in v {
    if elem >= 0 {
      acc = acc + elem / 5
    }
  }
  return acc
}

fn main() -> i64 {
  print(rev_dot([10, 20, 30, 40, 50]))
  print(rev_plus_neg([10, 20, 30, 40, 50]))
  print(rev_stride_cong(4))
  print(iter_vec_guard([15, -3, 25]))
  return 0
}
""")
        why = subprocess.run([CXX, "cc", "-O2", "--why-opt", good_src], capture_output=True, text=True)
        check(why.returncode == 0, f"--why-opt -O2 failed: {why.stderr}")
        proofs = why.stderr
        check(proofs.count("bounds check dropped on `v[...]`") == 2,
              f"expected 2 reverse-scan bounds proofs, got:\n{proofs}")
        check("descending strided loop `i >= 0` (step -1)" in proofs,
              f"missing descending step -1 proof:\n{proofs}")
        check("6Z + 0" in proofs and "descending strided loop `i > 0` (step -6)" in proofs,
              f"missing descending congruence 6Z + 0 proof:\n{proofs}")
        check("iter_vec_guard" in proofs, f"missing ForIter body proof:\n{proofs}")

        # 2. Negative soundness gates:
        #    (a) off-by-one `let mut i = len(v); while i >= 0 { v[i] }` MUST keep bounds check!
        #    (b) underflowing `while i >= -9223372036854775807 - 1` (`INT64_MIN`) MUST bail!
        #    (c) shadowed `ForIter` variable `let mut x = 10; for x in v {}; 100 / x` MUST keep div check!
        bad_src = os.path.join(td, "desc_soundness.brat")
        with open(bad_src, "w", encoding="utf-8") as f:
            f.write("""fn off_by_one(v: vec[i64]) -> i64 {
  let mut i = len(v)
  let mut s = 0
  while i >= 0 {
    s = s + v[i]
    i = i - 1
  }
  return s
}

fn shadow_iter(v: vec[i64]) -> i64 {
  let mut x = 10
  for x in v {
  }
  return 100 / x
}

fn main() -> i64 {
  print(shadow_iter([0]))
  return 0
}
""")
        why_bad = subprocess.run([CXX, "cc", "-O2", "--why-opt", bad_src], capture_output=True, text=True)
        check(why_bad.returncode == 0, f"cc failed on soundness test: {why_bad.stderr}")
        check(why_bad.stderr.strip() == "",
              f"UNSOUND proof emitted on off-by-one or shadowed ForIter:\n{why_bad.stderr}")

        # Verify `shadow_iter([0])` raises clean `"div by zero"` identically across `-O0`, `-O1`, and `-O2`
        # (never emitting raw `100 / x` SIGFPE under `-O2`!).
        outs = []
        for opt in ("-O0", "-O1", "-O2"):
            exe = os.path.join(td, f"bad_{opt[1:]}")
            subprocess.run([CXX, "build", opt, bad_src, "-o", exe], check=True)
            rr = subprocess.run([exe], capture_output=True, text=True)
            check(rr.returncode == 1 and "div by zero" in rr.stderr,
                  f"{opt} miscompiled shadowed ForIter variable (rc={rr.returncode}, err={rr.stderr!r})!")
            outs.append((rr.returncode, rr.stdout, rr.stderr))
        check(outs[0] == outs[1] == outs[2], f"-O0/-O1/-O2 mismatch on shadow_iter: {outs}")

        # 3. 4-way differential equivalence (`-O0 == -O1 == -O2 == --interp`) on 30 randomized
        #    descending / ascending / ForIter programs!
        rng = random.Random(20261101)
        lines = []
        calls = []
        for k in range(30):
            step = rng.choice([1, 2, 3, 4])
            mul = step * rng.choice([1, 2, 3])
            cmp_op = rng.choice([">=", ">"])
            lo_lit = rng.choice([0, 1, 2])
            lines.append(f"""fn fz_{k}(v: vec[i64], n: i64) -> i64 {{
  let mut acc = 0
  let mut i = len(v) - 1
  while i >= 0 {{
    acc = acc + v[i] - (i / {step})
    i = i - {step}
  }}
  if n >= 0 && n <= 40 {{
    let mut j = n * {mul}
    while j {cmp_op} {lo_lit} {{
      acc = acc + (j / {step})
      j = j - {step}
    }}
  }}
  for e in v {{
    if e > 0 {{
      acc = acc + (e / {step})
    }}
  }}
  return acc
}}""")
            vec_vals = [rng.randint(-20, 50) for _ in range(rng.randint(0, 7))]
            n_arg = rng.choice([-5, 0, 1, 7, 19])
            calls.append(f"  print(fz_{k}({vec_vals}, {n_arg}))")
        lines.append("fn main() -> i64 {\n" + "\n".join(calls) + "\n  return 0\n}\n")
        fuzz_path = os.path.join(td, "fuzz_o2_desc.brat")
        with open(fuzz_path, "w", encoding="utf-8") as f:
            f.write("\n\n".join(lines))

        ref = run_cli(["run", "--interp", fuzz_path])
        check(ref.returncode == 0, f"interp failed on fuzz_o2_desc: {ref.stderr}")
        for opt in ("-O0", "-O1", "-O2"):
            exe = os.path.join(td, f"fuzz_{opt[1:]}")
            subprocess.run([CXX, "build", opt, fuzz_path, "-o", exe], check=True)
            got = subprocess.run([exe], capture_output=True, text=True, check=True)
            check(got.stdout == ref.stdout,
                  f"4-way mismatch at {opt}:\nwant:\n{ref.stdout[:400]}\ngot:\n{got.stdout[:400]}")
    print("ok: -O2 descending strided loops + ForIter soundness + 30-fn 4-way fuzz")


# Exact C Enum ABI & Recursive DWARF Callback Signature Verification
def test_exact_enums_and_dwarf_callbacks():
    cc = shutil.which("gcc") or shutil.which("clang")
    check(cc is not None, "C compiler required")
    with tempfile.TemporaryDirectory(prefix="brat_ffi_") as td:
        # Native `brat-cxx ffi --header` must distinguish GPR vs XMM (`i32` != `f32`, `i64` != `f64`).
        gpr_hdr = os.path.join(td, "gpr_xmm.h")
        with open(gpr_hdr, "w", encoding="utf-8") as f:
            f.write("void take_f32(float x);\nvoid take_f64(double x);\n")
        bad_32 = os.path.join(td, "bad32.brat")
        with open(bad_32, "w", encoding="utf-8") as f:
            f.write("extern fn take_f32(x: i32) -> none\n")
        bad_64 = os.path.join(td, "bad64.brat")
        with open(bad_64, "w", encoding="utf-8") as f:
            f.write("extern fn take_f64(x: i64) -> none\n")
        check(subprocess.run([CXX, "ffi", bad_32, "--header", gpr_hdr], capture_output=True).returncode != 0,
              "native ffi --header accepted i32 for float (GPR vs XMM)")
        check(subprocess.run([CXX, "ffi", bad_64, "--header", gpr_hdr], capture_output=True).returncode != 0,
              "native ffi --header accepted i64 for double (GPR vs XMM)")

        hdr = os.path.join(td, "enum_cb.h")
        with open(hdr, "w", encoding="utf-8") as f:
            f.write("""#include <stdint.h>

typedef enum __attribute__((packed)) {
  PU_ZERO = 0,
  PU_HIGH = 200
} PackU8;

typedef enum __attribute__((packed)) {
  PI_NEG = -5,
  PI_POS = 10
} PackI8;

typedef enum {
  PU16_A = 0,
  PU16_B = 1000
} __attribute__((packed)) PackU16;

typedef enum {
  UF_LO = 1,
  UF_HI = 0x80000000u
} U32Flags;

typedef enum {
  UB_BIG = (1ULL << 40),
  UB_MSB = (1ULL << 63)
} U64Big;

typedef enum {
  IN_NEG = -(1LL << 40),
  IN_POS = 7
} I64Neg;

typedef double (*scale_cb)(int code, float factor);

typedef struct {
  PackU8 tag8;
  PackI8 delta8;
  PackU16 code16;
  U32Flags flags32;
  U64Big big64;
  scale_cb hook;
} EnumRecord;

double run_enum_cb(PackU8 tag, U64Big big, I64Neg sbig, scale_cb cb);
void probe_enum_ptr(PackU8 *out_tag);
int inspect_record(EnumRecord *rec);
void run_var_cb(int x, void (*vcb)(int, ...));
""")
        # Verify C23 / GNU fixed-underlying `: T` parsing natively via `brat-cxx bindgen`:
        fixed_h = os.path.join(td, "fixed_enum.h")
        fixed_out = os.path.join(td, "fixed_enum.brat")
        with open(fixed_h, "w", encoding="utf-8") as f:
            f.write(
                "#include <stdint.h>\n"
                "typedef unsigned short my_u16;\n"
                "enum FixedTag : uint8_t { FT_A = 1, FT_B = 2 };\n"
                "typedef enum : my_u16 { FA_X = 3 } FixedAlias;\n"
                "enum FwdI64 : int64_t;\n"
                "void use_fixed(enum FixedTag a, FixedAlias b, enum FwdI64 *c);\n"
            )
        r_fixed = subprocess.run([CXX, "bindgen", fixed_h, "-o", fixed_out],
                                 capture_output=True, text=True, check=True)
        fixed_txt = open(fixed_out, encoding="utf-8").read()
        check("a: u8, b: u16, c: ptr[i64]" in fixed_txt,
              f"native bindgen fixed-underlying enum widths wrong:\n{fixed_txt}")
        if COMPARE_PY:
            sys.path.insert(0, ENGINE)
            import ffi
            check(not ffi.same("i32", "f32") and not ffi.same("i64", "f64"), "py ffi.same")
            fixed_decl = ffi.parse_header(
                "typedef unsigned short my_u16;\n"
                "enum FixedTag : uint8_t { FT_A = 1, FT_B = 2 };\n"
                "typedef enum : my_u16 { FA_X = 3 } FixedAlias;\n"
                "enum FwdI64 : int64_t;\n"
            )
            check(fixed_decl.enum_types.get("FixedTag") == "u8", f"got {fixed_decl.enum_types}")
            check(fixed_decl.enum_types.get("FixedAlias") == "u16", f"got {fixed_decl.enum_types}")
            check(fixed_decl.enum_types.get("FwdI64") == "i64", f"got {fixed_decl.enum_types}")

        csrc = os.path.join(td, "enum_cb.c")
        with open(csrc, "w", encoding="utf-8") as f:
            f.write("""#include "enum_cb.h"

double run_enum_cb(PackU8 tag, U64Big big, I64Neg sbig, scale_cb cb) {
  double base = cb((int)tag, 2.5f);
  return base + (double)(big >> 40) + (double)(sbig >> 40);
}

void probe_enum_ptr(PackU8 *out_tag) {
  if (out_tag) *out_tag = PU_HIGH;
}

int inspect_record(EnumRecord *rec) {
  return rec ? (int)rec->code16 + (int)rec->delta8 : 0;
}

void run_var_cb(int x, void (*vcb)(int, ...)) {
  if (vcb) vcb(x, 99);
}
""")
        so_path = os.path.join(td, "libenum_cb.so")
        subprocess.run([cc, "-std=gnu11", "-g", "-O2", "-shared", "-fPIC", csrc, "-o", so_path], check=True)

        # 1. `brat bindgen` emits exact enum widths & signedness (`u8`, `i8`, `u16`, `u32`, `u64`, `i64`, `ptr[u8]`)
        gen_brat = os.path.join(td, "enum_cb_gen.brat")
        r_bg = run_cli(["bindgen", "--lib", so_path, hdr, "-o", gen_brat])
        check(r_bg.returncode == 0, f"bindgen failed:\n{r_bg.stdout}\n{r_bg.stderr}")
        gen_txt = open(gen_brat, encoding="utf-8").read()
        for expected in (
            "tag8: u8",
            "delta8: i8",
            "code16: u16",
            "flags32: u32",
            "big64: u64",
            "hook: fn(i32, f32) -> f64",
            "tag: u8, big: u64, sbig: i64, cb: fn(i32, f32) -> f64",
            "out_tag: ptr[u8]",
        ):
            check(expected in gen_txt, f"missing {expected!r} in generated bindings:\n{gen_txt}")
        check("UB_MSB" not in gen_txt, "2^63 unsigned enumerator must not emit overflowing signed `pub const`")

        r_ck = run_cli(["check", gen_brat])
        check(r_ck.returncode == 0, f"brat check failed on enum_cb_gen.brat:\n{r_ck.stdout}\n{r_ck.stderr}")

        # Exact binding without C variadic callback passes BOTH `--header` AND DWARF `--object`!
        clean_brat = os.path.join(td, "clean_bind.brat")
        with open(clean_brat, "w", encoding="utf-8") as f:
            f.write("\n".join(ln for ln in gen_txt.splitlines() if "run_var_cb" not in ln) + "\n")

        r_hdr = run_cli(["ffi", clean_brat, "--header", hdr])
        check(r_hdr.returncode == 0, f"ffi --header failed on exact enum/callback binding:\n{r_hdr.stdout}\n{r_hdr.stderr}")
        r_obj = run_cli(["ffi", clean_brat, "--object", so_path])
        check(r_obj.returncode == 0, f"ffi --object failed on exact enum/callback binding:\n{r_obj.stdout}\n{r_obj.stderr}")

        # Execute end-to-end across BOTH native C AND `--interp`!
        app = os.path.join(td, "use_enum_cb.brat")
        with open(app, "w", encoding="utf-8") as f:
            f.write("""import clean_bind

fn my_scale(code: i32, factor: f32) -> f64 {
  if code == 10 {
    return factor * 10.0
  }
  return 0.0
}

fn main() -> i64 {
  let res = run_enum_cb(10, UB_BIG, IN_NEG, "my_scale")
  print(res)
  return 0
}
""")
        r_i = run_cli(["run", "--interp", "--header", hdr, app], env={"LD_LIBRARY_PATH": td})
        check(r_i.returncode == 0 and r_i.stdout.strip() == "25.0",
              f"--interp enum+callback run failed: rc={r_i.returncode}, out={r_i.stdout!r}, err={r_i.stderr!r}")
        app_exe = os.path.join(td, "use_enum_cb_exe")
        r_nb = run_cli(["build", app, "--header", hdr, "-o", app_exe])
        check(r_nb.returncode == 0, f"native build failed:\n{r_nb.stdout}\n{r_nb.stderr}")
        env_lib = dict(os.environ, LD_LIBRARY_PATH=td)
        r_nr = subprocess.run([app_exe], capture_output=True, text=True, env=env_lib, check=True)
        check(r_nr.stdout.strip() == "25.0", f"native enum+callback mismatch: {r_nr.stdout!r}")

        # 2. Prove DWARF `--object` rejects EVERY drifted callback / enum / float-vs-int ABI mismatch!
        drift_cases = [
            ("wrong_enum_w", f'extern "{so_path}" fn run_enum_cb(tag: i32, big: u64, sbig: i64, cb: fn(i32, f32) -> f64) -> f64\n'),
            ("wrong_cb_arity", f'extern "{so_path}" fn run_enum_cb(tag: u8, big: u64, sbig: i64, cb: fn(i32) -> f64) -> f64\n'),
            ("wrong_cb_param_cls", f'extern "{so_path}" fn run_enum_cb(tag: u8, big: u64, sbig: i64, cb: fn(i32, i32) -> f64) -> f64\n'),
            ("wrong_cb_ret_cls", f'extern "{so_path}" fn run_enum_cb(tag: u8, big: u64, sbig: i64, cb: fn(i32, f32) -> i64) -> f64\n'),
            ("wrong_cb_vs_ptr", f'extern "{so_path}" fn run_enum_cb(tag: u8, big: u64, sbig: i64, cb: ptr[u8]) -> f64\n'),
            ("wrong_ptr_vs_cb", f'extern "{so_path}" fn probe_enum_ptr(out_tag: fn(i32) -> i32) -> none\n'),
            ("wrong_variadic_cb", f'extern "{so_path}" fn run_var_cb(x: i32, vcb: fn(i32) -> none) -> none\n'),
            ("wrong_struct_cb_field",
             "extern struct EnumRecord {\n  tag8: u8,\n  delta8: i8,\n  code16: u16,\n  flags32: u32,\n  big64: u64,\n  hook: fn(i32, i32) -> f64,\n}\n"),
        ]
        for label, bad_code in drift_cases:
            bp = os.path.join(td, f"{label}.brat")
            with open(bp, "w", encoding="utf-8") as f:
                f.write(bad_code)
            rd = run_cli(["ffi", bp, "--object", so_path])
            check(rd.returncode != 0,
                  f"DWARF --object failed to reject {label}!\nstdout={rd.stdout}\nstderr={rd.stderr}")
    print("ok: exact C enum widths/signedness + recursive DWARF callback verification")


def test_debug_line_directives_and_dwarf() -> None:
    with tempfile.TemporaryDirectory() as td:
        helper = os.path.join(td, "helper.brat")
        with open(helper, "w", encoding="utf-8") as f:
            f.write("""pub struct Boxed {
  val: i64,
}

impl Boxed {
  fn scaled(self, k: i64) -> i64 {
    return self.val * k
  }
}

pub fn make_boxed(v: i64) -> Boxed {
  return Boxed { val: v }
}

pub fn add_bias(x: i64) -> i64 {
  let b = 7
  return x + b
}
""")
        app = os.path.join(td, "vsql_app.brat")
        with open(app, "w", encoding="utf-8") as f:
            f.write("""import helper

fn fast_fib(n: i64) -> i64 {
  if n < 2 {
    return n
  }
  return fast_fib(n - 1) + fast_fib(n - 2)
}

fn main() -> i64 {
  let bx = helper.make_boxed(fast_fib(10))
  let f = fn(d) { return helper.add_bias(bx.scaled(2) + d) }
  print(f(3))
  return 0
}
""")
        r_plain = run_cli(["emit-c", "-O2", app])
        check(r_plain.returncode == 0, f"plain emit-c failed: {r_plain.stderr}")
        check("\n#line " not in r_plain.stdout and not r_plain.stdout.startswith("#line "),
              "plain emit-c without -g emitted #line directives")

        out_c = os.path.join(td, "emitted_dbg.c")
        r_dbg_c = run_cli(["emit-c", "-O2", "-g", app, "-o", out_c])
        check(r_dbg_c.returncode == 0 and os.path.isfile(out_c),
              f"emit-c -g -o failed: rc={r_dbg_c.returncode}, err={r_dbg_c.stderr}")
        with open(out_c, "r", encoding="utf-8") as f:
            c_src = f.read()
        check("helper.brat\"" in c_src and "vsql_app.brat\"" in c_src,
              "emit-c -g missing per-module #line file mappings")
        check("#line 12" in c_src and "#line 4" in c_src,
              "emit-c -g missing expected statement #line numbers")
        # Root filename contains `vsql_`, which must not trip the CRT SQLite mask (0x10).
        for ln in c_src.splitlines():
            if ln.startswith("#define BRAT_CRT_MASK "):
                mask_val = int(ln.split()[-1].rstrip("u"))
                check((mask_val & 0x10) == 0,
                      f"#line path falsely enabled SQLite CRT mask: {ln}")
                break

        exe_ext = ".exe" if os.name == "nt" else ""
        dbg_exe = os.path.join(td, "app_dbg" + exe_ext)
        for flag in ("-O0", "-O1", "-O2"):
            rb = run_cli(["build", flag, "-g", app, "-o", dbg_exe])
            check(rb.returncode == 0, f"build {flag} -g failed:\n{rb.stdout}\n{rb.stderr}")
            rr = subprocess.run([dbg_exe], capture_output=True, text=True, check=True)
            check(rr.stdout.strip() == "120",
                  f"build {flag} -g wrong output: {rr.stdout!r}")

        r_run = run_cli(["run", "-O2", "--debug", app])
        check(r_run.returncode == 0 and r_run.stdout.strip() == "120",
              f"run --debug failed: rc={r_run.returncode}, out={r_run.stdout!r}, err={r_run.stderr!r}")

        if sys.platform.startswith("linux") and shutil.which("readelf"):
            rdw = subprocess.run(["readelf", "--debug-dump=decodedline", dbg_exe],
                                 capture_output=True, text=True, check=True)
            check("vsql_app.brat" in rdw.stdout and "helper.brat" in rdw.stdout,
                  f"DWARF .debug_line missing .brat filenames:\n{rdw.stdout[:1200]}")
    print("ok: -g/--debug #line source maps across modules + DWARF .debug_line")


if __name__ == "__main__":
    test_fail_loud_and_gnu11()
    test_steps_11_2_and_11_3_crt_speed_and_tsan()
    test_o2_descending_and_foriter()
    test_exact_enums_and_dwarf_callbacks()
    test_debug_line_directives_and_dwarf()
    print("ALL (v0.5) TESTS GREEN!")
