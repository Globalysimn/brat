"""A socket handle is linear: closing it consumes it.

The runtime already refuses a stale slot (`tcp_close` twice returns
`("err", "bad socket slot")`), but a runtime error is something a program can
ignore. The checker is what makes double-close impossible to write: `tcp_close`
takes the handle, and a second close is a use of a moved value, reported before
the program runs, with the line of the first close.

What this does not do: follow the handle through another function. The check is
per function, like the rest of ownership (docs/guide/ownership.md).
"""
import _scratch

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
CXX = os.path.abspath(os.path.join(HERE, "..", "cxx", "bin", "brat-cxx"))
TMP = _scratch.mkdtemp(prefix="brat-sock-")

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def write(name, text):
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def run(argv):
    return subprocess.run([sys.executable, CLI] + argv, capture_output=True, text=True,
                          timeout=300)


DOUBLE = '''fn main() -> i64 {
  let l = tcp_listen("127.0.0.1:47371")
  match l {
    Ok(s) => {
      let a = tcp_close(s)
      let b = tcp_close(s)
      print(a)
      print(b)
    },
    Err(e) => { print("listen failed {e}") }
  }
  return 0
}
'''

path = write("double.brat", DOUBLE)
r = run(["check", path])
check(r.returncode == 1, "double close is an error, rc=%d" % r.returncode)
check("use of closed `s` (hint: the handle was closed; open a new one instead of "
      "reusing this)" in r.stdout, "message: %r" % r.stdout)
check(":5: note: `s` closed here" in r.stdout, "note line: %r" % r.stdout)
check(r.stdout.count("error:") == 1, "one error, not two: %r" % r.stdout)

# Using the handle after close is the same rule, not a special case.
RECV = '''fn main() -> i64 {
  let l = tcp_listen("127.0.0.1:47372")
  match l {
    Ok(s) => {
      let a = tcp_close(s)
      print(a)
      let r = tcp_recv(s, 16)
      print(r)
    },
    Err(e) => { print("listen failed {e}") }
  }
  return 0
}
'''
path = write("recv.brat", RECV)
r = run(["check", path])
check(r.returncode == 1 and "use of closed `s`" in r.stdout,
      "recv after close: rc=%d %r" % (r.returncode, r.stdout))

# Closing once is clean, and both backends agree on the result.
ONCE = '''fn main() -> i64 {
  let l = tcp_listen("127.0.0.1:47373")
  match l {
    Ok(s) => {
      let r = tcp_close(s)
      print(r)
    },
    Err(e) => { print("listen failed {e}") }
  }
  return 0
}
'''
path = write("once.brat", ONCE)
r = run(["check", path])
check(r.returncode == 0 and "0 error(s)" in r.stdout, "single close: %r" % r.stdout)

interp = run(["run", "--interp", path])
check(interp.returncode == 0 and interp.stdout == "('ok', 0)\n",
      "interp: rc=%d %r" % (interp.returncode, interp.stdout + interp.stderr))

exe = os.path.join(TMP, "once.exe")
built = run(["build", path, "-o", exe])
check(built.returncode == 0, "build: %r" % (built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
check(native.returncode == 0 and native.stdout == interp.stdout,
      "native: %r vs %r" % (native.stdout, interp.stdout))

BAD_TYPES = '''fn main() -> i64 {
  let l = tcp_listen("127.0.0.1:47377")
  match l {
    Ok(s) => {
      print(tcp_recv(s, 1.5))
      print(tcp_close(1.5))
      print(tcp_close(s))
    },
    Err(e) => { print(e) }
  }
  return 0
}
'''
path = write("bad_types.brat", BAD_TYPES)
interp = run(["run", "--interp", path])
exe = os.path.join(TMP, "bad_types.exe")
built = run(["build", path, "-o", exe])
check(built.returncode == 0, "bad socket types build: %r" % (built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
check(interp.returncode == 0 and native.returncode == 0
      and interp.stdout == native.stdout
      and "recv wants 0 <= n <= 1048576" in native.stdout
      and "bad socket slot" in native.stdout,
      "fractional counts/handles must be refused identically: %r vs %r"
      % (interp.stdout, native.stdout))

# The runtime is still the second line of defence: a program that builds today
# and closes twice through a call gets an error value, not a crash.
RUNTIME = '''fn close_twice(s: i64) -> i64 {
  let a = tcp_close(s)
  let b = tcp_close(s)
  if is_err(b) {
    match b {
      Ok(v) => { print("closed again") },
      Err(e) => { print("refused {e}") }
    }
  }
  return 0
}

fn main() -> i64 {
  let l = tcp_listen("127.0.0.1:47374")
  match l {
    Ok(s) => { print(close_twice(s)) },
    Err(e) => { print("listen failed {e}") }
  }
  return 0
}
'''
path = write("runtime.brat", RUNTIME)
r = run(["run", "--interp", path])
check(r.returncode == 0 and "refused bad socket slot" in r.stdout,
      "runtime guard: %r" % (r.stdout + r.stderr))

# A close on an error branch that returns does not make the close at the end a
# second close: only an arm that can fall through carries its moves onward.
BRANCH = '''fn serve(fd: i64, r: i64) -> i64 {
  let w = tcp_accept(fd)
  match w {
    Ok(c) => {
      let x = tcp_close(c)
      print(x)
    },
    Err(reason) => {
      tcp_close(fd)
      return 1
    }
  }
  let y = tcp_close(fd)
  print(y)
  return 0
}

fn main() -> i64 {
  let l = tcp_listen("127.0.0.1:47375")
  match l {
    Ok(s) => { print(serve(s, 0)) },
    Err(e) => { print("listen failed {e}") }
  }
  return 0
}
'''
path = write("branch.brat", BRANCH)
r = run(["check", path])
check(r.returncode == 0 and "0 error(s)" in r.stdout,
      "close on a returning branch then at the end: %r" % r.stdout)

RACE = '''fn main() -> i64 {
  let l = tcp_listen("127.0.0.1:47376")
  match l {
    Ok(s) => {
      spawn { tcp_recv(s, 1) }
      tcp_close(s)
      join_all()
    },
    Err(_) => {}
  }
  return 0
}
'''
path = write("close_race.brat", RACE)
r = run(["check", path])
check(r.returncode != 0 and "tcp" in r.stdout and "join_all" in r.stdout,
      "close racing a socket operation must be refused: %r" % r.stdout)
if os.path.isfile(CXX):
    native_check = subprocess.run([CXX, "check", path], capture_output=True,
                                  text=True, timeout=300)
    check(native_check.returncode != 0 and "tcp" in native_check.stdout
          and "join_all" in native_check.stdout,
          "C++ checker allowed socket close race: %r" % native_check.stdout)

print("SOCK LINEAR ALL GREEN (%d asserts)" % n)
