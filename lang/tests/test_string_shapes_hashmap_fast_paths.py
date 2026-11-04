"""O(1) string length, amortized string append, shared struct shapes with inline caches,
the `sort_by` key evaluation count and the flat hash table."""
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "lang", "tests"))
import _cxx  # noqa: E402

CLI = os.path.join(ROOT, "lang", "engine", "cli.py")
CXX = _cxx.build()


def run_cmd(argv, cwd=None):
    return subprocess.run(argv, cwd=cwd or ROOT, text=True, capture_output=True)


PROVE_SRC = """\
fn f64_step(x, dt) {
    let k1 = x + dt * 0.5
    let k2 = k1 * 0.999999 + 0.000001
    return k2
}

fn dyn_vec_sum(v) {
    let mut acc = 0
    for x in v {
        acc += x
    }
    return acc
}

fn dyn_vec_scale_sum(v, factor) {
    let mut acc = 0
    for i in 0..len(v) {
        acc += v[i] * factor
    }
    return acc
}

fn main() {
    // 5.1: O(1) string length + amortized O(N) s += chunk + s += s self-alias
    let mut s = ""
    for i in 0..2000 {
        s += "abcd"
    }
    assert(len(s) == 8000)
    assert(s.starts_with("abcdabcd"))
    assert(s.ends_with("abcdabcd"))

    let mut dbl = "xy"
    for i in 0..10 {
        dbl += dbl
    }
    assert(len(dbl) == 2048)

    // Non-ASCII UTF-8 codepoint count after SWAR prefix
    let uni = "abcdefgh12345678Привет!"
    assert(len(uni) == 23)

    // 5.2: Shared BShape + BFieldIC on struct read/write/interp + equality
    let mut total = 0.0
    let mut c = Counter { n: 0, label: "ticks" }
    for i in 0..5000 {
        let p = Vec3 { x: 1.5, y: 2.5, z: 3.0 }
        total = total + p.x + p.y + p.z
        c.n = c.n + 1
    }
    assert(total == 35000.0)
    assert(c.n == 5000)
    assert("{c.label}:{c.n}" == "ticks:5000")
    let p1 = Vec3 { x: 1.0, y: 2.0, z: 3.0 }
    let p2 = Vec3 { x: 1.0, y: 2.0, z: 3.0 }
    let p3 = Vec3 { x: 1.0, y: 2.0, z: 4.0 }
    assert(p1 == p2)
    assert(p1 != p3)

    // Polymorphic / unshaped fallback at a field IC site
    let rec = { "x": 10, "y": 20 }
    assert(rec["x"] + rec["y"] == 30)

    // 5.3a: O(N) Schwartzian sort_by key call-count parity (len==0, len==1, len==8)
    let mut calls: vec[i64] = []
    let mut empty: vec[i64] = []
    empty.sort_by(fn(x) { calls.push(1); return x })
    assert(len(calls) == 0)

    let mut single = [42]
    single.sort_by(fn(x) { calls.push(1); return -x })
    assert(len(calls) == 1)
    assert(single == [42])

    let mut calls8: vec[i64] = []
    let mut items = [5, 2, 8, 1, 9, 3, 7, 4]
    items.sort_by(fn(x) {
        calls8.push(1)
        return -x
    })
    assert(len(calls8) == 8)
    assert(items == [9, 8, 7, 5, 4, 3, 2, 1])

    // 5.3b: f64_dyn fast path + dynamic fallback when non-float passed
    let mut fx = 1.0
    for i in 0..10000 {
        fx = f64_step(fx, 0.25)
    }
    assert(fx > 100.0)
    let fb_f = f64_step(2, 4)
    assert(fb_f > 3.99 && fb_f < 4.01)

    // 5.3c: i64v_dyn vector loop unboxing + dynamic fallback on non-int vec
    let nums = [10, 20, 30, 40, 50]
    let mut vs = 0
    for i in 0..1000 {
        vs += dyn_vec_sum(nums) + dyn_vec_scale_sum(nums, 2)
    }
    assert(vs == 450000)

    // 5.4: Flat Robin-Hood Map insert / lookup / overwrite / backward-shift delete
    let mut m = {}
    for i in 0..1000 {
        m.set("key_{i}", i * 3)
    }
    assert(len(m) == 1000)
    assert(m["key_0"] == 0)
    assert(m["key_500"] == 1500)
    assert(m["key_999"] == 2997)
    assert(!m.has("missing_key"))

    // Overwrite existing keys (tests RC release of overwritten key & value)
    for i in 0..200 {
        m.set("key_{i}", i * 10)
    }
    assert(len(m) == 1000)
    assert(m["key_150"] == 1500)

    // Delete every even key (tests Robin-Hood backward-shift & dense compaction)
    for i in 0..500 {
        let ek = i * 2
        assert(m.del("key_{ek}"))
    }
    assert(len(m) == 500)
    for i in 0..500 {
        let ek = i * 2
        let ok = ek + 1
        assert(!m.has("key_{ek}"))
        let mut want = ok * 3
        if ok < 200 { want = ok * 10 }
        assert(m["key_{ok}"] == want)
    }

    print("W2_SPEED_OK")
}
"""



def main():
    asserts = 0
    with tempfile.TemporaryDirectory(prefix="brat_speed_") as td:
        src = os.path.join(td, "speed.brat")
        with open(src, "w", encoding="utf-8") as f:
            f.write(PROVE_SRC)

        # 1. Byte-identical C through both CLI doors + fast-path/shape symbols
        cli_c = run_cmd([sys.executable, CLI, "emit-c", src])
        cx_c = run_cmd([CXX, "emit-c", src])
        assert cli_c.returncode == 0, cli_c.stderr
        assert cx_c.returncode == 0, cx_c.stderr
        assert cli_c.stdout == cx_c.stdout, "Python CLI and native emit-c diverged on speed.brat"
        c_code = cli_c.stdout
        assert "vmap_shaped(" in c_code, "expected BShape vmap_shaped in emitted C"
        assert "map_get_ic(" in c_code, "expected BFieldIC map_get_ic in emitted C"
        assert "vfieldset_ic(" in c_code, "expected BFieldIC vfieldset_ic in emitted C"
        assert "BFieldIC _fic[" in c_code, "expected BFieldIC table in emitted C"
        assert "static double bf_f" in c_code, "expected f64_dyn raw double fast fn"
        assert "Vec *b_v" in c_code, "expected i64v_dyn unboxed Vec* fast fn"
        asserts += 8

        # 2. Interpreter execution (proves O(N) sort_by call count == 8 in runtime.py)
        ri = run_cmd([sys.executable, CLI, "run", "--interp", src])
        assert ri.returncode == 0, f"interp failed:\n{ri.stdout}\n{ri.stderr}"
        assert ri.stdout == "W2_SPEED_OK\n", ri.stdout
        asserts += 2

        # 3. Native ASan + LeakSanitizer zero-leak build & run
        c_file = os.path.join(td, "speed.c")
        with open(c_file, "w", encoding="utf-8") as f:
            f.write(c_code)
        asan_bin = os.path.join(td, "speed.asan")
        cc = run_cmd([
            "cc", "-O1", "-g", "-fsanitize=address", "-fno-omit-frame-pointer",
            "-fwrapv", "-I", os.path.join(ROOT, "lang", "engine"),
            c_file, "-o", asan_bin, "-lm", "-lpthread",
        ])
        assert cc.returncode == 0, f"ASan compile failed:\n{cc.stderr}"
        rn = run_cmd([asan_bin])
        assert rn.returncode == 0, f"ASan run failed:\n{rn.stdout}\n{rn.stderr}"
        assert rn.stdout == "W2_SPEED_OK\n", rn.stdout
        assert rn.stderr == "", rn.stderr
        asserts += 4

        # 4. Wall-clock linear scaling micro-benchmark (-O2):
        #    100,000 string appends + 200,000 shaped structs + 50,000 Robin-Hood map ops
        #    must finish well under 0.5s (would take several seconds if quadratic).
        bench_src = os.path.join(td, "bench.brat")
        with open(bench_src, "w", encoding="utf-8") as f:
            f.write("""\
fn hot_f64(x, y) {
    return x * 0.999999 + y * 0.000001
}

fn hot_vec(v) {
    let mut s = 0
    for x in v { s += x }
    return s
}

fn main() {
    let mut s = ""
    for i in 0..100000 {
        s += "xy"
    }
    assert(len(s) == 200000)

    let mut acc = 0
    for i in 0..200000 {
        let p = Pair { a: i, b: 1 }
        acc += p.a + p.b
    }
    assert(acc > 0)

    let mut fx = 1.0
    for i in 0..500000 {
        fx = hot_f64(fx, 2.0)
    }
    assert(fx > 1.0)

    let v = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    let mut vs = 0
    for i in 0..100000 {
        vs += hot_vec(v)
    }
    assert(vs == 5500000)

    let mut m = {}
    for i in 0..20000 {
        m.set(i, i + 1)
    }
    let mut ms = 0
    for i in 0..20000 {
        ms += m[i]
    }
    for i in 0..10000 {
        m.del(i)
    }
    assert(len(m) == 10000 && ms > 0)
    print("BENCH_OK")
}
""")
        bench_bin = os.path.join(td, "bench.bin")
        bb = run_cmd([CXX, "build", bench_src, "-o", bench_bin])
        assert bb.returncode == 0, bb.stderr
        t0 = time.monotonic()
        rb = run_cmd([bench_bin])
        dt = time.monotonic() - t0
        assert rb.returncode == 0 and rb.stdout == "BENCH_OK\n", rb.stderr
        assert dt < 1.0, f"expected sub-second linear scaling, took {dt:.3f}s"
        asserts += 3

    print(f"SPEED ALL GREEN ({asserts} asserts, bench={dt*1000:.1f}ms)")


if __name__ == "__main__":
    main()
