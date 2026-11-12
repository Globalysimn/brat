"""The checker gate on `run -O0`, bindgen of constants, function pointers and inline arrays,
and the iterative runtime walkers on very deep values.

Verifies:
  - `brat run -O0` runs the type/borrow/capability gate by default,
    `--no-check` bypasses it for low-level runtime-guard tests, `"--"` forwards
    argv flags untouched, and `--trace-lines` emits source line numbers on
    uncaught native runtime crashes, with identical C through both CLI doors.
  - `brat bindgen` extracts `#define`/enum constants (`pub const`),
    fn-ptr typedefs, fn-ptr struct fields, nested inline structs, and
    `--inline-arrays` fixed-size C struct array fields (`Elem[N]`), proven via
    `_Static_assert` + `brat ffi verify` and marshaled end-to-end across both
    native and `-O0` backends.
  - Iterative heap-stack runtime walkers (`vrelease`, `vprint_f`,
    `vsprint_len_q`, `vsprint_q`, `veq`, `vcmp_`, `_brepr`, `_eq_deep`, `_ncmp`)
    handle 150,000-deep drops under a 256 KB native stack limit with zero leaks
    and 1,500-deep values across both backends, plus a full ThreadSanitizer
    (`-fsanitize=thread`) concurrency regression suite across `spawn`/`scope`/
    `chan`, `par for`, SSO/heap string sharing, `BShape`/`BFieldIC`, and plugins.
"""
import _cxx
import _scratch
import os
import resource
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
CXX = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-safety-")

n_asserts = 0


def check(cond, msg):
    global n_asserts
    assert cond, msg
    n_asserts += 1


def write(name, content):
    p = os.path.join(TMP, name)
    with open(p, "w", encoding="utf-8") as f:
        f.write(content)
    return p


def run_cli(args, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    return subprocess.run(_cxx.cli_args(*(args)), capture_output=True, text=True, env=e, timeout=120)


def test_interp_gate_and_trace_lines():
    # 1. `brat run -O0` rejects type errors, move violations, and missing capabilities by default.
    bad_ty = write("bad_ty.brat", "fn add(a: i64, b: i64) -> i64 { return a + b }\nprint(add(1, \"x\"))\n")
    r_ty = run_cli(["run", "-O0", bad_ty])
    check(r_ty.returncode != 0 and "i64" in (r_ty.stdout + r_ty.stderr),
          "run_o0 gate must reject type mismatch: %r" % (r_ty.stdout + r_ty.stderr))

    bad_mv = write("bad_mv.brat",
                   "fn eat(move v: vec[i64]) -> i64 { return len(v) }\n"
                   "fn main() -> i64 {\n"
                   "  let xs = [1, 2, 3]\n"
                   "  let a = eat(move xs)\n"
                   "  print(xs)\n"
                   "  return a\n"
                   "}\n")
    r_mv = run_cli(["run", "-O0", bad_mv])
    check(r_mv.returncode != 0 and "moved" in (r_mv.stdout + r_mv.stderr),
          "run_o0 gate must reject use-after-move: %r" % (r_mv.stdout + r_mv.stderr))

    bad_cap = write("bad_cap.brat",
                    "fn add(a: i64, b: i64) -> i64 { return a + b }\n"
                    "fn main() -> i64 {\n"
                    "  let xs = [1, 2, 3]\n"
                    "  let s = par for x in xs reduce(add, 0) {\n"
                    "    print(x)\n"
                    "    return x\n"
                    "  }\n"
                    "  return s\n"
                    "}\n")
    r_cap = run_cli(["run", "-O0", bad_cap])
    check(r_cap.returncode != 0 and "par for" in (r_cap.stdout + r_cap.stderr),
          "run_o0 gate must reject impure par for effect violation: %r" % (r_cap.stdout + r_cap.stderr))

    bad_race = write("bad_race.brat",
                     "fn main() -> i64 {\n"
                     "  let mut shared = 0\n"
                     "  scope {\n"
                     "    spawn { shared = 1 }\n"
                     "  }\n"
                     "  return shared\n"
                     "}\n")
    r_race = run_cli(["run", "-O0", bad_race])
    check(r_race.returncode != 0 and "spawn" in (r_race.stdout + r_race.stderr),
          "run_o0 gate must reject spawn write race: %r" % (r_race.stdout + r_race.stderr))

    # 2. `--no-check` still enforces FFI type safety; flags after `--` pass through to `args()`.
    bad_ffi = write("bad_ffi.brat", "extern fn nope(x: u128) -> i64;\nprint(1)\n")
    r_gated = run_cli(["run", "-O0", bad_ffi])
    check(r_gated.returncode != 0 and "unsupported FFI type" in (r_gated.stdout + r_gated.stderr),
          "gated run should fail static FFI check")
    r_nocheck = run_cli(["run", "-O0", "--no-check", "--unsafe-ffi", bad_ffi])
    check(r_nocheck.returncode != 0 and "u128" in (r_nocheck.stdout + r_nocheck.stderr),
          "--no-check must still reject unsupported FFI type u128: %r" % (r_nocheck.stdout + r_nocheck.stderr))

    argv_src = write("argv_pass.brat", "print(args())\n")
    r_argv = run_cli(["run", "-O0", argv_src, "--", "--no-check", "--trace-lines"])
    check(r_argv.returncode == 0 and "'--no-check'" in r_argv.stdout and "'--trace-lines'" in r_argv.stdout,
          "flags after `--` must reach args(): %r" % r_argv.stdout)

    # 3. `--trace-lines` stamps `.brat` line numbers on uncaught native crashes
    #    and clears line state across caught `try/catch`.
    crash_src = write("trace_crash.brat",
                      "fn ok_catch() -> i64 {\n"
                      "  try {\n"
                      "    let z = 1 / 0\n"
                      "    return z\n"
                      "  } catch e {\n"
                      "    return 42\n"
                      "  }\n"
                      "}\n"
                      "fn boom(x: i64) -> i64 {\n"
                      "  let y = x + 1\n"
                      "  return y / 0\n"
                      "}\n"
                      "fn main() -> i64 {\n"
                      "  print(ok_catch())\n"
                      "  print(boom(10))\n"
                      "  return 0\n"
                      "}\n")
    cxx_c = subprocess.run([CXX, "emit-c", "--trace-lines", crash_src],
                           capture_output=True, text=True, check=True).stdout
    check("#define BRAT_TRACE_DEFAULT 1" in cxx_c,
          "--trace-lines must define BRAT_TRACE_DEFAULT 1 in emitted C")

    r_tr = run_cli(["run", "--trace-lines", crash_src])
    check(r_tr.returncode != 0 and r_tr.stdout == "42\n" and "div by zero" in r_tr.stderr
          and "at line 11" in r_tr.stderr,
          "--trace-lines must report crash at line 11: out=%r err=%r" % (r_tr.stdout, r_tr.stderr))

    r_notr = run_cli(["run", crash_src])
    check(r_notr.returncode != 0 and "at line " not in r_notr.stderr,
          "default build without --trace-lines must have zero line-trace suffix: %r" % r_notr.stderr)
    print("ok: -O0 gate + --trace-lines")


def test_bindgen_and_inline_arrays():
    hdr = write("pkt.h",
                "#define PKT_MAGIC 0x2A\n"
                "#define PKT_SHIFT (1 << 4)\n"
                "#define PKT_MASK (0xFF & ~0x0F)\n"
                "enum PktMode {\n"
                "  MODE_IDLE = 0,\n"
                "  MODE_FAST = 3,\n"
                "  MODE_ERR = -1\n"
                "};\n"
                "typedef int (*pkt_op_fn)(int, int);\n"
                "typedef struct {\n"
                "  int lo;\n"
                "  int hi;\n"
                "} Span;\n"
                "typedef struct {\n"
                "  int tag;\n"
                "  Span span;\n"
                "  int lanes[4];\n"
                "} Packet;\n"
                "typedef struct {\n"
                "  int version;\n"
                "  int (*combine)(int, int);\n"
                "} OpsTable;\n"
                "int pkt_sum(Packet p);\n"
                "Packet pkt_make(int base);\n"
                "int pkt_apply(pkt_op_fn f, int a, int b);\n")
    csrc = write("pkt.c",
                 "#include \"pkt.h\"\n"
                 "int pkt_sum(Packet p) {\n"
                 "  return p.tag + p.span.lo + p.span.hi + p.lanes[0] + p.lanes[1] + p.lanes[2] + p.lanes[3];\n"
                 "}\n"
                 "Packet pkt_make(int base) {\n"
                 "  Packet p;\n"
                 "  p.tag = PKT_MAGIC;\n"
                 "  p.span.lo = base;\n"
                 "  p.span.hi = base + 10;\n"
                 "  for (int i = 0; i < 4; i++) p.lanes[i] = base + i + 1;\n"
                 "  return p;\n"
                 "}\n"
                 "int pkt_apply(pkt_op_fn f, int a, int b) { return f(a, b); }\n")
    shlib = os.path.join(TMP, "libpkt.so")
    subprocess.run(["gcc", "-shared", "-fPIC", "-g", "-O2", csrc, "-o", shlib], check=True)

    # Default bindgen still skips inline array structs cleanly unless --inline-arrays is passed.
    bg_def = run_cli(["bindgen", hdr])
    check(bg_def.returncode == 0 and "inline array fields" in (bg_def.stdout + bg_def.stderr),
          "default bindgen must report skipped inline array struct: %r" % (bg_def.stdout + bg_def.stderr))

    # `--inline-arrays` emits `lanes: i32[4]`, `pub const` macros/enums, fn-ptr typedefs & struct fields, nested `Span`.
    gen_brat = os.path.join(TMP, "pkt_gen.brat")
    bg_arr = run_cli(["bindgen", "--inline-arrays", "--lib", shlib, hdr, "-o", gen_brat])
    check(bg_arr.returncode == 0, "bindgen --inline-arrays failed: %r" % (bg_arr.stdout + bg_arr.stderr))
    gen_txt = open(gen_brat, encoding="utf-8").read()
    for needle in ("pub const PKT_MAGIC = 42",
                   "pub const PKT_SHIFT = 16",
                   "pub const PKT_MASK = 240",
                   "pub const MODE_ERR = -1",
                   "lanes: i32[4]",
                   "span: Span",
                   "combine: fn(i32, i32) -> i32"):
        check(needle in gen_txt, "missing %r in generated bindings:\n%s" % (needle, gen_txt))

    ck = run_cli(["check", gen_brat])
    check(ck.returncode == 0, "generated pkt_gen.brat must check clean: %r" % (ck.stdout + ck.stderr))
    ck_cxx = subprocess.run([CXX, "check", gen_brat], capture_output=True, text=True)
    check(ck_cxx.returncode == 0, "brat-cxx check on pkt_gen.brat failed: %r" % (ck_cxx.stdout + ck_cxx.stderr))

    # `brat ffi` proves layout via target compiler probe (`--header`), via DWARF debug info (`shlib`),
    # and re-proves the built BRAT binary (`--header --bin exe`)!
    vf_probe = run_cli(["ffi", gen_brat, "--header", hdr])
    check(vf_probe.returncode == 0, "ffi --header failed: %r" % (vf_probe.stdout + vf_probe.stderr))
    vf_dwarf = run_cli(["ffi", gen_brat, "--object", shlib])
    check(vf_dwarf.returncode == 0, "ffi DWARF failed: %r" % (vf_dwarf.stdout + vf_dwarf.stderr))

    # End-to-end execution of the built binary and of `run -O0` (`Elem[N]`, a nested struct, a fn ptr).
    app = write("use_pkt.brat",
                "import pkt_gen\n"
                "fn mul_add(a: i32, b: i32) -> i32 { return a * b + 1 }\n"
                "fn main() -> i64 {\n"
                "  let p = pkt_make(10)\n"
                "  print(p.tag == PKT_MAGIC)\n"
                "  print(PKT_SHIFT)\n"
                "  print(PKT_MASK)\n"
                "  print(MODE_FAST + MODE_ERR)\n"
                "  print(p.span.lo)\n"
                "  print(p.span.hi)\n"
                "  print(p.lanes)\n"
                "  print(pkt_sum(p))\n"
                "  let p2 = {\"tag\": 5, \"span\": {\"lo\": 2, \"hi\": 3}, \"lanes\": [10, 20, 30, 40]}\n"
                "  print(pkt_sum(p2))\n"
                "  print(pkt_apply(\"mul_add\", 6, 7))\n"
                "  return 0\n"
                "}\n")
    exe = os.path.join(TMP, "use_pkt.exe")
    b = run_cli(["build", app, "--header", hdr, "-o", exe])
    check(b.returncode == 0, "native build with inline array FFI failed: %r" % (b.stdout + b.stderr))
    vf_bin = run_cli(["ffi", gen_brat, "--header", hdr, "--bin", exe])
    check(vf_bin.returncode == 0, "ffi --bin re-proof failed: %r" % (vf_bin.stdout + vf_bin.stderr))
    env_lib = dict(os.environ)
    env_lib["LD_LIBRARY_PATH"] = TMP
    rn = subprocess.run([exe], capture_output=True, text=True, env=env_lib, timeout=60)
    ri = run_cli(["run", "-O0", "--unsafe-ffi", app], env={"LD_LIBRARY_PATH": TMP})
    want = "true\n16\n240\n2\n10\n20\n[11, 12, 13, 14]\n122\n110\n43\n"
    check(rn.returncode == 0 and rn.stdout == want, "native inline-array FFI output mismatch: %r (err=%r)" % (rn.stdout, rn.stderr))
    check(ri.returncode == 0 and ri.stdout == want, "run_o0 inline-array FFI output mismatch: %r (err=%r)" % (ri.stdout, ri.stderr))
    print("ok: bindgen constants/fnptrs/nested structs + Elem[N] FFI")


def test_iterative_walkers_and_tsan():
    # 1. Iterative `vrelease`: build and drop a 150,000-deep nested container inside a function
    #    under a strict 256 KB RLIMIT_STACK + ASan/LSan (recursive vrelease would SIGSEGV immediately),
    #    plus test >14-char struct field names (`vmap_shaped` lock safety) and 1,500-deep values across both backends!
    deep_src = write("deep_iter.brat",
                     "fn drop_150k() -> i64 {\n"
                     "  let mut v = []\n"
                     "  let mut i = 0\n"
                     "  while i < 150000 {\n"
                     "    v = [v]\n"
                     "    i += 1\n"
                     "  }\n"
                     "  return 150000\n"
                     "}\n"
                     "fn main() -> i64 {\n"
                     "  print(drop_150k())\n"
                     "  let rec = {\"very_long_struct_field_alpha\": 19, \"very_long_struct_field_beta\": 23}\n"
                     "  print(rec.very_long_struct_field_alpha + rec.very_long_struct_field_beta)\n"
                     "  let mut a = []\n"
                     "  let mut b = []\n"
                     "  let mut k = 0\n"
                     "  while k < 1500 {\n"
                     "    a = [a]\n"
                     "    b = [b]\n"
                     "    k += 1\n"
                     "  }\n"
                     "  print(len(str(a)))\n"
                     "  print(a == b)\n"
                     "  print(a < b)\n"
                     "  return 0\n"
                     "}\n")
    deep_c = os.path.join(TMP, "deep_iter.c")
    deep_exe = os.path.join(TMP, "deep_iter.exe")
    deep_asan_exe = os.path.join(TMP, "deep_iter_asan.exe")
    em_deep = subprocess.run([CXX, "emit-c", deep_src], capture_output=True, text=True, check=True)
    with open(deep_c, "w", encoding="utf-8") as f:
        f.write(em_deep.stdout)
    subprocess.run(["gcc", "-std=c99", "-O2", "-I", ENGINE, deep_c, "-lpthread", "-ldl", "-lm", "-o", deep_exe], check=True)
    subprocess.run(["gcc", "-std=c99", "-O1", "-g", "-fsanitize=address,leak", "-I", ENGINE,
                    deep_c, "-lpthread", "-ldl", "-lm", "-o", deep_asan_exe], check=True)

    def limit_stack():
        resource.setrlimit(resource.RLIMIT_STACK, (256 * 1024, resource.RLIM_INFINITY))

    rn_deep = subprocess.run([deep_exe], capture_output=True, text=True, preexec_fn=limit_stack, timeout=60)
    rn_asan = subprocess.run([deep_asan_exe], capture_output=True, text=True, timeout=60)
    ri_deep = run_cli(["run", "-O0", deep_src])
    want_deep = "150000\n42\n3002\ntrue\nfalse\n"
    check(rn_deep.returncode == 0 and rn_deep.stdout == want_deep,
          "deep_iter native (256KB stack) failed: rc=%d out=%r err=%r"
          % (rn_deep.returncode, rn_deep.stdout, rn_deep.stderr))
    check(rn_asan.returncode == 0 and rn_asan.stdout == want_deep,
          "deep_iter ASan+LSan failed: rc=%d out=%r err=%r"
          % (rn_asan.returncode, rn_asan.stdout, rn_asan.stderr))
    check(ri_deep.returncode == 0 and ri_deep.stdout == want_deep,
          "deep_iter -O0 (depth=1500) failed: rc=%d out=%r err=%r"
          % (ri_deep.returncode, ri_deep.stdout, ri_deep.stderr))

    # 2. ThreadSanitizer (`-fsanitize=thread`) concurrency regression suite:
    #    Exercises concurrent `scope` + `spawn` + `chan`, SSO & heap strings, shaped structs & `BFieldIC`,
    #    `tagof`, `par for ... reduce`, and concurrent native C plugin calls!
    plug_c = write("tsan_plug.c",
                   "#include <stdint.h>\n"
                   "int64_t plug_mix(int64_t a, int64_t b) { return a * 3 + b; }\n")
    plug_so = os.path.join(TMP, "libtsanplug.so")
    subprocess.run(["gcc", "-shared", "-fPIC", "-O1", "-g", plug_c, "-o", plug_so], check=True)

    tsan_brat = write("tsan_suite.brat",
                      "fn add_i64(a: i64, b: i64) -> i64 { return a + b }\n"
                      "fn read_item(it: map[str, i64]) -> i64 {\n"
                      "  return it.short_k + it.very_long_concurrent_field_key\n"
                      "}\n"
                      "fn worker_calc(id: i64, sso_s: str, heap_s: str) -> i64 {\n"
                      "  let mut acc = 0\n"
                      "  let mut i = 0\n"
                      "  while i < 80 {\n"
                      "    let st = {\"short_k\": id + i, \"very_long_concurrent_field_key\": len(sso_s) + len(heap_s)}\n"
                      "    let opt = Some(read_item(st))\n"
                      "    if tagof(opt) == \"some\" {\n"
                      "      acc += opt?\n"
                      "    }\n"
                      "    i += 1\n"
                      "  }\n"
                      "  return acc\n"
                      "}\n"
                      "fn main() -> i64 {\n"
                      "  let h = lib_open(\"" + plug_so.replace("\\", "/") + "\")\n"
                      "  let sym = lib_sym(h, \"plug_mix\")\n"
                      "  let ch = chan(8)\n"
                      "  let sso_shared = \"hello_sso\"\n"
                      "  let heap_shared = \"heap_shared_string_longer_than_fourteen_bytes\"\n"
                      "  scope {\n"
                      "    spawn {\n"
                      "      let base = worker_calc(0, own(sso_shared), own(heap_shared))\n"
                      "      let mixed = call(sym, \"i64(i64,i64)\", base, 0)\n"
                      "      ch.send(mixed)\n"
                      "    }\n"
                      "    spawn {\n"
                      "      let base = worker_calc(1, own(sso_shared), own(heap_shared))\n"
                      "      let mixed = call(sym, \"i64(i64,i64)\", base, 1)\n"
                      "      ch.send(mixed)\n"
                      "    }\n"
                      "    spawn {\n"
                      "      let base = worker_calc(2, own(sso_shared), own(heap_shared))\n"
                      "      let mixed = call(sym, \"i64(i64,i64)\", base, 2)\n"
                      "      ch.send(mixed)\n"
                      "    }\n"
                      "    spawn {\n"
                      "      let base = worker_calc(3, own(sso_shared), own(heap_shared))\n"
                      "      let mixed = call(sym, \"i64(i64,i64)\", base, 3)\n"
                      "      ch.send(mixed)\n"
                      "    }\n"
                      "  }\n"
                      "  ch.close()\n"
                      "  let mut chan_sum = 0\n"
                      "  for _ in 0..4 {\n"
                      "    let r = ch.recv()\n"
                      "    chan_sum += r?\n"
                      "  }\n"
                      "  lib_close(h)\n"
                      "  let mut nums = []\n"
                      "  for x in 1..65 {\n"
                      "    nums.push(x)\n"
                      "  }\n"
                      "  let psum = par for x in nums reduce(add_i64, 0) { return x * x }\n"
                      "  print(chan_sum)\n"
                      "  print(psum)\n"
                      "  return 0\n"
                      "}\n")
    tsan_c = os.path.join(TMP, "tsan_suite.c")
    tsan_exe = os.path.join(TMP, "tsan_suite.exe")
    em = run_cli(["emit-c", "--unsafe-ffi", tsan_brat])
    check(em.returncode == 0, "tsan_suite emit-c failed: %r" % (em.stdout + em.stderr))
    with open(tsan_c, "w", encoding="utf-8") as f:
        f.write(em.stdout)
    cc_tsan = subprocess.run(
        ["gcc", "-std=c99", "-O1", "-g", "-fsanitize=thread", "-I", ENGINE,
         tsan_c, "-lpthread", "-ldl", "-lm", "-o", tsan_exe],
        capture_output=True, text=True, timeout=120)
    check(cc_tsan.returncode == 0, "TSan gcc compile failed: %r" % (cc_tsan.stdout + cc_tsan.stderr))

    env_tsan = dict(os.environ)
    env_tsan["BRAT_THREADS"] = "4"
    env_tsan["TSAN_OPTIONS"] = "halt_on_error=1:second_symbolicate=1"
    tsan_cmd = [tsan_exe]
    import platform as _plat, shutil as _shutil
    if sys.platform.startswith("linux") and _shutil.which("setarch"):
        tsan_cmd = ["setarch", _plat.machine(), "-R", tsan_exe]
    rn_tsan = subprocess.run(tsan_cmd, capture_output=True, text=True, env=env_tsan, timeout=120)
    ri_tsan = run_cli(["run", "-O0", "--unsafe-ffi", tsan_brat])
    check(rn_tsan.returncode == 0 and "ThreadSanitizer" not in rn_tsan.stderr,
          "TSan detected a race or failure: rc=%d out=%r err=%r"
          % (rn_tsan.returncode, rn_tsan.stdout, rn_tsan.stderr))
    check(rn_tsan.stdout == ri_tsan.stdout,
          "TSan native output != run_o0 output: %r vs %r" % (rn_tsan.stdout, ri_tsan.stdout))
    print("ok: iterative walkers + 256KB stack + TSan clean")


def main():
    test_interp_gate_and_trace_lines()
    test_bindgen_and_inline_arrays()
    test_iterative_walkers_and_tsan()
    print("SAFETY ALL GREEN (%d asserts)" % n_asserts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
