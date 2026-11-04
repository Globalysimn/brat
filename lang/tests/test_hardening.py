"""Pins for the audit findings that no other test covered.

Each case names the failure it prevents:

* a literal holding `??` must survive a strict-ISO-C compiler (trigraphs);
* `arena { }` programs must not reach the interpreter when the ownership
  checker rejects them -- the gate pattern used to be double-escaped and never
  matched, so `brat run` ran what `brat build` refused;
* `BRAT_FILES` must parse the same exact, trimmed, case-insensitive allowlist
    in both backends; a value beginning with `1` is not an opt-out;
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
    escaped = os.path.join(os.path.dirname(TMP), 'escaped.txt')
    for value, allow in (('0', False), ('1no', False), (' YES ', True)):
        env = dict(ENV)
        env['BRAT_FILES'] = value
        exe = os.path.join(TMP, 'fs_off_' + value)
        rc, out = cli(['build', path, '-o', exe], env=env)
        if rc != 0:
            check(False, 'brat_files_off: build failed (%r)' % out[:200])
            return
        r = subprocess.run([exe], capture_output=True, text=True, env=env,
                           cwd=TMP, timeout=120)
        ri, oi = cli(['run', '--interp', path], env=env)
        expected = "'ok'" if allow else "'err'"
        check(expected in r.stdout and expected in oi and r.stdout == oi,
              'brat_files: BRAT_FILES=%r diverged, got %r / %r'
              % (value, r.stdout, oi))
        check(os.path.exists(escaped) == allow,
              'brat_files: BRAT_FILES=%r outside file exists=%r' %
              (value, os.path.exists(escaped)))
        if os.path.exists(escaped):
            os.unlink(escaped)


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


def test_vlong_range():
    src = r'''
#include "crt.h"
BFN Val vcall_any(Val fn, Vec *a) { (void)fn; (void)a; return vnil(); }
static int rejected(double value) {
    (void)vlong(vfloat(value));
    if (!verr_pending()) return 0;
    vdrop(verr_take());
    return 1;
}
int main(void) {
    return rejected(1e300) && rejected(-1e300) && rejected(INFINITY) && rejected(NAN)
        ? 0 : 1;
}
'''
    cf = write('vlong.c', src)
    exe = os.path.join(TMP, 'vlong')
    r = subprocess.run(['cc', '-std=c11', '-O1', '-fsanitize=undefined,float-cast-overflow',
                        '-fno-sanitize-recover=all', '-I', ENGINE, cf, '-o', exe,
                        '-lpthread', '-ldl', '-lm'], capture_output=True, text=True)
    check(r.returncode == 0, 'vlong: sanitizer build failed (%r)' % (r.stderr or '')[-300:])
    if r.returncode != 0:
        return
    b = subprocess.run([exe], capture_output=True, text=True)
    check(b.returncode == 0 and 'runtime error' not in b.stderr,
          'vlong: out-of-range float conversion must be rejected before cast, got %r'
          % (b.stdout + b.stderr))


def test_mcp_body_cap():
    import importlib.util
    import io
    spec = importlib.util.spec_from_file_location('mcp', os.path.join(ENGINE, 'mcp.py'))
    mcp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mcp)
    sess = mcp.Server(io.BytesIO(b'Content-Length: 999999999\r\n\r\n'),
                       io.BytesIO())
    check(sess.read_message() is mcp._INVALID,
          'mcp: a 1 GB Content-Length must be refused, not allocated')
    long_header = b'X-Pad: ' + b'a' * (mcp.MAX_HEADER_LINE + 10) + b'\r\n\r\n'
    sess = mcp.Server(io.BytesIO(long_header), io.BytesIO())
    check(sess.read_message() is mcp._INVALID, 'mcp: an over-long header line must be '
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
                 'print(env_get("PLAIN_URI"))\n'
                 'print(env_get(1))\n')
    env = dict(ENV,
               DATABASE_URL='postgres://admin:supersecret@db:5432/prod',
               PLAIN_URI='redis://user:pass@host:6379/0',
               **{'1': 'numeric-key-value'})
    rc_i, out_i = cli(['run', '--interp', path], env=env)
    rc_n, out_n = cli(['run', path], env=env)
    check('supersecret' not in out_n and 'env_get refused' in out_n
          and 'numeric-key-value' not in out_i and out_i == out_n,
          'env_get: credential values and non-string keys must agree (%r vs %r)'
          % (out_i, out_n))


def test_os_bytes_argv_env():
    raw = b'A\xffB'
    raw_text = raw.decode('utf-8', 'surrogateescape')
    env = dict(ENV)
    env['BRAT_TEST_RAW'] = raw_text
    bytes_path = write('os_bytes.brat',
                       'import std.env\n'
                       'print(args_bytes())\n'
                       'print(env_get_bytes("BRAT_TEST_RAW"))\n'
                       'print(std.env.get_bytes("BRAT_TEST_RAW"))\n'
                       'print(utf8_str(env_get_bytes("BRAT_TEST_RAW")))\n'
                       'print(env_get("BRAT_TEST_RAW"))\n')
    expected = (b'[[65, 255, 66], [111, 107]]\n'
                b'[65, 255, 66]\n'
                b"('ok', [65, 255, 66])\n"
                b"('err', 'invalid utf-8')\n"
                b"('err', \"env_get invalid utf-8: 'BRAT_TEST_RAW'\")\n")
    ri = subprocess.run([sys.executable, '-X', 'utf8', CLI, 'run', '--interp',
                         bytes_path, '--', raw, 'ok'],
                        capture_output=True, timeout=300, env=env)
    check(ri.returncode == 0 and ri.stdout == expected,
          'os bytes: interpreter argv/env bytes diverged (rc=%d, out=%r, err=%r)'
          % (ri.returncode, ri.stdout, ri.stderr[-300:]))
    exe = os.path.join(TMP, 'os_bytes')
    rc_b, out_b = cli(['build', bytes_path, '-o', exe], env=env)
    check(rc_b == 0, 'os bytes: native build failed (%r)' % out_b[:300])
    if rc_b == 0:
        rn = subprocess.run([exe, raw, 'ok'], capture_output=True, timeout=120, env=env)
        check(rn.returncode == 0 and rn.stdout == expected,
              'os bytes: native argv/env bytes diverged (rc=%d, out=%r, err=%r)'
              % (rn.returncode, rn.stdout, rn.stderr[-300:]))
    str_path = write('os_args_str.brat', 'print(args())\n')
    ri = subprocess.run([sys.executable, '-X', 'utf8', CLI, 'run', '--interp',
                         str_path, '--', raw],
                        capture_output=True, timeout=300, env=env)
    out_i = ri.stdout + ri.stderr
    check(ri.returncode != 0 and b'invalid utf-8 in argument 1; use args_bytes()' in out_i,
          'os bytes: interpreter args() accepted invalid UTF-8 (rc=%d, out=%r)'
          % (ri.returncode, out_i[-300:]))
    exe_s = os.path.join(TMP, 'os_args_str')
    rc_b, out_b = cli(['build', str_path, '-o', exe_s], env=env)
    check(rc_b == 0, 'os bytes: native args build failed (%r)' % out_b[:300])
    if rc_b == 0:
        rn = subprocess.run([exe_s, raw], capture_output=True, timeout=120, env=env)
        out_n = rn.stdout + rn.stderr
        check(rn.returncode != 0 and b'invalid utf-8 in argument 1; use args_bytes()' in out_n,
              'os bytes: native args() accepted invalid UTF-8 (rc=%d, out=%r)'
              % (rn.returncode, out_n[-300:]))
    bad_src = os.path.join(TMP, 'bad_utf8.brat')
    with open(bad_src, 'wb') as f:
        f.write(b'print("\xff")\n')
    rb = subprocess.run([sys.executable, BRAT, bad_src], capture_output=True, timeout=120, env=ENV)
    out_bad = rb.stdout + rb.stderr
    check(rb.returncode != 0 and b'is not valid UTF-8' in out_bad and b'Traceback' not in out_bad,
          'os bytes: bare brat.py traceback on invalid UTF-8 source (rc=%d, out=%r)'
          % (rb.returncode, out_bad[-300:]))
    bad_link = os.path.join(TMP, 'badlink')
    try:
        os.symlink(b'bad\xff', bad_link)
    except FileExistsError:
        pass
    link_path = write('os_badlink.brat', 'print(read_file("badlink"))\n')
    rc_i, out_i = cli(['run', '--interp', link_path], env=env)
    rc_n, out_n = cli(['run', link_path], env=env)
    expected_link = "('err', '[Errno 2] os error')\n"
    check(out_i == expected_link and out_i == out_n,
          'os bytes: non-UTF-8 path leaked into Err string (%r vs %r)' % (out_i, out_n))


def test_os_bytes_egress():
    if os.name == 'nt':
        return
    fs_path = write('os_egress_fs.brat',
                    'let bname = [98, 255, 46, 116, 120, 116]\n'
                    'print(write_bytes(bname, [104, 105]))\n'
                    'print(read_bytes(bname))\n'
                    'print(write_file(bname, "yo"))\n'
                    'print(read_file(bname))\n'
                    'let bname2 = [98, 254, 46, 116, 120, 116]\n'
                    'print(fs_rename(bname, bname2))\n'
                    'print(read_file(bname2))\n'
                    'print(fs_remove(bname2))\n'
                    'print(read_bytes(bname2))\n'
                    'print(write_bytes([98, 0, 99], [1]))\n'
                    'print(write_bytes([98, 300], [1]))\n')
    expected_fs = ("('ok', 2)\n"
                   "('ok', [104, 105])\n"
                   "('ok', 2)\n"
                   "('ok', 'yo')\n"
                   "('ok', 0)\n"
                   "('ok', 'yo')\n"
                   "('ok', 0)\n"
                   "('err', '[Errno 2] os error')\n"
                   "('err', 'embedded NUL in file path')\n"
                   "('err', 'write_bytes byte out of range 0..255')\n")
    rc_i, out_i = cli(['run', '--interp', fs_path])
    check(rc_i == 0 and out_i == expected_fs,
          'os egress: interpreter byte paths diverged (rc=%d, out=%r)' % (rc_i, out_i))
    rc_n, out_n = cli(['run', fs_path])
    check(rc_n == 0 and out_n == expected_fs,
          'os egress: native byte paths diverged (rc=%d, out=%r)' % (rc_n, out_n))
    check(os.path.exists(os.path.join(os.fsencode(TMP), b'b\xff.txt')) is False
          and os.listdir(os.fsencode(TMP)).count(b'b\xfe.txt') == 0,
          'os egress: byte-named file left behind')

    db_raw = os.path.join(os.fsencode(TMP), b'd\xff.db')
    sql_path = write('os_egress_sql.brat',
                     'let h = unwrap(sql_open([100, 255, 46, 100, 98]))\n'
                     'print(sql_close(h))\n')
    for args in (['run', '--interp', sql_path], ['run', sql_path]):
        if os.path.exists(db_raw):
            os.unlink(db_raw)
        rc, out = cli(args)
        check(rc == 0 and out == "('ok', 0)\n" and os.path.exists(db_raw),
              'os egress: sql_open byte path failed %r (rc=%d, out=%r)' % (args[1], rc, out))

    env = dict(ENV)
    env['BRAT_EXEC'] = '1'
    proc_path = write('os_egress_proc.brat',
                      'match proc_run("/bin/echo", [[104, 105], "yo"]) {\n'
                      '  Ok(m) => { print(m.get("out")) },\n'
                      '  Err(e) => { print("bad:{e}") }\n'
                      '}\n'
                      'print(proc_run("/bin/echo", [[65, 255]]))\n'
                      'print(proc_run("/bin/echo", [[65, 300]]))\n'
                      'print(proc_run("/bin/echo", [5]))\n')
    expected_proc = ('hi yo\n\n'
                     "('err', 'invalid utf-8')\n"
                     "('err', 'proc_run byte out of range 0..255')\n"
                     "('err', 'proc_run wants vec[str] args')\n")
    rc_i, out_i = cli(['run', '--interp', proc_path], env=env)
    check(rc_i == 0 and out_i == expected_proc,
          'os egress: interpreter byte argv diverged (rc=%d, out=%r)' % (rc_i, out_i))
    rc_n, out_n = cli(['run', proc_path], env=env)
    check(rc_n == 0 and out_n == expected_proc,
          'os egress: native byte argv diverged (rc=%d, out=%r)' % (rc_n, out_n))

    nul_path = write('os_egress_proc_nul.brat',
                     'print(proc_run("/bin/echo", [[65, 0]]))\n')
    rc_i, out_i = cli(['run', '--interp', nul_path], env=env)
    check(rc_i == 0 and out_i == "('err', 'proc_run nul byte in argument')\n",
          'os egress: interpreter byte argv NUL accepted (rc=%d, out=%r)' % (rc_i, out_i))
    rc_n, out_n = cli(['run', nul_path], env=env)
    check(rc_n == 0 and out_n == "('err', 'embedded NUL in command argument')\n",
          'os egress: native byte argv NUL accepted (rc=%d, out=%r)' % (rc_n, out_n))


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
               test_vnorm_cap, test_vlong_range, test_mcp_body_cap, test_fetch_url_file_scheme,
               test_unsafe_is_not_a_switch, test_write_file_symlink_escape,
               test_read_file_nul_rejected, test_env_get_credentials_refused,
               test_os_bytes_argv_env, test_os_bytes_egress,
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
