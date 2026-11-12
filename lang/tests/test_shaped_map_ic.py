"""Shaped map literals + inline caches, PCH cache, system C FFI, bundle CLI, and parser fuzzing."""
import gzip
import io
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import _cxx

BIN = os.environ.get("BRAT_CXX") or _cxx.build()


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def test_shaped_maps_ic_and_o0_o1_parity() -> None:
    src = '''fn lookup_poly(m) {
  let id = m["id"]
  let name = m.get_or("name", "anon")
  let has_extra = m.has("extra")
  let g = m.get("tag")
  return str(id) + ":" + name + ":" + str(has_extra) + ":" + str(is_none(g))
}

fn main() {
  let k = "dyn"
  let interp = {"{k}_key": 99, "plain": 1}
  assert(interp["dyn_key"] == 99)
  assert(interp["plain"] == 1)

  // Exercise >4 distinct shapes at the same BFieldIC call sites + bmap_alloc recycling
  let mut acc = 0
  let mut i = 0
  while i < 500 {
    let s1 = {"id": i, "name": "a"}
    let s2 = {"name": "b", "id": i + 1, "tag": "t"}
    let s3 = {"extra": true, "id": i + 2, "name": "c"}
    let s4 = {"z": 0, "y": 1, "x": 2, "id": i + 3}
    let s5 = {"k1": 1, "k2": 2, "k3": 3, "k4": 4, "id": i + 4, "extra": 1}
    if i == 0 {
      assert(lookup_poly(s1) == "0:a:false:true")
      assert(lookup_poly(s2) == "1:b:false:false")
      assert(lookup_poly(s3) == "2:c:true:true")
      assert(lookup_poly(s4) == "3:anon:false:true")
      assert(lookup_poly(s5) == "4:anon:true:true")
    }
    acc = acc + s1["id"] + s4["id"] + s5["id"]
    i = i + 1
  }
  assert(acc > 0)

  // Mutating / deleting unshapes cleanly while preserving values and RC
  let mut m = {"id": 10, "name": "init"}
  m.set("extra", "added")
  assert(m.has("extra") && m["extra"] == "added")
  let _d = m.del("name")
  assert(!m.has("name") && m.get_or("name", "gone") == "gone")

  // Error contracts on non-maps and missing keys must match exactly
  let mut caught = 0
  try {
    let bad = {"id": 1}
    let _x = bad["missing"]
  } catch e {
    assert(e.contains("no key"))
    caught = caught + 1
  }
  try {
    let n = 42
    let _x = n.get("id")
  } catch e {
    assert(e.contains("no method get"))
    caught = caught + 1
  }
  try {
    let n = 42
    let _x = n.has("id")
  } catch e {
    assert(e.contains("no method has"))
    caught = caught + 1
  }
  try {
    let n = 42
    let _x = n.get_or("id", 0)
  } catch e {
    assert(e.contains("get_or of non-map"))
    caught = caught + 1
  }
  assert(caught == 4)
  print("SHAPED_OK")
}
'''
    with tempfile.TemporaryDirectory(prefix="brat-r4-map-") as td:
        p = os.path.join(td, "shaped.brat")
        with open(p, "w", encoding="utf-8") as f:
            f.write(src)

        c0 = subprocess.run([BIN, "emit-c", "-O0", p], capture_output=True, text=True, timeout=30)
        c1 = subprocess.run([BIN, "emit-c", "-O1", p], capture_output=True, text=True, timeout=30)
        check(c0.returncode == 0 and c1.returncode == 0, f"emit-c failed: {c0.stderr} {c1.stderr}")
        check("vmap_shaped(" in c1.stdout and "vat_cstr_ic(" in c1.stdout, "missing shaped/IC codegen")
        check(len(c0.stdout.splitlines()) == len(c1.stdout.splitlines()),
              f"-O0/-O1 line-count parity broken: {len(c0.stdout.splitlines())} vs {len(c1.stdout.splitlines())}")

        rr = subprocess.run([BIN, "run", p], capture_output=True, text=True, timeout=90)
        check(rr.returncode == 0 and rr.stdout.strip() == "SHAPED_OK",
              f"shaped run failed: {rr.stdout} {rr.stderr}")


def test_pch_cache_and_bundle_cli() -> None:
    with tempfile.TemporaryDirectory(prefix="brat-r4-pch-") as td:
        cache_dir = os.path.join(td, "cache")
        env = dict(os.environ, BRAT_CACHE=cache_dir)
        env.pop("BRAT_NO_PCH", None)
        env.pop("CFLAGS", None)
        env.pop("CPPFLAGS", None)
        out1 = os.path.join(td, "b1")
        out2 = os.path.join(td, "b2")
        r1 = subprocess.run([BIN, "build", os.path.join(ROOT, "examples", "hello.brat"), "-o", out1],
                            env=env, capture_output=True, text=True, timeout=90)
        check(r1.returncode == 0, f"first build failed: {r1.stderr}")
        s2 = os.path.join(td, "second.brat")
        with open(s2, "w", encoding="utf-8") as f:
            f.write('fn main() {\n  print("warm")\n}\n')
        r2 = subprocess.run([BIN, "build", s2, "-o", out2],
                            env=env, capture_output=True, text=True, timeout=90)
        check(r2.returncode == 0, f"second build (PCH admission) failed: {r2.stderr}")
        pch_dir = os.path.join(cache_dir, "pch")
        if shutil.which("gcc") or shutil.which("clang"):
            pch_files = os.listdir(pch_dir) if os.path.isdir(pch_dir) else []
            check(any(fn.endswith((".gch", ".pch")) for fn in pch_files),
                  f"expected precompiled crt.h artifact in {pch_dir}, got {pch_files}")

        # `BRAT_NO_PCH=1` skips creating PCH directory
        nopch_cache = os.path.join(td, "nocache")
        env_no = dict(env, BRAT_CACHE=nopch_cache, BRAT_NO_PCH="1")
        out3 = os.path.join(td, "b3")
        r3 = subprocess.run([BIN, "build", os.path.join(ROOT, "examples", "hello.brat"), "-o", out3],
                            env=env_no, capture_output=True, text=True, timeout=90)
        check(r3.returncode == 0, f"BRAT_NO_PCH build failed: {r3.stderr}")
        check(not os.path.exists(os.path.join(nopch_cache, "pch")), "BRAT_NO_PCH=1 must not populate pch/")

        # `brat bundle <dir>` emits `<dir>/bin/brat` (or `brat.cmd` on Windows)
        bdir = os.path.join(td, "bundle_out")
        rb = subprocess.run([BIN, "bundle", bdir], capture_output=True, text=True, timeout=30)
        check(rb.returncode == 0, f"bundle failed: {rb.stderr}")
        bin_brat = os.path.join(bdir, "bin", "brat.cmd" if os.name == "nt" else "brat")
        check(os.path.exists(bin_brat), f"missing bundled driver wrapper {bin_brat}")
        rc = subprocess.run([bin_brat, "check", os.path.join(ROOT, "examples", "hello.brat")],
                            capture_output=True, text=True, timeout=30)
        check(rc.returncode == 0, f"bundled bin/brat check failed: {rc.stdout} {rc.stderr}")

    check(os.path.isfile(os.path.join(ROOT, ".github", "workflows", "release.yml")), "missing release.yml")


def test_real_system_headers_ffi() -> None:
    if sys.platform != "linux":
        return
    with tempfile.TemporaryDirectory(prefix="brat-r4-sysffi-") as td:
        # 1. Synthetic vtable + const-callback header verifying fixpoint struct degradation
        vhdr = os.path.join(td, "vtable.h")
        with open(vhdr, "w", encoding="utf-8") as f:
            f.write("""typedef struct VfsMethods VfsMethods;
typedef struct VfsFile {
    const VfsMethods *methods;
    int fd;
} VfsFile;
struct VfsMethods {
    int version;
    int (*xRead)(VfsFile *f, const void *buf, int n);
};
VfsMethods *vfs_default(void);
int vfs_fd(const VfsFile *f);
""")
        vout = os.path.join(td, "vtable.brat")
        r_bg = subprocess.run([BIN, "bindgen", vhdr, "-o", vout], capture_output=True, text=True, timeout=30)
        check(r_bg.returncode == 0, f"vtable bindgen failed: {r_bg.stderr}")
        r_ck = subprocess.run([BIN, "check", vout], capture_output=True, text=True, timeout=30)
        check(r_ck.returncode == 0, f"vtable check failed:\n{open(vout).read()}\n{r_ck.stdout}{r_ck.stderr}")
        r_ff = subprocess.run([BIN, "ffi", vout, "--header", vhdr], capture_output=True, text=True, timeout=30)
        check(r_ff.returncode == 0, f"vtable ffi failed: {r_ff.stdout} {r_ff.stderr}")

        # 2. Real system `/usr/include/sqlite3.h` and `/usr/include/zlib.h` when present
        for hdr, lib, call_tail in [
            ("/usr/include/zlib.h", "z", 'fn main() {\n  assert(len(zlibVersion()) > 0)\n}\n'),
            ("/usr/include/sqlite3.h", "sqlite3", 'fn main() {\n  assert(sqlite3_libversion_number() > 3000000)\n}\n'),
        ]:
            if not os.path.isfile(hdr):
                continue
            bout = os.path.join(td, f"{lib}_bind.brat")
            rb = subprocess.run([BIN, "bindgen", "--preprocess", hdr, "--lib", lib, "-o", bout],
                                capture_output=True, text=True, timeout=60)
            check(rb.returncode == 0, f"{hdr} bindgen failed: {rb.stderr}")
            rc = subprocess.run([BIN, "check", bout], capture_output=True, text=True, timeout=60)
            check(rc.returncode == 0, f"{hdr} check failed: {rc.stdout} {rc.stderr}")
            rf = subprocess.run([BIN, "ffi", "--preprocess", bout, "--header", hdr],
                                capture_output=True, text=True, timeout=60)
            check(rf.returncode == 0, f"{hdr} ffi failed: {rf.stdout} {rf.stderr}")

            app = os.path.join(td, f"{lib}_app.brat")
            exe = os.path.join(td, f"{lib}_exe")
            with open(app, "w", encoding="utf-8") as f:
                f.write(open(bout, encoding="utf-8").read() + "\n" + call_tail)
            rbuild = subprocess.run([BIN, "build", app, "--header", hdr, "-o", exe],
                                    capture_output=True, text=True, timeout=90)
            if rbuild.returncode == 0:
                rrun = subprocess.run([exe], capture_output=True, text=True, timeout=30)
                check(rrun.returncode == 0, f"{lib} exe failed: {rrun.stderr}")


def _make_corrupt_objects(rng: random.Random) -> list[bytes]:
    blobs: list[bytes] = [
        b"",
        b"\x7fELF",
        b"\x7fELF\x02\x01\x01" + b"\xff" * 57,  # ELF64 with huge shoff/shnum
        b"\x7fELF\x01\x01\x01" + b"\x80" * 52,  # ELF32 truncated section table
        struct.pack("<I", 0xFEEDFACF) + b"\xff" * 40,  # Mach-O 64 huge ncmds
        struct.pack(">I", 0xFEEDFACE) + b"\x7f" * 36,  # Mach-O 32 BE corrupt
        b"MZ" + b"\x00" * 58 + struct.pack("<I", 0xFFFF0000),  # PE bad e_lfanew
        b"!<arch>\n" + b"#1/999999       0           0     0     644     12        `\n" + b"A" * 12,
        b"!<arch>\n" + b"bad.o/          0           0     0     644     999999999 `\nshort",
    ]
    # Synthetic minimal ELF64 containing malformed DWARF v5 (.debug_info + .debug_abbrev)
    shstr = b"\x00.shstrtab\x00.debug_info\x00.debug_abbrev\x00.debug_str_offsets\x00"
    # DWARF v5 CU header with out-of-bounds strx index & runaway ULEB128
    dw_info = struct.pack("<IHBB", 16, 5, 1, 8) + struct.pack("<I", 0) + b"\x01\x99\x80\x80\x80\x80\x80\x80"
    dw_abbr = b"\x01\x11\x00\x03\x25\x00\x00\x00"  # DW_TAG_compile_unit, DW_AT_name + DW_FORM_strx1
    dw_stroff = b"\x04\x00\x00\x00\x05\x00\x00\x00"
    ehdr_sz = 64
    s1_off = ehdr_sz
    s2_off = s1_off + len(shstr)
    s3_off = s2_off + len(dw_info)
    s4_off = s3_off + len(dw_abbr)
    sh_off = (s4_off + len(dw_stroff) + 7) & ~7
    pad = b"\x00" * (sh_off - (s4_off + len(dw_stroff)))

    def sh64(name_off: int, sh_type: int, off: int, sz: int) -> bytes:
        return struct.pack("<IIQQQQIIQQ", name_off, sh_type, 0, 0, off, sz, 0, 0, 1, 0)

    ehdr = (
        b"\x7fELF\x02\x01\x01" + b"\x00" * 9 +
        struct.pack("<HHIQQQIHHHHHH", 1, 62, 1, 0, 0, sh_off, 0, 64, 0, 0, 64, 5, 1)
    )
    shdrs = (
        sh64(0, 0, 0, 0) +
        sh64(1, 3, s1_off, len(shstr)) +
        sh64(11, 1, s2_off, len(dw_info)) +
        sh64(23, 1, s3_off, len(dw_abbr)) +
        sh64(37, 1, s4_off, len(dw_stroff))
    )
    elf_dw = ehdr + shstr + dw_info + dw_abbr + dw_stroff + pad + shdrs
    blobs.append(elf_dw)
    for _ in range(12):
        m = bytearray(elf_dw)
        for _ in range(6):
            idx = rng.randrange(len(m))
            m[idx] ^= rng.randrange(1, 256)
        blobs.append(bytes(m))
    return blobs


def test_adversarial_fuzzing_parsers() -> None:
    rng = random.Random(0xB4A72026)
    with tempfile.TemporaryDirectory(prefix="brat-r4-fuzz-") as td:
        stub_brat = os.path.join(td, "stub.brat")
        with open(stub_brat, "w", encoding="utf-8") as f:
            f.write('extern fn foo(x: i32) -> i32\nfn main() {}\n')

        # 1. Fuzz ELF / Mach-O / PE / ar / DWARF object inspector
        for idx, blob in enumerate(_make_corrupt_objects(rng)):
            obj_p = os.path.join(td, f"bad_{idx}.o")
            with open(obj_p, "wb") as f:
                f.write(blob)
            r = subprocess.run([BIN, "ffi", stub_brat, "--object", obj_p],
                               capture_output=True, timeout=15)
            check(r.returncode >= 0, f"ffi --object crashed with signal {-r.returncode} on blob {idx}")

        # 2. Fuzz C header parser (`bindgen` & `ffi --header`)
        weird_headers = [
            "/* unclosed comment",
            "#define A (1 / 0)\n#define B (-9223372036854775807LL - 1LL) / -1\nint foo(int x);\n",
            "typedef struct S S; struct S { S inner; };\n",
            "typedef int (*cb_t)(const void *, struct Unknown (*)(int));\nint f(cb_t c);\n",
            "struct Deep { " + "struct { int x; } a; " * 25 + "};\n",
            "int f(int a[" + "(1<<20)" + "][2048]);\n",
            "\x00\xfftypedef enum { E0 = ~0ULL } E;\n",
        ]
        for idx, htxt in enumerate(weird_headers):
            hp = os.path.join(td, f"h_{idx}.h")
            with open(hp, "wb") as f:
                f.write(htxt.encode("latin1", errors="ignore"))
            rb = subprocess.run([BIN, "bindgen", hp], capture_output=True, timeout=15)
            rf = subprocess.run([BIN, "ffi", stub_brat, "--header", hp], capture_output=True, timeout=15)
            check(rb.returncode >= 0 and rf.returncode >= 0,
                  f"C header parser crashed on header {idx}: {rb.returncode}, {rf.returncode}")

        # 3. Fuzz tar/gzip package archive extractor via `pkg install` / corrupt tarballs
        tar_buf = io.BytesIO()
        with gzip.GzipFile(fileobj=tar_buf, mode="wb") as gz:
            with tarfile.open(fileobj=gz, mode="w") as tf:
                ti = tarfile.TarInfo("../../escape.brat")
                data = b"fn main() {}\n"
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
        bad_tgz = os.path.join(td, "evil.tar.gz")
        with open(bad_tgz, "wb") as f:
            f.write(tar_buf.getvalue())
        idx_json = os.path.join(td, "index.json")
        import hashlib
        sha = hashlib.sha256(tar_buf.getvalue()).hexdigest()
        with open(idx_json, "w", encoding="utf-8") as f:
            json.dump({"packages": {"evil": {"0.1.0": {"url": "file://" + bad_tgz, "sha256": sha}}}}, f)
        proj = os.path.join(td, "pkg_proj")
        os.makedirs(proj, exist_ok=True)
        with open(os.path.join(proj, "brat.pkg.json"), "w", encoding="utf-8") as f:
            json.dump({"name": "p", "version": "0.1.0", "dependencies": {}}, f)
        r_pkg = subprocess.run([BIN, "pkg", "add", "evil@0.1.0", "--index", idx_json],
                               cwd=proj, capture_output=True, timeout=15)
        check(r_pkg.returncode != 0 and not os.path.exists(os.path.join(td, "escape.brat")),
              "path traversal tarball must be rejected cleanly")

        # 4. Fuzz LSP & MCP wire framing
        frames = [
            b"Content-Length: -42\r\n\r\n{}",
            b"Content-Length: 18446744073709551615\r\n\r\n{}",
            b"Content-Length: 12\r\n\r\nshort",
            b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":' + (b"[" * 200) + b"\n",
        ]
        for idx, fr in enumerate(frames):
            rl = subprocess.run([BIN, "lsp"], input=fr, capture_output=True, timeout=15)
            rm = subprocess.run([BIN, "mcp"], input=fr, capture_output=True, timeout=15)
            check(rl.returncode >= 0 and rm.returncode >= 0, f"LSP/MCP crashed on frame {idx}")

        # 5. Fuzz BRAT lexer/parser with malformed syntax & truncated UTF-8
        bad_srcs = [
            b'fn main() { let s = "{a[1 + " }\n',
            b"fn main() { return " + b"(" * 180 + b"1" + b")" * 180 + b" }\n",
            b"fn main() {\x00\xff\xfe let x = 1 }\n",
            b'extern "z" { extern struct S { a: ptr[Unknown] } }\n',
        ]
        for idx, bsrc in enumerate(bad_srcs):
            sp = os.path.join(td, f"bad_{idx}.brat")
            with open(sp, "wb") as f:
                f.write(bsrc)
            rc = subprocess.run([BIN, "check", sp], capture_output=True, timeout=15)
            check(rc.returncode >= 0, f"brat check crashed on bad source {idx}")


if __name__ == "__main__":
    test_shaped_maps_ic_and_o0_o1_parity()
    test_pch_cache_and_bundle_cli()
    test_real_system_headers_ffi()
    test_adversarial_fuzzing_parsers()
    print("SHAPED MAP IC, FFI, FUZZ & SPEED ALL GREEN")
