"""A .bri interface promises types, not just names: parameters and return are checked."""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = _scratch.mkdtemp(prefix="brat-iface-types-")


def write(rel, src):
    path = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def build(path, tag):
    return subprocess.run(_cxx.cli_args("build", path, "-o", os.path.join(TMP, tag + ".exe")),
                          capture_output=True, text=True, timeout=300)


def run(path):
    return subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True, text=True,
                          timeout=120)


write("ok/circle.brat",
      "fn area(r: i64) -> i64 { return r * r }\n")
write("ok/circle.bri", "fn area(r: i64) -> i64\n")
ok = write("ok/main.brat",
           "import circle\nfn main() -> i64 { print(circle.area(6)); return 0 }\n")
b = build(ok, "ok")
assert b.returncode == 0, b.stdout + b.stderr
exe = os.path.join(TMP, "ok.exe")
n = subprocess.run([exe], capture_output=True, text=True, timeout=60)
assert n.returncode == 0 and n.stdout == "36\n", n.stdout + n.stderr

write("ptype/circle.brat", "fn area(r: f64) -> i64 { return 0 }\n")
write("ptype/circle.bri", "fn area(r: i64) -> i64\n")
bad = write("ptype/main.brat", "import circle\nfn main() -> i64 { return circle.area(6) }\n")
for r in (run(bad), build(bad, "ptype")):
    out = r.stdout + r.stderr
    assert r.returncode != 0 and "parameter 1 is `i64` in the interface, `f64` in the implementation" in out, out

write("rtype/circle.brat", "fn area(r: i64) -> f64 { return 1.0 }\n")
write("rtype/circle.bri", "fn area(r: i64) -> i64\n")
bad = write("rtype/main.brat", "import circle\nfn main() -> i64 { return circle.area(6) }\n")
for r in (run(bad), build(bad, "rtype")):
    out = r.stdout + r.stderr
    assert r.returncode != 0 and "returns `i64` in the interface, `f64` in the implementation" in out, out

write("arity/circle.brat", "fn area(r: i64, k: i64) -> i64 { return r * k }\n")
write("arity/circle.bri", "fn area(r: i64) -> i64\n")
bad = write("arity/main.brat", "import circle\nfn main() -> i64 { return circle.area(6) }\n")
for r in (run(bad), build(bad, "arity")):
    out = r.stdout + r.stderr
    assert r.returncode != 0 and "takes 2 parameter(s) in the implementation, 1 in the interface" in out, out

write("ret_none/circle.brat", "fn area(r: i64) -> i64 { return r }\n")
write("ret_none/circle.bri", "fn area(r: i64)\n")
bad = write("ret_none/main.brat", "import circle\nfn main() -> i64 { return circle.area(6) }\n")
for r in (run(bad), build(bad, "ret_none")):
    out = r.stdout + r.stderr
    assert r.returncode != 0 and "returns `none` in the interface, `i64` in the implementation" in out, out

# The root source is a module too: a sibling .bri must be parsed and enforced
# even though the root has no namespace.
root_src = write("rootapi/main.brat", "fn main() -> i64 { return 0 }\n")
write("rootapi/main.bri", "fn main() -> i64\n")
for env in ({}, {}):
    ok = subprocess.run(_cxx.cli_args("check", root_src), capture_output=True,
                       text=True, timeout=120, env=dict(os.environ, **env))
    assert ok.returncode == 0, ok.stdout + ok.stderr
write("rootapi/main.bri", "fn main() -> str\n")
for env in ({}, {}):
    mismatch = subprocess.run(_cxx.cli_args("check", root_src), capture_output=True,
                              text=True, timeout=120, env=dict(os.environ, **env))
    out = mismatch.stdout + mismatch.stderr
    assert mismatch.returncode != 0 and "returns `str` in the interface, `i64` in the implementation" in out, out

print("INTERFACE TYPES ALL GREEN")
