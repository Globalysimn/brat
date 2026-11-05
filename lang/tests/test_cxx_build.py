"""The native C++ frontend checks, emits and builds without Python."""
import _cxx
import _scratch

import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-cxx-build-")
BIN = _cxx.build()


def write(path, source):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(source)
    return path


def run_build(*args):
    return subprocess.run([BIN, "build"] + list(args), capture_output=True, text=True,
                          timeout=300)


lib_dir = os.path.join(TMP, "lib")
os.makedirs(lib_dir)
write(os.path.join(lib_dir, "mathx.brat"),
      "pub fn square(x: i64) -> i64 { return x * x }\n")
source = write(os.path.join(TMP, "app with spaces.brat"),
               "import lib.mathx\nfn main() -> i64 {\n"
               "  print(lib.mathx.square(7))\n  return 0\n}\n")
exe = os.path.join(TMP, "app")
build = run_build(source, "-o", exe)
assert build.returncode == 0, build.stdout + build.stderr
run = subprocess.run([exe], capture_output=True, timeout=30)
assert run.returncode == 0 and run.stdout == b"49\n", (run.returncode, run.stdout, run.stderr)

# Two builds with different staging paths must produce byte-identical artifacts.
repeat_exe = os.path.join(TMP, "app-repeat")
repeat = run_build(source, "-o", repeat_exe)
assert repeat.returncode == 0, repeat.stdout + repeat.stderr
with open(exe, "rb") as f:
    first_hash = hashlib.sha256(f.read()).hexdigest()
with open(repeat_exe, "rb") as f:
    repeat_hash = hashlib.sha256(f.read()).hexdigest()
assert first_hash == repeat_hash, (first_hash, repeat_hash)

# A bundle contains the driver and runtime header in the layout the driver expects.
bundle = os.path.join(TMP, "release")
packed = subprocess.run([BIN, "bundle", bundle], capture_output=True, text=True,
                        timeout=30)
assert packed.returncode == 0, packed.stdout + packed.stderr
bundled_bin = os.path.join(bundle, "lang", "cxx", "bin", "brat-cxx")
assert os.path.isfile(bundled_bin)
assert os.path.isfile(os.path.join(bundle, "lang", "engine", "crt.h"))
bundled_exe = os.path.join(TMP, "bundled-app")
bundled_build = subprocess.run([bundled_bin, "build", source, "-o", bundled_exe],
                               capture_output=True, text=True, timeout=300)
assert bundled_build.returncode == 0, bundled_build.stdout + bundled_build.stderr
run = subprocess.run([bundled_exe], capture_output=True, timeout=30)
assert run.returncode == 0 and run.stdout == b"49\n", (
    run.returncode, run.stdout, run.stderr)

# The regular CLI delegates a fresh frontend build to the native command too.
wrapper_exe = os.path.join(TMP, "wrapper")
wrapped = subprocess.run(_cxx.cli_args("build", source, "-o", wrapper_exe),
                         capture_output=True, text=True, timeout=300)
assert wrapped.returncode == 0, wrapped.stdout + wrapped.stderr
run = subprocess.run([wrapper_exe], capture_output=True, timeout=30)
assert run.returncode == 0 and run.stdout == b"49\n", (run.returncode, run.stdout, run.stderr)

bad = write(os.path.join(TMP, "type-error.brat"),
            'fn main() -> i64 { return "not an integer" }\n')
refused = run_build(bad, "-o", os.path.join(TMP, "type-error"))
assert refused.returncode == 1 and "type error" in refused.stdout, (
    refused.returncode, refused.stdout, refused.stderr)
assert not os.path.exists(os.path.join(TMP, "type-error"))

bad = write(os.path.join(TMP, "move-error.brat"),
            'fn main() -> i64 {\n  let a = ["x"]\n  let b = move a\n'
            '  print(len(a))\n  return 0\n}\n')
refused = run_build(bad, "-o", os.path.join(TMP, "move-error"))
assert refused.returncode == 1 and "use of moved" in refused.stdout, (
    refused.returncode, refused.stdout, refused.stderr)
assert not os.path.exists(os.path.join(TMP, "move-error"))

mem = run_build(source, "-o", os.path.join(TMP, "mem"), "--mem", "16M")
assert mem.returncode == 0, mem.stdout + mem.stderr

# `brat-cxx run` is the product path with no Python in it: check, emit, link
# once into a content-addressed cache, exec. The cache must never serve a stale
# program, and the backend the user gets must behave like the interpreter.
env = dict(os.environ, BRAT_CACHE=os.path.join(TMP, "cache"))


def run_native(path, *args):
    return subprocess.run([BIN, "run", path] + list(args), capture_output=True,
                          text=True, timeout=300, env=env)


RUN_CASES = {
    "prints": ('fn main() -> i64 {\n  print("ok")\n  return 0\n}\n', "ok\n", 0),
    "exit_code": ('fn main() -> i64 {\n  print("x")\n  return 3\n}\n', "x\n", 3),
    "args": ('fn main() -> i64 {\n  print(len(args()))\n  return 0\n}\n', "2\n", 0),
}
for name, (src, want, rc) in RUN_CASES.items():
    path = write(os.path.join(TMP, "run-%s.brat" % name), src)
    extra = ["--", "a", "b"] if name == "args" else []
    got = run_native(path, *extra)
    assert got.returncode == rc and got.stdout == want, (name, got.returncode, got.stdout, got.stderr)
    interp = subprocess.run(_cxx.run_args(path, *(extra[1:])),
                            capture_output=True, text=True, timeout=300)
    assert interp.stdout == want, ("interp disagrees", name, interp.stdout)

# A clean run says nothing but the program's own output.
assert ": 0 error(s)" not in run_native(write(os.path.join(TMP, "quiet.brat"),
                                             RUN_CASES["prints"][0])).stdout

# The cache is keyed by content: editing the source changes the answer, and a
# second run of the same source does not recompile (no compiler invocation).
cached = write(os.path.join(TMP, "cached.brat"),
               'fn main() -> i64 {\n  print("one")\n  return 0\n}\n')
first = run_native(cached)
assert first.stdout == "one\n", first.stdout
again = run_native(cached)
assert again.stdout == "one\n", again.stdout
write(cached, 'fn main() -> i64 {\n  print("two")\n  return 0\n}\n')
assert run_native(cached).stdout == "two\n"
entries = [f for f in os.listdir(os.path.join(TMP, "cache", "run")) if not f.endswith(".c")]
assert len(entries) == len(RUN_CASES) + 2, entries

# A program the checker refuses is refused before anything is compiled.
bad_run = write(os.path.join(TMP, "run-bad.brat"),
                'fn main() -> i64 {\n  let a = ["x"]\n  let b = move a\n'
                '  print(len(a))\n  return 0\n}\n')
refused_run = run_native(bad_run)
assert refused_run.returncode == 1 and "use of moved" in refused_run.stderr and refused_run.stdout == "", (
    refused_run.returncode, refused_run.stdout, refused_run.stderr)

print("CXX BUILD ALL GREEN (5 cases + native run)")
