"""The -O2 error paths: the interpreter and the native build must agree, and a
credential-shaped env name is refused in both."""
import _cxx
import _scratch
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')
TMP = _scratch.mkdtemp(prefix='brat-o2-')
BRAT = os.path.join(ENGINE, 'brat.py')


_spec = importlib.util.spec_from_file_location('cc_tc_o2', os.path.join(HERE, '_toolchain.py'))
_tc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tc)

CASES = {
    # Far past the worker-pool bound; must not crash.
    'spawn_many': 'fn main() -> i64 {\n'
                  '  for i in 0..400 {\n'
                  '    spawn {\n'
                  '      let x = i * 2\n'
                  '    }\n'
                  '  }\n'
                  '  join_all()\n'
                  '  print("spawn ok")\n'
                  '  return 0\n'
                  '}',
    'repeat_oom': 'fn main() -> i64 {\n'
                  '  try {\n'
                  '    let s = "x".repeat(2000000000)\n'
                  '    print(len(s))\n'
                  '  } catch e {\n'
                  '    print(e)\n'
                  '  }\n'
                  '  return 0\n'
                  '}',
    # A refused name is an err value; its repr must match Python.
    'env_secret': 'fn main() -> i64 {\n'
                  '  print(env_get("MY_SECRET"))\n'
                  '  return 0\n'
                  '}',
    # Positive control: a non-credential name still yields its value.
    'env_ok': 'fn main() -> i64 {\n'
              '  print(env_get("MY_PUBLIC"))\n'
              '  return 0\n'
              '}',
    # Repr quote selection: the interpreter prints double quotes here.
    'repr_quote': 'fn main() -> i64 {\n'
                  '  print(Err("it\'s here"))\n'
                  '  return 0\n'
                  '}',
}
ENVS = {'env_secret': {'MY_SECRET': 'leaked'}, 'env_ok': {'MY_PUBLIC': 'visible'}}

asserts = 0
bad = 0


def check(cond, msg):
    global asserts, bad
    asserts += 1
    if not cond:
        bad += 1
        print('FAIL %s' % msg)


for name, src in CASES.items():
    path = os.path.join(TMP, name + '.brat')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    env = dict(os.environ)
    env.update(ENVS.get(name, {}))

    ip = subprocess.run(_cxx.run_args(path),
                        capture_output=True, text=True, env=env, timeout=120)

    cfile = os.path.join(TMP, name + '.c')
    exe = os.path.join(TMP, name + '.exe')
    with open(cfile, 'w', encoding='utf-8') as f:
        f.write(_cxx.emit_source(src, TMP))
    rb = subprocess.run(_tc.resolve_cc().argv(cfile, exe, static=True) + ['-O2', '-I', ENGINE, '-lm'],
                        capture_output=True, text=True, timeout=300)
    check(rb.returncode == 0, '%s build: %s' % (name, (rb.stdout + rb.stderr)[-300:]))
    if rb.returncode != 0:
        continue
    np = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=120)

    check(ip.returncode == 0, '%s interp rc=%d' % (name, ip.returncode))
    check(np.returncode == 0, '%s native rc=%d (signal → 139/11)' % (name, np.returncode))
    check(ip.stdout == np.stdout, '%s interp=%r native=%r' % (name, ip.stdout[:120], np.stdout[:120]))

    if name == 'env_secret':
        check('leaked' not in ip.stdout and 'leaked' not in np.stdout,
              'env_secret: secret leaked')
        check('refused' in ip.stdout and 'MY_SECRET' in ip.stdout,
              'env_secret: diagnostic must name the refused key and reason')

if bad:
    print('---- O2 RUNTIME FAIL (%d/%d)' % (bad, asserts))
    sys.exit(1)
print('O2 RUNTIME ALL GREEN (%d asserts)' % asserts)