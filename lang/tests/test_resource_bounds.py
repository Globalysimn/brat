"""Resource bounds on channel capacities (`BRAT_MAX_CHAN_CAP`) and regex memory."""
import _cxx
import _scratch
import os
import resource
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
TMP = _scratch.mkdtemp(prefix="brat-bounds-")

CASES = {
    # name: (source, expected exit code, expected stdout)
    "chan_over_cap": ("""let ch = chan(1048577)
print(ch)
""", 0, "Err('chan: capacity too large')\n"),
    "chan_at_cap": ("""let ch = chan(1048576)
ch.send(1)
print(ch.recv())
""", 0, "1\n"),
    "chan_zero": ("""print(chan(0))
""", 0, "Err('chan: capacity must be at least 1')\n"),
    "chan_negative": ("""print(chan(-5))
""", 0, "Err('chan: capacity must be at least 1')\n"),
    "chan_small_roundtrip": ("""let ch = chan(2)
ch.send("a")
ch.send("b")
print(ch.recv())
print(ch.recv())
ch.close()
print(ch.recv())
""", 0, "a\nb\nNone\n"),
    # Groups nest 200 deep; the pattern compiler recurses once per level.
    "regex_nesting_at_limit": ("""fn main() -> i64 {
  print(re_match("xay", "(".repeat(200) + "a" + ")".repeat(200)))
  return 0
}
""", 0, "true\n"),
    "regex_nesting_over_limit": ("""fn main() -> i64 {
  for n in [201, 20000] {
    try {
      print(re_match("xay", "(".repeat(n) + "a" + ")".repeat(n)))
    } catch e {
      print(e)
    }
  }
  print(re_match("xay", "(a)".repeat(300) + "|a"))
  return 0
}
""", 0, "bad regex pattern\nbad regex pattern\ntrue\n"),
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
        b = run(_cxx.cli_args("build", path, "-o", exe))
        if b.returncode != 0:
            print("FAIL %s: build: %s" % (name, (b.stdout + b.stderr)[-400:]))
            bad += 1
            continue
        nat = run([exe])
        run_o0 = run(_cxx.run_args(path))
        for tag, r in (("native", nat), ("-O0", run_o0)):
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
    b = run(_cxx.cli_args("build", mem_path, "-o", mem_exe))
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
            if (r.returncode, r.stdout) != (0, "true\n"):
                print("FAIL regex_class_repeat_memory: %r/%r %r" % (r.returncode, r.stdout, r.stderr))
                bad += 1

    # The same refusal on a 1 MiB stack, the Windows default.
    deep_exe = os.path.join(TMP, "regex_nesting_over_limit.exe")
    if os.path.exists(deep_exe):
        def limit_stack():
            resource.setrlimit(resource.RLIMIT_STACK, (1 << 20, 1 << 20))

        r = subprocess.run([deep_exe], capture_output=True, text=True, timeout=60,
                           preexec_fn=limit_stack)
        if (r.returncode, r.stdout) != (0, CASES["regex_nesting_over_limit"][2]):
            print("FAIL regex_nesting_small_stack: %r/%r %r" % (r.returncode, r.stdout, r.stderr[-300:]))
            bad += 1

    quad_path = os.path.join(TMP, "regex_no_match_scan.brat")
    with open(quad_path, "w", encoding="utf-8") as f:
        f.write('fn main() -> i64 {\n'
                '  print(re_match("a".repeat(50000), "a+b"))\n'
                '  return 0\n'
                '}\n')
    quad_exe = os.path.join(TMP, "regex_no_match_scan.exe")
    b = run(_cxx.cli_args("build", quad_path, "-o", quad_exe))
    if b.returncode != 0:
        print("FAIL regex_no_match_scan: build: %s" % ((b.stdout + b.stderr)[-400:],))
        bad += 1
    else:
        for argv in (_cxx.cli_args("run", "-O0", quad_path), [quad_exe]):
            try:
                r = subprocess.run(argv, capture_output=True, text=True, timeout=10)
            except Exception as e:
                print("FAIL regex_no_match_scan: %r" % (e,))
                bad += 1
            else:
                if (r.returncode, r.stdout) != (0, "false\n"):
                    print("FAIL regex_no_match_scan: %r/%r %r" % (r.returncode, r.stdout, r.stderr))
                    bad += 1

    if bad:
        print("RESOURCE BOUNDS: %d case(s) failed" % bad)
        return 1
    print("RESOURCE BOUNDS OK (%d cases, -O0 and -O2 say the same thing)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
