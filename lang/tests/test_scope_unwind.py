"""`break`/`continue` must unwind lexical task scopes in native programs.

The interpreter has used `finally` to join a `scope` since its introduction.
This regression compares it with native programs produced through the Python
CLI and through the standalone C++ build, including defer frames nested under
an if inside the loop body.
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
        b"break if defer\nbreak scope defer\nbreak child\n"
        b"error child\nparent\nchild\ndone\n")

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

# Emit through the Python CLI's required-native path, then compile manually.
bin_path = _cxx.build()
cli_c = os.path.join(TMP, "scope-unwind-cli.c")
cli_env = dict(os.environ, BRAT_CXX=bin_path, BRAT_NO_NATIVE_CLI="1")
emitted = subprocess.run([sys.executable, CLI, "emit-c", FIXTURE], capture_output=True,
                         env=cli_env, timeout=120)
assert emitted.returncode == 0, emitted.stdout.decode(errors="replace") + emitted.stderr.decode(errors="replace")
with open(cli_c, "wb") as f:
    f.write(emitted.stdout)
cli_exe = os.path.join(TMP, "scope-unwind-cli")
compile_cli = subprocess.run(cc.argv(cli_c, cli_exe) + ["-O1", "-lm", "-I", ENGINE],
                             capture_output=True, timeout=180)
assert compile_cli.returncode == 0, compile_cli.stderr.decode(errors="replace")[-2000:]
run_program([cli_exe], "Python CLI native C")

# Standalone C++ build path must preserve the exact same join semantics.
cxx_exe = os.path.join(TMP, "scope-unwind-cxx")
built = subprocess.run([bin_path, "build", FIXTURE, "-o", cxx_exe], capture_output=True,
                       text=True, timeout=300)
assert built.returncode == 0, built.stdout + built.stderr
run_program([cxx_exe], "C++ frontend")

print("SCOPE UNWIND ALL GREEN (interpreter, Python CLI native C, C++ build)")
