"""Ownership soundness across `defer`, `try`/`catch`, early `return`, and `raise`.

Covers:
  1. `defer` reading a raw/borrowed pointer extends borrow liveness to block exit
     and rejects freeing/moving the pointer before the deferred read runs.
  2. `defer free(p)` / `defer free(move p)` allows ordinary uses in the block,
     consumes the owned pointer at block/return exit, and rejects an explicit
     free on any path (including early `return`).
  3. `try`/`catch` propagates moves executed inside `try` into `catch` (an error
     after a move reaches `catch` with that binding already consumed), while
     respecting `always_exits` on `try`/`catch` arms so error-path cleanup that
     returns does not poison fallthrough cleanup.
  4. Owned pointers (`-> move ptr`) are checked on early `return` (no silent leak)
     and not falsely reported at unreachable block bottoms when every branch
     frees and returns.
  5. Uncaught `raise(...)` / `?` from scalar functions marks the callee in
     `heap_returns` so discarded `Err` tags are released without leaking.
  6. Under `--no-check`, passing a moved binding to an FFI call or moving it
     twice raises `use of moved value` rather than an FFI type-tag error.
"""
import _cxx
import os
import shutil
import subprocess
import sys

import _scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CXX = os.path.join(ROOT, "lang", "cxx", "bin", "brat-cxx")
ENGINE = os.path.join(ROOT, "lang", "engine")
TMP = _scratch.mkdtemp(prefix="brat-defer-try-")

DECLS = (
    "extern fn malloc(size: i64) -> move ptr\n"
    "extern fn free(move p: ptr)\n"
)

CHECK_CASES = [
    (
        "defer_raw_ptr_read_after_free",
        DECLS + (
            "fn main() -> i64 {\n"
            "  unsafe {\n"
            "    let p = malloc(16)\n"
            "    defer ptr_write_i64(p, 99)\n"
            "    free(move p)\n"
            "  }\n"
            "  return 0\n"
            "}\n"
        ),
        1,
        "use of moved `p`",
    ),
    (
        "defer_borrowed_ptr_extends_nll",
        (
            "fn main() -> i64 {\n"
            "  unsafe {\n"
            "    let s = \"hello\"\n"
            "    let p = ptr_of_str(s)\n"
            "    defer print(ptr_is_null(p))\n"
            "    let t = move s\n"
            "    print(t)\n"
            "  }\n"
            "  return 0\n"
            "}\n"
        ),
        1,
        "cannot move `s` while borrowed by `p`",
    ),
    (
        "defer_free_bare_allows_body_use",
        DECLS + (
            "fn main() -> i64 {\n"
            "  unsafe {\n"
            "    let p = malloc(16)\n"
            "    defer free(p)\n"
            "    ptr_write_i64(p, 42)\n"
            "    print(ptr_read_i64(p))\n"
            "    return 0\n"
            "  }\n"
            "}\n"
        ),
        0,
        None,
    ),
    (
        "defer_free_catches_early_return_double_free",
        DECLS + (
            "fn run(c: bool) -> i64 {\n"
            "  unsafe {\n"
            "    let p = malloc(16)\n"
            "    defer free(move p)\n"
            "    if c {\n"
            "      free(move p)\n"
            "      return 1\n"
            "    }\n"
            "    return 0\n"
            "  }\n"
            "}\n"
            "fn main() -> i64 { return run(true) }\n"
        ),
        1,
        "use of moved `p`",
    ),
    (
        "early_return_owned_ptr_leak",
        DECLS + (
            "fn run(c: bool) -> i64 {\n"
            "  let p = malloc(16)\n"
            "  if c {\n"
            "    return 1\n"
            "  }\n"
            "  free(move p)\n"
            "  return 0\n"
            "}\n"
            "fn main() -> i64 { return run(true) }\n"
        ),
        1,
        "leaked owned pointer `p`",
    ),
    (
        "both_branches_free_and_return_ok",
        DECLS + (
            "fn run(c: bool) -> i64 {\n"
            "  let p = malloc(16)\n"
            "  if c {\n"
            "    free(move p)\n"
            "    return 1\n"
            "  } else {\n"
            "    free(move p)\n"
            "    return 0\n"
            "  }\n"
            "}\n"
            "fn main() -> i64 { return run(true) }\n"
        ),
        0,
        None,
    ),
    (
        "try_move_used_in_catch",
        DECLS + (
            "fn boom() -> i64 {\n"
            "  raise(\"fail\")\n"
            "  return 0\n"
            "}\n"
            "fn main() -> i64 {\n"
            "  let p = malloc(16)\n"
            "  try {\n"
            "    free(move p)\n"
            "    let _x = boom()?\n"
            "  } catch e {\n"
            "    free(move p)\n"
            "  }\n"
            "  return 0\n"
            "}\n"
        ),
        1,
        "use of moved `p`",
    ),
    (
        "try_move_return_catch_fallthrough_used_after",
        (
            "fn boom() -> i64 {\n"
            "  raise(\"fail\")\n"
            "  return 0\n"
            "}\n"
            "fn main() -> i64 {\n"
            "  let v = [1, 2, 3]\n"
            "  try {\n"
            "    let w = move v\n"
            "    print(len(w))\n"
            "    let _x = boom()?\n"
            "    return 1\n"
            "  } catch e {\n"
            "    print(e)\n"
            "  }\n"
            "  print(len(v))\n"
            "  return 0\n"
            "}\n"
        ),
        1,
        "use of moved `v`",
    ),
    (
        "try_clean_catch_frees_and_returns_ok",
        DECLS + (
            "fn boom(c: bool) -> i64 {\n"
            "  if c { raise(\"fail\") }\n"
            "  return 7\n"
            "}\n"
            "fn run(c: bool) -> i64 {\n"
            "  let p = malloc(16)\n"
            "  try {\n"
            "    let _x = boom(c)?\n"
            "  } catch e {\n"
            "    free(move p)\n"
            "    return -1\n"
            "  }\n"
            "  free(move p)\n"
            "  return 0\n"
            "}\n"
            "fn main() -> i64 { return run(false) }\n"
        ),
        0,
        None,
    ),
    (
        "try_both_arms_return_ok",
        (
            "fn boom() -> i64 {\n"
            "  raise(\"fail\")\n"
            "  return 0\n"
            "}\n"
            "fn run() -> i64 {\n"
            "  let v = [1, 2]\n"
            "  try {\n"
            "    let w = move v\n"
            "    let _x = boom()?\n"
            "    return len(w)\n"
            "  } catch e {\n"
            "    return 0\n"
            "  }\n"
            "}\n"
            "fn main() -> i64 { return run() }\n"
        ),
        0,
        None,
    ),
]


def run_check(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=60)


for name, src, want_rc, want_frag in CHECK_CASES:
    path = os.path.join(TMP, f"{name}.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    r_cxx = run_check([CXX, "check", path])
    r_py = run_check(_cxx.cli_args("check", "--py", path))
    out_cxx = r_cxx.stdout + r_cxx.stderr
    out_py = r_py.stdout + r_py.stderr
    assert r_cxx.returncode == want_rc, (
        f"{name} (cxx): want rc={want_rc}, got {r_cxx.returncode}:\n{out_cxx}"
    )
    assert r_py.returncode == want_rc, (
        f"{name} (py): want rc={want_rc}, got {r_py.returncode}:\n{out_py}"
    )
    if want_frag is not None:
        assert want_frag in out_cxx, f"{name} (cxx): missing {want_frag!r} in:\n{out_cxx}"
        assert want_frag in out_py, f"{name} (py): missing {want_frag!r} in:\n{out_py}"


# Runtime 1: uncaught `raise(...)` from a scalar function must not leak `Err` tags.
raise_src = (
    "fn may_fail(x: i64) -> i64 {\n"
    "  if x > 0 {\n"
    "    raise(\"boom\")\n"
    "  }\n"
    "  return 0\n"
    "}\n"
    "fn propagate(x: i64) -> i64 {\n"
    "  return may_fail(x)?\n"
    "}\n"
    "fn main() -> i64 {\n"
    "  let mut caught = 0\n"
    "  for i in 0..200 {\n"
    "    may_fail(1)\n"
    "    let r = may_fail(1)\n"
    "    if is_err(r) { caught = caught + 1 }\n"
    "    try {\n"
    "      let _v = propagate(1)?\n"
    "    } catch e {\n"
    "      caught = caught + 1\n"
    "    }\n"
    "  }\n"
    "  print(caught)\n"
    "  return 0\n"
    "}\n"
)
raise_brat = os.path.join(TMP, "raise_noleak.brat")
raise_c = os.path.join(TMP, "raise_noleak.c")
with open(raise_brat, "w", encoding="utf-8") as f:
    f.write(raise_src)
emit_r = subprocess.run([CXX, "emit-c", raise_brat], capture_output=True, text=True, check=True)
with open(raise_c, "w", encoding="utf-8") as f:
    f.write(emit_r.stdout)

cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
assert cc is not None, "no C compiler found"
exe_asan = os.path.join(TMP, "raise_noleak_asan")
cc_asan = subprocess.run(
    [cc, "-std=c99", "-O1", "-g", "-fsanitize=address,undefined", "-I", ENGINE,
     raise_c, "-lm", "-lpthread", "-ldl", "-o", exe_asan],
    capture_output=True, text=True,
)
if cc_asan.returncode == 0:
    lsan = "0" if sys.platform == "darwin" else "1"
    env = dict(os.environ, ASAN_OPTIONS=f"detect_leaks={lsan}:halt_on_error=1")
    rr = subprocess.run([exe_asan], capture_output=True, text=True, env=env, timeout=60)
    assert rr.returncode == 0 and rr.stdout == "400\n", (
        f"raise_noleak ASan/LSan failed (rc={rr.returncode}):\n{rr.stdout}{rr.stderr}"
    )
else:
    exe_plain = os.path.join(TMP, "raise_noleak_plain")
    subprocess.run(
        [cc, "-std=c99", "-O1", "-I", ENGINE, raise_c, "-lm", "-lpthread", "-ldl", "-o", exe_plain],
        check=True,
    )
    rr = subprocess.run([exe_plain], capture_output=True, text=True, timeout=60)
    assert rr.returncode == 0 and rr.stdout == "400\n", rr.stdout + rr.stderr


# Runtime 2: `--no-check` FFI call and second `move` on a moved value report
# `use of moved value` (matching the interpreter), never `ffi: arg 1 ... wants ptr`.
moved_ffi_src = (
    "extern fn malloc(size: i64) -> move ptr\n"
    "extern fn free(move p: ptr)\n"
    "fn main() -> i64 {\n"
    "  let p = malloc(16)\n"
    "  free(move p)\n"
    "  try {\n"
    "    free(p)\n"
    "  } catch e {\n"
    "    print(e)\n"
    "  }\n"
    "  try {\n"
    "    let _q = move p\n"
    "  } catch e {\n"
    "    print(e)\n"
    "  }\n"
    "  return 0\n"
    "}\n"
)
moved_brat = os.path.join(TMP, "moved_ffi.brat")
moved_c = os.path.join(TMP, "moved_ffi.c")
moved_exe = os.path.join(TMP, "moved_ffi.exe")
with open(moved_brat, "w", encoding="utf-8") as f:
    f.write(moved_ffi_src)
emit_m = subprocess.run([CXX, "emit-c", "--no-check", "--unsafe-ffi", moved_brat],
                        capture_output=True, text=True, check=True)
with open(moved_c, "w", encoding="utf-8") as f:
    f.write(emit_m.stdout)
subprocess.run(
    [cc, "-std=c99", "-O1", "-I", ENGINE, moved_c, "-lm", "-lpthread", "-ldl", "-o", moved_exe],
    check=True,
)
rm = subprocess.run([moved_exe], capture_output=True, text=True, timeout=30)
assert rm.returncode == 0, f"moved_ffi rc={rm.returncode}: {rm.stderr}"
assert rm.stdout == "use of moved value\nuse of moved value\n", f"got: {rm.stdout!r}"

print("ok: defer/try/return/raise/V_MOVED soundness verified")
