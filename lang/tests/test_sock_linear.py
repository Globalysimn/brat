"""A socket handle is linear: closing it consumes it.

The runtime already refuses a stale slot (`tcp_close` twice returns
`("err", "bad socket slot")`), but a runtime error is something a program can
ignore. The checker is what makes double-close impossible to write: `tcp_close`
takes the handle, and a second close is a use of a moved value, reported before
the program runs, with the line of the first close.

What this does not do: follow the handle through another function. The check is
per function, like the rest of ownership (docs/guide/ownership.md).
"""
import _cxx
import _scratch

import os
import re
import socket
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
CXX = os.path.abspath(os.path.join(HERE, "..", "cxx", "bin", "brat-cxx"))
TMP = _scratch.mkdtemp(prefix="brat-sock-")

n = 0
_next_port = 19400


def _alloc_port():
    global _next_port
    while _next_port < 30000:
        cand = _next_port
        _next_port += 1
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind(("127.0.0.1", cand))
                return cand
            except OSError:
                continue
    raise RuntimeError("no free local port in 19400..29999")


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def write(name, text):
    port_map = {}
    def _sub(m):
        old = m.group(1)
        if old not in port_map:
            port_map[old] = str(_alloc_port())
        return "127.0.0.1:" + port_map[old]
    text = re.sub(r"127\.0\.0\.1:(473[78]\d)", _sub, text)
    path = os.path.join(TMP, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def run(argv):
    return subprocess.run(_cxx.cli_args(*(argv)), capture_output=True, text=True,
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

# Closing once is clean, and -O0 and -O2 agree on the result.
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

run_o0 = run(["run", "-O0", path])
check(run_o0.returncode == 0 and run_o0.stdout == "Ok(0)\n",
      "run_o0: rc=%d %r" % (run_o0.returncode, run_o0.stdout + run_o0.stderr))

exe = os.path.join(TMP, "once.exe")
built = run(["build", path, "-o", exe])
check(built.returncode == 0, "build: %r" % (built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
check(native.returncode == 0 and native.stdout == run_o0.stdout,
      "native: %r vs %r" % (native.stdout, run_o0.stdout))

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
run_o0 = run(["run", "-O0", path])
exe = os.path.join(TMP, "bad_types.exe")
built = run(["build", path, "-o", exe])
check(built.returncode == 0, "bad socket types build: %r" % (built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
check(run_o0.returncode == 0 and native.returncode == 0
      and run_o0.stdout == native.stdout
      and "recv wants 0 <= n <= 1048576" in native.stdout
      and "bad socket slot" in native.stdout,
      "fractional counts/handles must be refused identically: %r vs %r"
      % (run_o0.stdout, native.stdout))

# A stale integer copy must not alias the next socket. Native table slots are
# reused, so the public handle has to be fresh every time.
STALE = '''fn close_it(s: i64) -> i64 {
  print(tcp_close(s))
  return 0
}

fn main() -> i64 {
  let first = tcp_listen("127.0.0.1:47378")
  match first {
    Ok(s) => {
      let stale = s + 0
      print(tcp_close(s))
      let second = tcp_listen("127.0.0.1:47379")
      match second {
        Ok(t) => {
          print(t)
          close_it(stale)
          print(tcp_close(t))
        },
        Err(e) => { print(e) }
      }
    },
    Err(e) => { print(e) }
  }
  return 0
}
'''
path = write("stale.brat", STALE)
want_stale = "Ok(0)\n2\nErr('bad socket slot')\nOk(0)\n"
run_o0 = run(["run", "-O0", path])
check(run_o0.returncode == 0 and run_o0.stdout == want_stale,
      "stale handle run_o0: rc=%d %r" % (run_o0.returncode, run_o0.stdout + run_o0.stderr))
exe = os.path.join(TMP, "stale.exe")
built = run(["build", path, "-o", exe])
check(built.returncode == 0, "stale handle build: %r" % (built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
check(native.returncode == 0 and native.stdout == want_stale,
      "stale handle native: rc=%d %r" % (native.returncode, native.stdout + native.stderr))

MANY = '''fn main() -> i64 {
  let l = tcp_listen("127.0.0.1:47380")
  match l {
    Ok(s) => {
      let mut clients = []
      let mut servers = []
      for i in 0..50 {
        let c = tcp_connect("127.0.0.1:47380")
        match c {
          Ok(cf) => { clients.push(cf) },
          Err(e) => {
            print(e)
            return 1
          }
        }
        let a = tcp_accept(s)
        match a {
          Ok(af) => { servers.push(af) },
          Err(e) => {
            print(e)
            return 1
          }
        }
      }
      print(len(clients) + len(servers) + 1)
      for cf in clients {
        tcp_close(cf)
      }
      for af in servers {
        tcp_close(af)
      }
      tcp_close(s)
      print("done")
    },
    Err(e) => { print(e) }
  }
  return 0
}
'''
path = write("many.brat", MANY)
want_many = "101\ndone\n"
run_o0 = run(["run", "-O0", path])
check(run_o0.returncode == 0 and run_o0.stdout == want_many,
      "many sockets run_o0: rc=%d %r" % (run_o0.returncode, run_o0.stdout + run_o0.stderr))
exe = os.path.join(TMP, "many.exe")
built = run(["build", path, "-o", exe])
check(built.returncode == 0, "many sockets build: %r" % (built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
check(native.returncode == 0 and native.stdout == want_many,
      "many sockets native: rc=%d %r" % (native.returncode, native.stdout + native.stderr))

CLOSE_RACE_RUNTIME = '''fn recv_it(s: i64, ready: i64) -> i64 {
  ready.send(1)
  print(tcp_recv(s, 16))
  return 0
}

fn close_it(s: i64) -> i64 {
  print(tcp_close(s))
  return 0
}

fn main() -> i64 {
  let ready = chan(1)
  let l = tcp_listen("127.0.0.1:47381")
  match l {
    Ok(s) => {
      let c = tcp_connect("127.0.0.1:47381")
      match c {
        Ok(cf) => {
          let a = tcp_accept(s)
          match a {
            Ok(af) => {
              let stale = cf + 0
              spawn {
                recv_it(cf, ready)
              }
              print(ready.recv())
              sleep_ms(100)
              close_it(stale)
              print(tcp_send(af, "x"))
              join_all()
              tcp_close(af)
            },
            Err(e) => { print(e) }
          }
        },
        Err(e) => { print(e) }
      }
      tcp_close(s)
    },
    Err(e) => { print(e) }
  }
  print("done")
  return 0
}
'''
path = write("close_race_runtime.brat", CLOSE_RACE_RUNTIME)
def valid_close_race_output(output):
    # The main thread prints close before send; the receiving thread can wake
    # before either print. Assert that partial order, not a scheduler choice.
    lines = output.splitlines()
    close, send, recv = "Ok(0)", "Ok(1)", "Err('recv failed')"
    return (len(lines) == 5 and lines[0] == "1" and lines[-1] == "done"
            and sorted(lines[1:-1]) == sorted([close, send, recv])
            and lines.index(close) < lines.index(send))


for middle in (("Ok(0)", "Ok(1)", "Err('recv failed')"),
               ("Ok(0)", "Err('recv failed')", "Ok(1)"),
               ("Err('recv failed')", "Ok(0)", "Ok(1)")):
    check(valid_close_race_output("1\n" + "\n".join(middle) + "\ndone\n"),
          "valid close/recv scheduling rejected")
check(not valid_close_race_output("1\nOk(1)\nOk(0)\nErr('recv failed')\ndone\n"),
      "main-thread send-before-close must be rejected")
check(not valid_close_race_output("1\nOk(0)\nOk(1)\nOk('x')\ndone\n"),
      "a successful stale receive must be rejected")

run_o0 = run(["run", "-O0", path])
check(run_o0.returncode == 0 and valid_close_race_output(run_o0.stdout),
      "close race run_o0: rc=%d %r" % (run_o0.returncode, run_o0.stdout + run_o0.stderr))
exe = os.path.join(TMP, "close_race_runtime.exe")
built = run(["build", path, "-o", exe])
check(built.returncode == 0, "close race build: %r" % (built.stdout + built.stderr))
native = subprocess.run([exe], capture_output=True, text=True, timeout=60)
check(native.returncode == 0 and valid_close_race_output(native.stdout),
      "close race native: rc=%d %r" % (native.returncode, native.stdout + native.stderr))

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
r = run(["run", "-O0", "--no-check", path])
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
