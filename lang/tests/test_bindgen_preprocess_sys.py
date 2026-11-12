"""Regression tests for `brat bindgen --preprocess` system-header filtering and `--header-only`."""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "lang", "tests"))
import _cxx  # noqa: E402

CXX = os.environ.get("BRAT_CXX") or _cxx.build()


def run_cxx(args):
    return subprocess.run([CXX] + list(args), capture_output=True, text=True)


def test_user_header_excludes_system_includes_and_resolves_posix_typedefs():
    with tempfile.TemporaryDirectory() as td:
        hdr = os.path.join(td, "service_api.h")
        with open(hdr, "w", encoding="utf-8") as f:
            f.write(
                "#include <stdio.h>\n"
                "#include <stdint.h>\n"
                "#include <unistd.h>\n"
                "#include <sys/types.h>\n"
                "\n"
                "#define SERVICE_PORT 8080\n"
                "#define SERVICE_FLAGS (1 << 4)\n"
                "enum ServiceStatus { SVC_OK = 0, SVC_BUSY = 2 };\n"
                "struct ServiceItem { uint32_t id; int64_t quota; };\n"
                "int svc_open(const char *path, mode_t mode, pid_t owner, off_t offset);\n"
            )
        r = run_cxx(["bindgen", "--preprocess", hdr])
        assert r.returncode == 0, r.stderr
        out = r.stdout
        assert "pub const SERVICE_PORT = 8080" in out, out
        assert "pub const SERVICE_FLAGS = 16" in out, out
        assert "pub const SVC_OK = 0" in out, out
        assert "pub const SVC_BUSY = 2" in out, out
        assert "extern struct ServiceItem { id: u32, quota: i64 }" in out, out
        assert "extern fn svc_open(path: str, mode: u32, owner: i32, offset: i64) -> i32;" in out, out
        # System headers (<stdio.h>, <unistd.h>, <stdc-predef.h>) must not leak into user bindings:
        for leaked in ("printf", "vfork", "unlink", "_STDC_PREDEF_H", "__GLIBC_"):
            assert leaked not in out, f"unexpected leak {leaked!r} in:\n{out}"

        mod = os.path.join(td, "check_mod.brat")
        with open(mod, "w", encoding="utf-8") as f:
            f.write(out + "\nfn main() -> i64 { return SERVICE_PORT - 8080 }\n")
        chk = run_cxx(["check", mod])
        assert chk.returncode == 0, chk.stdout + chk.stderr


def test_header_only_keeps_sub_typedefs_without_sub_functions():
    with tempfile.TemporaryDirectory() as td:
        sub = os.path.join(td, "internal_types.h")
        pub = os.path.join(td, "public_api.h")
        with open(sub, "w", encoding="utf-8") as f:
            f.write(
                "#define INTERNAL_SECRET 999\n"
                "typedef unsigned int handle_id_t;\n"
                "struct InternalCtx { handle_id_t hid; int flags; };\n"
                "int internal_helper_do_not_export(int x);\n"
            )
        with open(pub, "w", encoding="utf-8") as f:
            f.write(
                '#include "internal_types.h"\n'
                "#define PUBLIC_VERSION 3\n"
                "int api_create(struct InternalCtx *ctx, handle_id_t id);\n"
            )
        for extra_flags in (["--header-only"], ["--preprocess", "--header-only"]):
            r = run_cxx(["bindgen"] + extra_flags + [pub])
            assert r.returncode == 0, r.stderr
            out = r.stdout
            assert "pub const PUBLIC_VERSION = 3" in out, out
            assert "INTERNAL_SECRET" not in out, out
            assert "internal_helper_do_not_export" not in out, out
            assert "extern struct InternalCtx { hid: u32, flags: i32 }" in out, out
            assert "extern fn api_create(ctx: ptr[InternalCtx], id: u32) -> i32;" in out, out


def test_direct_system_header_preprocess_when_available():
    poll_h = "/usr/include/poll.h"
    if not os.path.isfile(poll_h):
        return
    r = run_cxx(["bindgen", "--preprocess", poll_h])
    assert r.returncode == 0, r.stderr
    out = r.stdout
    assert "pub const POLLIN =" in out, out
    assert "extern struct pollfd" in out, out
    assert "extern fn poll(" in out, out
    assert "_STDC_PREDEF_H" not in out, out
    assert "__GLIBC_" not in out, out

    with tempfile.TemporaryDirectory() as td:
        mod = os.path.join(td, "poll_mod.brat")
        with open(mod, "w", encoding="utf-8") as f:
            f.write(out + "\nfn main() -> i64 { return 0 }\n")
        chk = run_cxx(["check", mod])
        assert chk.returncode == 0, chk.stdout + chk.stderr


def test_preprocess_with_closed_standard_fds():
    if os.name == "nt":
        return
    with tempfile.TemporaryDirectory() as td:
        hdr = os.path.join(td, "api.h")
        with open(hdr, "w", encoding="utf-8") as f:
            f.write("int add_one(int value);\n")
        for fds in ((0,), (1,), (2,), (0, 1), (0, 2), (1, 2), (0, 1, 2)):
            out = os.path.join(td, "bindings_" + "".join(map(str, fds)) + ".brat")

            def close_streams():
                for fd in fds:
                    try:
                        os.close(fd)
                    except OSError:
                        pass

            r = subprocess.run(
                [CXX, "bindgen", "--preprocess", hdr, "-o", out],
                capture_output=True, text=True, preexec_fn=close_streams, timeout=40,
            )
            assert r.returncode == 0, f"closed {fds}: {r.stderr}"
            with open(out, encoding="utf-8") as f:
                assert "extern fn add_one(value: i32) -> i32;" in f.read(), fds


if __name__ == "__main__":
    test_user_header_excludes_system_includes_and_resolves_posix_typedefs()
    test_header_only_keeps_sub_typedefs_without_sub_functions()
    test_direct_system_header_preprocess_when_available()
    test_preprocess_with_closed_standard_fds()
    print("BINDGEN PREPROCESS SYS ALL GREEN")
