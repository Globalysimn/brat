"""Channels and `scope`: a value has one holder at a time, and a block joins.

Every case runs on both backends and the two stdouts must match byte for byte.
The memory cases go through AddressSanitizer with leak detection on, because a
channel that keeps a value nobody reads looks exactly like a working program.
"""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
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

CASES["nested_blocking_spawn"] = '''fn main() -> i64 {
  let ready = chan(1)
  let ch = chan(1)
  scope {
    spawn {
      ready.send(1)
      sleep_ms(100)
      spawn {
        ch.send(42)
      }
      print(ch.recv())
    }
    print(ready.recv())
  }
  print("done")
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

CASES["child_error_caught"] = '''fn main() -> i64 {
  try {
    scope {
      spawn {
        raise("child")
      }
    }
  } catch e {
    print(e)
  }
  print("done")
  return 0
}
'''

CASES["parent_error_first"] = '''fn main() -> i64 {
  try {
    scope {
      spawn {
        sleep_ms(50)
        raise("child")
      }
      raise("parent")
    }
  } catch e {
    print(e)
  }
  print("done")
  return 0
}
'''

CASES["many_live_channels"] = '''fn main() -> i64 {
  let mut cs = []
  for i in 0..100 {
    let c = chan(1)
    c.send(i)
    cs.push(c)
  }
  let mut total = 0
  for c in cs {
    total = total + c.recv()
    c.close()
  }
  print(total)
  print(len(cs))
  return 0
}
'''

def build(src, name):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    c = subprocess.run(_cxx.cli_args("emit-c", path), capture_output=True, text=True,
                       timeout=120)
    assert c.returncode == 0, "%s emit-c: %r" % (name, c.stdout + c.stderr)
    return path, c.stdout


def run_both(path, name):
    run_o0 = subprocess.run(_cxx.cli_args("run", "-O0", path), capture_output=True, text=True,
                            timeout=120)
    exe = os.path.join(TMP, name + ".exe")
    built = subprocess.run(_cxx.cli_args("build", path, "-o", exe),
                           capture_output=True, text=True, timeout=300)
    assert built.returncode == 0, "%s build: %r" % (name, (built.stdout + built.stderr)[-800:])
    native = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    assert run_o0.returncode == 0, "%s run_o0 rc=%d %r" % (name, run_o0.returncode,
                                                           run_o0.stdout + run_o0.stderr)
    assert native.returncode == 0, "%s native rc=%d %r" % (name, native.returncode,
                                                           native.stdout + native.stderr)
    assert native.stdout == run_o0.stdout, "%s run_o0=%r native=%r" % (
        name, run_o0.stdout, native.stdout)
    return run_o0.stdout


WANT = {
    "roundtrip": "one\ntwo\nNone\n",
    "pipeline": "690\njoined\n",
    "backpressure": "50\n",
    "nested_scope": "12\n",
    "nested_blocking_spawn": "1\n42\ndone\n",
    "return_joins": "7\nfrom child\n",
    "send_after_close": "send on closed channel\nNone\n",
    "child_error_caught": "child\ndone\n",
    "parent_error_first": "parent\ndone\n",
    "many_live_channels": "4950\n100\n",
}

for name, src in CASES.items():
    path, _ = build(src, name)
    out = run_both(path, name)
    assert out == WANT[name], "%s stdout=%r want=%r" % (name, out, WANT[name])

path, _ = build(CASES["nested_blocking_spawn"], "nested_blocking_spawn_one_thread")
exe = os.path.join(TMP, "nested_blocking_spawn_one_thread.exe")
built = subprocess.run(_cxx.cli_args("build", path, "-o", exe),
                       capture_output=True, text=True, timeout=300)
assert built.returncode == 0, "one-thread build: %r" % ((built.stdout + built.stderr)[-800:])
native = subprocess.run([exe], capture_output=True, text=True, timeout=10,
                        env=dict(os.environ, BRAT_THREADS="1"))
assert native.returncode == 0, "one-thread rc=%d %r" % (native.returncode, native.stdout + native.stderr)
assert native.stdout == WANT["nested_blocking_spawn"], "one-thread stdout=%r" % native.stdout

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

tsan_src = '''fn main() -> i64 {
  let mut round = 0
  while round < 200 {
    let ch = chan(64)
    scope {
      spawn {
        let mut i = 0
        while i < 64 {
          ch.send(i)
          i = i + 1
        }
        ch.close()
      }
      spawn {
        let mut seen = 0
        let mut go = true
        while go {
          let v = ch.recv()
          if is_none(v) {
            go = false
          } else {
            seen = seen + 1
          }
        }
        if seen != 64 {
          print(seen)
        }
      }
    }
    round = round + 1
  }
  print("done")
  return 0
}
'''
path, csrc = build(tsan_src, "close_drain_tsan")
cfile = os.path.join(TMP, "close_drain_tsan.c")
with open(cfile, "w", encoding="utf-8") as f:
    f.write(csrc)
exe = os.path.join(TMP, "close_drain_tsan.exe")
cc = subprocess.run(["cc", "-fsanitize=thread", "-g", "-O1", "-fPIE", "-pie", "-I", ENGINE,
                     cfile, "-o", exe, "-lm", "-lpthread", "-ldl"],
                    capture_output=True, text=True, timeout=600)
if cc.returncode == 0:
    import platform, shutil as _shutil
    tsan_env = dict(os.environ, TSAN_OPTIONS="halt_on_error=1")
    sa = _shutil.which("setarch") if sys.platform.startswith("linux") else None
    seen = subprocess.run([sa, platform.machine(), "-R", exe] if sa else [exe],
                          capture_output=True, text=True, timeout=120, env=tsan_env)
    if sa and seen.returncode != 0 and "setarch" in seen.stderr:
        seen = subprocess.run([exe], capture_output=True, text=True, timeout=120, env=tsan_env)
    both = seen.stdout + seen.stderr
    assert seen.returncode == 0, "tsan run rc=%d %s" % (seen.returncode, both[-1200:])
    assert "ThreadSanitizer" not in both, "channel race: %s" % both[-1200:]
    assert seen.stdout == "done\n", "tsan run stdout=%r" % seen.stdout

if sys.platform.startswith("linux"):
    no_worker_src = '''fn main() -> i64 {
  let ch = chan(1)
  try {
    spawn {
      ch.send(42)
    }
    print(ch.recv())
  } catch e {
    print(e)
  }
  print("done")
  return 0
}
'''
    path, csrc = build(no_worker_src, "spawn_no_worker")
    cfile = os.path.join(TMP, "spawn_no_worker.c")
    with open(cfile, "w", encoding="utf-8") as f:
        f.write(csrc)
    exe = os.path.join(TMP, "spawn_no_worker.exe")
    cc = subprocess.run(["cc", "-O1", "-I", ENGINE, cfile, "-o", exe, "-lm", "-lpthread", "-ldl"],
                        capture_output=True, text=True, timeout=600)
    assert cc.returncode == 0, "no-worker cc: %r" % (cc.stdout + cc.stderr)
    fail_c = os.path.join(TMP, "fail_pthread_create.c")
    with open(fail_c, "w", encoding="utf-8") as f:
        f.write("#include <errno.h>\n"
                "#include <pthread.h>\n"
                "int pthread_create(pthread_t *thread, const pthread_attr_t *attr, "
                "void *(*start_routine)(void *), void *arg) {\n"
                "    (void)thread; (void)attr; (void)start_routine; (void)arg;\n"
                "    return EAGAIN;\n"
                "}\n")
    fail_so = os.path.join(TMP, "fail_pthread_create.so")
    cc = subprocess.run(["cc", "-shared", "-fPIC", fail_c, "-o", fail_so, "-pthread"],
                        capture_output=True, text=True, timeout=600)
    assert cc.returncode == 0, "interposer cc: %r" % (cc.stdout + cc.stderr)
    seen = subprocess.run([exe], capture_output=True, text=True, timeout=10,
                          env=dict(os.environ, LD_PRELOAD=fail_so))
    assert seen.returncode == 0, "no-worker rc=%d %r" % (seen.returncode, seen.stdout + seen.stderr)
    assert seen.stdout == "spawn failed: could not start worker thread\ndone\n", seen.stdout

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
r = subprocess.run(_cxx.cli_args("check", bad), capture_output=True, text=True,
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
r = subprocess.run(_cxx.cli_args("check", ref), capture_output=True, text=True,
                   timeout=120)
assert r.returncode == 1, "sending a ref must fail, got %r" % (r.stdout,)
assert "cannot send `s`" in r.stdout, r.stdout

print("CHANNELS ALL GREEN (%d cases, 2 refusals)" % len(CASES))
