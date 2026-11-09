"""`arena { }` in the -O0 build and in the C backend.

Each program must print the same output in both. An error raised inside a
block must still leave the arena. A loop that fills arenas far beyond the
address-space limit only finishes if every block gives its memory back.
"""
import _cxx
import _scratch
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


ENV = dict(os.environ)

PROGRAMS = {
    'basic': '''
fn main() -> i64 {
  let mut total = 0
  arena {
    let v = [1, 2, 3, 4]
    let mut m = {"a": 1}
    m.set("b", 2)
    total = len(v) + m.get("b")
  }
  print("total {total}")
  return 0
}
''',
    'grow': '''
fn main() -> i64 {
  let mut sum = 0
  arena {
    let mut v = []
    let mut i = 0
    while i < 3000 {
      v.push(i)
      i = i + 1
    }
    let mut m = {}
    let mut j = 0
    while j < 500 {
      m.set("k{j}", j)
      j = j + 1
    }
    sum = len(v) + len(m.keys())
  }
  print("grow {sum}")
  return 0
}
''',
    'nested': '''
fn main() -> i64 {
  let mut outer = 0
  let mut inner = 0
  arena {
    let mut a = [10, 20]
    arena {
      let mut b = [1, 2, 3]
      inner = len(b)
    }
    outer = a[0] + a[1] + inner
  }
  print("nested {outer} {inner}")
  return 0
}
''',
    'error_unwinds': '''
fn fail_inside() -> i64 {
  arena {
    let mut v = [1, 2, 3]
    raise("boom")
  }
  return 0
}
fn main() -> i64 {
  try {
    let r = fail_inside()?
    print("unreached {r}")
  } catch e {
    print("caught {e}")
  }
  let mut again = 0
  arena {
    let v = [5, 6]
    again = len(v)
  }
  print("again {again}")
  return 0
}
''',
    'reclaim': '''
fn main() -> i64 {
  let mut total = 0
  let mut round = 0
  while round < 400 {
    arena {
      let mut v = []
      let mut i = 0
      while i < 2000 {
        v.push(i)
        i = i + 1
      }
      total = total + len(v)
    }
    round = round + 1
  }
  print("reclaim {total}")
  return 0
}
''',
}


def run_interp(src):
    return subprocess.run(_cxx.run_args(src), capture_output=True, text=True,
                          timeout=300, env=ENV)


def build_and_run(src, exe, limit_bytes=None):
    b = subprocess.run(_cxx.cli_args('build', src, '-o', exe),
                       capture_output=True, text=True, timeout=300, env=ENV)
    if b.returncode != 0:
        return b, None

    def limit():
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (limit_bytes, limit_bytes))

    r = subprocess.run([exe], capture_output=True, text=True, timeout=300,
                       preexec_fn=limit if limit_bytes else None)
    return b, r


tmp = _scratch.mkdtemp(prefix='brat-arena-diff-')
try:
    for name, src_text in PROGRAMS.items():
        src = os.path.join(tmp, name + '.brat')
        with open(src, 'w', encoding='utf-8') as f:
            f.write(src_text)
        ip = run_interp(src)
        check(ip.returncode == 0, '%s: run_o0 exit=%d err=%s' % (name, ip.returncode, ip.stderr[-400:]))
        exe = os.path.join(tmp, name + '_bin')
        # 16 MiB of address space: a leaking arena cannot finish the reclaim loop.
        limit = 16 * 1024 * 1024 if name == 'reclaim' else None
        b, cr = build_and_run(src, exe, limit)
        check(b.returncode == 0, '%s: build failed: %s' % (name, (b.stdout + b.stderr)[-400:]))
        check(cr.returncode == 0, '%s: C exit=%d err=%s' % (name, cr.returncode, cr.stderr[-400:]))
        check(ip.stdout == cr.stdout, '%s: outputs differ\ninterp: %r\nC: %r' % (name, ip.stdout, cr.stdout))
        print('  %s: %s' % (name, ip.stdout.strip().replace('\n', ' | ')))

    n += 1
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print('ARENA DIFF ALL GREEN (%d asserts)' % n)