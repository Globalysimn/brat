"""`arena { }` as a request handler that rolls back on Err.

The handler builds its working state inside an arena, returns Err with a
message copied out with own(), and the loop runs many requests, most of them
successful. Checked here:

- `run -O0` and the built binary print the same output;
- under a 16 MiB address-space limit both the arena and the plain build
  finish, and the peak RSS does not grow when the request loop is run four
  times longer, so a request gives its working state back on both the Ok and
  the Err path;
- AddressSanitizer reports no memory errors: the Err message and the tag
  arguments are not left pointing into a released arena.
"""
import _cxx
import _scratch
import os
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')

PROGRAM = '''enum Res {
  Ok(total),
  Err(reason)
}

fn handle(req: str) -> Res {
  arena {
    let parts = req.split(",")
    let mut total = 0
    for p in parts {
      if p == "" {
        return Err(own("empty field among {len(parts)} fields"))
      }
      total = total + len(p)
    }
    return Ok(total)
  }
  return Err("unreachable")
}

fn main() -> i64 {
  let mut base = "ab"
  let mut f = 0
  while f < 150 {
    base = base + ",cd"
    f = f + 1
  }
  let mut ok = 0
  let mut failed = 0
  let mut last = ""
  let mut i = 0
  while i < 3000 {
    let mut suffix = ""
    if i % 5 == 0 {
      suffix = ",,"
    }
    let line = base + suffix
    match handle(line) {
      Ok(t) => { ok = ok + 1 },
      Err(r) => {
        failed = failed + 1
        last = r
      }
    }
    i = i + 1
  }
  print("ok {ok} failed {failed}")
  print("last: {last}")
  return 0
}
'''

WANT = 'ok 2400 failed 600\nlast: empty field among 153 fields\n'
LONG = 12000
WANT_LONG = 'ok 9600 failed 2400\nlast: empty field among 153 fields\n'
LIMIT = 16 * 1024 * 1024
SLACK = 4 * 1024


def peak_rss(exe, limit=None):
    """Run exe, return (exit status, stdout, stderr, peak RSS in KiB)."""
    r, w = os.pipe()
    rerr, werr = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(r)
        os.close(rerr)
        os.dup2(w, 1)
        os.dup2(werr, 2)
        if limit is not None:
            import resource
            resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
        try:
            os.execv(exe, [exe])
        finally:
            os._exit(127)
    os.close(w)
    os.close(werr)
    out = os.fdopen(r, 'rb').read().decode('utf-8', 'replace')
    err = os.fdopen(rerr, 'rb').read().decode('utf-8', 'replace')
    _, status, ru = os.wait4(pid, 0)
    return os.waitstatus_to_exitcode(status), out, err, ru.ru_maxrss

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


tmp = _scratch.mkdtemp(prefix='brat-arena-request-')
try:
    src = os.path.join(tmp, 'req.brat')
    with open(src, 'w', encoding='utf-8') as f:
        f.write(PROGRAM)
    noarena = os.path.join(tmp, 'noarena.brat')
    long_ = os.path.join(tmp, 'long.brat')
    with open(noarena, 'w', encoding='utf-8') as f:
        f.write(PROGRAM.replace('  arena {', '  if true {'))
    with open(long_, 'w', encoding='utf-8') as f:
        f.write(PROGRAM.replace('i < 3000', 'i < %d' % LONG))

    ip = subprocess.run(_cxx.run_args(src), capture_output=True, text=True, timeout=600)
    check(ip.returncode == 0 and ip.stdout == WANT, 'run_o0: rc=%d out=%r' % (ip.returncode, ip.stdout))

    exe = os.path.join(tmp, 'req_bin')
    b = subprocess.run(_cxx.cli_args('build', src, '-o', exe),
                       capture_output=True, text=True, timeout=600)
    check(b.returncode == 0, 'build failed: %s' % (b.stdout + b.stderr)[-500:])
    rc_, out, err, rss = peak_rss(exe, LIMIT)
    check(rc_ == 0 and out == WANT, 'C under 16MiB: rc=%d out=%r err=%r' % (rc_, out, err[-300:]))

    exe_na = os.path.join(tmp, 'noarena_bin')
    b2 = subprocess.run(_cxx.cli_args('build', noarena, '-o', exe_na),
                       capture_output=True, text=True, timeout=600)
    check(b2.returncode == 0, 'noarena build failed: %s' % (b2.stdout + b2.stderr)[-500:])
    rc_, out, err, rss_na = peak_rss(exe_na, LIMIT)
    check(rc_ == 0 and out == WANT,
          'C without arena under 16MiB: rc=%d out=%r err=%r' % (rc_, out, err[-300:]))

    long_exe = os.path.join(tmp, 'long_bin')
    b3 = subprocess.run(_cxx.cli_args('build', long_, '-o', long_exe),
                        capture_output=True, text=True, timeout=600)
    check(b3.returncode == 0, 'long build failed: %s' % (b3.stdout + b3.stderr)[-500:])
    rc_, out, err, rss_long = peak_rss(long_exe)
    check(rc_ == 0 and out == WANT_LONG, 'long run: rc=%d out=%r' % (rc_, out[:80]))
    check(rss_long - rss <= SLACK,
          'peak RSS grew with the request count: %d KiB at 3000, %d KiB at %d'
          % (rss, rss_long, LONG))
    check(rss_na <= rss + SLACK,
          'the plain build costs %d KiB against %d KiB for the arena' % (rss_na, rss))

    emitted = os.path.join(tmp, 'req_bundle.c')
    e = subprocess.run(_cxx.cli_args('emit-c', '--bundle', src),
                       capture_output=True, text=True, timeout=600)
    check(e.returncode == 0, 'emit-c failed: %s' % e.stderr[-300:])
    with open(emitted, 'w', encoding='utf-8') as f:
        f.write(e.stdout)
    asan_exe = os.path.join(tmp, 'req_asan')
    cc = subprocess.run(['gcc', '-O1', '-g', '-I', ENGINE, emitted, '-o', asan_exe,
                         '-lm', '-lpthread', '-fsanitize=address'],
                        capture_output=True, text=True, timeout=600)
    if cc.returncode == 0:
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        ar = subprocess.run([asan_exe], capture_output=True, text=True, timeout=600, env=env)
        check(ar.returncode == 0 and ar.stdout == WANT and 'AddressSanitizer' not in ar.stderr,
              'ASan: rc=%d out=%r err=%r' % (ar.returncode, ar.stdout, ar.stderr[-600:]))
        asan_note = 'AddressSanitizer clean'
    else:
        asan_note = 'AddressSanitizer skipped (no gcc -fsanitize)'
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print('ARENA REQUEST ALL GREEN (%d asserts, %s)' % (n, asan_note))