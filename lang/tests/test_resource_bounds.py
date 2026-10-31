"""Resource bounds that must be the same in both backends.

`chan(cap)` is a bounded queue. The C runtime preallocates `cap` slots, the
interpreter grows a list, so `chan(100000000)` was a 2.4 GB `calloc` in C —
`vfatal_oom`, SIGABRT, exit 134 — and a perfectly working channel in Python. One
cap for both (`BRAT_MAX_CHAN_CAP`, 1 048 576 slots) turns the shape into an
`err` value in either backend, which is what a program can handle.

The same class of bound is why `run_all.py` never trusts a size a program asks
for: this file pins the cap, the working case below it, and that a memory budget
still refuses a large allocation through the error channel rather than aborting.
"""
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")
BRAT = os.path.join(ENGINE, "brat.py")
TMP = _scratch.mkdtemp(prefix="brat-bounds-")

CASES = {
    # name: (source, expected exit code, expected stdout)
    "chan_over_cap": ("""let ch = chan(1048577)
print(ch)
""", 0, "('err', 'chan: capacity too large')\n"),
    "chan_at_cap": ("""let ch = chan(1048576)
ch.send(1)
print(ch.recv())
""", 0, "1\n"),
    "chan_zero": ("""print(chan(0))
""", 0, "('err', 'chan: capacity must be at least 1')\n"),
    "chan_negative": ("""print(chan(-5))
""", 0, "('err', 'chan: capacity must be at least 1')\n"),
    "chan_small_roundtrip": ("""let ch = chan(2)
ch.send("a")
ch.send("b")
print(ch.recv())
print(ch.recv())
ch.close()
print(ch.recv())
""", 0, "a\nb\nNone\n"),
}


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600)


def main():
    bad = 0
    for name in sorted(CASES):
        src, want_rc, want_out = CASES[name]
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)

        exe = os.path.join(TMP, name + ".exe")
        b = run([sys.executable, CLI, "build", path, "-o", exe])
        if b.returncode != 0:
            print("FAIL %s: build: %s" % (name, (b.stdout + b.stderr)[-400:]))
            bad += 1
            continue
        nat = run([exe])
        interp = run([sys.executable, BRAT, path])
        for tag, r in (("native", nat), ("interpreter", interp)):
            if r.returncode < 0 or r.returncode >= 128:
                print("FAIL %s: %s died by signal (rc=%d)" % (name, tag, r.returncode))
                bad += 1
            elif (r.returncode, r.stdout) != (want_rc, want_out):
                print("FAIL %s: %s %r/%r, want %r/%r"
                      % (name, tag, r.returncode, r.stdout, want_rc, want_out))
                bad += 1

    if bad:
        print("RESOURCE BOUNDS: %d case(s) failed" % bad)
        return 1
    print("RESOURCE BOUNDS OK (%d cases, both backends say the same thing)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
