"""C integer types are fixed width: BRAT i64 is int64_t, never a bare `long`.

`long` is 32 bits under Windows LLP64, so a value type spelled `long` compiles
and silently truncates there. Both the runtime and the emitted C use int64_t.
"""
import _scratch

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")


def declared_lines(text):
    """Lines where `long` appears, ignoring C type keyword tables in the Python source."""
    return [ln for ln in text.split("\n") if re.search(r"(?<![\w.])long(?![\w])", ln)]


rt = open(os.path.join(ENGINE, "crt.h"), encoding="utf-8").read()
bad = [ln for ln in declared_lines(rt) if not re.search(r"long long", ln)]
assert not bad, "bare `long` in crt.h:\n%s" % "\n".join(bad[:5])
assert "typedef int64_t bint;" in rt
assert '_Static_assert(sizeof(bint) == 8' in rt
assert '"%" PRId64' in rt

tmp = _scratch.mkdtemp(prefix="brat-ctypes-")
src = os.path.join(tmp, "t.brat")
with open(src, "w", encoding="utf-8") as f:
    f.write('fn main() -> i64 {\n'
            '  let n: i64 = 9223372036854775807;\n'
            '  print(n);\n'
            '  print(n - 1);\n'
            '  return 0;\n'
            '}\n')

emitted = subprocess.run([sys.executable, CLI, "emit-c", src],
                         capture_output=True, text=True, timeout=120)
assert emitted.returncode == 0, emitted.stdout + emitted.stderr
bad = [ln for ln in declared_lines(emitted.stdout) if "int64_t" not in ln and "long long" not in ln
       and "PRId64" not in ln]
assert not bad, "bare `long` in emitted C:\n%s" % "\n".join(bad[:5])

exe = os.path.join(tmp, "t.exe")
built = subprocess.run([sys.executable, CLI, "build", src, "-o", exe],
                       capture_output=True, text=True, timeout=300)
assert built.returncode == 0, built.stdout + built.stderr
want = "9223372036854775807\n9223372036854775806\n"
out = subprocess.run([exe], capture_output=True, text=True, timeout=60)
assert out.returncode == 0 and out.stdout == want, out.stdout + out.stderr
interp = subprocess.run([sys.executable, CLI, "run", "--interp", src], capture_output=True, text=True, timeout=60)
assert interp.returncode == 0 and interp.stdout == want, interp.stdout + interp.stderr

print("C TYPES ALL GREEN")
