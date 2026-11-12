"""A program that calls C is not built without a proof.

The declaration in the file is what the generated C calls through. If the header
disagrees, that call is a call through the wrong ABI: it returns garbage or
writes over something else, and nothing at run time says which. So every command
that produces or runs a binary refuses a program whose C is unproven, and names
the two ways out:

    brat build prog.brat --header lib.h    the declarations compared to C
    brat build prog.brat --unsafe-ffi      build it anyway, on your head

`run -O0` calls the same C, so it takes the same proof gate.
"""
import _cxx
import _scratch

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CXX = os.path.abspath(os.path.join(HERE, "..", "cxx", "bin", "brat-cxx"))
TMP = _scratch.mkdtemp(prefix="brat-mandate-")
LIB = os.path.join(TMP, "libm.so")

with open(os.path.join(TMP, "libm.c"), "w", encoding="utf-8") as f:
    f.write("long long triple(long long x) { return x * 3; }\n")
r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", os.path.join(TMP, "libm.c"),
                    "-o", LIB], capture_output=True, text=True, timeout=120)
assert r.returncode == 0, r.stderr

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def write(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


env = dict(os.environ, LD_LIBRARY_PATH=TMP)


def cli(*args, **kw):
    # The proofs below are hand-written on purpose: this file tests the gate, so
    # it must not opt out of it.
    return subprocess.run(_cxx.cli_args(*(list(args))), capture_output=True,
                          text=True, timeout=600, env=env, **kw)


CALLS_C = ('extern "%s" fn triple(x: i64) -> i64;\n'
           'fn main() -> i64 {\n'
           '  print(triple(14));\n'
           '  return 0\n'
           '}\n') % LIB

GOOD = write("good.h", "long long triple(long long x);\n")
BAD = write("bad.h", "int triple(int x);\n")
prog = write("prog.brat", CALLS_C)
plain = write("plain.brat", 'fn main() -> i64 {\n  print(7);\n  return 0\n}\n')

REFUSAL = "calls C and no header was given"
HATCHES = ("--header FILE", "--unsafe-ffi")

# `brat run` gates before exec, and `-O0` goes through the same proof.
NATIVE = os.path.isfile(CXX) and os.access(CXX, os.X_OK)

# 1. Every command that builds or runs a binary refuses the same way
COMMANDS = [("build", ["-o", os.path.join(TMP, "a.exe")]),
            ("dist", ["-o", os.path.join(TMP, "dist")]),
            ("emit-c", []),
            ("run", ["-O0"])]
if NATIVE:
    COMMANDS.append(("run", []))
for cmd, extra in COMMANDS:
    r = cli(cmd, prog, *extra)
    out = r.stdout + r.stderr
    check(r.returncode != 0, "%s built a program whose C is unproven: rc=%d" % (cmd, r.returncode))
    check(REFUSAL in out, "%s: the refusal is not the one this file is about: %r" % (cmd, out))
    for hatch in HATCHES:
        # A refusal that does not say how to proceed is a wall.
        check(hatch in out, "%s: the refusal names only one way out: %r" % (cmd, out))
    check("undefined behaviour" in out, "%s: the refusal does not say why: %r" % (cmd, out))

# The names in the two ways out are the command that was run, so the line can be
# pasted: `brat run --header` is not a thing to type after `--unsafe-ffi`.
r = cli("dist", prog)
check("brat dist %s --unsafe-ffi" % prog in r.stderr, r.stderr)
if NATIVE:
    r = cli("run", prog)
    check("brat run %s --header FILE" % prog in r.stderr, r.stderr)

exe = os.path.join(TMP, "unsafe.exe")
r = cli("build", prog, "--unsafe-ffi", "-o", exe)
check(r.returncode == 0, "build --unsafe-ffi: %r" % (r.stdout + r.stderr))
check("calls C without a proof (--unsafe-ffi)" in r.stderr,
      "the opt-out was silent: %r" % r.stderr)
check(r.stderr.count("--unsafe-ffi") == 1, "said twice: %r" % r.stderr)
check(os.path.exists(exe), "no binary")
ran = subprocess.run([exe], capture_output=True, text=True, timeout=120, env=env)
check(ran.returncode == 0 and ran.stdout == "42\n", "the unproven build is wrong: %r" % ran.stdout)

dist_dir = os.path.join(TMP, "dist-unsafe")
r = cli("dist", prog, "--unsafe-ffi", "-o", dist_dir)
check(r.returncode == 0, "dist --unsafe-ffi: %r" % (r.stdout + r.stderr))
check(r.stderr.count("calls C without a proof (--unsafe-ffi)") == 1,
      "dist must report the explicit FFI opt-out once: %r" % r.stderr)
dist_manifest = json.load(open(os.path.join(dist_dir, "manifest.json"), encoding="utf-8"))
dist_artifact = os.path.join(dist_dir, dist_manifest["artifacts"][0]["artifact"])
ran = subprocess.run([dist_artifact], capture_output=True, text=True, timeout=120, env=env)
check(ran.returncode == 0 and ran.stdout == "42\n",
      "dist --unsafe-ffi artifact is wrong: %r" % (ran.stdout + ran.stderr))
dist_repeat = os.path.join(TMP, "dist-unsafe-repeat")
repeat = cli("dist", prog, "--unsafe-ffi", "-o", dist_repeat)
check(repeat.returncode == 0, "repeat dist failed: %r" % (repeat.stdout + repeat.stderr))
repeat_manifest = json.load(open(os.path.join(dist_repeat, "manifest.json"), encoding="utf-8"))
check(repeat_manifest == dist_manifest,
      "identical source/target dist runs were not reproducible: %r != %r"
      % (dist_manifest, repeat_manifest))

# The note is a warning about this build, so it goes to stderr and never into the
# program's own stdout: a build log is not the program's output.
check("42" not in r.stdout, "--unsafe-ffi wrote to stdout: %r" % r.stdout)

# 3. A header is the other way out, and it is checked
good_exe = os.path.join(TMP, "proved.exe")
r = cli("build", prog, "--header", GOOD, "-o", good_exe)
check(r.returncode == 0, "build --header: %r" % (r.stdout + r.stderr))
check("--unsafe-ffi" not in r.stderr, "a proven build mentions the opt-out: %r" % r.stderr)
ran = subprocess.run([good_exe], capture_output=True, text=True, timeout=120, env=env)
check(ran.stdout == "42\n", "the proven build is wrong: %r" % ran.stdout)
interp_proved = cli("run", "-O0", prog, "--header", GOOD)
check(interp_proved.returncode == 0 and interp_proved.stdout == "42\n",
      "the -O0 build must run a C call after the same header proof: %r"
      % (interp_proved.stdout + interp_proved.stderr))

# A header that disagrees is a refusal, not a warning with a binary next to it.
r = cli("build", prog, "--header", BAD, "-o", os.path.join(TMP, "bad.exe"))
out = r.stdout + r.stderr
check(r.returncode != 0, "a disagreeing header still produced a binary")
check("triple" in out, "the drift does not name the function: %r" % out)
check(not os.path.exists(os.path.join(TMP, "bad.exe")), "a binary was left behind")

# 4. `--unsafe-ffi` is a flag of `brat`, not of the program
# Everything after `--` belongs to the program. A program is allowed an argument
# called `--header`, and feeding it to the proof would prove the wrong file.
if NATIVE:
    r = cli("run", prog, "--", "--header", GOOD)
    check(r.returncode != 0 and REFUSAL in r.stderr,
          "an argument of the program was taken for a header: %r" % (r.stdout + r.stderr))
    r = cli("run", prog, "--unsafe-ffi", "--", "--header", GOOD)
    check(r.returncode == 0 and r.stdout == "42\n",
          "--unsafe-ffi before `--` must still reach the gate: %r" % (r.stdout + r.stderr))
else:
    # `run` goes to the -O0 build, but `build` still takes both flags the same
    # way, so the argument-splitting rule is checked where it can be.
    r = cli("build", prog, "--", "--header", GOOD, "-o", os.path.join(TMP, "dd.exe"))
    check(r.returncode != 0, "an argument after `--` reached the proof: %r" % (r.stdout + r.stderr))

# 5. A program that calls no C is not gated
r = cli("build", plain, "-o", os.path.join(TMP, "plain.exe"))
check(r.returncode == 0, "a program with no externs was refused: %r" % (r.stdout + r.stderr))
check("--unsafe-ffi" not in r.stderr, "%r" % r.stderr)
unused = write("unused.brat", ('extern "%s" fn triple(x: i64) -> i64;\n'
                                'fn main() -> i64 { return 0 }\n') % LIB)
r = cli("run", "-O0", unused)
check(r.returncode != 0 and REFUSAL in r.stderr,
      "an extern declaration must still require FFI proof in native run: %r"
      % (r.stdout + r.stderr))

# 6. The -O0 build requires an explicit unsafe acknowledgement when no header
# or .bratbind proves the foreign prototype.
r = cli("run", "-O0", prog)
check(r.returncode != 0 and REFUSAL in r.stderr, r.stderr)
r = cli("run", "-O0", "--no-check", prog)
check(r.returncode != 0 and REFUSAL in r.stderr,
      "--no-check bypassed unoptimized (-O0) FFI proof gate: %r" % (r.stdout + r.stderr))
r = cli("run", "-O0", "--unsafe-ffi", prog)
check(r.returncode == 0 and r.stdout == "42\n",
      "-O0 --unsafe-ffi: %r" % (r.stdout + r.stderr))
check("calls C without a proof (--unsafe-ffi)" in r.stderr, r.stderr)

# Calls inside an imported module cross the same boundary and require the same proof.
module_dir = os.path.join(TMP, "ffi")
os.makedirs(module_dir)
with open(os.path.join(module_dir, "calls.brat"), "w", encoding="utf-8") as f:
    f.write(('extern "%s" fn triple(x: i64) -> i64;\n'
             'pub fn run() -> i64 { return triple(14) }\n') % LIB)
imported = write("imported.brat", 'import ffi.calls\n'
                 'fn main() -> i64 { print(ffi.calls.run()); return 0 }\n')
r = cli("run", "-O0", imported)
check(r.returncode != 0 and REFUSAL in r.stderr,
      "an imported module's C call bypassed the -O0 build gate: %r" % (r.stdout + r.stderr))
r = cli("run", "-O0", "--no-check", imported)
check(r.returncode != 0 and REFUSAL in r.stderr,
      "--no-check bypassed an imported module's FFI gate: %r" % (r.stdout + r.stderr))
r = cli("run", "-O0", "--unsafe-ffi", imported)
check(r.returncode == 0 and r.stdout == "42\n",
      "an imported module's C call failed with the explicit opt-out: %r" % (r.stdout + r.stderr))
if NATIVE:
    r = cli("run", imported)
    check(r.returncode != 0 and REFUSAL in r.stderr,
          "an imported module's C call bypassed the native gate: %r" % (r.stdout + r.stderr))

# 7. `brat run` refuses before it execs
# The native path replaces this process with the compiled binary; a gate that
# ran after the exec would never run at all.
if NATIVE:
    r = cli("run", prog)
    check(r.returncode != 0, "run exec'd into a binary it had not proven")
    check(REFUSAL in r.stderr, r.stderr)
    r = cli("run", prog, "--unsafe-ffi")
    check(r.returncode == 0 and r.stdout == "42\n", "run --unsafe-ffi: %r" % (r.stdout + r.stderr))
else:
    print("mandate skip: no brat-cxx here, the native run path is not exercised")

print("FFI MANDATE ALL GREEN (%d asserts)" % n)
