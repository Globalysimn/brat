"""The capability lattice: what a place still allows, joined across the flow.

A list of live views per root can only answer "is any of them live here". The
lattice keeps one capability per place and joins the two sides of a branch,
which answers two more questions:

  * a view whose last use is behind us no longer holds the place, so the write
    that follows it is accepted;
  * a place moved on one path only is "maybe gone" at the join, so a use after
    it is refused.

Every case goes through `check` and must report each listed fragment, or pass
when it lists none.
"""
import _cxx
import _scratch
import os
import subprocess
import sys

TMP = _scratch.mkdtemp(prefix="brat-caps-")

# name, source, substrings that must appear (empty = the program is accepted)
CASES = [
    ("alias_dies_then_push", '''fn main() -> i64 {
  let mut v = [1, 2]
  let y = v
  print(len(y))
  v.push(3)
  return 0
}
''', []),
    ("alias_in_one_branch", '''fn main() -> i64 {
  let mut v = [1, 2]
  if len(v) > 0 {
    let y = v
    print(len(y))
  }
  v.push(3)
  return 0
}
''', []),
    ("moved_field_leaves_the_rest", '''fn main() -> i64 {
  let p = Pair { x: "a", y: "b" }
  let a = move p.x
  print(p.y)
  print(a)
  return 0
}
''', []),
    ("defer_move_then_use", '''fn main() -> i64 {
  let v = "x"
  defer move v
  print(v)
  return 0
}
''', []),
    ("reading_view_still_live", '''fn main() -> i64 {
  let mut v = [1, 2]
  let y = v
  v.push(3)
  print(len(y))
  return 0
}
''', ["while borrowed by `y`"]),
    ("two_writers", '''fn main() -> i64 {
  let mut v = [1, 2]
  let mut y = v
  y.push(3)
  return 0
}
''', ["double mut borrow"]),
    ("use_after_move_through_field", '''fn main() -> i64 {
  let p = Pair { x: "a", y: "b" }
  let q = move p
  print(p.y)
  print(q.x)
  return 0
}
''', ["use of moved `p`"]),
    ("moved_inside_a_branch", '''fn main() -> i64 {
  let p = Pair { x: "a", y: "b" }
  let mut used = 0
  if used == 0 {
    let q = move p
    print(q.x)
  }
  print(p.y)
  return 0
}
''', ["use of moved `p`"]),
    # The C cleanup idiom: release on the error branch, return, release on the
    # normal path. Two paths, one free each -- and a checker that joins the
    # branch that returns calls the second one a second free.
    ("free_on_the_error_branch", '''extern fn malloc(n: i64) -> move ptr
extern fn free(move p: ptr)

fn take(c: i64) -> i64 {
  let p = malloc(8)
  if c > 0 {
    free(move p)
    return 1
  }
  free(move p)
  return 0
}

fn main() -> i64 {
  return take(1)
}
''', []),
    # And the same shape with no `return` really is one free too many.
    ("free_twice_without_a_return", '''extern fn malloc(n: i64) -> move ptr
extern fn free(move p: ptr)

fn main() -> i64 {
  let p = malloc(8)
  if len("x") > 0 {
    free(move p)
  }
  free(move p)
  return 0
}
''', ["use of moved `p`"]),
    # `break` does not leave the function: the free inside the branch happens
    # before the loop ends, and the free after it is the same pointer again.
    ("free_through_a_break", '''extern fn malloc(n: i64) -> move ptr
extern fn free(move p: ptr)

fn main() -> i64 {
  let p = malloc(8)
  let mut i = 0
  while i < 3 {
    if i == 1 {
      free(move p)
      break
    }
    i = i + 1
  }
  free(move p)
  return 0
}
''', ["use of moved `p`"]),
    ("alias_dies_in_three_arm_match", '''fn step(k: i64) -> i64 {
  let mut v = [1, 2]
  match k {
    0 => {
      let y = v
      print(len(y))
    },
    1 => {
      let a = 1
      let b = 2
      let c = 3
      print(a + b + c)
    },
    _ => {
      print(0)
    }
  }
  v.push(3)
  return len(v)
}

fn main() -> i64 {
  return step(0) - 3
}
''', []),
    ("view_live_across_three_arm_match", '''fn step(k: i64) -> i64 {
  let mut v = [1, 2]
  let r = v
  match k {
    0 => { print(0) },
    1 => {
      let a1 = 1
      let a2 = 2
      let a3 = 3
      let a4 = 4
      let a5 = 5
      print(a1 + a2 + a3 + a4 + a5)
    },
    _ => { print(9) }
  }
  v.push(3)
  return len(r)
}

fn main() -> i64 {
  return step(0)
}
''', ["while borrowed by `r`"]),
    ("view_used_twice_still_live", '''fn main() -> i64 {
  let mut v = [1, 2]
  let y = v
  let first = len(y)
  v.push(3)
  return first + len(y)
}
''', ["while borrowed by `y`"]),
]

FAILED = []


def check(cond, msg):
    if not cond:
        FAILED.append(msg)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    for name, src, expect in CASES:
        path = os.path.join(TMP, name + ".brat")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        r = run(_cxx.cli_args("check", path))
        for want in expect:
            check(want in r.stdout,
                  "%s: `%s` not reported\n%s" % (name, want, r.stdout))
        if not expect:
            check(r.returncode == 0, "%s: refused\n%s" % (name, r.stdout))
        print("%-28s %s" % (name, "green" if not expect else "refused as expected"))

    if FAILED:
        print("\nCAPS FAILED (%d)" % len(FAILED))
        for f in FAILED:
            print("  " + f)
        sys.exit(1)
    print("\nCAPS ALL GREEN (%d cases)" % len(CASES))


if __name__ == "__main__":
    main()
