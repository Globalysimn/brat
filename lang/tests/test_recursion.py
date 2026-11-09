"""Recursion fast-fail: the depth budget is a language number, both backends agree.

The budget is driven small through the environment so the guard under test cannot
itself be the slow path it exists to prevent.
"""
import _cxx
import _scratch
import importlib.util
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENG = os.path.join(HERE, '..', 'engine')


_tc_spec = importlib.util.spec_from_file_location('cc_toolchain', os.path.join(HERE, '_toolchain.py'))
_tc = importlib.util.module_from_spec(_tc_spec)
_tc_spec.loader.exec_module(_tc)

TMP = _scratch.mkdtemp(prefix='brat-rec-')
BUDGET = 300
# main() holds one frame, so a legal descent of `down(n)` costs n + 1.
ENV = dict(os.environ, BRAT_RECURSION_BUDGET=str(BUDGET))
TS = re.compile(r'"ts": [\d.]+')

RUNAWAY = '''fn f(n: i64) -> i64 {
  return f(n + 1)
}

fn main() -> i64 {
  return f(0)
}
'''

CAUGHT = '''fn f(n: i64) -> i64 {
  return f(n + 1)
}

fn main() -> i64 {
  let mut r = 0
  try {
    r = f(0)
  } catch e {
    print(e)
    r = -1
  }
  print(r)
  return 0
}
'''

RESET = '''fn f(n: i64) -> i64 {
  return f(n + 1)
}

fn down(n: i64) -> i64 {
  if n <= 0 { return 0 }
  return down(n - 1)
}

fn main() -> i64 {
  try {
    f(0)
  } catch e {
    print(e)
  }
  print(down(%d))
  return 0
}
''' % (BUDGET - 2)

DEFER = '''fn f(n: i64) -> i64 {
  defer log.warn("bye")
  return f(n + 1)
}

fn main() -> i64 {
  try {
    f(0)
  } catch e {
    print(e)
  }
  return 0
}
'''

PROGS = {'runaway': RUNAWAY, 'caught': CAUGHT, 'reset': RESET, 'defer': DEFER}

n = 0
OUT = {}


def norm(s):
    return TS.sub('"ts": T', s.replace('\r\n', '\n'))


def build(name, src):
    path = os.path.join(TMP, name + '.brat')
    open(path, 'w', encoding='utf-8').write(src)
    cf = os.path.join(TMP, name + '.c')
    exe = os.path.join(TMP, name + '.exe')
    open(cf, 'w', encoding='utf-8').write(_cxx.emit_file(path))
    r = subprocess.run(_tc.resolve_cc().argv(cf, exe, static=True) + ['-O2', '-I', ENG, '-lm'],
                       capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, 'cc failed for %s:\n%s' % (name, (r.stdout + r.stderr)[-2000:])
    return exe


def run(cmd, cwd=None):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=30, cwd=cwd)
    except subprocess.TimeoutExpired:
        raise AssertionError('timed out (no fast refusal): %s' % cmd)
    return p.returncode, norm(p.stdout), p.stderr


for name, src in PROGS.items():
    exe = build(name, src)
    irc, iout, ierr = run(_cxx.run_args(os.path.join(TMP, name + '.brat')))
    nrc, nout, nerr = run([exe])
    assert irc == nrc, '%s exit: run_o0=%d native=%d' % (name, irc, nrc)
    assert iout == nout, '%s DIFF:\n--- run_o0 ---\n%s\n--- native ---\n%s' % (name, iout, nout)
    OUT[name] = iout
    n += 2
    print('recursion ok: %s (rc=%d)' % (name, irc))

# defer must be visible, not merely equal: the unwinding frame runs its defer
# before the refusal is reported, in both backends (SPEC 9 / DESIGN 2.8.5).
assert 'bye' in OUT['defer'] and 'recursion too deep' in OUT['defer'], \
    'defer unwind not visible: %r' % OUT['defer']
assert OUT['defer'].count('bye') >= BUDGET - 2, \
    'too few defers ran on unwind: %d' % OUT['defer'].count('bye')
n += 2

# Fail-closed shape: the uncaught runaway refuses with exit 1, empty stdout,
# and a diagnostic naming the refusal on both backends.
irc, iout, ierr = run(_cxx.run_args(os.path.join(TMP, 'runaway.brat')))
nrc, nout, nerr = run([os.path.join(TMP, 'runaway.exe')])
assert irc == 1 and iout == '', 'runaway must exit 1 with empty stdout, got rc=%d %r' % (irc, iout)
assert nrc == 1 and nout == '', 'native runaway must exit 1 with empty stdout, got rc=%d %r' % (nrc, nout)
assert 'recursion too deep' in ierr, '-O0 diagnostic missing: %r' % ierr
assert 'recursion too deep' in nerr, 'native diagnostic missing: %r' % nerr
n += 4

# Positive control: the same mechanism lets a legal descent through.
pos = os.path.join(TMP, 'down.brat')
open(pos, 'w', encoding='utf-8').write(
    'fn down(n: i64) -> i64 {\n'
    '  if n <= 0 { return 0 }\n'
    '  return down(n - 1)\n'
    '}\n\n'
    'fn main() -> i64 {\n'
    '  print(down(%d))\n'
    '  return 0\n'
    '}\n' % (BUDGET - 2))
rc, out, _ = run(_cxx.run_args(pos))
assert rc == 0 and out == '0\n', 'legal depth %d refused: rc=%d %r' % (BUDGET - 2, rc, out)
n += 2

# The caught payload is the comparand that reaches stdout: it must be byte-equal.
_, cout, _ = run(_cxx.run_args(os.path.join(TMP, 'caught.brat')))
assert cout == 'recursion too deep\n-1\n', 'caught payload: %r' % cout
n += 1

print('RECURSION ALL GREEN (%d asserts)' % n)