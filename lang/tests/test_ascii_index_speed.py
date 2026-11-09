"""ASCII string indexing avoids temporary character values."""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
TMP = _scratch.mkdtemp(prefix="brat-speed-")


def run(argv, timeout=180):
    return subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                          cwd=ROOT)


def main():
    cxx = _cxx.build()
    source = '''fn is_digit(s: str, i: i64) -> bool {
  return s[i] >= "0" && s[i] <= "9"
}
fn is_b(s: str, i: i64) -> bool {
  return s[i] == "b"
}
fn main() -> i64 {
  print(is_digit("a123Z", 1))
  print(is_digit("a123Z", 0))
  print(is_b("abc", 1))
  print(is_b("привет", 0))
  try { print(is_digit("abc", 3)) } catch e { print(e) }
  return 0
}
'''
    path = os.path.join(TMP, "ascii.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(source)

    cpp = run([cxx, "cc", path])
    assert cpp.returncode == 0, cpp.stderr
    assert "vstr_ascii_cmp(" in cpp.stdout, "ASCII comparison lowering was not selected"

    run_o0 = run(_cxx.cli_args("run", "-O0", path))
    assert run_o0.returncode == 0, run_o0.stderr
    assert run_o0.stdout == "true\nfalse\ntrue\nfalse\nindex out of range\n"

    cfile = os.path.join(TMP, "ascii.c")
    exe = os.path.join(TMP, "ascii")
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(cpp.stdout)
    compiled = run(["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I", ENGINE, cfile, "-o", exe,
                    "-lm", "-pthread"])
    assert compiled.returncode == 0, compiled.stderr
    native = run([exe])
    assert (native.returncode, native.stdout, native.stderr) == \
           (run_o0.returncode, run_o0.stdout, ""), native.stderr

    print("ASCII STRING FAST PATH ALL GREEN")


if __name__ == "__main__":
    main()
