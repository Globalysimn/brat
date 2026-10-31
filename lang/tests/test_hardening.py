"""Pins for the audit findings that no other test covered.

Each case names the failure it prevents:

* a literal holding `??` must survive a strict-ISO-C compiler (trigraphs);
* `arena { }` programs must not reach the interpreter when the ownership
  checker rejects them -- the gate pattern used to be double-escaped and never
  matched, so `brat run` ran what `brat build` refused;
* `BRAT_FILES=0` must leave the sandbox on: any non-empty value used to
  disable it;
* a path with more components than the normaliser holds must be refused, not
  silently truncated to a different path;
* the MCP server must not allocate whatever body size a client claims;
* `file://` may be named by a local index or a typed URL, never by a remote
  one;
* `unsafe { }` does not turn the ownership checker off.
"""
import _scratch
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, '..', 'engine'))
CLI = os.path.join(ENGINE, 'cli.py')
BRAT = os.path.join(ENGINE, 'brat.py')
TMP = _scratch.mkdtemp(prefix='brat-hard-')

ENV = dict(os.environ)
ENV['PYTHONIOENCODING'] = 'utf-8'
ENV.pop('BRAT_FILES', None)

n = 0
FAILED = []


def check(cond, msg):
    global n
    if cond:
        n += 1
    else:
        FAILED.append(msg)
        print('  FAIL: %s' % msg)


def write(name, src):
    path = os.path.join(TMP, name)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(src)
    return path


def cli(args, env=None, cwd=None):
    r = subprocess.run([sys.executable, '-X', 'utf8', CLI] + args,
                       capture_output=True, text=True, encoding='utf-8',
                       timeout=300, env=env or ENV, cwd=cwd)
    return r.returncode, r.stdout + r.stderr


def test_trigraph():
    src = 'fn main() -> i64 {\n  print("why??/what?x")\n  return 0\n}\n'
    path = write('trigraph.brat', src)
    rc, out = cli(['emit-c', path])
    check(rc == 0, 'trigraph: emit-c failed (%r)' % out[:200])
    cf = os.path.join(TMP, 'trigraph.c')
    with open(cf, 'w', encoding='utf-8') as f:
        f.write(out[out.index('#include'):] if '#include' in out else out)
    # `-std=c11 -trigraphs` is the strict reading (C11 is the runtime's floor):
    # without the split `??/` becomes a backslash and the literal changes.
    r = subprocess.run(['cc', '-std=c11', '-trigraphs', '-w', '-I', ENGINE,
                        cf, '-o', os.path.join(TMP, 'trigraph'), '-lpthread',
                        '-ldl', '-lm'], capture_output=True, text=True)
    check(r.returncode == 0,
          'trigraph: strict ISO C build failed (%r)' % (r.stderr or '')[-300:])
    if r.returncode == 0:
        b = subprocess.run([os.path.join(TMP, 'trigraph')],
                           capture_output=True, text=True)
        check(b.stdout == 'why??/what?x\n',
              'trigraph: got %r, want %r' % (b.stdout, 'why??/what?x\n'))


def test_arena_gate():
    src = ('''
fn make() -> vec[i64] {
  arena {
    let v = [1, 2]
    return v
  }
  return []
}
fn main() -> i64 {
  let x = make()
  print(x)
  return 0
}
''')
    path = write('arena_gate.brat', src)
    rc, out = cli(['check', path])
    check(rc != 0, 'arena gate: the checker must reject this program')
    rc, out = cli(['run', path])
    check(rc != 0 and 'refusing to run' in out,
          'arena gate: `brat run` must refuse a program the checker rejects '
          '(rc=%d, out=%r)' % (rc, out[:200]))


def test_brat_files_off():
    src = ('fn main() -> i64 {\n'
           '  print(write_file("../escaped.txt", "x"))\n  return 0\n}\n')
    path = write('fs_off.brat', src)
    env = dict(ENV)
    env['BRAT_FILES'] = '0'
    rc, out = cli(['build', path, '-o', os.path.join(TMP, 'fs_off')], env=env)
    if rc != 0:
        check(False, 'brat_files_off: build failed (%r)' % out[:200])
        return
    r = subprocess.run([os.path.join(TMP, 'fs_off')], capture_output=True,
                       text=True, env=env, cwd=TMP, timeout=120)
    check("'err'" in r.stdout, 'brat_files_off: BRAT_FILES=0 must keep the '
          'sandbox on, got %r' % r.stdout)
    check(not os.path.exists(os.path.join(os.path.dirname(TMP),
                                          'escaped.txt')),
          'brat_files_off: a file escaped the program directory')


def test_vnorm_cap():
    src = r'''
#include "crt.h"
/* crt.h leaves the callback entry point to the generated program. */
BFN Val vcall_any(Val fn, Vec *a) { (void)fn; (void)a; return vnil(); }
#include <stdio.h>
#include <string.h>
int main(void) {
    char a[4096];
    memset(a, 0, sizeof a);
    strcpy(a, "a");
    for (int i = 0; i < 300; i++) strcat(a, "/a");
    printf("many=%d\n", vnorm(a));
    char b[64];
    strcpy(b, "x/./y/../z");
    printf("ok=%d %s\n", vnorm(b), b);
    return 0;
}
'''
    cf = write('vnorm.c', src)
    exe = os.path.join(TMP, 'vnorm')
    r = subprocess.run(['cc', '-std=c11', '-O1', '-I', ENGINE, cf, '-o', exe,
                        '-lpthread', '-ldl', '-lm'],
                       capture_output=True, text=True)
    check(r.returncode == 0, 'vnorm: build failed (%r)' % (r.stderr or '')[-300:])
    if r.returncode != 0:
        return
    b = subprocess.run([exe], capture_output=True, text=True)
    check('many=-1' in b.stdout,
          'vnorm: a path over the component cap must be refused, got %r'
          % b.stdout)
    check('ok=0 x/z' in b.stdout,
          'vnorm: ordinary normalisation broke, got %r' % b.stdout)


def test_mcp_body_cap():
    import importlib.util
    import io
    spec = importlib.util.spec_from_file_location('mcp', os.path.join(ENGINE, 'mcp.py'))
    mcp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mcp)
    sess = mcp.Server(io.BytesIO(b'Content-Length: 999999999\r\n\r\n'),
                       io.BytesIO())
    check(sess.read_message() is None,
          'mcp: a 1 GB Content-Length must be refused, not allocated')
    long_header = b'X-Pad: ' + b'a' * (mcp.MAX_HEADER_LINE + 10) + b'\r\n\r\n'
    sess = mcp.Server(io.BytesIO(long_header), io.BytesIO())
    check(sess.read_message() is None, 'mcp: an over-long header line must be '
          'refused')
    body = b'{"method": "ping"}'
    good = b'Content-Length: %d\r\n\r\n' % len(body) + body
    sess = mcp.Server(io.BytesIO(good), io.BytesIO())
    check(sess.read_message() == {'method': 'ping'},
          'mcp: an ordinary message must still be read')


def test_fetch_url_file_scheme():
    import importlib.util
    spec = importlib.util.spec_from_file_location('cli_mod', CLI)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    secret = write('secret.txt', 'local file contents\n')
    url = 'file://' + secret
    try:
        mod.fetch_url(url)
    except ValueError:
        check(True, '')
    except Exception as e:  # noqa: BLE001
        check(False, 'fetch_url: unexpected error %r' % e)
    else:
        check(False, 'fetch_url: a remote index must not be able to read '
                     'file:// URLs')
    text, _ = mod.fetch_url(url, allow_file=True)
    check(text.strip() == 'local file contents',
          'fetch_url: allow_file must still read a local path')


def test_unsafe_is_not_a_switch():
    src = ('fn f(v: vec[i64]) -> i64 {\n  unsafe {\n    move v\n  }\n'
           '  return len(v)\n}\nfn main() -> i64 { return 0 }\n')
    path = write('unsafe_move.brat', src)
    rc, out = cli(['check', path])
    check(rc != 0 and 'cannot move borrowed parameter' in out,
          'unsafe: it must not suppress the ownership rules (rc=%d, %r)'
          % (rc, out[:200]))


def test_write_file_symlink_escape():
    outside = tempfile.mkdtemp(prefix='brat-outside-')
    try:
        link = os.path.join(TMP, 'escape_dir_link')
        if os.path.lexists(link):
            os.unlink(link)
        os.symlink(outside, link)
        src = 'print(write_file("escape_dir_link/pwned", "owned"))\n'
        path = write('wf_symlink.brat', src)
        rc_i, out_i = cli(['run', '--interp', path])
        rc_n, out_n = cli(['run', path])
        check(not os.path.exists(os.path.join(outside, 'pwned')),
              'write_file: must not create file through symlink outside sandbox')
        check('outside the program directory' in out_i and out_i == out_n,
              'write_file: symlink escape must be refused identically (%r vs %r)'
              % (out_i, out_n))
    finally:
        shutil.rmtree(outside, ignore_errors=True)


def test_read_file_nul_rejected():
    nul_path = os.path.join(TMP, 'has_nul.bin')
    with open(nul_path, 'wb') as f:
        f.write(b'abc\x00def')
    path = write('rf_nul.brat', 'print(read_file("has_nul.bin"))\n')
    rc_i, out_i = cli(['run', '--interp', path])
    rc_n, out_n = cli(['run', path])
    check(out_i.strip() == "('err', 'invalid utf-8')" and out_i == out_n,
          'read_file: NUL bytes must be rejected identically (%r vs %r)'
          % (out_i, out_n))


def test_env_get_credentials_refused():
    path = write('env_cred.brat',
                 'print(env_get("DATABASE_URL"))\n'
                 'print(env_get("PLAIN_URI"))\n')
    env = dict(ENV,
               DATABASE_URL='postgres://admin:supersecret@db:5432/prod',
               PLAIN_URI='redis://user:pass@host:6379/0')
    rc_i, out_i = cli(['run', '--interp', path], env=env)
    rc_n, out_n = cli(['run', path], env=env)
    check('supersecret' not in out_n and 'env_get refused' in out_n and out_i == out_n,
          'env_get: DATABASE_URL and credential URIs must be refused (%r vs %r)'
          % (out_i, out_n))


def test_fast_shortcircuit_And_or_multi_source_borrow():
    sc = write('fast_sc.brat',
               'fn tick(x: i64) -> i64 {\n  print(x)\n  return x\n}\n'
               'let a = tick(1) || tick(2)\nprint(a)\n'
               'let b = tick(0) && tick(2)\nprint(b)\n')
    rc_i, out_i = cli(['run', '--interp', sc])
    rc_n, out_n = cli(['run', sc])
    check(out_i == '1\n1\n0\n0\n' and out_i == out_n,
          'fast short-circuit: LHS must evaluate once (%r vs %r)' % (out_i, out_n))

    esc = write('multi_esc.brat',
                'fn leak(p: vec[i64]) -> vec[vec[i64]] {\n'
                '  let local = [1, 2, 3]\n'
                '  let v = [p, local]\n'
                '  let r = ref(v)\n'
                '  return r\n'
                '}\n')
    rc_c, out_c = cli(['check', esc])
    check(rc_c != 0 and 'escapes via return' in out_c,
          'multi-source borrow escape must be rejected (%r)' % out_c)


def main():
    for fn in (test_trigraph, test_arena_gate, test_brat_files_off,
               test_vnorm_cap, test_mcp_body_cap, test_fetch_url_file_scheme,
               test_unsafe_is_not_a_switch, test_write_file_symlink_escape,
               test_read_file_nul_rejected, test_env_get_credentials_refused,
               test_fast_shortcircuit_And_or_multi_source_borrow):
        try:
            fn()
        except Exception as e:  # noqa: BLE001
            FAILED.append('%s raised %r' % (fn.__name__, e))
            print('  ERROR in %s: %r' % (fn.__name__, e))
    if FAILED:
        print('\nHARDENING FAILED -- %d problem(s), %d ok:' % (len(FAILED), n))
        for f in FAILED:
            print('  - %s' % f)
        sys.exit(1)
    print('HARDENING ALL GREEN (%d asserts)' % n)


if __name__ == '__main__':
    main()
