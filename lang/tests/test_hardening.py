"""Boundary tests for C trigraphs, arena ownership gating, BRAT_FILES parsing, path normalization, MCP body limits, and local package URLs.

Covers:

* a literal holding `??` must survive a strict-ISO-C compiler (trigraphs);
* `arena { }` programs rejected by the ownership checker are refused by `brat run`;
* `BRAT_FILES` parses an exact, trimmed, case-insensitive allowlist; a value
  beginning with `1` is not an opt-out;
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
sys.path.insert(0, HERE)
import _cxx
CXX = os.environ.get('BRAT_CXX') or _cxx.build()
ENGINE = os.path.abspath(os.path.join(HERE, '..', 'engine'))
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
    r = subprocess.run(_cxx.cli_args(*(args)),
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
    sub = os.path.join(TMP, 'fs_sub')
    os.makedirs(sub, exist_ok=True)
    path = os.path.join(sub, 'fs_off.brat')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('fn main() -> i64 {\n  print(write_file("../escaped.txt", "x"))\n  return 0\n}\n')
    escaped = os.path.join(TMP, 'escaped.txt')
    for value, allow in (('0', False), ('1no', False), (' YES ', True)):
        if os.path.exists(escaped):
            os.unlink(escaped)
        env = dict(ENV)
        env['BRAT_FILES'] = value
        exe = os.path.join(sub, 'fs_off_' + value.strip())
        rc, out = cli(['build', path, '-o', exe], env=env, cwd=sub)
        if rc != 0:
            check(False, 'brat_files_off: build failed (%r)' % out[:200])
            return
        r = subprocess.run([exe], capture_output=True, text=True, env=env,
                           cwd=sub, timeout=120)
        if os.path.exists(escaped):
            ex_after_exe = True
            os.unlink(escaped)
        else:
            ex_after_exe = False
        ri, oi = cli(['run', '-O0', path], env=env, cwd=sub)
        expected = "Ok(" if allow else "Err("
        check(expected in r.stdout and expected in oi and r.stdout == oi,
              'brat_files: BRAT_FILES=%r diverged, got %r / %r'
              % (value, r.stdout, oi))
        check(ex_after_exe == allow and os.path.exists(escaped) == allow,
              'brat_files: BRAT_FILES=%r outside file exists=%r/%r' %
              (value, ex_after_exe, os.path.exists(escaped)))
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
    r_big = subprocess.run([CXX, 'mcp'],
                           input=b'Content-Length: 999999999\r\n\r\n',
                           capture_output=True, timeout=15)
    check(r_big.returncode == 0 and b'"code":-32700' in r_big.stdout,
          'mcp: a 1 GB Content-Length must be refused cleanly, got %r' % r_big.stdout)
    long_header = b'X-Pad: ' + b'a' * 20000 + b'\r\n\r\n'
    r_hdr = subprocess.run([CXX, 'mcp'], input=long_header,
                           capture_output=True, timeout=15)
    check(r_hdr.returncode == 0 and b'"code":-32700' in r_hdr.stdout,
          'mcp: an over-long header line must be refused, got %r' % r_hdr.stdout)
    body = b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
    good = b'Content-Length: %d\r\n\r\n' % len(body) + body
    r_ok = subprocess.run([CXX, 'mcp'], input=good, capture_output=True, timeout=15)
    check(r_ok.returncode == 0 and b'"id":1' in r_ok.stdout,
          'mcp: an ordinary message must still be read, got %r' % r_ok.stdout)


def test_fetch_url_file_scheme():
    import hashlib
    import http.server
    import json
    import threading
    secret = write('secret.txt', 'pub fn leak() -> i64 { return 1 }\n')
    sha = hashlib.sha256(open(secret, 'rb').read()).hexdigest()

    class _IdxHandler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            doc = {'packages': {'acme/sec': {'1.0': {'url': 'file://' + secret, 'sha256': sha}}}}
            raw = json.dumps(doc).encode('utf-8')
            self.send_response(200)
            self.end_headers()
            self.wfile.write(raw)

        def log_message(self, *_args):
            pass

    srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), _IdxHandler)
    port = srv.server_address[1]
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    sub = os.path.join(TMP, 'pkg_file_scheme')
    os.makedirs(sub, exist_ok=True)
    try:
        env = dict(ENV, BRAT_INDEX='http://127.0.0.1:%d/index.json' % port,
                   BRAT_PKG_ALLOW_INTERNAL='1')
        r = subprocess.run([CXX, 'pkg', 'add', 'acme/sec@1.0'],
                           cwd=sub, env=env, capture_output=True, text=True, timeout=30)
        check(r.returncode != 0,
              'fetch_url: a remote index must not be able to read file:// URLs')
    finally:
        srv.shutdown()
        srv.server_close()

    local_idx = os.path.join(sub, 'brat-index.json')
    with open(local_idx, 'w', encoding='utf-8') as f:
        json.dump({'packages': {'acme/sec': {'1.0': {'url': 'file://' + secret, 'sha256': sha}}}}, f)
    r_ok = subprocess.run([CXX, 'pkg', 'add', 'acme/sec@1.0'],
                          cwd=sub, env=ENV, capture_output=True, text=True, timeout=30)
    check(r_ok.returncode == 0, 'local index must still read a local file:// path: %r' % r_ok.stderr)


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
        rc_i, out_i = cli(['run', '-O0', path])
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
    rc_i, out_i = cli(['run', '-O0', path])
    rc_n, out_n = cli(['run', path])
    check(out_i.strip() == "Err('invalid utf-8')" and out_i == out_n,
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
    rc_i, out_i = cli(['run', '-O0', path], env=env)
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
                b"Ok([65, 255, 66])\n"
                b"Err('invalid utf-8')\n"
                b"Err(\"env_get invalid utf-8: 'BRAT_TEST_RAW'\")\n")
    ri = subprocess.run(_cxx.cli_args('run', '-O0', bytes_path, '--', raw, 'ok'),
                        capture_output=True, timeout=300, env=env)
    check(ri.returncode == 0 and ri.stdout == expected,
          'os bytes: -O0 argv/env bytes diverged (rc=%d, out=%r, err=%r)'
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
    ri = subprocess.run(_cxx.cli_args('run', '-O0', str_path, '--', raw),
                        capture_output=True, timeout=300, env=env)
    out_i = ri.stdout + ri.stderr
    check(ri.returncode != 0 and b'invalid utf-8 in argument 1; use args_bytes()' in out_i,
          'os bytes: -O0 args() accepted invalid UTF-8 (rc=%d, out=%r)'
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
    rb = subprocess.run(_cxx.run_args(bad_src), capture_output=True, timeout=120, env=ENV)
    out_bad = rb.stdout + rb.stderr
    check(rb.returncode != 0 and b'is not valid UTF-8' in out_bad and b'Traceback' not in out_bad,
          'os bytes: bare brat-cxx traceback on invalid UTF-8 source (rc=%d, out=%r)'
          % (rb.returncode, out_bad[-300:]))
    bad_link = os.path.join(TMP, 'badlink')
    try:
        os.symlink(b'bad\xff', bad_link)
    except FileExistsError:
        pass
    link_path = write('os_badlink.brat', 'print(read_file("badlink"))\n')
    rc_i, out_i = cli(['run', '-O0', link_path], env=env)
    rc_n, out_n = cli(['run', link_path], env=env)
    expected_link = "Err('[Errno 2] os error')\n"
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
    expected_fs = ("Ok(2)\n"
                   "Ok([104, 105])\n"
                   "Ok(2)\n"
                   "Ok('yo')\n"
                   "Ok(0)\n"
                   "Ok('yo')\n"
                   "Ok(0)\n"
                   "Err('[Errno 2] os error')\n"
                   "Err('embedded NUL in file path')\n"
                   "Err('write_bytes byte out of range 0..255')\n")
    rc_i, out_i = cli(['run', '-O0', fs_path])
    check(rc_i == 0 and out_i == expected_fs,
          'os egress: -O0 byte paths diverged (rc=%d, out=%r)' % (rc_i, out_i))
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
    for args in (['run', '-O0', sql_path], ['run', sql_path]):
        if os.path.exists(db_raw):
            os.unlink(db_raw)
        rc, out = cli(args)
        check(rc == 0 and out == "Ok(0)\n" and os.path.exists(db_raw),
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
                     "Err('invalid utf-8')\n"
                     "Err('proc_run byte out of range 0..255')\n"
                     "Err('proc_run wants vec[str] args')\n")
    rc_i, out_i = cli(['run', '-O0', proc_path], env=env)
    check(rc_i == 0 and out_i == expected_proc,
          'os egress: -O0 byte argv diverged (rc=%d, out=%r)' % (rc_i, out_i))
    rc_n, out_n = cli(['run', proc_path], env=env)
    check(rc_n == 0 and out_n == expected_proc,
          'os egress: native byte argv diverged (rc=%d, out=%r)' % (rc_n, out_n))

    nul_path = write('os_egress_proc_nul.brat',
                     'print(proc_run("/bin/echo", [[65, 0]]))\n')
    rc_i, out_i = cli(['run', '-O0', nul_path], env=env)
    check(rc_i == 0 and out_i == "Err('embedded NUL in command argument')\n",
          'os egress: -O0 byte argv NUL accepted (rc=%d, out=%r)' % (rc_i, out_i))
    rc_n, out_n = cli(['run', nul_path], env=env)
    check(rc_n == 0 and out_n == "Err('embedded NUL in command argument')\n",
          'os egress: native byte argv NUL accepted (rc=%d, out=%r)' % (rc_n, out_n))


def test_fast_shortcircuit_And_or_multi_source_borrow():
    sc = write('fast_sc.brat',
               'fn tick(x: i64) -> i64 {\n  print(x)\n  return x\n}\n'
               'let a = tick(1) || tick(2)\nprint(a)\n'
               'let b = tick(0) && tick(2)\nprint(b)\n')
    rc_i, out_i = cli(['run', '-O0', sc])
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


def test_deployed_base_and_crt_guards():
    bdir = os.path.join(TMP, 'build_origin')
    ddir = os.path.join(TMP, 'deploy_target')
    os.makedirs(bdir, exist_ok=True)
    os.makedirs(ddir, exist_ok=True)
    bsrc = os.path.join(bdir, 'app.brat')
    with open(bsrc, 'w', encoding='utf-8') as f:
        f.write('fn main() -> i64 {\n  print(read_file("local.txt"))\n  print(read_file("../secret.txt"))\n  return 0\n}\n')
    dexe = os.path.join(ddir, 'app')
    rc, out = cli(['build', bsrc, '-o', dexe])
    check(rc == 0, 'deployed build failed: %r' % out[:200])
    shutil.rmtree(bdir)
    with open(os.path.join(ddir, 'local.txt'), 'w', encoding='utf-8') as f:
        f.write('hello-deploy')
    with open(os.path.join(TMP, 'secret.txt'), 'w', encoding='utf-8') as f:
        f.write('top-secret')
    r = subprocess.run([dexe], capture_output=True, text=True, cwd=ddir, env=ENV, timeout=60)
    check(r.returncode == 0 and "Ok('hello-deploy')" in r.stdout and "Err(" in r.stdout,
          'deployed vbase_set failed: rc=%d out=%r' % (r.returncode, r.stdout + r.stderr))

    c_src = r'''
#define BRAT_CRT_MASK 0xFFFFu
#include "crt.h"
#include <locale.h>
BFN Val vcall_any(Val fn, Vec *a) { (void)fn; (void)a; return vnil(); }
int main(void) {
    /* 1. Uninitialized vbase_ready must fail closed on relative lib_open */
    Val bad_lib = vlib_open(vlit("./nonexistent_plugin.so"));
    if (bad_lib.tag != V_TAG || strcmp(bad_lib.as.tv->tag, "err") != 0) return 11;
    /* 2. Root base directory confinement */
    vbase_set("/");
    const char *cp = vconfine("/tmp");
    if (!cp || strncmp(cp, "/", 1) != 0) return 12;
    /* 3. Dual-pipe proc_run when STDOUT_FILENO (1) is closed in parent */
    setenv("BRAT_EXEC", "1", 1);
    int saved_out = dup(STDOUT_FILENO);
    close(STDOUT_FILENO);
    Vec *args = vvec();
    vec_push(args, vlit("-c"));
    vec_push(args, vlit("printf out_msg; printf err_msg >&2"));
    Val pr = vproc_run_to(vlit("/bin/sh"), vvecv(args), vint(5000));
    dup2(saved_out, STDOUT_FILENO);
    close(saved_out);
    if (pr.tag != V_TAG || strcmp(pr.as.tv->tag, "ok") != 0) return 13;
    Map *pm = pr.as.tv->items[0].as.map;
    Val *so = map_get(pm, "out");
    Val *se = map_get(pm, "err");
    if (!so || !se || strcmp(vstr_c(so), "out_msg") != 0 || strcmp(vstr_c(se), "err_msg") != 0) return 14;
    /* 4. Locale-independent float & JSON parsing */
    setlocale(LC_NUMERIC, "de_DE.UTF-8");
    setlocale(LC_NUMERIC, "fr_FR.UTF-8");
    int ok = 0;
    if (_bstrtod_c("1.5", 3, &ok) != 1.5 || !ok) return 15;
    if (_bstrtod_c("1,5", 3, &ok) != 0.0 || ok) return 16;
    Val pf = vparse_float(vlit("2.25"));
    if (pf.tag != V_TAG || strcmp(pf.as.tv->tag, "ok") != 0 || pf.as.tv->items[0].as.fval != 2.25) return 17;
    Val pfc = vparse_float(vlit("2,25"));
    if (pfc.tag != V_TAG || strcmp(pfc.as.tv->tag, "err") != 0) return 18;
    Val jp = vjson_parse(vlit("[1.5, -0.25]"));
    if (jp.tag != V_TAG || strcmp(jp.as.tv->tag, "ok") != 0) return 19;
    printf("crt-guards-ok\n");
    return 0;
}
'''
    cf = os.path.join(TMP, 'crt_guards.c')
    cexe = os.path.join(TMP, 'crt_guards')
    with open(cf, 'w', encoding='utf-8') as f:
        f.write(c_src)
    cc = subprocess.run(['cc', '-std=gnu11', '-O1', '-I', ENGINE, cf, '-o', cexe,
                         '-lpthread', '-ldl', '-lm'], capture_output=True, text=True)
    check(cc.returncode == 0, 'crt_guards compile failed: %r' % (cc.stderr or '')[-300:])
    if cc.returncode == 0:
        cr = subprocess.run([cexe], capture_output=True, text=True, timeout=60)
        check(cr.returncode == 0 and cr.stdout == 'crt-guards-ok\n',
              'crt_guards failed: rc=%d out=%r err=%r' % (cr.returncode, cr.stdout, cr.stderr))

    # Foreign-thread callback MT safety
    cb_c = os.path.join(TMP, 'mt_cb_driver.c')
    with open(cb_c, 'w', encoding='utf-8') as f:
        f.write('#include <pthread.h>\n'
                '#include <stdint.h>\n'
                'typedef int64_t (*cb_fn)(int64_t);\n'
                'static void *worker(void *arg) { cb_fn fn = (cb_fn)arg; for (int i = 0; i < 100; i++) fn(i); return 0; }\n'
                'int64_t run_foreign_threads(cb_fn fn) {\n'
                '  pthread_t th[4];\n'
                '  for (int i = 0; i < 4; i++) pthread_create(&th[i], 0, worker, (void *)fn);\n'
                '  for (int i = 0; i < 4; i++) pthread_join(th[i], 0);\n'
                '  return 400;\n'
                '}\n')
    cb_h = os.path.join(TMP, 'mt_cb_driver.h')
    with open(cb_h, 'w', encoding='utf-8') as f:
        f.write('#include <stdint.h>\nint64_t run_foreign_threads(int64_t (*fn)(int64_t));\n')
    cb_brat = write('mt_cb.brat',
                    'extern fn run_foreign_threads(cb: fn(i64) -> i64 thread(any)) -> i64\n'
                    'fn on_item(x: i64) -> i64 {\n'
                    '  let tmp = [x, x + 1]\n'
                    '  let copy = tmp.copy()\n'
                    '  return copy[0]\n'
                    '}\n'
                    'fn main() -> i64 {\n'
                    '  let n = run_foreign_threads("on_item")\n'
                    '  print(n)\n'
                    '  return 0\n'
                    '}\n')
    emitted = subprocess.run(_cxx.cli_args('emit-c', '--unsafe-ffi', cb_brat), capture_output=True, text=True, timeout=120)
    rc_e, out_e = emitted.returncode, emitted.stdout
    check(rc_e == 0, 'mt_cb emit-c failed: %r' % out_e[:200])
    if rc_e == 0:
        cb_gen_c = os.path.join(TMP, 'mt_cb_gen.c')
        cb_exe = os.path.join(TMP, 'mt_cb_exe')
        with open(cb_gen_c, 'w', encoding='utf-8') as f:
            f.write(out_e[out_e.index('#define'):] if '#define' in out_e else out_e)
        cc_mt = subprocess.run(['cc', '-std=gnu11', '-O1', '-fsanitize=address,undefined', '-I', ENGINE,
                                cb_gen_c, cb_c, '-o', cb_exe, '-lpthread', '-ldl', '-lm'],
                               capture_output=True, text=True)
        check(cc_mt.returncode == 0, 'mt_cb cc failed: %r' % (cc_mt.stderr or '')[-300:])
        if cc_mt.returncode == 0:
            rmt = subprocess.run([cb_exe], capture_output=True, text=True, timeout=60)
            check(rmt.returncode == 0 and rmt.stdout == '400\n',
                  'mt_cb failed: rc=%d out=%r err=%r' % (rmt.returncode, rmt.stdout, rmt.stderr[-400:]))


def main():
    for fn in (test_trigraph, test_arena_gate, test_brat_files_off,
               test_vnorm_cap, test_vlong_range, test_mcp_body_cap, test_fetch_url_file_scheme,
               test_unsafe_is_not_a_switch, test_write_file_symlink_escape,
               test_read_file_nul_rejected, test_env_get_credentials_refused,
               test_os_bytes_argv_env, test_os_bytes_egress,
               test_fast_shortcircuit_And_or_multi_source_borrow,
               test_deployed_base_and_crt_guards):
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
