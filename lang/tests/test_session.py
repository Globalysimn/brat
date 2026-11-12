"""Inferred channel protocols: the shape of a pipe, and whether it can finish.

Every case goes through `check`. A case with fragments must be refused with
each of them in the diagnostics, and a case without must pass.
"""
import _cxx
import _scratch
import os
import subprocess

TMP = _scratch.mkdtemp(prefix="brat-session-")

CASES = [
    # name, source, list of substrings that must appear in the diagnostics
    ("balanced", '''fn main() -> i64 {
  let ch = chan(2)
  ch.send(1)
  print(ch.recv())
  return 0
}
''', []),
    ("queue_holds_the_rest", '''fn main() -> i64 {
  let ch = chan(2)
  ch.send(1)
  ch.send(2)
  print(ch.recv())
  return 0
}
''', []),
    ("send_blocks", '''fn main() -> i64 {
  let ch = chan(1)
  ch.send(1)
  ch.send(2)
  ch.send(3)
  print(ch.recv())
  return 0
}
''', ["`ch`: 3 value(s) sent into a queue of 1 and only 1 received"]),
    ("recv_waits", '''fn main() -> i64 {
  let ch = chan(2)
  ch.send(1)
  print(ch.recv())
  print(ch.recv())
  return 0
}
''', ["`ch`: 2 receive(s) but only 1 send(s); `recv` waits forever"]),
    ("helper_hides_the_receive", '''fn drain(ch) -> i64 {
  return ch.recv()
}
fn main() -> i64 {
  let ch = chan(2)
  ch.send(1)
  ch.send(2)
  return drain(ch)
}
''', []),
    ("loop_is_not_counted", '''fn main() -> i64 {
  let ch = chan(8)
  for i in 0..3 {
    ch.send(i)
  }
  let mut t = 0
  for i in 0..3 {
    t = t + ch.recv()
  }
  return t
}
''', []),
    ("branch_is_not_counted", '''fn main() -> i64 {
  let ch = chan(2)
  if true {
    ch.send(1)
  } else {
    ch.send(2)
  }
  for i in 0..1 {
    print(ch.recv())
  }
  return 0
}
''', []),
    ("two_channels", '''fn main() -> i64 {
  let a = chan(1)
  let b = chan(1)
  a.send(1)
  b.send(2)
  b.send(3)
  b.send(4)
  print(a.recv())
  print(b.recv())
  return 0
}
''', ["`b`: 3 value(s) sent into a queue of 1"]),
    ("close_is_not_the_end", '''fn main() -> i64 {
  let ch = chan(2)
  ch.send(1)
  ch.close()
  print(ch.recv())
  print(ch.recv())
  return 0
}
''', []),
    ("queue_full_at_the_second_send", '''fn main() -> i64 {
  let ch = chan(1)
  ch.send(1)
  ch.send(2)
  print(ch.recv())
  return 0
}
''', ["`ch`: 2 value(s) sent into a queue of 1 and only 1 received"]),
    ("three_into_two", '''fn main() -> i64 {
  let ch = chan(2)
  ch.send(1)
  ch.send(2)
  ch.send(3)
  print(ch.recv())
  return 0
}
''', ["`ch`: 3 value(s) sent into a queue of 2 and only 1 received"]),
    ("drain_keeps_the_prefix_short", '''fn main() -> i64 {
  let ch = chan(1)
  ch.send(1)
  print(ch.recv())
  ch.send(2)
  print(ch.recv())
  return 0
}
''', []),
    ("spawn_send_balanced", '''fn main() -> i64 {
  let ch = chan(2)
  spawn {
    ch.send(7)
  }
  join_all()
  return ch.recv()
}
''', []),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=300, **kw)


def check(path):
    out = run(_cxx.cli_args("check", path))
    return out.returncode, out.stdout + out.stderr


def main():
    failures = 0
    for name, src, wants in CASES:
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        rc, text = check(path)
        for want in wants:
            if want not in text:
                print("FAIL %s: missing %r" % (name, want))
                print(text)
                failures += 1
        if wants and rc == 0:
            print("FAIL %s: should have been refused" % name)
            failures += 1
        if not wants and rc != 0:
            print("FAIL %s: refused a program that finishes" % name)
            print(text)
            failures += 1

    if failures:
        raise SystemExit("SESSION FAILED (%d)" % failures)
    print("SESSION ALL GREEN (%d cases)" % len(CASES))


if __name__ == "__main__":
    main()
