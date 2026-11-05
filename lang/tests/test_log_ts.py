"""Telemetry timestamps are wall clock in both backends.

`log.info` writes a JSON line with a `ts` field, and `time_ms()` returns epoch
milliseconds. A clock that starts at zero — CPU time since process start — is
the wrong clock: logs from two processes cannot be ordered, and a timestamp is
not comparable with anything outside the program.
"""
import _cxx
import _scratch

import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = _scratch.mkdtemp(prefix="brat-ts-")

SRC = '''fn main() -> i64 {
  log.info("first")
  print(time_ms())
  log.warn("second")
  return 0
}
'''

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def stamps(out):
    """Every JSON line from the program, parsed."""
    rows = []
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            rows.append(json.loads(line))
    return rows


def run(argv, env=None):
    return subprocess.run(argv, capture_output=True, text=True, timeout=300, env=env)


path = os.path.join(TMP, "ts.brat")
with open(path, "w", encoding="utf-8") as f:
    f.write(SRC)

before = time.time()
interp = run(_cxx.cli_args("run", "--interp", path))
after = time.time()
check(interp.returncode == 0, "run rc=%d %r" % (interp.returncode, interp.stderr))
rows = stamps(interp.stdout)
check(len(rows) == 2, "run: two log lines, got %r" % interp.stdout)
for r in rows:
    check(before - 5 <= r["ts"] <= after + 5,
          "run: ts %r is not wall clock around %r" % (r["ts"], before))
    check(r["lvl"] in ("INFO", "WARN"), "run: level %r" % r.get("lvl"))

ms = [int(line) for line in interp.stdout.splitlines() if line.isdigit()]
check(len(ms) == 1, "run: time_ms line, got %r" % interp.stdout)
check(abs(ms[0] - int(time.time() * 1000)) < 5000,
      "run: time_ms %d is not epoch ms" % ms[0])

exe = os.path.join(TMP, "ts.exe")
built = run(_cxx.cli_args("build", path, "-o", exe))
check(built.returncode == 0, "build: %r" % (built.stdout + built.stderr))
before = time.time()
native = run([exe])
after = time.time()
check(native.returncode == 0, "native rc=%d %r" % (native.returncode, native.stderr))
rows = stamps(native.stdout)
check(len(rows) == 2, "native: two log lines, got %r" % native.stdout)
for r in rows:
    check(before - 5 <= r["ts"] <= after + 5,
          "native: ts %r is not wall clock around %r" % (r["ts"], before))
    check(r["lvl"] in ("INFO", "WARN"), "native: level %r" % r.get("lvl"))

ms = [int(line) for line in native.stdout.splitlines() if line.isdigit()]
check(len(ms) == 1, "native: time_ms line, got %r" % native.stdout)
check(abs(ms[0] - int(time.time() * 1000)) < 5000,
      "native: time_ms %d is not epoch ms" % ms[0])

check(interp.stdout.count("\n") == native.stdout.count("\n"),
      "backends print a different number of lines")

print("LOG TS ALL GREEN (%d asserts)" % n)
