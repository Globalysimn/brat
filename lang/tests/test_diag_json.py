"""`brat check --json`: the diagnostic protocol an agent can branch on.

Structure is checked literally: one object per file, keys `version`, `file`,
`diagnostics`; every diagnostic has `line`, `severity`, `code`, `message`,
`note`, `helps`, `fix`. Codes are the contract: `E0382` use after move, `T0001`
type error, `P0001` syntax error, `W0001` unused variable. A caller reads codes,
never prose, so the text can change without breaking it.
"""
import _scratch

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
TMP = _scratch.mkdtemp(prefix="brat-json-")

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def write(name, text):
    path = os.path.join(TMP, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def run(args):
    return subprocess.run([sys.executable, CLI] + args, capture_output=True,
                          text=True, timeout=300)


def objects(out):
    """The payload is one JSON object per file; a stream of them is still JSON
    documents, so split on object boundaries by the decoder itself."""
    dec = json.JSONDecoder()
    text, rows, i = out.strip(), [], 0
    while i < len(text):
        while i < len(text) and text[i] in " \n\r\t":
            i += 1
        if i >= len(text):
            break
        obj, i = dec.raw_decode(text, i)
        rows.append(obj)
    return rows


def codes(payload):
    return [(d["code"], d["line"], d["severity"]) for d in payload["diagnostics"]]


CLEAN = '''fn main() -> i64 {
  print(1)
  return 0
}
'''

MOVED = '''fn main() -> i64 {
  let a = ["x"]
  let b = move a
  print(len(a))
  return 0
}
'''

TYPED = '''fn main() -> i64 {
  let x: i64 = "s"
  print(x)
  return 0
}
'''

BROKEN = '''fn main( -> i64 {
'''

path = write("clean.brat", CLEAN)
r = run(["check", path, "--json"])
check(r.returncode == 0, "clean rc=%d %r" % (r.returncode, r.stderr))
rows = objects(r.stdout)
check(len(rows) == 1, "clean: one object, got %r" % r.stdout)
check(rows[0] == {"version": 1, "file": path, "diagnostics": []},
      "clean payload: %r" % (rows[0],))
check(r.stderr.strip() == "", "clean: json mode prints nothing to stderr, got %r"
      % r.stderr)

path = write("moved.brat", MOVED)
r = run(["check", path, "--json"])
check(r.returncode == 1, "moved rc=%d" % r.returncode)
payload = objects(r.stdout)[0]
check(codes(payload) == [("W0001", 3, "warning"), ("E0382", 4, "error")],
      "moved codes: %r" % (codes(payload),))
err = payload["diagnostics"][1]
check(err["message"].startswith("use of moved `a`"), "moved message: %r" % err["message"])
check(err["note"] == {"line": 3, "message": "`a` moved here"},
      "moved note: %r" % (err["note"],))
check(err["helps"] == ["Use the variable the value was moved into, or copy it "
                       "beforehand (Copy types only)."], "moved helps: %r" % err["helps"])
check(err["fix"] is None, "moved fix: %r" % err["fix"])
check(set(err) == {"file", "line", "severity", "code", "message", "note", "helps", "fix"},
      "diagnostic keys: %r" % sorted(err))

borrowed = '''fn consume(v: vec[i64]) -> i64 {
  let owned = move v
  return len(owned)
}
'''
path = write("borrowed-move.brat", borrowed)
r = run(["check", path, "--json"])
payload = objects(r.stdout)[0]
check(r.returncode == 1 and codes(payload) == [("E0507", 2, "error")],
      "borrowed move code: %r" % codes(payload))
check(payload["diagnostics"][0]["helps"] ==
      ["Declare the parameter `move` to take ownership before consuming it."],
      "borrowed move help: %r" % payload["diagnostics"][0]["helps"])

path = write("typed.brat", TYPED)
r = run(["check", path, "--json"])
payload = objects(r.stdout)[0]
check(r.returncode == 1 and codes(payload) == [("T0001", 2, "error")],
      "typed codes: %r" % (codes(payload),))
check(payload["diagnostics"][0]["message"] == 'let `x`: annotated i64, got str',
      "typed message: %r" % payload["diagnostics"][0]["message"])

path = write("broken.brat", BROKEN)
r = run(["check", path, "--json"])
payload = objects(r.stdout)[0]
check(r.returncode == 1 and codes(payload) == [("P0001", 1, "error")],
      "broken codes: %r" % (codes(payload),))
check("Traceback" not in r.stderr, "broken: no traceback, got %r" % r.stderr)

# A module is reported on its own file, not folded into the root.
mod = write("lib/shapes.brat", '''pub fn area(r: i64) -> i64 {
  let n = 1
  return r * r
}
''')
write("lib/mod.brat", '''import shapes

fn main() -> i64 {
  print(shapes.area(3))
  return 0
}
''')
r = run(["check", os.path.join(TMP, "lib", "mod.brat"), "--json"])
check(r.returncode == 0, "module rc=%d %r" % (r.returncode, r.stdout + r.stderr))
rows = objects(r.stdout)
check([os.path.basename(o["file"]) for o in rows] == ["mod.brat", "shapes.brat"],
      "module files: %r" % [o["file"] for o in rows])
warn = [d for o in rows for d in o["diagnostics"]]
check([(d["code"], d["severity"]) for d in warn] == [("W0001", "warning")],
      "module warnings: %r" % (warn,))

# The same codes come out of `brat check` without --json, so the two paths
# cannot drift: the human text and the machine contract describe one run.
path = write("moved2.brat", MOVED)
human = run(["check", path])
moved_json = run(["check", path, "--json"])
check("use of moved `a`" in human.stdout, "human text: %r" % human.stdout)
check("use of moved `a`" in moved_json.stdout, "json text: %r" % moved_json.stdout)
check(human.returncode == moved_json.returncode == 1,
      "same exit code: %d vs %d" % (human.returncode, moved_json.returncode))

# `--json` before the file, and a missing file, both work.
r = run(["check", "--json", path])
check(r.returncode == 1 and objects(r.stdout)[0]["diagnostics"],
      "--json first: %r" % r.stdout)
r = run(["check", os.path.join(TMP, "nope.brat"), "--json"])
check(r.returncode == 1 and objects(r.stdout)[0]["diagnostics"][0]["code"] == "P0001",
      "missing file: %r" % r.stdout)

print("DIAG JSON ALL GREEN (%d asserts)" % n)
