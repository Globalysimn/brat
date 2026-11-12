import _cxx
import _scratch
import glob
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = "cli"
EXAMPLES = os.path.join(HERE, "..", "examples")
NEWCOMER = os.path.join(EXAMPLES, "newcomer.brat")
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
README = os.path.join(ROOT, "README.md")

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


ENV = dict(os.environ)
ENV["BRAT_FILES"] = "1"


def run(tool, *cmd):
    if tool == "own":
        argv = [_cxx.build(), "check", *cmd]
    else:
        argv = _cxx.cli_args(*cmd)
    return subprocess.run(argv, capture_output=True, text=True, timeout=120, env=ENV)


readme = open(README, encoding="utf-8").read()
test_count = len(glob.glob(os.path.join(HERE, "test_*.py")))
check("lang/tests/      %d test files" % test_count in readme,
      "README test count is stale (runner has %d files)" % test_count)
check("OWNER/BRAT" not in readme, "README CI badge still uses a placeholder")
check("git clone https://github.com/denionis23/brat.git brat && cd brat" in readme,
      "README clone command does not match the repository")
check("./lang/cxx/bin/brat-cxx run hello.brat" in readme,
      "README must provide a direct native quick start")
check("Python is not required" in readme,
      "README must distinguish native requirements from development dependencies")
# Every doc the README links must exist; a link to a deleted file is a doc bug
# the reader finds, not the reviewer.
import re as _re
for _link in _re.findall(r"\]\(([^)#]+)\)", readme):
    if _link.startswith(("http://", "https://")):
        continue
    check(os.path.exists(os.path.join(ROOT, _link)), "README links a missing file: %s" % _link)

check(os.path.isfile(NEWCOMER), "newcomer.brat missing")

lines = open(NEWCOMER, encoding="utf-8").read().splitlines()
check(40 <= len(lines) <= 90, "newcomer.brat is %d lines, want 40..90" % len(lines))

p = run(CLI, "check", NEWCOMER)
check(p.returncode == 0, "check newcomer exit=%d out=%s" % (p.returncode, (p.stdout + p.stderr)[-600:]))
check("%d error(s)" % 0 in p.stdout, "check newcomer wants 0 error(s) have %r" % p.stdout[-400:])
check("%d type error(s)" % 0 in p.stdout, "check newcomer wants 0 type error(s) have %r" % p.stdout[-400:])

p = run(CLI, "run", "-O0", NEWCOMER)
check(p.returncode == 0, "run newcomer exit=%d err=%s" % (p.returncode, (p.stdout + p.stderr)[-600:]))
for want in ["requests: 7 skipped: 1 bytes: 1843",
             "10.0.0.3: 2 hits, 129 bytes, 2 errors",
             "10.0.0.7: 2 hits, 1712 bytes, 0 errors",
             "10.0.0.9: 2 hits, 2 bytes, 1 errors",
             "2xx: 3", "4xx: 2", "5xx: 1"]:
    check(want in p.stdout, "run newcomer wants %r have %r" % (want, p.stdout[-600:]))

own = "own"
p = run(own, NEWCOMER)
check("0 error(s)" in p.stdout, "own newcomer wants 0 error(s) have %r" % p.stdout[-400:])


TMP = _scratch.mkdtemp(prefix="brat-newcomer-")


def probe(src):
    f = os.path.join(TMP, "m.brat")
    with open(f, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(src)
    return run(own, f)


p = probe('fn main() -> i64 {\n  print("{e["hits"]}")\n  return 0\n}\n')
check("use of undeclared var `hits`" in p.stdout, "cut string wants the var %r" % p.stdout[-400:])
check("string literal looks cut short" in p.stdout, "cut string wants the warning %r" % p.stdout[-400:])
check(p.stdout.count("cut short") == 1, "cut string warns once have %r" % p.stdout[-600:])

p = probe("fn main() -> i64 {\n  let rec = []\n  rec = 1\n  return 0\n}\n")
check("assign to immutable `rec`" in p.stdout, "immutable wants the error %r" % p.stdout[-400:])
check("`rec` declared here without `mut`" in p.stdout, "immutable wants the decl note %r" % p.stdout[-400:])

p = probe("fn main() -> i64 {\n  return pritn(1)\n}\n")
check("use of undeclared var `pritn`" in p.stdout, "typo wants the error %r" % p.stdout[-400:])
check("help: did you mean `print`?" in p.stdout, "typo wants the suggestion %r" % p.stdout[-400:])

p = probe("fn main() -> i64 {\n  let cat = 1\n  let can = 2\n  return cap\n}\n")
check("help: did you mean" not in p.stdout, "ambiguous typo must not suggest a name %r" % p.stdout[-400:])

p = probe("trait T {\n  fn m(self) -> i64\n}\n\nimpl T for P {\n  fn n(self) -> i64 {\n    return 0\n  }\n}\n")
check("impl T for P missing m" in p.stdout, "impl missing wants the error %r" % p.stdout[-400:])
check(":5: error: impl T for P missing m" in p.stdout, "impl missing wants a line %r" % p.stdout[-400:])
check("  | impl T for P {" in p.stdout, "impl missing wants a snippet %r" % p.stdout[-400:])

p = probe("fn main() -> i64 {\n  let mut m = {}\n  m[\"a\"] = {\"hits\": 0}\n  let mut e = m[\"a\"]\n  e[\"hits\"] = 1\n  return 0\n}\n")
check("double mut borrow of `m`" in p.stdout, "alias wants the error %r" % p.stdout[-400:])
check("second mutable place over `m`" in p.stdout, "alias wants the help note %r" % p.stdout[-400:])


LOGSTAT = os.path.join(EXAMPLES, "logstat.brat")
check(os.path.isfile(LOGSTAT), "logstat.brat missing")
p = run(CLI, "check", LOGSTAT)
check(p.returncode == 0, "check logstat exit=%d out=%s" % (p.returncode, (p.stdout + p.stderr)[-600:]))
check("%d error(s)" % 0 in p.stdout, "check logstat wants 0 error(s) have %r" % p.stdout[-400:])
check("%d type error(s)" % 0 in p.stdout, "check logstat wants 0 type error(s) have %r" % p.stdout[-400:])

FIX = os.path.join(TMP, "access.log")
with open(FIX, "w", encoding="utf-8", newline="\n") as fh:
    fh.write(
        '10.0.0.1 - - [10/Oct/2026:12:00:00 +0000] "GET /index.html HTTP/1.1" 200 512 "-" "curl/8.0"\n'
        '10.0.0.1 - - [10/Oct/2026:12:00:01 +0000] "GET /style.css HTTP/1.1" 200 128 "-" "curl/8.0"\n'
        '10.0.0.5 - - [10/Oct/2026:12:00:02 +0000] "POST /api/login HTTP/1.1" 401 64 "-" "curl/8.0"\n'
        '10.0.0.5 - - [10/Oct/2026:12:00:03 +0000] "GET /missing HTTP/1.1" 404 0 "-" "curl/8.0"\n'
        '10.0.0.5 - - [10/Oct/2026:12:00:04 +0000] "GET /index.html HTTP/1.1" 200 512 "-" "curl/8.0"\n'
        "garbage line here\n")

p = run(CLI, "run", "-O0", LOGSTAT, "--", FIX)
check(p.returncode == 0, "run logstat exit=%d err=%s" % (p.returncode, (p.stdout + p.stderr)[-600:]))
for want in ["requests: 5", "bad lines: 1", "bytes: 1216",
             "  200: 3", "  401: 1", "  404: 1", "  /index.html: 2"]:
    check(want in p.stdout, "logstat wants %r have %r" % (want, p.stdout[-600:]))

p = run(CLI, "run", "-O0", LOGSTAT, "--", FIX)
check("requests: 5" in p.stdout, "logstat no-separator wants the report have %r" % p.stdout[-600:])

p = run(CLI, "run", "-O0", LOGSTAT, "--", os.path.join(TMP, "no-such.log"))
check(p.returncode == 1, "logstat missing file wants exit=1 have %d" % p.returncode)
check("cannot read" in (p.stdout + p.stderr), "logstat missing file wants the message have %r" % (p.stdout + p.stderr)[-300:])

UW = os.path.join(TMP, "uw.brat")
with open(UW, "w", encoding="utf-8", newline="\n") as fh:
    fh.write("fn main() -> i64 {\n  let v = 5\n  print(unwrap(v))\n  return 0\n}\n")
p = run(CLI, "run", "-O0", UW)
check(p.returncode == 1, "unwrap typed wants exit=1 have %d" % p.returncode)
check("`unwrap` wants Option or Result, got i64" in p.stderr, "unwrap static check have %r" % p.stderr[-300:])

UW_DYN = os.path.join(TMP, "uw_dyn.brat")
with open(UW_DYN, "w", encoding="utf-8", newline="\n") as fh:
    fh.write("fn main() {\n  let v = 5\n  print(unwrap(v))\n}\n")
p = run(CLI, "run", "-O0", UW_DYN)
check(p.returncode == 1, "unwrap dyn wants exit=1 have %d" % p.returncode)
check("unwrap of i64 (want Ok/Err/Some)" in p.stderr, "unwrap wants the type have %r" % p.stderr[-300:])


# Every test file the documentation names must exist. A doc that points at
# `test_checker_parity.py` when the tree has `test_cxx_checker.py` sends the
# reader to a dead end, and exactly that happened while writing the 2026-10-28
# notes.
# CHANGELOG.md is history: naming a test that has since been removed is the
# point of it. Everywhere else a dead test name is a bug unless the line says it
# is gone ("used to", "merged", "replaced").
PAST = ("used to", "merged", "replaced", "deleted", "renamed", "was ", "were ")
DOCS = [os.path.join(ROOT, n) for n in
        ("README.md", "AGENTS.md", "CONTRIBUTING.md", "SECURITY.md")]
DOCS.append(os.path.join(ROOT, "lang", "spec", "SPEC.md"))
DOCS.extend(sorted(glob.glob(os.path.join(ROOT, "docs", "*", "*.md"))))
missing = []
for doc in DOCS:
    if not os.path.isfile(doc) or os.path.basename(doc) == "CHANGELOG.md":
        continue
    for line in open(doc, encoding="utf-8"):
        if any(mark in line for mark in PAST):
            continue
        for name in set(re.findall(r"test_[a-z0-9_]+\.py", line)):
            if not os.path.isfile(os.path.join(HERE, name)):
                missing.append("%s -> %s" % (os.path.relpath(doc, ROOT), name))
check(not missing, "documentation names tests that do not exist: %s" % (missing,))

print("NEWCOMER ALL GREEN (%d asserts)" % n)
