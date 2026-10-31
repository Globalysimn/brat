import _scratch
import importlib.util
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')
LANG_EX = os.path.join(HERE, '..', 'examples')
TMP = _scratch.mkdtemp(prefix='brat-native-')
os.makedirs(TMP, exist_ok=True)

_spec = importlib.util.spec_from_file_location('cc_native', os.path.join(ENGINE, 'codegen_c.py'))
cc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cc)

_tc_spec = importlib.util.spec_from_file_location('cc_toolchain', os.path.join(ENGINE, 'toolchain.py'))
_tc = importlib.util.module_from_spec(_tc_spec)
_tc_spec.loader.exec_module(_tc)

BRAT = os.path.join(ENGINE, 'brat.py')
GOOD = ['hello.brat', 'control.brat', 'own_good.brat', 'result.brat', 'defer.brat',
        'kpipe.brat', 'krouter.brat', 'kapp.brat', 'krange.brat', 'impl.brat',
        'trait.brat', 'struct.brat', 'enum.brat',
        'generic.brat', 'closure.brat', 'letflow.brat', 'enummeth.brat',
        'nested.brat', 'modapi.brat', 'modapp.brat', 'lib.brat', 'klib.brat',
        'strlib.brat', 'veclib.brat', 'stdlib.brat', 'json.brat', 'jsontest.brat',
        'map.brat', 'fast.brat', 'spawn.brat',
        'inop.brat', 'meth2.brat', 'f64fast.brat', 'meth3.brat',
        'fastprint.brat', 'compound.brat', 'destruct.brat', 'meth4.brat',
        'defaults.brat', 'trycatch.brat', 'guards.brat', 'utf8.brat',
        'syslib.brat', 'funclib.brat', 'rematch.brat', 'jsonlibtest.brat',
         'fastvec.brat', 'own_paths.brat', 'boolmatch.brat', 'nested2.brat',
         'span.brat', 'precedence.brat', 'interp.brat', 'interp_warn.brat']
PROGS = [os.path.join(LANG_EX, f) for f in GOOD]

TS = re.compile(r'"ts": [\d.]+')


def norm(s):
    return TS.sub('"ts": T', s.replace('\r\n', '\n'))


def run_interp(path):
    p = subprocess.run([sys.executable, BRAT, path], capture_output=True, text=True, timeout=120)
    return p.returncode, norm(p.stdout)


def build(path):
    tag = os.path.splitext(os.path.basename(path))[0]
    cfile = os.path.join(TMP, tag + '.c')
    exe = os.path.join(TMP, tag + '.exe')
    with open(cfile, 'w', encoding='utf-8') as f:
        f.write(cc.emit_file(path))
    r = _tc.cc_cached(_tc.resolve_cc().argv(cfile, exe, static=True) + ["-O2", "-I", ENGINE, "-lm"],
                      cfile, exe)
    assert r.returncode == 0, 'cc failed for %s:\n%s' % (tag, (r.stdout + r.stderr)[-2000:])
    return exe


def run_native(exe):
    p = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    return p.returncode, norm(p.stdout)


n = 0
for prog in PROGS:
    rc1, out1 = run_interp(prog)
    exe = build(prog)
    rc2, out2 = run_native(exe)
    assert rc1 == rc2, '%s exit: interp=%d native=%d' % (prog, rc1, rc2)
    if os.path.basename(prog) == 'spawn.brat':
        left, right = out1.splitlines(), out2.splitlines()
        assert sorted(left[:3]) == sorted(right[:3]) and left[3:] == right[3:], \
            '%s DIFF (spawn send order is unspecified):\n%s\n%s' % (prog, out1, out2)
    else:
        assert out1 == out2, '%s DIFF:\n--- interp ---\n%s\n--- native ---\n%s' % (
            prog, out1[:1500], out2[:1500])
    n += 1
    print('native ok: %s' % os.path.basename(prog))

print('NATIVE ALL GREEN (%d progs)' % n)