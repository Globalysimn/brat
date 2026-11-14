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

# The benchmark tables say "Median of N runs": `make bench` has to run N.
bench_src = open(os.path.join(ROOT, "scripts", "bench.py"), encoding="utf-8").read()
bench_runs = re.search(r'"--runs", type=int, default=(\d+)', bench_src)
check(bench_runs, "bench.py lost its --runs option")
limits_md = open(os.path.join(ROOT, "docs", "guide", "limits.md"), encoding="utf-8").read()
for label, text in (("README", readme), ("limits.md", limits_md)):
    check("Median of %s runs" % bench_runs.group(1) in text,
          "%s names a different run count than bench.py (default %s)" % (label, bench_runs.group(1)))

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
check("string literal looks cut short" in p.stderr, "cut string wants the error %r" % p.stderr[-400:])
check(p.stderr.count("cut short") == 1, "cut string is reported once, have %r" % p.stderr[-600:])

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


# Every test file the documentation names must exist, unless the line says the
# test is gone ("used to", "merged", "replaced").
PAST = ("used to", "merged", "replaced", "deleted", "renamed", "was ", "were ")
DOCS = [os.path.join(ROOT, n) for n in ("README.md", "CONTRIBUTING.md", "SECURITY.md")]
DOCS.append(os.path.join(ROOT, "lang", "spec", "SPEC.md"))
DOCS.extend(sorted(glob.glob(os.path.join(ROOT, "docs", "*", "*.md"))))
SOURCES = {os.path.basename(p) for p in glob.glob(os.path.join(ROOT, "lang", "cxx", "src", "*.cpp"))}
missing = []
for doc in DOCS:
    for line in open(doc, encoding="utf-8"):
        if any(mark in line for mark in PAST):
            continue
        for name in set(re.findall(r"test_[a-z0-9_]+\.py", line)):
            if not os.path.isfile(os.path.join(HERE, name)):
                missing.append("%s -> %s" % (os.path.relpath(doc, ROOT), name))
        for name in set(re.findall(r"\b[a-z_]+\.cpp\b", line)):
            if name not in SOURCES:
                missing.append("%s -> %s" % (os.path.relpath(doc, ROOT), name))
check(not missing, "documentation names files that do not exist: %s" % (missing,))

# Every command row of the CLI reference names a command the binary has, and
# every flag in the row is one that command accepts.
cli_doc = open(os.path.join(ROOT, "docs", "guide", "cli.md"), encoding="utf-8").read()
rows = re.findall(r"^\| `(brat [^`]+)` \|", cli_doc, re.M)
check(rows, "cli.md has no command rows")
UNKNOWN = re.compile(r"unknown (flag|option|command|subcommand)")
for row in rows:
    sub = row.split()[1]
    p = subprocess.run(_cxx.cli_args(sub, "--help"), capture_output=True, text=True, timeout=60, env=ENV, cwd=TMP)
    check(not UNKNOWN.search(p.stdout + p.stderr), "cli.md documents `brat %s`, which does not exist" % sub)
    for flag in sorted(set(re.findall(r"(?<![\w-])(--[a-z][a-z-]*|-[A-Za-z])(?![\w-])", row))):
        p = subprocess.run(_cxx.cli_args(sub, flag, "x"), capture_output=True, text=True, timeout=60, env=ENV, cwd=TMP)
        check(not UNKNOWN.search(p.stdout + p.stderr), "cli.md documents `brat %s %s`, which it rejects" % (sub, flag))

# The diagnostics the ownership table quotes are ones the compiler prints. A row that
# shows one instance of a message built from names, not the message, is listed here.
INSTANCES = ("impl Shout for Point missing loud",)
compiler = "".join(open(p, encoding="utf-8").read() for p in glob.glob(os.path.join(ROOT, "lang", "cxx", "src", "*.cpp")))
ownership = open(os.path.join(ROOT, "docs", "guide", "ownership.md"), encoding="utf-8").read()
rules = ownership[ownership.index("## Checked rules"):ownership.index("### Call-site exclusivity")]
for row in rules.split("\n"):
    if not row.startswith("| ") or row.startswith("| Condition") or row.startswith("|---"):
        continue
    message = row.strip("| ").split(" | ")[-1].replace("\\`", "\0")
    for quoted in re.findall(r"`([^`]+)`", message):
        quoted = quoted.replace("\0", "`")
        if quoted in INSTANCES:
            continue
        for part in re.split(r"`[^`]*`|\.\.\.", quoted):
            if len(part.strip()) >= 8:
                check(part.strip() in compiler, "ownership.md quotes a diagnostic the compiler does not print: %r" % part.strip())

# Every `brat ...` command line in the documents takes the flags it is shown with.
shapes = {}
command = re.compile(r"(?:\./bin/brat|\./lang/cxx/bin/brat-cxx|brat-cxx|(?<![\w/.-])brat)\s+(%s)((?:\s+[^\s`|]+)*)"
                     % "|".join(sorted(_cxx._CLI_SUBS, key=len, reverse=True)))
for doc in DOCS:
    for lineno, line in enumerate(open(doc, encoding="utf-8"), 1):
        for m in command.finditer(line):
            lead = (m.group(1),) + tuple(m.group(2).split()[:1] if m.group(1) == "pkg" else ())
            for flag in re.findall(r"(?<![\w-])(--[a-z][\w-]*)", m.group(2)):
                shapes.setdefault((lead, flag), "%s:%d" % (os.path.relpath(doc, ROOT), lineno))
check(shapes, "no command line with a flag found in the documents")
for (lead, flag), where in sorted(shapes.items()):
    p = subprocess.run(_cxx.cli_args(*lead, flag, "x"), capture_output=True, text=True, timeout=60, env=ENV, cwd=TMP)
    check(not UNKNOWN.search(p.stdout + p.stderr), "%s shows `brat %s %s`, which is rejected" % (where, " ".join(lead), flag))

# The environment variables the code reads are exactly the ones the reference lists.
read = set()
for pattern in ("cxx/src/*.cpp", "cxx/src/*.h", "engine/crt/*.h"):
    for path in glob.glob(os.path.join(ROOT, "lang", pattern)):
        read.update(re.findall(r'getenv\(\s*"(BRAT_[A-Z0-9_]+)"', open(path, encoding="utf-8").read()))
listed = set(re.findall(r"^\| `(BRAT_[A-Z0-9_]+)` \|", cli_doc, re.M))
check(read == listed, "read but not listed in cli.md: %s; listed but never read: %s"
      % (sorted(read - listed), sorted(listed - read)))

# The methods builtins.md lists are the ones the checker knows the size of.
checker = open(os.path.join(ROOT, "lang", "cxx", "src", "check.cpp"), encoding="utf-8").read()
table = checker[checker.index("builtin_arity = {"):]
known = set(re.findall(r'\{"([a-z_]+)", \{', table[:table.index("};")]))
builtins_md = open(os.path.join(ROOT, "docs", "guide", "builtins.md"), encoding="utf-8").read()
listed_methods = set(re.findall(r"`[a-z]+\.([a-z_]+)\(", builtins_md))
check(known == listed_methods, "methods only in check.cpp: %s; only in builtins.md: %s"
      % (sorted(known - listed_methods), sorted(listed_methods - known)))

# The keyword list of the specification is the lexer's.
lexer = open(os.path.join(ROOT, "lang", "cxx", "src", "lexer.cpp"), encoding="utf-8").read()
table = lexer[lexer.index("words[] = {"):]
keywords = set(re.findall(r'"([a-z]+)"', table[:table.index("};")]))
spec = open(os.path.join(ROOT, "lang", "spec", "SPEC.md"), encoding="utf-8").read()
listed_words = set(re.findall(r"`([a-z]+)`", spec[spec.index("- Keywords:"):spec.index(" `struct`, `scope`")]))
check(keywords == listed_words, "SPEC.md keywords differ from the lexer: only in the lexer %s, only in SPEC.md %s"
      % (sorted(keywords - listed_words), sorted(listed_words - keywords)))

# The numbers the documents quote are the ones the code compiles in.
def constant(path, pattern):
    return re.search(pattern, open(os.path.join(ROOT, "lang", path), encoding="utf-8").read()).group(1)


pool = constant("engine/crt/conc.h", r"#define BPOOL_MAX (\d+)")
chan_cap = int(constant("engine/crt/conc.h", r"#define BRAT_MAX_CHAN_CAP (\d+)"))
nest = constant("cxx/src/parser.cpp", r"kMaxNest = (\d+)")
height = constant("cxx/src/parser.cpp", r"kMaxTreeDepth = (\d+)")
ledger = open(os.path.join(ROOT, "docs", "project", "LEDGER.md"), encoding="utf-8").read()
limits = open(os.path.join(ROOT, "docs", "guide", "limits.md"), encoding="utf-8").read()
check("at most %s OS threads" % pool in ledger, "LEDGER.md does not give the pool size %s" % pool)
check("`cap <= %s`" % format(chan_cap, ",") in ledger, "LEDGER.md does not give the channel limit %d" % chan_cap)
check(re.search(r"BRAT_THREADS.*at most %s\." % pool, cli_doc), "cli.md does not give the pool size %s" % pool)
arena_md = open(os.path.join(ROOT, "docs", "design", "ARENA.md"), encoding="utf-8").read()
kept_chunks = constant("engine/crt/core.h", r"#define BARENA_POOL_CHUNKS (\d+)")
kept_kib = int(constant("engine/crt/core.h", r"#define BARENA_POOL_BYTES \(\(size_t\)(\d+) \* 1024\)"))
chunk_kib = int(constant("engine/crt/core.h", r"#define BARENA_CHUNK \(\(size_t\)(\d+) \* 1024\)"))
value_depth = constant("engine/crt/core.h", r"#define BRAT_MAX_VALUE_DEPTH (\d+)")
check("(with a %s-level recursion guard)" % value_depth in arena_md, "ARENA.md does not give the own() depth %s" % value_depth)
check("same %s-level nesting cap" % value_depth in open(os.path.join(ROOT, "docs", "guide", "limits.md"), encoding="utf-8").read(),
      "limits.md does not give the value depth %s" % value_depth)
check("up to %s chunks (`%d KiB`)" % (kept_chunks, kept_kib) in arena_md, "ARENA.md does not give the kept pool")
check("at least\n  %d KiB" % chunk_kib in arena_md, "ARENA.md does not give the chunk size %d KiB" % chunk_kib)
check("nested %s levels deep" % nest in limits, "limits.md does not give the nesting limit %s" % nest)
check("more than %s levels tall" % height in limits, "limits.md does not give the tree limit %s" % height)

print("NEWCOMER ALL GREEN (%d asserts)" % n)
