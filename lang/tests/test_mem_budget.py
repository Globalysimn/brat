"""`brat build --mem N` bakes a hard address-space budget into the binary: a
bounded program runs, a growing one aborts instead of taking the host down,
and the same source without --mem is unaffected."""
import _scratch
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')
CLI = os.path.join(ENGINE, 'cli.py')
TMP = _scratch.mkdtemp(prefix='brat-mem-')

BOUNDED = ('fn main() -> i64 {\n'
           '  let mut keep = []\n'
           '  let mut i = 0\n'
           '  while i < 1000 { keep.push("row")  i = i + 1 }\n'
           '  print(len(keep))\n'
           '  return 0\n'
           '}\n')

GROWING = ('fn main() -> i64 {\n'
           '  let mut keep = []\n'
           '  let mut i = 0\n'
           '  while i < 20000000 { keep.push("row" + ":x")  i = i + 1 }\n'
           '  print(len(keep))\n'
           '  return 0\n'
           '}\n')

asserts = 0
bad = 0


def check(cond, msg):
    global asserts, bad
    asserts += 1
    if not cond:
        bad += 1
        print('FAIL %s' % msg)


def cli(*argv):
    return subprocess.run([sys.executable, '-X', 'utf8', CLI] + list(argv),
                          capture_output=True, text=True, timeout=300)


def write(name, src):
    path = os.path.join(TMP, name + '.brat')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    return path


bounded = write('bounded', BOUNDED)
growing = write('growing', GROWING)

rb = cli('build', bounded, '--mem', '32M', '-o', os.path.join(TMP, 'bounded.exe'))
check(rb.returncode == 0, 'bounded build: %s' % (rb.stdout + rb.stderr)[-300:])
if rb.returncode == 0:
    r = subprocess.run([os.path.join(TMP, 'bounded.exe')],
                       capture_output=True, text=True, timeout=60)
    check(r.returncode == 0 and r.stdout == '1000\n',
          'bounded run rc=%d out=%r' % (r.returncode, r.stdout[:60]))

rb = cli('build', growing, '--mem', '32M', '-o', os.path.join(TMP, 'growing.exe'))
check(rb.returncode == 0, 'growing build: %s' % (rb.stdout + rb.stderr)[-300:])
if rb.returncode == 0:
    r = subprocess.run([os.path.join(TMP, 'growing.exe')],
                       capture_output=True, text=True, timeout=120)
    check(r.returncode != 0, 'growing under budget must abort, rc=0')

rc = cli('emit-c', growing)
check(rc.returncode == 0 and '#define BRAT_MEM_BYTES' not in rc.stdout[:200],
      'emit-c without --mem must not bake a budget')

rc = cli('build', bounded, '--mem', 'lots')
check(rc.returncode == 2 and 'bad --mem' in rc.stderr,
      'bad --mem: rc=%d err=%r' % (rc.returncode, rc.stderr[:80]))

rc = cli('emit-c', bounded, '--mem', '16M')
check(rc.returncode == 0 and rc.stdout.startswith('#define BRAT_MEM_BYTES 16777216\n'),
      'emit-c --mem header: %r' % rc.stdout[:60])

if bad:
    print('---- MEM-BUDGET FAIL (%d/%d)' % (bad, asserts))
    sys.exit(1)
print('MEM-BUDGET ALL GREEN (%d asserts)' % asserts)