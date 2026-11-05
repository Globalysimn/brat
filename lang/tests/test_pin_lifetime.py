"""Regression tests for string pinning and extern effect parsing."""
import os
import subprocess
import sys
import tempfile

import _cxx

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CXX = _cxx.build()


def run(args, env=None):
    return subprocess.run(args, capture_output=True, text=True, timeout=90,
                          env={**os.environ, **(env or {})})


with tempfile.TemporaryDirectory() as td:
    src = os.path.join(td, 'pin.brat')
    with open(src, 'w', encoding='utf-8') as f:
        f.write('''let s = "hello"
let p = ptr_of_str(s)
for i in 0..5000 {
  let t = "k{i}"
  let q = ptr_of_str(t)
  let ignored = str_from_ptr(q, len(t))
}
print(str_from_ptr(p, 5))
''')
    for cmd in (_cxx.cli_args('run', '--interp', src),
                [CXX, 'run', src],
                _cxx.cli_args('run', src)):
        r = run(cmd)
        assert r.returncode == 0 and r.stdout.strip() == 'hello', (cmd, r.stdout, r.stderr)

    src = os.path.join(td, 'effect.brat')
    with open(src, 'w', encoding='utf-8') as f:
        f.write('extern fn f(p: ptr pure)\n')
    for cmd, env in (([CXX, 'check', src], {}),
                     (_cxx.cli_args('check', src), {'BRAT_NO_NATIVE_CLI': '1'})):
        r = run(cmd, env)
        assert r.returncode != 0 and 'effect of p, not a parameter type' in r.stdout + r.stderr, (cmd, r.stdout, r.stderr)

print('pin lifetime and extern effects: ok')
