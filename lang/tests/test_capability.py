"""Sharing rules: reference cycles and writes that escape a `spawn`.

Every case runs through both checkers, so the two cannot drift: the diagnostic
must be identical, and a program that the rule accepts must build with a
working binary on both backends.

The rules exist because the runtime has no cycle collector and `spawn` copies a
frame but not the objects the frame points at (see docs/guide/ownership.md).
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Built through `_cxx.build()`, not hardcoded: on a fresh clone the binary does
# not exist yet, and the contract in `_skip.py` is that a test which cannot run
# exits 77 instead of dying in `subprocess.run` with FileNotFoundError. The
# helper also builds to a temp name and renames, so a parallel test never sees
# a half-written file.
CXX = None
TMP = _scratch.mkdtemp(prefix="brat-caps-")

CYCLE = "reference cycle"
RACE = "written inside `spawn`"
RACE_R = "read inside `spawn`"
LOOP = "launched in a loop"

# name -> (source, expected diagnostic fragment or None, expected stdout)
CASES = {
    # A parent that rebinds a name while the child reads it is two frames, not
    # two threads on one object: `spawn` copies the frame, so the child holds the
    # value from the launch. That is only true since the snapshot exists -- the
    # child used to read the script-level global directly, the checker said
    # nothing, and the backends disagreed (see LEDGER, 2026-10-30).
    "read_in_child_write_in_parent": ('''fn main() -> i64 {
  let mut x = 0
  spawn {
    print(x)
  }
  x = 2
  join_all()
  print(x)
  return 0
}
''', None, "0\n2\n"),
    "read_in_child_write_in_parent_joined": ('''fn main() -> i64 {
  let mut x = 0
  spawn {
    print(x)
  }
  join_all()
  x = 2
  print(x)
  return 0
}
''', None, "0\n2\n"),
    # a value put into itself: a refcount can never reach zero again
    "cycle_self_vec": ('''fn main() -> i64 {
  let mut v = [1]
  v.push(v)
  print(len(v))
  return 0
}
''', CYCLE, ""),
    "cycle_self_map": ('''fn main() -> i64 {
  let mut m = {}
  m.set("me", m)
  print(len(m))
  return 0
}
''', CYCLE, ""),
    "cycle_two_steps": ('''fn main() -> i64 {
  let mut a = []
  let mut b = [1]
  a.push(b)
  b.push(a)
  print(len(a))
  return 0
}
''', CYCLE, ""),
    "cycle_through_alias": ('''fn main() -> i64 {
  let mut a = []
  let mut b = []
  a.push(b)
  let c = a
  b.push(c)
  return 0
}
''', CYCLE, ""),
    # nesting that is not a cycle stays legal
    "nest_ok": ('''fn main() -> i64 {
  let mut outer = []
  let mut inner = [1]
  outer.push(inner)
  inner.push(2)
  print(outer)
  print(inner)
  return 0
}
''', None, "[[1, 2]]\n[1, 2]\n"),
    "nest_fresh_ok": ('''fn main() -> i64 {
  let mut v = []
  v.push([1, 2])
  v.push({"k": 1})
  print(len(v))
  return 0
}
''', None, "2\n"),

    # a write inside spawn, read by the parent with no join in between
    "spawn_write_no_join": ('''fn main() -> i64 {
  let mut cell = [0]
  spawn {
    cell[0] = 1
  }
  print(cell[0])
  join_all()
  return 0
}
''', RACE, ""),
    "spawn_write_joined": ('''fn main() -> i64 {
  let mut cell = [0]
  spawn {
    cell[0] = 1
  }
  join_all()
  print(cell[0])
  return 0
}
''', None, "1\n"),
    "spawn_two_writers": ('''fn main() -> i64 {
  let mut v = []
  spawn {
    v.push(1)
  }
  spawn {
    v.push(2)
  }
  join_all()
  print(len(v))
  return 0
}
''', RACE, ""),
    "spawn_read_only_ok": ('''fn main() -> i64 {
  let v = [1, 2, 3]
  let ch = chan(2)
  spawn {
    ch.send(len(v))
  }
  join_all()
  print(ch.recv())
  return 0
}
''', None, "3\n"),
    # C wrote through the pointer: `ptr_write_i64(p, v)` is a write to the
    # place `p` names, even though no `let` in the program says so. Two threads
    # calling it on one pointer are two writers of it.
    "spawn_ptr_write_no_join": ('''extern fn malloc(n: i64) -> move ptr

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    ptr_write_i64(p, 1)
  }
  ptr_write_i64(p, 2)
  join_all()
  ptr_read_i64(p)
  return 0
}
''', RACE, ""),
    # and freeing is a write too: the storage ends here, whatever the other
    # thread was reading through it.
    "spawn_ptr_read_parent_free": ('''extern fn malloc(n: i64) -> move ptr
extern fn free(move p: ptr)

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    ptr_read_i64(p)
  }
  free(move p)
  return 0
}
''', RACE_R, ""),
    # a read inside spawn, written by the parent with no join in between:
    # the other direction of the same race, and the one that used to pass
    "spawn_read_parent_write": ('''fn main() -> i64 {
  let mut cell = [0]
  spawn {
    print(cell[0])
  }
  cell[0] = 5
  join_all()
  return 0
}
''', RACE_R, ""),
    "spawn_read_parent_push": ('''fn main() -> i64 {
  let mut v = [1]
  spawn {
    print(v[0])
  }
  v.push(2)
  join_all()
  return 0
}
''', RACE_R, ""),
    "spawn_read_two_reads_ok": ('''fn main() -> i64 {
  let v = [7, 8]
  let ch = chan(1)
  spawn {
    ch.send(len(v))
  }
  let seen = v[1]
  join_all()
  print(ch.recv())
  print(seen)
  return 0
}
''', None, "2\n8\n"),
    "spawn_read_joined_write_ok": ('''fn main() -> i64 {
  let mut cell = [0]
  spawn {
    print(cell[0])
  }
  join_all()
  cell[0] = 5
  print(cell[0])
  return 0
}
''', None, "0\n5\n"),
    "spawn_loop_no_join": ('''fn main() -> i64 {
  let mut seen = []
  for i in 1..4 {
    spawn {
      seen.push(i)
    }
  }
  join_all()
  print(len(seen))
  return 0
}
''', LOOP, ""),
    "spawn_loop_joined_ok": ('''fn main() -> i64 {
  let mut cell = [0]
  for i in 1..4 {
    spawn {
      cell[0] = i
    }
    join_all()
  }
  print(cell[0])
  return 0
}
''', None, "3\n"),
    "spawn_local_ok": ('''fn main() -> i64 {
  let ch = chan(4)
  spawn {
    let mut local = []
    local.push(1)
    local.push(2)
    ch.send(len(local))
  }
  join_all()
  print(ch.recv())
  return 0
}
''', None, "2\n"),

    "race_via_mut_helper": ('''\
fn bump(mut d: map[str, i64]) -> i64 {
  d.set("k", 1)
  return 0
}
fn main() -> i64 {
  let mut m = {"k": 0}
  spawn {
    bump(m)
  }
  bump(m)
  join_all()
  return 0
}
''', "`m` is written inside `spawn` and touched again with no `join_all()` in between", None),

    "spawn_inner_shadow_race": ('''\
fn main() -> i64 {
  let mut v = ["a"]
  spawn {
    if false {
      let v = 0
      print(v)
    }
    v.push("b")
  }
  v.push("c")
  join_all()
  return 0
}
''', RACE, None),

    "spawn_helper_inner_shadow_global_race": ('''\
let mut shared = [1]
fn touch_shared() -> i64 {
  if false {
    let shared = 0
    print(shared)
  }
  shared.push(2)
  return 0
}
fn main() -> i64 {
  spawn {
    touch_shared()
  }
  shared.push(3)
  join_all()
  return 0
}
''', "`shared` is written inside `spawn` and touched again with no `join_all()` in between", None),

    "cycle_via_mut_helper": ('''\
fn link(mut a: map[str, i64], b: map[str, i64]) -> i64 {
  a.set("next", b)
  return 0
}
fn main() -> i64 {
  let mut x = {}
  let mut y = {}
  link(x, y)
  link(y, x)
  return 0
}
''', "reference cycle: `x` goes into `y`, which already owns it", None),
}


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)


def fail(name, msg):
    sys.stdout.write("FAIL %s: %s\n" % (name, msg))
    sys.stdout.flush()
    raise SystemExit(1)


def check_one(path):
    """Checker diagnostics, normalised to the lines that carry a message."""
    p = sh([CXX, "check", path])
    return p.returncode, "\n".join(l for l in p.stdout.splitlines() if ": error: " in l)


def main():
    global CXX
    CXX = _cxx.build()
    for name, (src, want, stdout) in sorted(CASES.items()):
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        rc, text = check_one(path)
        if want is None:
            if rc != 0:
                fail(name, "must be accepted, got:\n%s" % text)
            exe = os.path.join(TMP, name + ".exe")
            b = sh([CXX, "build", path, "-o", exe])
            if b.returncode != 0:
                fail(name, "build: %s" % (b.stdout + b.stderr)[-500:])
            run = sh([exe])
            if run.stdout != stdout:
                fail(name, "stdout %r, want %r" % (run.stdout, stdout))
            run_o0 = sh(_cxx.run_args(path))
            if run_o0.stdout != stdout:
                fail(name, "run_o0 stdout %r, want %r" % (run_o0.stdout, stdout))
        else:
            if rc == 0:
                fail(name, "must be rejected, got 0 error(s)")
            if want not in text:
                fail(name, "want %r in the diagnostic, got:\n%s" % (want, text))

    # The same two rules, joined properly: a write through the pointer after
    # `join_all()` is not a race, or the rule would only ever refuse.
    accepted = {
        "spawn_ptr_write_joined_ok": '''extern fn malloc(n: i64) -> move ptr
extern fn free(move p: ptr)

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    ptr_write_i64(p, 1)
  }
  join_all()
  print(ptr_read_i64(p))
  free(move p)
  return 0
}
''',
        "spawn_ptr_free_joined_ok": '''extern fn malloc(n: i64) -> move ptr
extern fn free(move p: ptr)

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    print(ptr_read_i64(p))
  }
  join_all()
  free(move p)
  return 0
}
''',
    }
    for name in sorted(accepted):
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(accepted[name])
        rc, text = check_one(path)
        if rc != 0:
            fail(name, "a joined pointer use must be accepted, got:\n%s" % text)

    print("CAPABILITY ALL GREEN (%d cases)" % (len(CASES) + len(accepted)))


def run_gate():
    """`brat run` must refuse what `brat check` refuses, not run it anyway."""
    path = os.path.join(TMP, "gate.brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write("fn main() -> i64 {\n  let mut cell = [0]\n  spawn { print(cell[0]) }\n"
                "  cell[0] = 5\n  join_all()\n  return 0\n}\n")
    p = sh(_cxx.cli_args("run", "-O0", path))
    assert p.returncode != 0 and "refusing to run" in (p.stdout + p.stderr), \
        "brat run must refuse a race: rc=%d %r" % (p.returncode, (p.stdout + p.stderr)[-300:])
    print("CAPABILITY gate ok (brat run refuses the race)")


run_gate()


if __name__ == "__main__":
    main()
