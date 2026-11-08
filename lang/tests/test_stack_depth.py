"""A compiled program whose recursion budget outlasts its stack gets `recursion too deep`, not SIGSEGV.

The budget counts BRAT frames; the C stack ends after somewhere between 10,000 and 20,000 of them at
8 MiB. With `BRAT_RECURSION_BUDGET=1000000` the budget used to be spent only after the stack was
gone, so the process died with SIGSEGV out of reach of `try`. `bdepth_push` now also refuses when
the thread's stack is within 256 KiB of its end, and every thread works that out for itself.

The stack limit is set to 8 MiB for the child whatever the harness runs under.
"""
import _cxx
import _skip
import _scratch

import os
import subprocess

if os.name != "nt":
    import resource
else:
    resource = None

NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-stack-depth-")
EIGHT_MIB = 8 << 20

if resource is not None:
    hard = resource.getrlimit(resource.RLIMIT_STACK)[1]
    if hard != resource.RLIM_INFINITY and hard < EIGHT_MIB:
        _skip.skip("the hard stack limit is below 8 MiB")
else:
    hard = None

DEPTH = """fn depth(n) {
  if n == 0 {
    return 0
  }
  return 1 + depth(n - 1)
}

fn depth_typed(n: i64) -> i64 {
  if n == 0 {
    return 0
  }
  return 1 + depth_typed(n - 1)
}

"""
PROGRAMS = {
    "main thread": (DEPTH + """fn main() -> i64 {
  try {
    print(depth(900000))
  } catch e {
    print(e)
  }
  try {
    print(depth_typed(900000))
  } catch e {
    print(e)
  }
  print(depth(1000))
  print(depth_typed(1000))
  return 0
}
""", "recursion too deep\nrecursion too deep\n1000\n1000\n"),
    "spawned thread": (DEPTH + """let ch = chan(3)
spawn {
  try {
    ch.send(depth(900000))
  } catch e {
    ch.send(e)
  }
  try {
    ch.send(depth_typed(900000))
  } catch e2 {
    ch.send(e2)
  }
}
join_all()
print(ch.recv())
print(ch.recv())
print(depth(500))
""", "recursion too deep\nrecursion too deep\n500\n"),
}


def small_stack():
    resource.setrlimit(resource.RLIMIT_STACK, (EIGHT_MIB, hard))


n = 0
for label, (source, want) in PROGRAMS.items():
    path = os.path.join(TMP, label.replace(" ", "_") + ".brat")
    with open(path, "w") as f:
        f.write(source)
    run_kw = dict(capture_output=True, text=True, timeout=120,
                  env=dict(os.environ, BRAT_RECURSION_BUDGET="1000000"))
    if resource is not None:
        run_kw["preexec_fn"] = small_stack
    done = subprocess.run([NATIVE, "run", path], **run_kw)
    assert done.returncode == 0, "%s: exit %d (%s): %r" % (label, done.returncode,
                                                          "SIGSEGV" if done.returncode in (-11, 139) else "?",
                                                          done.stderr[-200:])
    assert done.stdout == want, "%s: %r, want %r" % (label, done.stdout, want)
    n += 2

print("STACK DEPTH ALL GREEN (%d asserts)" % n)
