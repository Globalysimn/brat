"""An arena owns the strings built inside it, and hands out copies, not views.

Before strings were refcounted, `join`, `split`, `repeat`, `replace` and the case
helpers returned `malloc` buffers, so a string built inside an arena block was heap
memory the arena never saw: the block's reset could not free it and `own()` had
nothing to copy out of. Now every string is a `balloc` value, so an arena block
captures string work too and a block can be reset in one move.

What this file pins: `run -O0` and the built binary print the same thing, the binary is clean
under AddressSanitizer and LeakSanitizer, `own(...)` hands out a value that survives
the reset, and the escape rule still refuses assigning an arena string to an outer
binding. The three timings at the end are a measurement, not a threshold: they are
printed so the next reader has numbers instead of an adjective, including the case
where the arena loses.
"""
import _cxx

import _scratch

import os
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-arenastr-")

# the body of a bench loop: five runtime-built strings per round
BODY = '''    let parts = "alpha,beta,gamma,delta".split(",")
    let joined = parts.join("|").upper()
    let marked = joined.replace("|", ":").repeat(1)
    n = n + len(marked) + len("x=y".split("=").join("->"))
'''

WORKLOAD = '''fn work(rounds: i64) -> i64 {
  let mut i = 0
  let mut n = 0
  while i < rounds {
    let parts = "alpha,beta,gamma,delta".split(",")
    let joined = parts.join("|").upper()
    let marked = joined.replace("|", ":").repeat(1)
    n = n + len(marked) + len("x=y".split("=").join("->"))
    i = i + 1
  }
  return n
}

fn main() -> i64 {
  let mut keep = ""
  arena {
    let n = work(200)
    keep = own("checksum {n}")
  }
  print(keep)
  print(work(3))
  return 0
}
'''

ESCAPE = '''fn main() -> i64 {
  let mut keep = ""
  arena {
    let s = "ab".repeat(3)
    keep = s
  }
  print(keep)
  return 0
}
'''


def bench_src(rounds, arena_pos):
    """arena_pos: "none", "round" (a block per round) or "all" (one block)."""
    body = BODY
    if arena_pos == "round":
        body = "    arena {\n" + "".join("  " + line + "\n" for line in BODY.split("\n") if line) + "    }\n"
    loop = "  while i < %d {\n%s    i = i + 1\n  }\n" % (rounds, body)
    if arena_pos == "all":
        loop = "  arena {\n" + "".join("  " + line + "\n" for line in loop.split("\n") if line) + "  }\n"
    return ("fn main() -> i64 {\n  let mut i = 0\n  let mut n = 0\n" + loop +
            '  print(n)\n  return 0\n}\n')


def write(name, src):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def emit(path):
    r = subprocess.run(_cxx.cli_args("emit-c", path), capture_output=True, text=True,
                       timeout=120)
    assert r.returncode == 0, "emit-c %s: %r" % (path, (r.stdout + r.stderr)[-600:])
    return r.stdout


def build(name, src, sanitize=False):
    path = write(name, src)
    cfile = os.path.join(TMP, name + ".c")
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(emit(path))
    exe = os.path.join(TMP, name + (".asan" if sanitize else ".exe"))
    cmd = ["cc"] + (["-fsanitize=address", "-g"] if sanitize else ["-O2"]) + \
          ["-I", ENGINE, cfile, "-o", exe, "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, "cc %s: %r" % (name, (r.stdout + r.stderr)[-600:])
    return path, exe


want = "checksum 5200\n78\n"

# 1. the program with one arena block: `run -O0` and a sanitized build, leak free
path, exe = build("arena", WORKLOAD, sanitize=True)
run_o0 = subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True, text=True,
                        timeout=300)
assert run_o0.returncode == 0 and run_o0.stdout == want, \
    "run_o0 rc=%d out=%r" % (run_o0.returncode, (run_o0.stdout + run_o0.stderr)[-600:])
run = subprocess.run([exe], capture_output=True, text=True, timeout=300,
                     env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=0"))
out = run.stdout + run.stderr
assert "AddressSanitizer" not in out, "arena program not clean: %s" % out[-1200:]
assert run.returncode == 0 and run.stdout == want, "native rc=%d out=%r" % (run.returncode, out[-600:])

# 2. own() is a copy: the value it returns outlives the reset
assert run_o0.stdout.splitlines()[0] == "checksum 5200", \
    "own() did not survive the arena reset: %r" % run_o0.stdout

# 3. the escape rule still refuses the shortcut, in `check` and in `build`
bad = write("escape", ESCAPE)
for tag, argv in (("check", _cxx.cli_args("check", bad)),
                  ("build", _cxx.cli_args("build", bad, "-o", os.path.join(TMP, "no.exe")))):
    r = subprocess.run(argv, capture_output=True, text=True, timeout=300)
    text = r.stdout + r.stderr
    assert r.returncode != 0, "%s accepted an arena value escaping: %r" % (tag, text[-400:])
    assert "a value made inside the block" in text, "%s wrong message: %r" % (tag, text[-400:])


def timed(src, name):
    """Best of three runs: wall seconds and peak RSS of the child, via wait4."""
    _p, exe = build(name, src)
    quiet = [(os.POSIX_SPAWN_OPEN, 1, os.devnull, os.O_WRONLY, 0o666),
             (os.POSIX_SPAWN_OPEN, 2, os.devnull, os.O_WRONLY, 0o666)]
    best = None
    for _ in range(3):
        start = time.perf_counter()
        pid = os.posix_spawn(exe, [exe], os.environ, file_actions=quiet)
        _pid, status, ru = os.wait4(pid, 0)
        assert status == 0, "%s exited with %d" % (name, status)
        wall, rss = time.perf_counter() - start, ru.ru_maxrss
        best = (wall, rss) if best is None or wall < best[0] else best
    return best


# 4. the measurement, at 50k rounds, three shapes of the same work
ROUNDS = 50000
sizes = {pos: timed(bench_src(ROUNDS, pos), "bench_" + pos) for pos in ("none", "round", "all")}
print("ARENA STRINGS ALL GREEN (-O0 and -O2, ASan+LSan clean, own() copies, escape refused)")
print("  measured, %d rounds, best of 3 (wall / peak RSS):" % ROUNDS)
for pos, label in (("none", "no arena      "), ("round", "arena per round"),
                   ("all", "one arena block")):
    wall, rss = sizes[pos]
    print("    %s %.3fs / %d KB" % (label, wall, rss))
