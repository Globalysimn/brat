"""Escape rules for `arena { }` blocks.

Each rule has a program that must be rejected with a message naming it. Programs
that keep arena values inside their block, or pass only scalars out, must check
clean and print the same in both backends.
"""
import _cxx
import _scratch
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')
CXX = os.path.abspath(os.path.join(HERE, '..', 'cxx', 'bin', 'brat-cxx'))
HAVE_CXX = os.path.isfile(CXX)

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


REJECT = {
    'return_value': ('a value made inside the block is returned out of it', '''
fn make() -> vec[i64] {
  arena {
    let v = [1, 2]
    return v
  }
  return []
}
fn main() -> i64 {
  let x = make()
  return 0
}
'''),
    'assign_outer': ('assigned to `keep`', '''
fn main() -> i64 {
  let mut keep = []
  arena {
    let v = [1, 2]
    keep = v
  }
  return 0
}
'''),
    'push_outer': ('stored into `all`', '''
fn main() -> i64 {
  let mut all = []
  arena {
    let v = [1, 2]
    all.push(v)
  }
  return 0
}
'''),
    'setidx_outer': ('stored into `rows`', '''
fn main() -> i64 {
  let mut rows = [[0]]
  arena {
    let v = [1, 2]
    rows[0] = v
  }
  return 0
}
'''),
    'alias_of_outer': ('stored into `alias`', '''
fn main() -> i64 {
  let mut all = [[0]]
  arena {
    let inner_v = [1, 2]
    let alias = all
    alias.push(inner_v)
  }
  return 0
}
'''),
    'loop_var_out': ('assigned to `last`', '''
fn main() -> i64 {
  let mut last = ""
  arena {
    let words = ["a", "b"]
    for w in words {
      last = w
    }
  }
  return 0
}
'''),
    'spawn_capture': ('a spawn inside the block captures', '''
fn main() -> i64 {
  arena {
    let v = [1, 2]
    spawn {
      print("{len(v)}")
    }
  }
  return 0
}
'''),
    'closure_capture': ('a closure inside the block captures', '''
fn main() -> i64 {
  arena {
    let v = [1, 2]
    let g = fn() { print("{len(v)}") }
  }
  return 0
}
'''),
    'tainted_container_spawn': ('a spawn inside the block captures', '''
fn main() -> i64 {
  let mut box = [0]
  arena {
    let v = [1, 2]
    box.push(v)
    spawn {
      print("{len(box)}")
    }
  }
  return 0
}
'''),
    'field_escape': ('stored into `box`', '''
fn main() -> i64 {
  let mut box = {"v": [0]}
  arena {
    let v = [1, 2]
    box.v = v
  }
  print("{len(box.v)}")
  return 0
}
'''),
}

ACCEPT = {
    'scalars_out': '''
fn main() -> i64 {
  let mut total = 0
  let mut name = ""
  arena {
    let v = [1, 2, 3]
    let mut m = {"k": 1}
    m.set("z", 2)
    total = len(v) + m.get("z")
    let k = len(v)
    total = total + k
    name = "done"
  }
  print("total {total} {name}")
  return 0
}
''',
    'own_copy_out': '''
enum Res {
  Ok(v),
  Err(reason)
}
fn check(n: i64) -> Res {
  arena {
    let mut parts = []
    let mut i = 0
    while i < n {
      parts.push(i)
      i = i + 1
    }
    if len(parts) > 2 {
      return Err(own("too many: {len(parts)}"))
    }
    return Ok(own(parts))
  }
  return Err("unreachable")
}
fn main() -> i64 {
  match check(2) {
    Ok(v) => { print("ok {len(v)}") },
    Err(r) => { print("err {r}") }
  }
  match check(5) {
    Ok(v) => { print("ok {len(v)}") },
    Err(r) => { print("err {r}") }
  }
  return 0
}
''',
    'nested_inner_to_outer_arena': '''
fn main() -> i64 {
  let mut out = 0
  arena {
    let mut a = [10]
    arena {
      let mut b = [1, 2]
      a.push(len(b))
    }
    out = a[1]
  }
  print("out {out}")
  return 0
}
''',
    'newcomer_no_ownership': '''
fn main() -> i64 {
  let mut total = 0
  arena {
    let nums = [1, 2, 3, 4, 5]
    for n in nums {
      total = total + n
    }
  }
  print("sum {total}")
  return 0
}
''',
}


def write(d, name, text):
    p = os.path.join(d, name + '.brat')
    with open(p, 'w', encoding='utf-8') as f:
        f.write(text)
    return p


tmp = _scratch.mkdtemp(prefix='brat-arena-escape-')
try:
    for name, (want, text) in REJECT.items():
        p = write(tmp, name, text)
        r = subprocess.run(_cxx.cli_args('check', p), capture_output=True, text=True, timeout=300)
        out = r.stdout + r.stderr
        check(r.returncode != 0 and want in out,
              'reject %s: want %r in output, rc=%d out=%s' % (name, want, r.returncode, out[-400:]))
        check('own(' in out or name == 'closure_capture',
              'reject %s: the refusal should point at `own(...)` as the way out' % name)
        if HAVE_CXX:
            # `brat run` decides with the same checker, so a refusal that one
            # door misses is a program that builds and runs with a dangling
            # arena pointer. Require identical bytes from both.
            py = subprocess.run(_cxx.cli_args('check', p),
                                capture_output=True, text=True, timeout=300)
            cx = subprocess.run([CXX, 'check', p],
                                capture_output=True, text=True, timeout=300)
            check((py.returncode, py.stdout, py.stderr) == (cx.returncode, cx.stdout, cx.stderr),
                  'reject %s: checkers disagree\n  py  rc=%d %r\n  cxx rc=%d %r'
                  % (name, py.returncode, (py.stdout + py.stderr)[-300:],
                     cx.returncode, (cx.stdout + cx.stderr)[-300:]))
        print('  rejected %-18s %s' % (name, want))

    for name, text in ACCEPT.items():
        p = write(tmp, name, text)
        r = subprocess.run(_cxx.cli_args('check', p), capture_output=True, text=True, timeout=300)
        check(r.returncode == 0 and 'arena:' not in r.stdout + r.stderr,
              'accept %s: check failed: %s' % (name, (r.stdout + r.stderr)[-400:]))
        ip = subprocess.run(_cxx.run_args(p), capture_output=True, text=True, timeout=300)
        exe = os.path.join(tmp, name + '_bin')
        b = subprocess.run(_cxx.cli_args('build', p, '-o', exe), capture_output=True, text=True, timeout=300)
        check(b.returncode == 0, 'accept %s: build failed: %s' % (name, (b.stdout + b.stderr)[-400:]))
        cr = subprocess.run([exe], capture_output=True, text=True, timeout=300)
        check(ip.returncode == 0 and cr.returncode == 0 and ip.stdout == cr.stdout,
              'accept %s: backends differ: %r vs %r' % (name, ip.stdout, cr.stdout))
        print('  accepted %-28s %s' % (name, ip.stdout.strip()))

    arena_src = os.path.join(tmp, 'noncheck.brat')
    with open(arena_src, 'w', encoding='utf-8') as f:
        f.write(ACCEPT['scalars_out'])
    nc = subprocess.run(_cxx.cli_args('emit-c', '--no-check', arena_src),
                        capture_output=True, text=True, timeout=300)
    check(nc.returncode != 0 and 'escape rules' in nc.stderr,
          'emit-c --no-check must refuse arena blocks: rc=%d err=%s' % (nc.returncode, nc.stderr[-300:]))
    print('  refused emit-c --no-check on an arena block')
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print('ARENA ESCAPE ALL GREEN (%d asserts)' % n)