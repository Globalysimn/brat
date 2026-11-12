"""Regressions from differential fuzzing (vectors and maps).

1. A program with only warnings (here: an unused variable) must build. Before,
   `brat build` refused it because the gate treated any diagnostic as blocking.
2. A missing map key must say which key, on both backends. Native printed only
   `no key`, so the same program printed different output on each backend.
"""
import _cxx
import _scratch
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = _scratch.mkdtemp(prefix="brat-fuzzreg-")

SRC = r'''fn main() -> i64 {
    let mut unused = []
    let mut m = {"a": 28}
    m["zz"] = 1
    print(len(m))
    try {
        print(m["c"])
    } catch e {
        print(e)
    }
    return 0;
}
'''

WANT = b"2\nno key c\n"


def main():
    path = os.path.join(TMP, "reg.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(SRC)

    # (1) warnings alone must not refuse the build
    exe = os.path.join(TMP, "reg.exe")
    b = subprocess.run(_cxx.cli_args("build", path, "-o", exe),
                       capture_output=True, timeout=300)
    assert b.returncode == 0 and os.path.isfile(exe), \
        "build refused a warnings-only program:\n" + (b.stdout + b.stderr)[-600:].decode("utf-8", "replace")

    # (2) same output on both backends
    ri = subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True, timeout=120)
    assert ri.returncode == 0 and ri.stdout == WANT, "run_o0 rc=%d out=%r" % (ri.returncode, ri.stdout)
    rn = subprocess.run([exe], capture_output=True, timeout=60)
    assert rn.returncode == 0 and rn.stdout == WANT, "native rc=%d out=%r" % (rn.returncode, rn.stdout)
    print("FUZZ REGRESSIONS ALL GREEN (2 cases, 2 backends)")


if __name__ == "__main__":
    main()
