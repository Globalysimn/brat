"""A `par for ... reduce` must give one answer, whatever the machine does.

The roadmap carried this as a gap: "with floats the result depends on the
number of workers and on the schedule". It does not, and never did in this
implementation -- the fold runs after the join, in index order, so the
combining is a left-to-right sum and the workers only compute the elements.

That claim was wrong, so it is retired by a test rather than fixed by code: a
guarantee nobody verifies is a rumour. `BRAT_THREADS` pins the pool so the test
can vary the worker count instead of inheriting it from the machine.

What this does NOT prove: that a *parallel* combine would stay identical -- it
would not for f64, which is why the combine is sequential and why the exact
accumulator is listed as an optimisation, not a correction.
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, '..', 'engine'))
CXX = os.path.abspath(os.path.join(HERE, '..', 'cxx', 'bin', 'brat-cxx'))
TMP = _scratch.mkdtemp(prefix='brat-parfloat-')

n = 0
FAILED = []


def check(cond, msg):
    global n
    if cond:
        n += 1
    else:
        FAILED.append(msg)
        print('  FAIL: %s' % msg)


FLOAT_FOLD = '''
fn add_f(a: f64, b: f64) -> f64 { return a + b }

fn work(x: i64) -> f64 {
  return 1.0 / (1.0 + x * 1.0)
}

fn main() -> i64 {
  let n = 20000
  let mut v = []
  for i in 0..n { v.push(i) }
  let par = par for x in v reduce(add_f, 0.0) { return work(x) }
  let mut seq = 0.0
  for x in v { seq = add_f(seq, work(x)) }
  if par == seq { print("identical") } else { print("DIFFERS") }
  print(par)
  return 0
}
'''

INT_FOLD = '''
fn add(a: i64, b: i64) -> i64 { return a + b }
fn work(x: i64) -> i64 { return x * x - 3 }
fn main() -> i64 {
  let mut v = []
  for i in 0..2000 { v.push(i) }
  let par = par for x in v reduce(add, 0) { return work(x) }
  let mut seq = 0
  for x in v { seq = add(seq, work(x)) }
  print(par)
  print(seq)
  return 0
}
'''


def write(name, src):
    path = os.path.join(TMP, name)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(src)
    return path


def run_native(path, threads):
    env = dict(os.environ, BRAT_THREADS=str(threads),
               BRAT_CACHE=os.path.join(TMP, 'cache'))
    return subprocess.run([CXX, 'run', path], capture_output=True, text=True,
                          timeout=600, env=env)


def run_o0(path, threads):
    env = dict(os.environ, BRAT_THREADS=str(threads))
    return subprocess.run(_cxx.run_args(path),
                          capture_output=True, text=True, timeout=600, env=env)


def fold_is_deterministic():
    path = write('float_fold.brat', FLOAT_FOLD)
    seen = set()
    for threads in (1, 2, 4, 8):
        native = run_native(path, threads)
        check(native.returncode == 0,
              'native threads=%d rc=%d %r' % (threads, native.returncode, native.stderr[-300:]))
        lines = native.stdout.splitlines()
        check(lines and lines[0] == 'identical',
              'native threads=%d: par != seq (%r)' % (threads, lines[:2]))
        seen.add(native.stdout)
        unopt = run_o0(path, threads)
        check(unopt.returncode == 0 and unopt.stdout == native.stdout,
              'run -O0 threads=%d disagrees: %r vs %r'
              % (threads, unopt.stdout, native.stdout))
    check(len(seen) == 1, 'float fold changed with the worker count: %r' % (seen,))


def int_fold_matches():
    path = write('int_fold.brat', INT_FOLD)
    outs = set()
    for threads in (1, 3, 8):
        native = run_native(path, threads)
        lines = native.stdout.splitlines()
        check(len(lines) == 2 and lines[0] == lines[1],
              'int threads=%d par=%r' % (threads, lines))
        outs.add(native.stdout)
    check(len(outs) == 1, 'int fold changed with the worker count: %r' % (outs,))


def empty_fold():
    path = write('empty.brat', '''
fn add_f(a: f64, b: f64) -> f64 { return a + b }
fn main() -> i64 {
  let v = []
  let z = par for x in v reduce(add_f, 7.5) { return x }
  print(z)
  return 0
}
''')
    native = run_native(path, 4)
    check(native.returncode == 0 and native.stdout == '7.5\n',
          'empty fold wants the unit, got %r' % native.stdout)


def main():
    for fn in (fold_is_deterministic, int_fold_matches, empty_fold):
        try:
            fn()
        except Exception as e:  # noqa: BLE001
            FAILED.append('%s raised %r' % (fn.__name__, e))
            print('  ERROR in %s: %r' % (fn.__name__, e))
    if FAILED:
        print('\nPARFOR FLOAT FAILED -- %d problem(s), %d ok:' % (len(FAILED), n))
        for f in FAILED:
            print('  - %s' % f)
        sys.exit(1)
    print('PARFOR FLOAT ALL GREEN (%d asserts, 1/2/4/8 workers, -O0 and -O2)' % n)


if __name__ == '__main__':
    main()
