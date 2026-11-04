"""Every C++ header that GCC accepts on its own is accepted by clang too.

The README tells people to build with `c++ -std=c++20 lang/cxx/src/*.cpp`, and on many systems `c++`
is clang. `check.h` once declared `std::vector<std::pair<std::string, Ty>>` as a member of `Ty`:
GCC with libstdc++ tolerates a pair of an incomplete type, clang does not, so no file that included
the header compiled. A one-line translation unit per header shows that in seconds; compiling all
24 sources with clang takes minutes.

Skipped when there is no clang++. Headers that do not compile alone under GCC are not judged.
"""
import _skip

import os
import shutil
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", "cxx", "src"))
CLANG = shutil.which("clang++")
GXX = shutil.which("g++")
if not CLANG or not GXX:
    _skip.skip("needs both clang++ and g++")

FLAGS = ["-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-I", SRC]
headers = sorted(f for f in os.listdir(SRC) if f.endswith(".h"))
assert len(headers) >= 20, headers
tmp = tempfile.mkdtemp(prefix="brat-clang-headers-")


def compiles(compiler, header):
    unit = os.path.join(tmp, header + ".cpp")
    with open(unit, "w") as f:
        f.write('#include "%s"\n' % header)
    done = subprocess.run([compiler] + FLAGS + [unit], capture_output=True, text=True, timeout=120)
    return done.returncode == 0, done.stderr


def verdict(header):
    gcc_ok, _ = compiles(GXX, header)
    if not gcc_ok:
        return header, None, ""
    ok, err = compiles(CLANG, header)
    return header, ok, err


with ThreadPoolExecutor(max_workers=2) as pool:
    results = list(pool.map(verdict, headers))

judged = [r for r in results if r[1] is not None]
refused = [(h, err) for h, ok, err in judged if not ok]
assert len(judged) >= 10, "only %d headers compile alone under g++" % len(judged)
assert not refused, "clang refuses %s:\n%s" % ([h for h, _ in refused], refused[0][1][:800])
shutil.rmtree(tmp, ignore_errors=True)
print("CLANG HEADERS ALL GREEN (%d of %d headers judged)" % (len(judged), len(headers)))
