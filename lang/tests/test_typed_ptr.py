"""Typed pointers and slices: `ptr[T]`, `slice[T]`, bounds checked on both backends."""
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-slice-")


def write(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def expect_both(path, tag, stdout):
    interp = subprocess.run([sys.executable, CLI, "run", "--interp", path], capture_output=True,
                            text=True, timeout=120)
    assert interp.returncode == 0 and interp.stdout == stdout, \
        "run: rc=%d out=%r" % (interp.returncode, interp.stdout + interp.stderr)
    exe = os.path.join(TMP, tag + ".exe")
    built = subprocess.run([sys.executable, CLI, "build", path, "-o", exe],
                           capture_output=True, text=True, timeout=300)
    assert built.returncode == 0, "build: %r" % (built.stdout + built.stderr)
    native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    assert native.returncode == 0 and native.stdout == stdout, \
        "native: rc=%d out=%r" % (native.returncode, native.stdout + native.stderr)


def expect_runtime_fail(path, text):
    interp = subprocess.run([sys.executable, CLI, "run", "--interp", path], capture_output=True,
                            text=True, timeout=120)
    out = interp.stdout + interp.stderr
    assert interp.returncode != 0 and text in out, "run: rc=%d out=%r" % (interp.returncode, out)
    exe = os.path.join(TMP, "rt.exe")
    built = subprocess.run([sys.executable, CLI, "build", path, "-o", exe],
                           capture_output=True, text=True, timeout=300)
    assert built.returncode == 0, "build: %r" % (built.stdout + built.stderr)
    native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    out = native.stdout + native.stderr
    assert native.returncode != 0 and text in out, "native: rc=%d out=%r" % (native.returncode, out)


def expect_type_fail(path, text):
    for cmd in ("check", "build"):
        argv = [sys.executable, CLI, cmd, path]
        if cmd == "build":
            argv += ["-o", os.path.join(TMP, "bad.exe")]
        r = subprocess.run(argv, capture_output=True, text=True, timeout=300)
        out = r.stdout + r.stderr
        assert r.returncode != 0 and text in out, "%s: rc=%d out=%r" % (cmd, r.returncode, out)


expect_both(write("view.brat",
                  "fn main() -> i64 {\n"
                  "  let v = [10, 20, 30, 40];\n"
                  "  let s: slice[i64] = slice_of(v, 1, 3);\n"
                  "  print(slice_len(s));\n"
                  "  print(slice_get(s, 0));\n"
                  "  slice_set(s, 1, 99);\n"
                  "  print(v[2]);\n"
                  "  print(slice_get(s, 1));\n"
                  "  return 0;\n"
                  "}\n"), "view", "2\n20\n99\n99\n")

expect_both(write("float.brat",
                  "fn main() -> i64 {\n"
                  "  let v = [1.5, 2.0];\n"
                  "  let s: slice[f64] = slice_of(v, 0, 2);\n"
                  "  print(slice_get(s, 0) + slice_get(s, 1));\n"
                  "  return 0;\n"
                  "}\n"), "float", "3.5\n")

expect_both(write("catch.brat",
                  "fn main() -> i64 {\n"
                  "  let v = [1, 2, 3];\n"
                  "  let s: slice[i64] = slice_of(v, 0, 2);\n"
                  "  try {\n"
                  "    print(slice_get(s, 2));\n"
                  "  } catch e {\n"
                  "    print(\"caught\");\n"
                  "  }\n"
                  "  return 0;\n"
                  "}\n"), "catch", "caught\n")

expect_runtime_fail(write("oob.brat",
                  "fn main() -> i64 {\n"
                  "  let v = [1, 2, 3];\n"
                  "  let s: slice[i64] = slice_of(v, 0, 2);\n"
                  "  return slice_get(s, 5);\n"
                  "}\n"), "slice index 5 out of range 0..2")

expect_runtime_fail(write("range.brat",
                  "fn main() -> i64 {\n"
                  "  let v = [1, 2, 3];\n"
                  "  let s: slice[i64] = slice_of(v, 1, 9);\n"
                  "  return slice_len(s);\n"
                  "}\n"), "slice range 1..9 out of range 0..3")

expect_type_fail(write("elem.brat",
                  "fn main() -> i64 {\n"
                  "  let v = [1, 2, 3];\n"
                  "  let s: slice[i64] = slice_of(v, 0, 2);\n"
                  "  slice_set(s, 0, \"x\");\n"
                  "  return 0;\n"
                  "}\n"), "slice_set: want i64, got str")

expect_type_fail(write("mix.brat",
                  "fn main() -> i64 {\n"
                  "  let a: ptr[i64] = ptr_null();\n"
                  "  let b: ptr[f64] = a;\n"
                  "  return 0;\n"
                  "}\n"), "annotated ptr[f64], got ptr[i64]")

# A byte slice over C memory: the same window machinery as i64/f64, one byte
# per element, and a value range that the write is checked against.
BYTES = ("fn main() -> i64 {\n"
         "  let p = ptr_of_str(\"ABC\");\n"
         "  let s = slice_of_ptr(p, 3, \"u8\");\n"
         "  print(slice_len(s));\n"
         "  print(slice_get(s, 0));\n"
         "  print(slice_get(s, 2));\n"
         "  slice_set(s, 1, 66);\n"
         "  print(slice_get(s, 1));\n"
         "  return 0;\n"
         "}\n")
path = write("bytes.brat", BYTES)
expect_both(path, "bytes", "3\n65\n67\n66\n")

expect_both(write("bytes_bounds.brat",
                  "fn main() -> i64 {\n"
                  "  let p = ptr_of_str(\"AB\");\n"
                  "  let s = slice_of_ptr(p, 2, \"u8\");\n"
                  "  try {\n"
                  "    print(slice_get(s, 5));\n"
                  "  } catch e {\n"
                  "    print(e);\n"
                  "  }\n"
                  "  try {\n"
                  "    slice_set(s, 0, 300);\n"
                  "  } catch e {\n"
                  "    print(e);\n"
                  "  }\n"
                  "  return 0;\n"
                  "}\n"),
             "bytes_bounds",
             "slice index 5 out of range 0..2\nslice_set: byte 300 out of range 0..255\n")

print("TYPED PTR ALL GREEN")
