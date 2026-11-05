"""A `let` re-run by a loop must release the value it overwrites, and a rebind
of a loop-declared binding must reclaim like any other: the unfixed emitter
leaks both per pass and aborts under RLIMIT_AS."""
import _cxx
import _scratch
import importlib.util
import os
import resource
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')
TMP = _scratch.mkdtemp(prefix='brat-loop-reclaim-')
BRAT = os.path.join(ENGINE, 'brat.py')


_spec = importlib.util.spec_from_file_location('tc_loop', os.path.join(HERE, '_toolchain.py'))
_tc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tc)

CAP = 32 * 1024 * 1024

# let re-run + rebind of a loop-local: two leaked 1 KB strings per pass (~80 MB
# over 40k passes if unreclaimed, well past the 32 MB RLIMIT_AS ceiling while
# keeping the pure-Python reference check fast).
RELET = ('fn main() -> i64 {\n'
         '  let base = "x".repeat(1024)\n'
         '  let mut i = 0\n'
         '  let mut acc = 0\n'
         '  while i < 40000 {\n'
         '    let line = base + ",ab"\n'
         '    line = line + ",cd"\n'
         '    acc = acc + len(line)\n'
         '    i = i + 1\n'
         '  }\n'
         '  print(acc)\n'
         '  return 0\n'
         '}\n')

# let re-run alone, no rebind: the previous pass value must still go (~60 MB if leaked).
PURE = ('fn main() -> i64 {\n'
        '  let pad = "y".repeat(1536)\n'
        '  let mut i = 0\n'
        '  let mut acc = 0\n'
        '  while i < 40000 {\n'
        '    let s = pad + "xyz"\n'
        '    acc = acc + len(s)\n'
        '    i = i + 1\n'
        '  }\n'
        '  print(acc)\n'
        '  return 0\n'
        '}\n')

# A loop-local stored into a container must stay alive after the next pass
# releases the binding: the share has its own count.
SHARED = ('fn main() -> i64 {\n'
          '  let mut keep = []\n'
          '  let mut i = 0\n'
          '  while i < 20000 {\n'
          '    let s = "row:" + "ab"\n'
          '    if i % 10000 == 0 { keep.push(s) }\n'
          '    i = i + 1\n'
          '  }\n'
          '  print(len(keep))\n'
          '  print(keep[0])\n'
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
        argv, capture_output=True, text=True, timeout=180,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_AS, (CAP, CAP)))


def run_case(name, src):
    src_path = os.path.join(TMP, name + '.brat')
    with open(src_path, 'w', encoding='utf-8') as f:
        f.write(src)
    ip = subprocess.run([_cxx.build(), 'run', '-O0', '--no-check', src_path],
                        capture_output=True, text=True, timeout=180)
    cfile = os.path.join(TMP, name + '.c')
    exe = os.path.join(TMP, name + '.exe')
    with open(cfile, 'w', encoding='utf-8') as f:
        f.write(_cxx.emit_source(src, TMP))
    rb = _tc.cc_cached(_tc.resolve_cc().argv(cfile, exe) + ['-O2', '-I', ENGINE, '-lm'],
                       cfile, exe)
    check(rb.returncode == 0, '%s build: %s' % (name, (rb.stdout + rb.stderr)[-300:]))
    if rb.returncode != 0:
        return
    np = capped([exe])
    check(np.returncode == 0, '%s native rc=%d under %d MiB (a leak aborts)'
          % (name, np.returncode, CAP // (1024 * 1024)))
    check(ip.returncode == 0 and ip.stdout == np.stdout,
          '%s -O0=%r -O2=%r' % (name, ip.stdout[:60], np.stdout[:60]))


run_case('relet', RELET)
run_case('pure', PURE)
run_case('shared', SHARED)

if bad:
    print('---- LOOP-RECLAIM FAIL (%d/%d)' % (bad, asserts))
    sys.exit(1)
print('LOOP-RECLAIM ALL GREEN (%d asserts)' % asserts)