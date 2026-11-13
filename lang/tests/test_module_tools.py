"""Module tooling: `brat deps` and `brat api` read the same loader the compiler uses.

`brat doc` refuses an interface file that does not lex, as `brat check` does.
"""
import _scratch

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _cxx
BIN = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-modtool-")

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def write(rel, src):
    path = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def cli(*args):
    return subprocess.run([BIN] + list(args), capture_output=True,
                          text=True, timeout=120)


write("shapes/cube.brat", "pub fn area(r: i64) -> i64 { return r * r }\n"
                          "pub fn perim(r: i64) -> i64 { return 6 * r }\n"
                          "fn helper() -> i64 { return 0 }\n")
write("shapes/circle.brat", "fn area(r: i64) -> i64 { return 3 * r * r }\n")
write("shapes/circle.bri", "fn area(r: i64) -> i64\n")
main = write("main.brat",
             "import shapes.cube\n"
             "import shapes.circle\n"
             "pub use shapes.cube.area\n"
             "fn main() -> i64 { print(cube.area(3)); print(circle.area(1)); return 0 }\n")

p = cli("deps", main)
check(p.returncode == 0, "deps rc=%d %r" % (p.returncode, p.stdout + p.stderr))
check(p.stdout == ("main -> shapes.circle (shapes.circle)\n"
                   "main -> shapes.cube (shapes.cube)\n"
                   "3 module(s), 2 import(s)\n"), "deps stdout %r" % (p.stdout,))

graph = os.path.join(TMP, "graph.json")
p = cli("deps", main, "-o", graph)
check(p.returncode == 0, "deps -o rc=%d %r" % (p.returncode, p.stdout + p.stderr))
data = json.load(open(graph, encoding="utf-8"))
check(data["modules"] == 3, "graph modules %r" % (data["modules"],))
check({"from": "main", "import": "shapes.cube", "to": "shapes.cube"} in data["edges"],
      "graph edges %r" % (data["edges"],))

p = cli("api", main)
check(p.returncode == 0, "api rc=%d %r" % (p.returncode, p.stdout + p.stderr))
out = p.stdout
check("- `area(r: i64) -> i64`" in out, "api misses cube.area: %r" % out)
check("- `perim(r: i64) -> i64`" in out, "api misses cube.perim: %r" % out)
check("helper" not in out, "api leaks a private function: %r" % out)
check("shapes.circle" in out and "shapes.cube" in out, "api misses a module: %r" % out)
check("shapes__" not in out, "api leaks mangled names: %r" % out)

# The same two answers as JSON, so an agent does not parse prose.
p = cli("deps", main, "--json")
check(p.returncode == 0, "deps --json rc=%d %r" % (p.returncode, p.stdout + p.stderr))
data = json.loads(p.stdout)
check(data["version"] == 1 and data["modules"] == 3, "deps --json %r" % (data,))
check({"from": "main", "import": "shapes.cube", "to": "shapes.cube"} in data["edges"],
      "deps --json edges %r" % (data["edges"],))

p = cli("api", main, "--json")
check(p.returncode == 0, "api --json rc=%d %r" % (p.returncode, p.stdout + p.stderr))
data = json.loads(p.stdout)
check(data["version"] == 1, "api --json version %r" % (data.get("version"),))
names = [f["name"] for m in data["modules"] for f in m["exports"]]
check("area" in names and "perim" in names, "api --json functions %r" % (names,))
check("helper" not in names, "api --json leaks a private function %r" % (names,))

bad = write("bad/main.brat", "import nope\nfn main() -> i64 { return 0 }\n")
p = cli("deps", bad)
check(p.returncode == 1 and "no module nope" in (p.stdout + p.stderr),
      "deps bad: rc=%d %r" % (p.returncode, p.stdout + p.stderr))
p = cli("api", bad)
check(p.returncode == 1 and "no module nope" in (p.stdout + p.stderr),
      "api bad: rc=%d %r" % (p.returncode, p.stdout + p.stderr))

torn = write("torn/shapes.brat", "pub fn area(r: i64) -> i64 { return r * r }\n")
write("torn/shapes.bri", "fn area(r: i64) -> i64\n@@\n")
p = cli("doc", torn)
check(p.returncode == 1 and "shapes.bri" in p.stderr and not p.stdout,
      "doc torn .bri: rc=%d %r" % (p.returncode, p.stdout + p.stderr))
p = cli("check", torn)
check(p.returncode != 0, "check torn .bri: rc=%d %r" % (p.returncode, p.stdout + p.stderr))

print("MODULE TOOLS ALL GREEN (%d asserts)" % n)
