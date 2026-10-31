"""`break`/`continue` must unwind lexical task scopes in every native emitter.

The interpreter has used `finally` to join a `scope` since its introduction.
This regression compares it with the Python C emitter and the standalone C++
emitter, including defer frames nested under an if inside the loop body.
"""
import _cxx
import _scratch
import _skip

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
CLI = os.path.join(ENGINE, "cli.py")
FIXTURE = os.path.join(HERE, "fixtures", "scope_unwind.brat")
TMP = _scratch.mkdtemp(prefix="brat-scope-unwind-")
WANT = (b"if defer 0\nscope defer 0\nscope child 0\n"
        b"if defer 1\nscope defer 1\nscope child 1\n"
        b"break if defer\nbreak scope defer\nbreak child\ndone\n")

try:
    sys.path.insert(0, ENGINE)
    import toolchain as _tc
    cc = _tc.resolve_cc()
except Exception as e:
    _skip.skip("no usable C compiler: %s" % e)


def run_program(argv, name):
    result = subprocess.run(argv, capture_output=True, timeout=60)
    assert result.returncode == 0, "%s rc=%d stdout=%r stderr=%r" % (
        name, result.returncode, result.stdout, result.stderr)
    assert result.stdout == WANT, "%s output mismatch: %r" % (name, result.stdout)


# Interpreter is the semantic reference.
run_program([sys.executable, CLI, "run", "--interp", FIXTURE], "interpreter")

# Force `emit-c` through the Python reference emitter even if brat-cxx exists.
py_c = os.path.join(TMP, "scope-unwind-python.c")
py_env = dict(os.environ, BRAT_CXX=os.path.join(TMP, "no-brat-cxx"))
emitted = subprocess.run([sys.executable, CLI, "emit-c", FIXTURE], capture_output=True,
                         env=py_env, timeout=120)
assert emitted.returncode == 0, emitted.stdout.decode(errors="replace") + emitted.stderr.decode(errors="replace")
with open(py_c, "wb") as f:
    f.write(emitted.stdout)
py_exe = os.path.join(TMP, "scope-unwind-python")
compile_py = subprocess.run(cc.argv(py_c, py_exe) + ["-O1", "-lm", "-I", ENGINE],
                            capture_output=True, timeout=180)
assert compile_py.returncode == 0, compile_py.stderr.decode(errors="replace")[-2000:]
run_program([py_exe], "Python C emitter")

# Standalone C++ frontend/build path must preserve the exact same join semantics.
bin_path = _cxx.build()
cxx_exe = os.path.join(TMP, "scope-unwind-cxx")
built = subprocess.run([bin_path, "build", FIXTURE, "-o", cxx_exe], capture_output=True,
                       text=True, timeout=300)
assert built.returncode == 0, built.stdout + built.stderr
run_program([cxx_exe], "C++ frontend")

print("SCOPE UNWIND ALL GREEN (interpreter, Python C, C++ C)")
