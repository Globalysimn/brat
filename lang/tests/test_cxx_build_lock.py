"""Two processes that both need the C++ front end build it one after the other, not side by side.

`_cxx.build_lock` is what `build()` holds while it compiles. Four child processes take the same
lock and log when they enter and leave it; the log must read in, out, in, out, with one pid per pair.
Then `build()` itself must hold that lock while it compiles, and not compile a current binary.
"""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CHILD = """
import os, sys, time
sys.path.insert(0, %r)
import _cxx
lock, log = sys.argv[1:3]
with _cxx.build_lock(lock):
    with open(log, "a") as f:
        f.write("in %%d\\n" %% os.getpid())
    time.sleep(0.2)
    with open(log, "a") as f:
        f.write("out %%d\\n" %% os.getpid())
""" % HERE

tmp = _scratch.mkdtemp(prefix="brat-build-lock-")
lock, log = os.path.join(tmp, "lock"), os.path.join(tmp, "log")
kids = [subprocess.Popen([sys.executable, "-c", CHILD, lock, log]) for _ in range(4)]
for k in kids:
    assert k.wait(timeout=60) == 0

events = [line.split() for line in open(log).read().splitlines()]
assert len(events) == 8, events
for i in range(0, 8, 2):
    (a, pa), (b, pb) = events[i], events[i + 1]
    assert (a, b) == ("in", "out") and pa == pb, "locks overlap: %r" % events
assert len({events[i][1] for i in range(0, 8, 2)}) == 4, events

if _cxx.fcntl:
    held = []

    def compile_stub(cc):
        with open(_cxx.BIN + ".lock", "w") as other:
            try:
                _cxx.fcntl.flock(other, _cxx.fcntl.LOCK_EX | _cxx.fcntl.LOCK_NB)
                held.append(False)
            except BlockingIOError:
                held.append(True)

    _cxx.BIN = os.path.join(tmp, "bin", "brat-cxx")
    _cxx.compile_all = compile_stub
    _cxx.build()
    assert held == [True], "build() compiled without holding the lock: %r" % held
    os.utime(_cxx.BIN + ".lock", None)
    with open(_cxx.BIN, "w"):
        pass
    os.utime(_cxx.BIN, (os.path.getmtime(_cxx.BIN) + 3600,) * 2)
    _cxx.build()
    assert held == [True], "build() compiled although the binary was current"

print("CXX BUILD LOCK ALL GREEN (%d events)" % len(events))
