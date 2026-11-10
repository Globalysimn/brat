import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _cxx      # noqa: E402
import _scratch  # noqa: E402

ENGINE = os.path.join(HERE, '..', 'engine')
EXAMPLES = os.path.join(HERE, '..', 'examples')
SPEC = os.path.join(HERE, '..', 'spec')
CXX = _cxx.build()

NATIVE_EMITTER = os.path.join(HERE, 'test_native_emitter.py')

GOOD = [os.path.join(EXAMPLES, f) for f in [
    'hello.brat', 'control.brat', 'own_good.brat', 'result.brat', 'defer.brat',
    'kpipe.brat', 'krouter.brat', 'kapp.brat', 'krange.brat', 'impl.brat',
    'trait.brat', 'struct.brat', 'enum.brat', 'generic.brat', 'closure.brat',
    'letflow.brat', 'enummeth.brat', 'nested.brat', 'modapi.brat', 'modapp.brat',
    'json.brat', 'jsontest.brat', 'map.brat', 'fast.brat', 'spawn.brat',
    'inop.brat', 'meth2.brat', 'f64fast.brat', 'meth3.brat', 'fastprint.brat',
    'compound.brat', 'destruct.brat', 'meth4.brat', 'defaults.brat', 'trycatch.brat',
    'guards.brat', 'utf8.brat', 'syslib.brat', 'funclib.brat', 'rematch.brat',
    'jsonlibtest.brat', 'fastvec.brat', 'own_paths.brat', 'boolmatch.brat',
    'nested2.brat', 'span.brat', 'precedence.brat', 'interp.brat', 'interp_warn.brat',
]]


def run_py(*cmd, timeout=120):
    return subprocess.run([sys.executable, *cmd], capture_output=True, text=True, timeout=timeout)


def run_cxx(*cmd, timeout=120, env=None):
    return subprocess.run([CXX, *cmd], capture_output=True, text=True, timeout=timeout, env=env)


n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


GOOD_T = [f for f in GOOD if 'own_' not in f]
p = run_cxx('check', *GOOD_T)
check(p.returncode == 0, 'cxx check good: ' + (p.stdout + p.stderr)[-500:])
lines = [l for l in p.stdout.splitlines() if 'error(s)' in l and 'type error(s)' not in l]
check(len(lines) >= len(GOOD_T) and all('0 error(s)' in l for l in lines),
      'cxx own good counts: ' + p.stdout[-500:])
tlines = [l for l in p.stdout.splitlines() if 'type error(s)' in l]
check(len(tlines) >= len(GOOD_T) and all('0 type error(s)' in l for l in tlines),
      'cxx typer good counts: ' + p.stdout[-500:])

for f in ('hello.brat', 'control.brat', 'result.brat', 'defer.brat', 'impl.brat',
          'trait.brat', 'enum.brat', 'closure.brat', 'modapp.brat', 'trycatch.brat', 'utf8.brat'):
    pr = run_cxx('run', os.path.join(EXAMPLES, f))
    check(pr.returncode == 0, f + ' exit!=0: ' + (pr.stdout + pr.stderr)[-500:])

for bad, cnt in [('own_bad.brat', 4), ('own_move.brat', 2), ('own_sig.brat', 1), ('own_paths_bad.brat', 3)]:
    p = run_cxx('check', os.path.join(EXAMPLES, bad))
    out = p.stdout + p.stderr
    check(p.returncode == 1 and ('%d error(s)' % cnt) in out, bad + ': ' + out[-500:])
    check(': error: ' in out, bad + ' diag format: ' + out[-500:])

NEG = _scratch.mkdtemp(prefix='brat-neg-')
with open(os.path.join(NEG, 'nonex.brat'), 'w', encoding='utf-8') as f:
    f.write('enum E {\n  A,\n  B\n}\nlet x = A\nmatch x {\n  A => print(1)\n}\n')

p = run_cxx('check', os.path.join(EXAMPLES, 'own_priv.brat'))
check('private in module `modapi`' in (p.stdout + p.stderr) and p.returncode == 2,
      'own_priv load error: rc=%d out=%r' % (p.returncode, (p.stdout + p.stderr)[-300:]))
p = run_cxx('run', os.path.join(EXAMPLES, 'own_priv.brat'))
check(p.returncode != 0 and 'private in module `modapi`' in (p.stdout + p.stderr),
      'own_priv run: rc=%d err=%r' % (p.returncode, (p.stdout + p.stderr)[-300:]))

p = run_cxx('run', os.path.join(NEG, 'nonex.brat'))
check(p.returncode == 1 and 'non-exhaustive match' in (p.stdout + p.stderr),
      'nonex: ' + (p.stdout + p.stderr)[-300:])
with open(os.path.join(NEG, 'divz.brat'), 'w', encoding='utf-8') as f:
    f.write('let x = 1\nlet y = 0\nprint(x / y)\n')
p = run_cxx('run', '--trace-lines', os.path.join(NEG, 'divz.brat'))
check(p.returncode == 1 and 'at line 3' in p.stderr, 'divz line: ' + (p.stdout + p.stderr)[-300:])

with open(os.path.join(NEG, 'tbad.brat'), 'w', encoding='utf-8') as f:
    f.write('fn f(a: i64) -> i64 {\n  return "nope"\n}\nlet x = f(1)\n')
p = run_cxx('check', os.path.join(NEG, 'tbad.brat'))
check(p.returncode == 1 and 'want i64, got str' in (p.stdout + p.stderr),
      'tbad: ' + (p.stdout + p.stderr)[-300:])
with open(os.path.join(NEG, 'tbad2.brat'), 'w', encoding='utf-8') as f:
    f.write('fn g(a: i64, b: i64) -> i64 {\n  return a + b\n}\nlet x = g("s", 1)\n')
p = run_cxx('check', os.path.join(NEG, 'tbad2.brat'))
check(p.returncode == 1 and 'arg 1 of `g`' in (p.stdout + p.stderr),
      'tbad2: ' + (p.stdout + p.stderr)[-300:])

with open(os.path.join(NEG, 'argv.brat'), 'w', encoding='utf-8') as f:
    f.write('let a = args()\nassert(len(a) == 2)\nassert(a[0] == "one")\nassert(a[1] == "two")\nprint("argv ok")\n')
p = run_cxx('run', os.path.join(NEG, 'argv.brat'), '--', 'one', 'two')
check(p.returncode == 0 and 'argv ok' in p.stdout, 'argv: ' + (p.stdout + p.stderr)[-300:])

# `.` is its own token and a dotted access is one AST node: `v.push(3)` must
# not arrive as a dotted identifier, and an uppercase base keeps its shape.
toks = [(k, v) for k, v, _ln in _cxx.lex_source('a.b(1)\nlet x = 1.5\n1..3\n')]
check(('DOT', '.') in toks, 'dot token: %r' % toks)
check(('NUM', '1.5') in toks, 'num dot: %r' % toks)
check([v for k, v in toks if k == 'ID'] == ['a', 'b', 'x'], 'id split: %r' % toks)
check(('OP', '..') in toks, 'range token: %r' % toks)

ast_probe_path = os.path.join(NEG, 'ast_shape.brat')
with open(ast_probe_path, 'w', encoding='utf-8') as f:
    f.write('fn main() -> i64 {\n  let mut v = [1]\n  v.push(3)\n  p.x = 9\n'
            '  a.b.c\n  C.len()\n  return 0\n}\n')
ast_txt = run_cxx('ast', ast_probe_path).stdout.strip()
check(_cxx.sx(('expr', ('mcall', ('var', 'v'), 'push', [('num', 3)]))) in ast_txt, 'mcall shape: %r' % ast_txt)
check(_cxx.sx(('setfield', ('var', 'p'), 'x', ('num', 9))) in ast_txt, 'setfield shape: %r' % ast_txt)
check(_cxx.sx(('expr', ('field', ('field', ('var', 'a'), 'b'), 'c'))) in ast_txt, 'field chain: %r' % ast_txt)
check(_cxx.sx(('expr', ('mcall', ('call', 'C', []), 'len', []))) in ast_txt, 'upper base: %r' % ast_txt)

with open(os.path.join(NEG, 'recv.brat'), 'w', encoding='utf-8') as f:
    f.write('fn main() -> i64 {\n  let mut V = [3, 1]\n  V.push(2)\n  V.sort()\n  print(V[0])\n'
            '  let mut M = {}\n  M.set("a", 1)\n  print(M.has("a"))\n'
            '  let mut P = Point { x: 1 }\n  P.x = 9\n  print(P.x)\n  return 0\n}\n')
p = run_cxx('run', os.path.join(NEG, 'recv.brat'))
check(p.returncode == 0 and p.stdout == '1\ntrue\n9\n', 'upper receivers: ' + (p.stdout + p.stderr)[-300:])

p = run_py(NATIVE_EMITTER)
check(p.returncode == 0 and 'native emitter tests ok' in p.stdout,
      'native emitter: ' + (p.stdout + p.stderr)[-500:])

if os.environ.get('BRAT_NESTED', '1') == '1':
    EDGES = os.path.join(HERE, 'test_language_edges.py')
    p = run_py(EDGES, timeout=300)
    check(p.returncode == 0 and 'edge failures: 0' in p.stdout, 'edges: ' + (p.stdout + p.stderr)[-500:])

check(os.path.isfile(os.path.join(SPEC, 'SPEC.md')), 'SPEC.md missing')

print('LANG ALL GREEN (%d asserts)' % n)
