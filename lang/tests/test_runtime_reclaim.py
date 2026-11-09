"""A rebind must reclaim the overwritten value while a value aliased elsewhere
stays alive: the unfixed emitter either grows per pass or aborts under RLIMIT_AS."""
import _cxx
import _scratch
import importlib.util
import os
import resource
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')
TMP = _scratch.mkdtemp(prefix='brat-reclaim-')


_spec = importlib.util.spec_from_file_location('tc_reclaim', os.path.join(HERE, '_toolchain.py'))
_tc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tc)

CAP = 32 * 1024 * 1024  # the measured leak crosses 32 MiB before n = 100000

SRC = ('fn build(n: i64) -> i64 {\n'
       '  let mut s = ""\n'
       '  let mut i = 0\n'
       '  while i < n {\n'
       '    s = s + "x"\n'
       '    i = i + 1\n'
       '  }\n'
       '  return len(s)\n'
       '}\n'
       '\n'
       'fn main() -> i64 {\n'
       '  print(build(%d))\n'
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


def capped(argv):
    return subprocess.run(
        argv, capture_output=True, text=True, timeout=120,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_AS, (CAP, CAP)))


for n in (40000, 100000):
    path = os.path.join(TMP, 'concat%d.brat' % n)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(SRC % n)

    ip = subprocess.run(_cxx.run_args(path),
                        capture_output=True, text=True, timeout=120)

    cfile = os.path.join(TMP, 'concat%d.c' % n)
    exe = os.path.join(TMP, 'concat%d.exe' % n)
    with open(cfile, 'w', encoding='utf-8') as f:
        f.write(_cxx.emit_source(SRC % n, TMP))
    rb = subprocess.run(_tc.resolve_cc().argv(cfile, exe) + ['-O2', '-I', ENGINE, '-lm'],
                        capture_output=True, text=True, timeout=300)
    check(rb.returncode == 0, 'n=%d build: %s' % (n, (rb.stdout + rb.stderr)[-300:]))
    if rb.returncode != 0:
        continue

    np = capped([exe])
    check(np.returncode == 0, 'n=%d native rc=%d under %d MiB (a leak aborts)'
          % (n, np.returncode, CAP // (1024 * 1024)))
    check(ip.stdout == np.stdout, 'n=%d run_o0=%r native=%r'
          % (n, ip.stdout[:60], np.stdout[:60]))

# A same-named binding in an earlier function must not suppress release.
F4 = ('fn helper() -> i64 {\n'
      '  let s = [1, 2, 3]\n'
      '  let box = [s]\n'
      '  return len(box)\n'
      '}\n'
      'fn acc(n: i64) -> i64 {\n'
      '  let mut s = ""\n'
      '  let mut i = 0\n'
      '  while i < n { s = s + "x"  i = i + 1 }\n'
      '  return len(s)\n'
      '}\n'
      'fn main() -> i64 { print(helper())  print(acc(100000))  return 0 }\n')

# A loop-local literal is never released, so heap-ifying it grows with n.
F5 = ('fn main() -> i64 {\n'
      '  let mut i = 0\n'
      '  while i < 2000000 { let s = "abcdefghij"  i = i + 1 }\n'
      '  print(i)\n'
      '  return 0\n'
      '}\n')

for name, src, want, emit in (('f4', F4, '1\n100000', _cxx.emit_source),
                              ('f4_mrt', F4, '1\n100000', _cxx.emit_source),
                              ('f5', F5, '2000000', _cxx.emit_source)):
    cfile = os.path.join(TMP, name + '.c')
    exe = os.path.join(TMP, name + '.exe')
    with open(cfile, 'w', encoding='utf-8') as f:
        f.write(emit(src, TMP))
    rb = subprocess.run(_tc.resolve_cc().argv(cfile, exe) + ['-O2', '-I', ENGINE, '-lm'],
                        capture_output=True, text=True, timeout=300)
    check(rb.returncode == 0, '%s build: %s' % (name, (rb.stdout + rb.stderr)[-300:]))
    if rb.returncode != 0:
        continue
    np = capped([exe])
    check(np.returncode == 0, '%s native rc=%d under %d MiB (a leak aborts)'
          % (name, np.returncode, CAP // (1024 * 1024)))
    check(np.stdout.strip() == want, '%s out=%r want=%r' % (name, np.stdout[:60], want))

if bad:
    print('---- RECLAIM FAIL (%d/%d)' % (bad, asserts))
    sys.exit(1)
print('RECLAIM ALL GREEN (%d asserts)' % asserts)