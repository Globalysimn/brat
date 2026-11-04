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
import resource
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

    mem_path = os.path.join(TMP, "regex_class_repeat_memory.brat")
    with open(mem_path, "w", encoding="utf-8") as f:
        f.write('fn main() -> i64 {\n'
                '  let pat = "[" + "a".repeat(200000) + "]{{1024}}"\n'
                '  print(re_match("a".repeat(1024), pat))\n'
                '  return 0\n'
                '}\n')
    mem_exe = os.path.join(TMP, "regex_class_repeat_memory.exe")
    b = run([sys.executable, CLI, "build", mem_path, "-o", mem_exe])
    if b.returncode != 0:
        print("FAIL regex_class_repeat_memory: build: %s" % ((b.stdout + b.stderr)[-400:],))
        bad += 1
    else:
        def limit_memory():
            resource.setrlimit(resource.RLIMIT_AS, (128 << 20, 128 << 20))

        try:
            r = subprocess.run([mem_exe], capture_output=True, text=True, timeout=60,
                               preexec_fn=limit_memory)
        except Exception as e:
            print("FAIL regex_class_repeat_memory: %r" % (e,))
            bad += 1
        else:
            if (r.returncode, r.stdout) != (0, "True\n"):
                print("FAIL regex_class_repeat_memory: %r/%r %r" % (r.returncode, r.stdout, r.stderr))
                bad += 1

    quad_path = os.path.join(TMP, "regex_no_match_scan.brat")
    with open(quad_path, "w", encoding="utf-8") as f:
        f.write('fn main() -> i64 {\n'
                '  print(re_match("a".repeat(50000), "a+b"))\n'
                '  return 0\n'
                '}\n')
    quad_exe = os.path.join(TMP, "regex_no_match_scan.exe")
    b = run([sys.executable, CLI, "build", quad_path, "-o", quad_exe])
    if b.returncode != 0:
        print("FAIL regex_no_match_scan: build: %s" % ((b.stdout + b.stderr)[-400:],))
        bad += 1
    else:
        for argv in ([sys.executable, CLI, "run", "--interp", quad_path], [quad_exe]):
            try:
                r = subprocess.run(argv, capture_output=True, text=True, timeout=10)
            except Exception as e:
                print("FAIL regex_no_match_scan: %r" % (e,))
                bad += 1
            else:
                if (r.returncode, r.stdout) != (0, "False\n"):
                    print("FAIL regex_no_match_scan: %r/%r %r" % (r.returncode, r.stdout, r.stderr))
                    bad += 1

    if bad:
        print("RESOURCE BOUNDS: %d case(s) failed" % bad)
        return 1
    print("RESOURCE BOUNDS OK (%d cases, both backends say the same thing)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
