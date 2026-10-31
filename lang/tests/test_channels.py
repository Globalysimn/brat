"""Channels and `scope`: a value has one holder at a time, and a block joins.

Every case runs on both backends and the two stdouts must match byte for byte.
The memory cases go through AddressSanitizer with leak detection on, because a
channel that keeps a value nobody reads looks exactly like a working program.
"""
import _scratch

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")
TMP = _scratch.mkdtemp(prefix="brat-chan-")

CASES = {}

CASES["roundtrip"] = '''fn main() -> i64 {
  let ch = chan(2)
  scope {
    spawn {
      ch.send("one")
      ch.send("two")
      ch.close()
    }
  }
  print(ch.recv())
  print(ch.recv())
  print(ch.recv())
  return 0
}
'''

CASES["pipeline"] = '''fn produce(ch: i64, n: i64) -> i64 {
  let mut i = 0
  while i < n {
    ch.send(Ok("item {i}"))
    i = i + 1
  }
  ch.send(Err("done"))
  ch.close()
  return 0
}

fn main() -> i64 {
  let ch = chan(4)
  scope {
    spawn {
      produce(ch, 100)
    }
    spawn {
      let mut go = true
      let mut n = 0
      while go {
        match ch.recv() {
          Ok(s) => { n = n + len(s) },
          Err(_) => { go = false }
        }
      }
      print(n)
    }
  }
  print("joined")
  return 0
}
'''

CASES["backpressure"] = '''fn main() -> i64 {
  let ch = chan(1)
  scope {
    spawn {
      let mut i = 0
      while i < 50 {
        ch.send(Ok(i))
        i = i + 1
      }
      ch.send(Err("done"))
      ch.close()
    }
    spawn {
      let mut total = 0
      let mut go = true
      while go {
        match ch.recv() {
          Ok(_) => { total = total + 1 },
          Err(_) => { go = false }
        }
      }
      print(total)
    }
  }
  return 0
}
'''

CASES["nested_scope"] = '''fn main() -> i64 {
  let ch = chan(8)
  scope {
    spawn {
      spawn {
        ch.send("nested")
      }
    }
    spawn {
      ch.send("direct")
    }
  }
  let mut n = 0
  n = n + len(ch.recv())
  n = n + len(ch.recv())
  print(n)
  return 0
}
'''

CASES["return_joins"] = '''fn early(ch: i64) -> i64 {
  scope {
    spawn {
      ch.send("from child")
    }
    return 7
  }
  return 0
}

fn main() -> i64 {
  let ch = chan(2)
  print(early(ch))
  print(ch.recv())
  return 0
}
'''

CASES["send_after_close"] = '''fn main() -> i64 {
  let ch = chan(2)
  ch.close()
  match ch.send("late") {
    Ok(_) => { print("sent") },
    Err(e) => { print(e) }
  }
  print(ch.recv())
  return 0
}
'''

def build(src, name):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    c = subprocess.run([sys.executable, CLI, "emit-c", path], capture_output=True, text=True,
                       timeout=120)
    assert c.returncode == 0, "%s emit-c: %r" % (name, c.stdout + c.stderr)
    return path, c.stdout


def run_both(path, name):
    interp = subprocess.run([sys.executable, CLI, "run", "--interp", path], capture_output=True, text=True,
                            timeout=120)
    exe = os.path.join(TMP, name + ".exe")
    built = subprocess.run([sys.executable, CLI, "build", path, "-o", exe],
                           capture_output=True, text=True, timeout=300)
    assert built.returncode == 0, "%s build: %r" % (name, (built.stdout + built.stderr)[-800:])
    native = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    assert interp.returncode == 0, "%s interp rc=%d %r" % (name, interp.returncode,
                                                           interp.stdout + interp.stderr)
    assert native.returncode == 0, "%s native rc=%d %r" % (name, native.returncode,
                                                           native.stdout + native.stderr)
    assert native.stdout == interp.stdout, "%s interp=%r native=%r" % (
        name, interp.stdout, native.stdout)
    return interp.stdout


WANT = {
    "roundtrip": "one\ntwo\nNone\n",
    "pipeline": "690\njoined\n",
    "backpressure": "50\n",
    "nested_scope": "12\n",
    "return_joins": "7\nfrom child\n",
    "send_after_close": "send on closed channel\nNone\n",
}

for name, src in CASES.items():
    path, _ = build(src, name)
    out = run_both(path, name)
    assert out == WANT[name], "%s stdout=%r want=%r" % (name, out, WANT[name])

# A program that sends and drains must leave nothing behind.
path, csrc = build(CASES["pipeline"], "pipeline_asan")
cfile = os.path.join(TMP, "pipeline_asan.c")
with open(cfile, "w", encoding="utf-8") as f:
    f.write(csrc)
exe = os.path.join(TMP, "pipeline_asan.exe")
cc = subprocess.run(["cc", "-fsanitize=address", "-g", "-O1", "-I", ENGINE, cfile, "-o", exe,
                     "-lm", "-lpthread"], capture_output=True, text=True, timeout=600)
assert cc.returncode == 0, "asan cc: %r" % (cc.stdout + cc.stderr)
seen = subprocess.run([exe], capture_output=True, text=True, timeout=120,
                      env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:exitcode=0"))
both = seen.stdout + seen.stderr
assert "AddressSanitizer" not in both, "pipeline broken: %s" % both[-800:]
assert "LeakSanitizer" not in both, "pipeline leaked: %s" % both[-800:]
assert seen.stdout == WANT["pipeline"], "asan run stdout=%r" % seen.stdout

# The checker refuses to use a value after handing it to a channel.
bad = os.path.join(TMP, "after_send.brat")
with open(bad, "w", encoding="utf-8") as f:
    f.write('''fn main() -> i64 {
  let ch = chan(2)
  let msg = "hello"
  ch.send(msg)
  print(msg)
  return 0
}
''')
r = subprocess.run([sys.executable, CLI, "check", bad], capture_output=True, text=True,
                   timeout=120)
assert r.returncode == 1, "use after send must fail, got rc=%d %r" % (r.returncode, r.stdout)
assert "use of sent `msg`" in r.stdout, r.stdout
assert "a channel send hands the value to the receiver" in r.stdout, r.stdout

# A reference may not be sent: the receiver would read the caller's frame.
ref = os.path.join(TMP, "send_ref.brat")
with open(ref, "w", encoding="utf-8") as f:
    f.write('''fn push(ch: i64, s: str) -> i64 {
  ch.send(s)
  return 0
}

fn main() -> i64 {
  let ch = chan(1)
  print(push(ch, "x"))
  return 0
}
''')
r = subprocess.run([sys.executable, CLI, "check", ref], capture_output=True, text=True,
                   timeout=120)
assert r.returncode == 1, "sending a ref must fail, got %r" % (r.stdout,)
assert "cannot send `s`" in r.stdout, r.stdout

print("CHANNELS ALL GREEN (%d cases, 2 refusals)" % len(CASES))
