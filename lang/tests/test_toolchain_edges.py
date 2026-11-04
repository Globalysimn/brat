"""Pins for the toolchain edges: source paths, the formatter, encodings, help.

Each case is a defect that was found by driving the CLI, not by reading it:

  * a source path holding `"` or `\\` used to reach the emitted C unescaped --
    the `"` one broke the build, the `\\` one pointed the program's base directory
    at a directory that does not exist, so `read_file` looked in the wrong place;
  * a string literal that spans lines used to be re-indented by `brat fmt`, which
    rewrote the literal's own bytes and changed what the program prints;
  * a source file that is not UTF-8 used to crash `run`/`emit-c`/`fmt` with a
    Python traceback while `check` answered `0 type error(s)`;
  * the NUL-in-literal diagnostic pointed at the line after the literal;
  * no command answered `--help`;
  * `brat dist` blamed the type checker for a file it could not read.

Both front ends are exercised where the file has one.
"""
import _cxx
import _scratch

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENG = os.path.join(ROOT, "lang", "engine")
CLI = [sys.executable, os.path.join(ENG, "cli.py")]
TMP = _scratch.mkdtemp(prefix="edges-")

CC = shutil.which("cc") or shutil.which("gcc")
if not CC:
    import _skip
    _skip.skip("no C compiler: nothing here can be compiled")

bad = 0


def ok(name, cond, detail=""):
    global bad
    if cond:
        print("%-22s ok" % name)
    else:
        bad += 1
        print("%-22s FAIL %s" % (name, detail))


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, timeout=kw.pop("timeout", 300), **kw)


def compile_c(src_path, exe_path):
    """Emitted C to a program, in the engine's own runtime include path."""
    return run([CC, "-std=c99", "-O1", "-I", ENG, "-o", exe_path, src_path, "-lm"])


# 1. a source directory whose name holds a quote or a backslash
LIB = 'pub fn twice(x: i64) -> i64 {\n  return x * 2\n}\n'
APP = ('import lib\n'
       'fn main() -> i64 {\n'
       '  let t = read_file("data.txt")\n'
       '  print(twice(21))\n'
       '  print(t)\n'
       '  return 0\n'
       '}\n')
WANT = b"42\n('ok', 'DATA-42\\n')\n"

for label, dirname in (("quote", 'we"ird'), ("backslash", "back\\slash")):
    base = os.path.join(TMP, dirname)
    os.makedirs(base, exist_ok=True)
    for name, text in (("lib.brat", LIB), ("main.brat", APP), ("data.txt", "DATA-42\n")):
        with open(os.path.join(base, name), "w", encoding="utf-8") as f:
            f.write(text)
    src = os.path.join(base, "main.brat")
    # Python CLI, then the C++ front end: same program, same answer.
    for who, emit in (("cli", CLI + ["emit-c", src]), ("cxx", [_cxx.build(), "cc", src])):
        try:
            r = run(emit)
            cfile = os.path.join(TMP, "%s_%s.c" % (label, who))
            with open(cfile, "wb") as f:
                f.write(r.stdout)
            if r.returncode != 0:
                ok("path_%s_%s" % (label, who), False, "emit rc=%d %r" % (r.returncode, r.stderr[:120]))
                continue
            exe = cfile[:-2]
            cb = compile_c(cfile, exe)
            if cb.returncode != 0:
                ok("path_%s_%s" % (label, who), False, "C: " + cb.stderr.decode("utf-8", "replace").strip().splitlines()[0][:110])
                continue
            got = run([exe]).stdout
            ok("path_%s_%s" % (label, who), got == WANT, "got %r want %r" % (got, WANT))
        except subprocess.TimeoutExpired:
            ok("path_%s_%s" % (label, who), False, "timeout")

# 2. the formatter keeps a literal that spans lines byte for byte
LIT = ('fn main() -> i64 {\n'
       'let s = "a\n'
       '  b"\n'
       'print(len(s))\n'
       'print(s)\n'
       'return 0\n'
       '}\n')
fmt_src = os.path.join(TMP, "multiline.brat")
with open(fmt_src, "w", encoding="utf-8") as f:
    f.write(LIT)
before = run(CLI + ["run", "--interp", fmt_src]).stdout
rf = run(CLI + ["fmt", fmt_src])
after_text = open(fmt_src, encoding="utf-8").read()
after = run(CLI + ["run", "--interp", fmt_src]).stdout
ok("fmt_multiline_literal", rf.returncode == 0 and before == after and before == b"5\na\n  b\n",
   "rc=%d before=%r after=%r text=%r" % (rf.returncode, before, after, after_text))

BAD = os.path.join(TMP, "bad8.brat")
with open(BAD, "wb") as f:
    f.write(b'fn main() -> i64 {\n  print("\xff")\n  return 0\n}\n')
for name, argv in (("utf8_run", CLI + ["run", "--interp", BAD]),
                   ("utf8_emit", CLI + ["emit-c", BAD]),
                   ("utf8_fmt", CLI + ["fmt", BAD]),
                   ("utf8_check_py", CLI + ["check", "--py", BAD]),
                   ("utf8_check_cxx", [_cxx.build(), "check", BAD])):
    r = run(argv)
    text = (r.stdout + r.stderr).decode("utf-8", "replace")
    ok(name, r.returncode != 0 and "Traceback" not in text and "not valid UTF-8" in text,
       "rc=%d %r" % (r.returncode, text[:160]))

# 4. the NUL diagnostic names the literal's line
NUL = os.path.join(TMP, "nul.brat")
with open(NUL, "wb") as f:
    f.write(b'fn main() -> i64 {\n  let a = 1\n  let b = 2\n  let s = "x\x00y"\n'
            b'  print(s)\n  return 0\n}\n')
for name, argv in (("nul_line_py", CLI + ["run", "--interp", NUL]),
                   ("nul_line_cxx", [_cxx.build(), "check", NUL])):
    r = run(argv)
    text = (r.stdout + r.stderr).decode("utf-8", "replace")
    first = text.strip().splitlines()[0] if text.strip() else ""
    ok(name, first.startswith(NUL + ":4: error:") and "NUL" in first, repr(first[:110]))

# 5. `--help`, and `--` still reaches the program
r = run(CLI + ["fmt", "--help"])
ok("help_flag", r.returncode == 0 and b"usage: brat" in r.stdout + r.stderr,
   "rc=%d %r" % (r.returncode, (r.stdout + r.stderr)[:80]))
args_src = os.path.join(TMP, "args.brat")
with open(args_src, "w", encoding="utf-8") as f:
    f.write('fn main() -> i64 {\n  for a in args() {\n    print(a)\n  }\n  return 0\n}\n')
r = run(CLI + ["run", "--interp", args_src, "--", "--help"])
ok("help_after_ddash", r.returncode == 0 and r.stdout == b"--help\n", "rc=%d %r" % (r.returncode, r.stdout))

# 6. `dist` does not blame the type checker for an unreadable file
r = run(CLI + ["dist", os.path.join(TMP, "missing.brat")])
text = (r.stdout + r.stderr).decode("utf-8", "replace")
ok("dist_missing_file", r.returncode == 1 and "type checker reported errors" not in text
   and "the check above failed" in text, "rc=%d %r" % (r.returncode, text[:160]))

print("---- toolchain edge diffs:", bad)
sys.exit(1 if bad else 0)
