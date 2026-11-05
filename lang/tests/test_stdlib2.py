import _cxx
import _scratch
import importlib.util
import os
import subprocess
import sys


HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, '..', 'engine'))
EXAMPLES = os.path.abspath(os.path.join(HERE, '..', 'examples'))
BRAT = os.path.join(ENGINE, 'brat.py')
TMP = _scratch.mkdtemp(prefix='brat-stdlib-')


_tc_spec = importlib.util.spec_from_file_location('cc_toolchain', os.path.join(HERE, '_toolchain.py'))
_tc = importlib.util.module_from_spec(_tc_spec)
_tc_spec.loader.exec_module(_tc)

LIBS = ['csv.brat', 'path.brat']
n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


for lib in LIBS:
    path = os.path.join(EXAMPLES, lib)
    check(os.path.isfile(path), lib + ' missing')

    a = subprocess.run(_cxx.run_args(path),
                        capture_output=True, text=True, timeout=300)
    check(a.returncode == 0, lib + ' exit=%d err=%s' % (a.returncode, a.stderr[-300:]))
    check(os.path.splitext(lib)[0] + ' ok' in a.stdout, lib + ' self-check missing: ' + a.stdout[-200:])

    c = subprocess.run(_cxx.cli_args('check', path),
                        capture_output=True, text=True, timeout=300)
    check(c.returncode == 0, lib + ' check exit=%d out=%s' % (c.returncode, c.stdout[-300:]))
    check('0 error(s), 0 warning(s)' in c.stdout, lib + ' check not clean: ' + c.stdout[-300:])

    tag = os.path.splitext(lib)[0]
    cfile = os.path.join(TMP, tag + '.c')
    exe = os.path.join(TMP, tag + '.exe')
    with open(cfile, 'w', encoding='utf-8') as f:
        f.write(_cxx.emit_file(path))
    r = subprocess.run(_tc.resolve_cc().argv(cfile, exe, static=True) + ["-O2", "-I", ENGINE],
                        capture_output=True, text=True, timeout=300)
    check(r.returncode == 0, lib + ' build failed: ' + (r.stdout + r.stderr)[-300:])
    b = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    check(a.stdout == b.stdout and a.returncode == b.returncode,
          lib + ' native parity:\ninterp: %r\nnative: %r' % (a.stdout[-300:], b.stdout[-300:]))

print('STDLIB2 ALL GREEN (%d asserts)' % n)