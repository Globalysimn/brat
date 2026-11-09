import _cxx
import _scratch
import os
import re
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def run_cli(*args, cwd=None):
    return subprocess.run(_cxx.cli_args(*(list(args))), capture_output=True,
                          text=True, timeout=120, cwd=cwd)


def run_brat(src, cwd):
    return subprocess.run(_cxx.run_args(src), capture_output=True,
                          text=True, timeout=120, cwd=cwd)


def tcp_ask(addr, payload, tries=100):
    """Connect (retrying while the service starts), send, return the reply."""
    last = None
    for _ in range(tries):
        try:
            with socket.create_connection(addr, timeout=5) as s:
                s.sendall(payload)
                return s.recv(64).decode('utf-8')
        except OSError as e:
            last = e
            time.sleep(0.1)
    raise AssertionError('no answer from service: %s' % last)


def scaffold(kind, tmp):
    """`brat new` a fresh project of `kind` and return its directory."""
    name = 'app_' + kind
    p = run_cli('new', name, '--kind', kind, cwd=tmp)
    check(p.returncode == 0, 'new %s exit=%d out=%s' % (kind, p.returncode,
                                                        (p.stdout + p.stderr)[-300:]))
    return os.path.join(tmp, name)


def assert_runs_and_checks(root, what, literals=True, run=True):
    """The promise of `brat new`: the generated program checks and runs."""
    main = os.path.join(root, 'main.brat')
    src = open(main, encoding='utf-8').read()
    check('log.info' in src, what + ': no log.info')
    check('match ' in src and 'Ok(' in src and 'Err(' in src, what + ': no Ok/Err match')
    check(re.search(r'"[^"\n]*\{[A-Za-z_][\w.]*\}', src) is not None,
          what + ': no "{expr}" interpolation')
    if literals:
        check(re.search(r'let mut \w+ = \{', src) is not None, what + ': no map literal')
        check(re.search(r'\w+ = \[', src) is not None, what + ': no vec literal')
    check('__NAME__' not in src, what + ': template token left in main.brat')

    check(os.path.isfile(os.path.join(root, 'README.md')), what + ': no README.md')
    readme = open(os.path.join(root, 'README.md'), encoding='utf-8').read()
    check(len(readme.splitlines()) == 10, what + ': README wants 10 lines have %d'
          % len(readme.splitlines()))
    check(root.split(os.sep)[-1] in readme, what + ': README does not name the project')
    check('run' in readme, what + ': README does not say how to run')

    p = run_cli('check', main, cwd=root)
    check(p.returncode == 0, what + ': check exit=%d out=%s'
          % (p.returncode, (p.stdout + p.stderr)[-500:]))
    check('0 error(s)' in p.stdout, what + ': check wants 0 error(s) have %r' % p.stdout[-300:])
    check('0 warning(s)' in p.stdout, what + ': check wants 0 warning(s) have %r'
          % p.stdout[-300:])
    check('0 type error(s)' in p.stdout, what + ': check wants 0 type error(s) have %r'
          % p.stdout[-300:])

    if not run:
        return None
    r = run_brat(main, cwd=root)
    check(r.returncode == 0, what + ': run exit=%d out=%s'
          % (r.returncode, (r.stdout + r.stderr)[-500:]))
    return r


tmp = _scratch.mkdtemp(prefix='brat-scaffold-')
try:
    p = run_cli('new', 'demo', cwd=tmp)
    check(p.returncode == 0, 'default new exit=%d out=%s' % (p.returncode,
                                                            (p.stdout + p.stderr)[-300:]))
    check(os.path.isfile(os.path.join(tmp, 'demo', 'main.brat')), 'default kind file')
    check('(service)' in p.stdout, 'default kind wants service have %r' % p.stdout[-300:])
    for token in ('demo/main.brat', 'demo/README.md', 'next:', 'run main.brat'):
        check(token in p.stdout, 'new output wants %r have %r' % (token, p.stdout[-400:]))
    check('brat run main.brat' in p.stdout, 'new hint wants brat run have %r' % p.stdout[-300:])
    check('python -X utf8' not in p.stdout, 'new hint no python cli have %r' % p.stdout[-300:])

    assert_runs_and_checks(os.path.join(tmp, 'demo'), 'service', literals=False, run=False)

    # The service is a real server: start it, ask it twice, let it stop itself.
    svc_dir = os.path.join(tmp, 'demo')
    svc = subprocess.Popen(_cxx.run_args(os.path.join(svc_dir, 'main.brat')),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True, cwd=svc_dir)
    try:
        replies = [tcp_ask(('127.0.0.1', 8088), b'ping\n') for _ in range(2)]
        out, err = svc.communicate(timeout=60)
    finally:
        if svc.poll() is None:
            svc.kill()
    check(replies == ['ok\n', 'ok\n'], 'service replies %r err=%r' % (replies, err[-300:]))
    check('served=2 failed=0' in out, 'service report have %r' % out[-300:])

    assert_runs_and_checks(scaffold('tool', tmp), 'tool')
    out = run_brat(os.path.join(tmp, 'app_tool', 'main.brat'),
                   cwd=os.path.join(tmp, 'app_tool')).stdout
    check('fields=2 skipped=1 broken=1' in out, 'tool report have %r' % out[-300:])
    check('host=127.0.0.1 port=9001' in out, 'tool fields have %r' % out[-300:])
    note = os.path.join(tmp, 'app_tool', 'notes.txt')
    with open(note, 'w', encoding='utf-8') as f:
        f.write('name: brat\n')
    r = run_cli('run', os.path.join(tmp, 'app_tool', 'main.brat'), '--', note)
    check(r.returncode == 0, 'tool with arg exit=%d out=%s'
          % (r.returncode, (r.stdout + r.stderr)[-300:]))
    check('name=brat' in r.stdout, 'tool arg not read have %r' % r.stdout[-300:])
    r = run_cli('run', os.path.join(tmp, 'app_tool', 'main.brat'), '--',
                os.path.join(tmp, 'outside.txt'))
    check(r.returncode == 0, 'tool outside path exit=%d out=%s'
          % (r.returncode, (r.stdout + r.stderr)[-300:]))
    check('cannot read' in r.stdout and 'outside the program directory' in r.stdout,
          'tool outside path wants the Err arm have %r' % r.stdout[-300:])
    check('host=127.0.0.1' in r.stdout,
          'tool outside path wants the sample kept have %r' % r.stdout[-300:])

    for kind in ('service', 'tool'):
        root = os.path.join(tmp, 'demo' if kind == 'service' else 'app_tool')
        f1 = run_cli('fmt', 'main.brat', cwd=root)
        check(f1.returncode == 0, 'fmt %s exit=%d' % (kind, f1.returncode))
        first = open(os.path.join(root, 'main.brat'), encoding='utf-8').read()
        f2 = run_cli('fmt', 'main.brat', cwd=root)
        check(f2.returncode == 0, 'fmt2 %s exit=%d' % (kind, f2.returncode))
        second = open(os.path.join(root, 'main.brat'), encoding='utf-8').read()
        check(first == second, 'generated main.brat not fmt-clean (%s)' % kind)

    p = run_cli('new', 'demo', cwd=tmp)
    check(p.returncode == 1, 'existing dir exit=%d out=%s'
          % (p.returncode, (p.stdout + p.stderr)[-300:]))
    check('already exists' in (p.stdout + p.stderr), 'existing dir message have %r'
          % (p.stdout + p.stderr)[-300:])

    p = run_cli('new', cwd=tmp)
    check(p.returncode == 2, 'no NAME exit=%d out=%s'
          % (p.returncode, (p.stdout + p.stderr)[-300:]))
    check('NAME is required' in (p.stdout + p.stderr), 'no NAME message')

    p = run_cli('new', 'kindless', '--kind', 'rocket', cwd=tmp)
    check(p.returncode == 2, 'bad kind exit=%d out=%s'
          % (p.returncode, (p.stdout + p.stderr)[-300:]))
    check('unknown kind' in (p.stdout + p.stderr), 'bad kind message')
    check(not os.path.isdir(os.path.join(tmp, 'kindless')), 'bad kind wrote nothing')

    p = run_cli('new', 'other', 'x', cwd=tmp)
    check(p.returncode == 2, 'two names exit=%d' % p.returncode)

    p = run_cli('new', '../escape', cwd=tmp)
    check(p.returncode == 1, 'path name exit=%d out=%s'
          % (p.returncode, (p.stdout + p.stderr)[-300:]))

    p = run_cli('new', 'eqkind', '--kind=tool', cwd=tmp)
    check(p.returncode == 0, '--kind= form exit=%d out=%s'
          % (p.returncode, (p.stdout + p.stderr)[-300:]))
    check(os.path.isfile(os.path.join(tmp, 'eqkind', 'README.md')), 'eqkind README')
    check('(tool)' in p.stdout, 'eqkind kind have %r' % p.stdout[-300:])
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print('SCAFFOLD ALL GREEN (%d asserts)' % n)