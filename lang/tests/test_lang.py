import _scratch
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')
EXAMPLES = os.path.join(HERE, '..', 'examples')
SPEC = os.path.join(HERE, '..', 'spec')

BRAT = os.path.join(ENGINE, 'brat.py')
OWN = os.path.join(ENGINE, 'borrow.py')
CBACK2 = os.path.join(HERE, 'test_codegen_c.py')

LANG_EX = os.path.join(HERE, '..', 'examples')
GOOD = [os.path.join(LANG_EX, f) for f in ['hello.brat', 'control.brat', 'own_good.brat', 'result.brat', 'defer.brat', 'kpipe.brat', 'krouter.brat', 'kapp.brat', 'krange.brat', 'impl.brat', 'trait.brat', 'struct.brat', 'enum.brat', 'generic.brat', 'closure.brat', 'letflow.brat', 'enummeth.brat', 'nested.brat', 'modapi.brat', 'modapp.brat', 'json.brat', 'jsontest.brat', 'map.brat', 'fast.brat', 'spawn.brat', 'inop.brat', 'meth2.brat', 'f64fast.brat', 'meth3.brat', 'fastprint.brat', 'compound.brat', 'destruct.brat', 'meth4.brat', 'defaults.brat', 'trycatch.brat', 'guards.brat', 'utf8.brat', 'syslib.brat', 'funclib.brat', 'rematch.brat', 'jsonlibtest.brat', 'fastvec.brat', 'own_paths.brat', 'boolmatch.brat', 'nested2.brat', 'span.brat', 'precedence.brat', 'interp.brat',
         'interp_warn.brat']]


def run(*cmd, timeout=120):
    p = subprocess.run([sys.executable, *cmd], capture_output=True, text=True, timeout=timeout)
    return p


n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


for f in GOOD:
    p = run(BRAT, f)
    check(p.returncode == 0, f + ' exit!=0: ' + p.stderr[-500:])

p = run(OWN, *GOOD)
check(p.returncode == 0, 'own good: ' + p.stdout[-500:])
lines = [l for l in p.stdout.splitlines() if 'error(s)' in l]
check(len(lines) >= len(GOOD) and all('0 error(s)' in l for l in lines), 'own good counts: ' + p.stdout[-500:])

for bad, cnt in [('own_bad.brat', 4), ('own_move.brat', 2), ('own_sig.brat', 1), ('own_paths_bad.brat', 3)]:
    p = run(OWN, os.path.join(EXAMPLES, bad))
    check(p.returncode == 1 and ('%d error(s)' % cnt) in p.stdout, bad + ': ' + p.stdout[-500:])
    check(': error: ' in p.stdout, bad + ' diag format: ' + p.stdout[-500:])

import tempfile
NEG = _scratch.mkdtemp(prefix='brat-neg-')
with open(os.path.join(NEG, 'nonex.brat'), 'w', encoding='utf-8') as f:
    f.write('enum E {\n  A,\n  B\n}\nlet x = A\nmatch x {\n  A => print(1)\n}\n')
# `own_priv.brat` reaches for a private function of another module. The loader
# refuses it before either checker runs, so it is not a GOOD program: the words
# are the contract.
p = run(OWN, os.path.join(EXAMPLES, 'own_priv.brat'))
check('private in module `modapi`' in p.stdout and p.returncode == 2,
      'own_priv load error: rc=%d out=%r' % (p.returncode, (p.stdout + p.stderr)[-300:]))
p = run(BRAT, os.path.join(EXAMPLES, 'own_priv.brat'))
check(p.returncode == 1 and 'private in module `modapi`' in p.stderr,
      'own_priv interp: rc=%d err=%r' % (p.returncode, (p.stdout + p.stderr)[-300:]))

p = run(BRAT, os.path.join(NEG, 'nonex.brat'))
check(p.returncode == 1 and 'non-exhaustive match' in p.stderr, 'nonex: ' + (p.stdout + p.stderr)[-300:])
with open(os.path.join(NEG, 'divz.brat'), 'w', encoding='utf-8') as f:
    f.write('let x = 1\nlet y = 0\nprint(x / y)\n')
p = run(BRAT, os.path.join(NEG, 'divz.brat'))
check(p.returncode == 1 and 'at line 3' in p.stderr, 'divz line: ' + (p.stdout + p.stderr)[-300:])

TYPER = os.path.join(ENGINE, 'typer.py')
GOOD_T = [f for f in GOOD if 'own_' not in f]
p = run(TYPER, *GOOD_T)
check(p.returncode == 0, 'typer good: ' + (p.stdout + p.stderr)[-500:])
tlines = [l for l in p.stdout.splitlines() if 'type error(s)' in l]
check(len(tlines) >= len(GOOD_T) and all('0 type error(s)' in l for l in tlines), 'typer good counts: ' + p.stdout[-500:])

with open(os.path.join(NEG, 'tbad.brat'), 'w', encoding='utf-8') as f:
    f.write('fn f(a: i64) -> i64 {\n  return "nope"\n}\nlet x = f(1)\n')
p = run(TYPER, os.path.join(NEG, 'tbad.brat'))
check(p.returncode == 1 and 'want i64, got str' in p.stdout, 'tbad: ' + (p.stdout + p.stderr)[-300:])
with open(os.path.join(NEG, 'tbad2.brat'), 'w', encoding='utf-8') as f:
    f.write('fn g(a: i64, b: i64) -> i64 {\n  return a + b\n}\nlet x = g("s", 1)\n')
p = run(TYPER, os.path.join(NEG, 'tbad2.brat'))
check(p.returncode == 1 and 'arg 1 of `g`' in p.stdout, 'tbad2: ' + (p.stdout + p.stderr)[-300:])

with open(os.path.join(NEG, 'argv.brat'), 'w', encoding='utf-8') as f:
    f.write('let a = args()\nassert(len(a) == 2)\nassert(a[0] == "one")\nassert(a[1] == "two")\nprint("argv ok")\n')
p = run(BRAT, os.path.join(NEG, 'argv.brat'), 'one', 'two')
check(p.returncode == 0 and 'argv ok' in p.stdout, 'argv: ' + (p.stdout + p.stderr)[-300:])

# `.` is its own token and a dotted access is one AST node: `v.push(3)` must
# not arrive as a dotted identifier, and an uppercase base keeps its shape.
sys.path.insert(0, ENGINE)
from brat.lexer import Cur, lex
from brat.parser import parse_prog

toks = [(t.k, t.v) for t in lex('a.b(1)\nlet x = 1.5\n1..3\n')]
check(('DOT', '.') in toks, 'dot token: %r' % toks)
check(('NUM', '1.5') in toks, 'num dot: %r' % toks)
check([v for k, v in toks if k == 'ID'] == ['a', 'b', 'x'], 'id split: %r' % toks)
check(('OP', '..') in toks, 'range token: %r' % toks)

ast = parse_prog(Cur(lex('fn main() -> i64 {\n  let mut v = [1]\n  v.push(3)\n  p.x = 9\n'
                         '  a.b.c\n  C.len()\n  return 0\n}\n')))
body = ast[1][0][3]
check(body[1] == ('expr', ('mcall', ('var', 'v'), 'push', [('num', 3)])), 'mcall shape: %r' % (body[1],))
check(body[2] == ('setfield', ('var', 'p'), 'x', ('num', 9)), 'setfield shape: %r' % (body[2],))
check(body[3] == ('expr', ('field', ('field', ('var', 'a'), 'b'), 'c')), 'field chain: %r' % (body[3],))
check(body[4] == ('expr', ('mcall', ('call', 'C', []), 'len', [])), 'upper base: %r' % (body[4],))

with open(os.path.join(NEG, 'recv.brat'), 'w', encoding='utf-8') as f:
    f.write('fn main() -> i64 {\n  let mut V = [3, 1]\n  V.push(2)\n  V.sort()\n  print(V[0])\n'
            '  let mut M = {}\n  M.set("a", 1)\n  print(M.has("a"))\n'
            '  let mut P = Point { x: 1 }\n  P.x = 9\n  print(P.x)\n  return 0\n}\n')
p = run(BRAT, os.path.join(NEG, 'recv.brat'))
check(p.returncode == 0 and p.stdout == '1\nTrue\n9\n', 'upper receivers: ' + (p.stdout + p.stderr)[-300:])

p = run(CBACK2)
check(p.returncode == 0 and 'codegen_c tests ok' in p.stdout, 'codegen_c: ' + p.stdout[-500:])

# run_all.py runs every test file once and sets BRAT_NESTED=0 so edge tests are
# not executed twice; run test_lang.py alone to keep them in this file's run.
if os.environ.get('BRAT_NESTED', '1') == '1':
    EDGES = os.path.join(HERE, 'test_language_edges.py')
    p = run(EDGES, timeout=300)
    check(p.returncode == 0 and 'edge failures: 0' in p.stdout, 'edges: ' + (p.stdout + p.stderr)[-500:])

check(os.path.isfile(os.path.join(SPEC, 'SPEC.md')), 'SPEC.md missing')

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
for root, dirs, _ in os.walk(ROOT):
    if '__pycache__' in dirs:
        shutil.rmtree(os.path.join(root, '__pycache__'), ignore_errors=True)
    for f in os.listdir(root):
        if f.endswith('.pyc'):
            try:
                os.unlink(os.path.join(root, f))
            except OSError:
                pass

print('LANG ALL GREEN (%d asserts)' % n)