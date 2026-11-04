"""Native memory safety under ASan and native/interpreter parity: a truncated
UTF-8 read and a stored container alias must not touch freed memory."""
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')
TMP = _scratch.mkdtemp(prefix='brat-mem-')
BRAT = os.path.join(ENGINE, 'brat.py')
# Final scope-locals, expression temporaries and container scaffolding are not
# released yet, so the leak check stays off.
ASAN = {'ASAN_OPTIONS': 'detect_leaks=0'}

# name: (data file, bytes, move_rt, source, want_stdout, want_stderr)
CASES = {
    # A lone lead byte is a truncated 4-byte sequence.
    'trunc_utf8': (
        'data.bin', b'\xf0', False,
        'fn main() -> i64 {\n'
        '  let s = read_file("data.bin")?\n'
        '  print(len(s))\n'
        '  print(s.upper())\n'
        '  return 0\n'
        '}',
        '', "unhandled: ('err', 'invalid utf-8')\n"),
    # Positive control: real multi-byte UTF-8 still reads and cases the same.
    'valid_utf8': (
        'ok.bin', 'héllo'.encode('utf-8'), False,
        'fn main() -> i64 {\n'
        '  let s = read_file("ok.bin")?\n'
        '  print(len(s))\n'
        '  print(s.upper())\n'
        '  return 0\n'
        '}',
        '5\nHÉLLO\n', ''),
    # The stored value must outlive the block that created it.
    'push_alias': (
        None, None, True,
        'fn main() -> i64 {\n'
        '  let mut b = []\n'
        '  if true {\n'
        '    let a = [111, 222, 333]\n'
        '    b.push(a)\n'
        '  }\n'
        '  let x = b[0]\n'
        '  print(len(x))\n'
        '  print(x[0])\n'
        '  return 0\n'
        '}',
        '3\n111\n', ''),
    'vec_alias': (
        None, None, True,
        'fn main() -> i64 {\n'
        '  let mut b = []\n'
        '  if true {\n'
        '    let a = [7, 8]\n'
        '    b = [a]\n'
        '  }\n'
        '  let x = b[0]\n'
        '  print(len(x))\n'
        '  print(x[1])\n'
        '  return 0\n'
        '}',
        '2\n8\n', ''),
    'setidx_alias': (
        None, None, True,
        'fn main() -> i64 {\n'
        '  let mut b = [0, 0]\n'
        '  if true {\n'
        '    let a = [42]\n'
        '    b[0] = a\n'
        '  }\n'
        '  let x = b[0]\n'
        '  print(x[0])\n'
        '  return 0\n'
        '}',
        '42\n', ''),
    'ins_alias': (
        None, None, True,
        'fn main() -> i64 {\n'
        '  let mut b = [1]\n'
        '  if true {\n'
        '    let a = [9]\n'
        '    b.ins(0, a)\n'
        '  }\n'
        '  let x = b[0]\n'
        '  print(x[0])\n'
        '  return 0\n'
        '}',
        '9\n', ''),
    'mapidx_alias': (
        None, None, True,
        'fn main() -> i64 {\n'
        '  let mut m = {}\n'
        '  m["k"] = 0\n'
        '  if true {\n'
        '    let a = [5, 6]\n'
        '    m["k"] = a\n'
        '  }\n'
        '  let x = m["k"]\n'
        '  print(len(x))\n'
        '  print(x[1])\n'
        '  return 0\n'
        '}',
        '2\n6\n', ''),
    # A rebound `mut` parameter receives a non-heap argument and must not release it.
    'param_rebind': (
        None, None, False,
        'fn f(mut s: str) -> i64 {\n'
        '  s = "zzz"\n'
        '  print(s)\n'
        '  0\n'
        '}\n'
        'fn main() -> i64 { f("abc")  0 }',
        'zzz\n', ''),
    # A closure copies the captured value, so rebinding the binding must not free it.
    'closure_capture_alias': (
        None, None, False,
        'fn main() -> i64 {\n'
        '  let mut s = "abcdefghij"\n'
        '  let c = fn() { return s }\n'
        '  s = "zz"\n'
        '  print(c())\n'
        '  return 0\n'
        '}',
        'abcdefghij\n', ''),
    # An assigned captured name must start from the snapshot, not an uninitialized local.
    'closure_capture_write': (
        None, None, False,
        'fn main() -> i64 {\n'
        '  let mut s = "abcdefghij"\n'
        '  let c = fn() { print(s)  s = "zz"  return s }\n'
        '  print(c())\n'
        '  print(c())\n'
        '  print(s)\n'
        '  return 0\n'
        '}',
        'abcdefghij\nzz\nabcdefghij\nzz\nabcdefghij\n', ''),
    # Same closure under the move runtime: the capture must survive both calls.
    'closure_capture_write_mrt': (
        None, None, True,
        'fn main() -> i64 {\n'
        '  let mut s = "abcdefghij"\n'
        '  let c = fn() { print(s)  s = "zz"  return s }\n'
        '  print(c())\n'
        '  print(c())\n'
        '  print(s)\n'
        '  return 0\n'
        '}',
        'abcdefghij\nzz\nabcdefghij\nzz\nabcdefghij\n', ''),
    # A spawn reads its captured copy from another thread.
    'spawn_capture_alias': (
        None, None, False,
        'fn main() -> i64 {\n'
        '  let mut s = "abcdefghij"\n'
        '  spawn { sleep_ms(150)  print(s) }\n'
        '  sleep_ms(20)\n'
        '  s = "zz"\n'
        '  sleep_ms(400)\n'
        '  print("main done")\n'
        '  return 0\n'
        '}',
        'abcdefghij\nmain done\n', ''),
    # A worker's vtxt free must not drop a buffer the main thread is reading.
    'vtxt_thread_slots': (
        None, None, False,
        'fn main() -> i64 {\n'
        '  let mut big = []\n'
        '  let mut i = 0\n'
        '  while i < 300000 {\n'
        '    big.push("0123456789abcdef")\n'
        '    i = i + 1\n'
        '  }\n'
        '  let mut med = []\n'
        '  let mut j = 0\n'
        '  while j < 200 {\n'
        '    med.push("0123456789abcdef")\n'
        '    j = j + 1\n'
        '  }\n'
        '  spawn {\n'
        '    let mut q = 0\n'
        '    while q < 20000 {\n'
        '      let s = "z" + med\n'
        '      q = q + 1\n'
        '    }\n'
        '  }\n'
        '  let mut t = ""\n'
        '  let mut k = 0\n'
        '  while k < 20 {\n'
        '    t = "y" + big\n'
        '    k = k + 1\n'
        '  }\n'
        '  sleep_ms(1500)\n'
        '  print(len(t))\n'
        '  print("done")\n'
        '  return 0\n'
        '}',
        '6000001\ndone\n', ''),
    # A defer captures the value at registration, so the late rebind is invisible.
    'defer_capture_alias': (
        None, None, False,
        'fn main() -> i64 {\n'
        '  let mut s = "abcdefghij"\n'
        '  defer print(s)\n'
        '  s = "zz"\n'
        '  print("done")\n'
        '  return 0\n'
        '}',
        'done\nabcdefghij\n', ''),
    # A map stores its key, so rebinding the key binding must not free it.
    'map_key_alias': (
        None, None, False,
        'fn main() -> i64 {\n'
        '  let mut k = "abcdefghij"\n'
        '  let mut m = {}\n'
        '  m[k] = 41\n'
        '  k = "zz"\n'
        '  print(m["abcdefghij"])\n'
        '  return 0\n'
        '}',
        '41\n', ''),
    # A store after the rebind in a loop still aliases the value being released.
    'loop_store_after_rebind': (
        None, None, False,
        'fn main() -> i64 {\n'
        '  let mut b = []\n'
        '  let mut s = "abcdefghij"\n'
        '  let mut i = 0\n'
        '  while i < 3 {\n'
        '    s = "newvalue!!"\n'
        '    b.push(s)\n'
        '    i = i + 1\n'
        '  }\n'
        '  print(b[0])\n'
        '  return 0\n'
        '}',
        'newvalue!!\n', ''),
    # A method can hand back its receiver, so the receiver's binding is aliased.
    'receiver_alias': (
        None, None, False,
        'impl Box {\n'
        '  fn me(self) -> Box {\n'
        '    return self\n'
        '  }\n'
        '}\n'
        'fn main() -> i64 {\n'
        '  let mut b = Box { v: 5 }\n'
        '  let d = b.me()\n'
        '  b = Box { v: 7 }\n'
        '  print(d.v)\n'
        '  return 0\n'
        '}',
        '5\n', ''),
}

asserts = 0
bad = 0


def check(cond, msg):
    global asserts, bad
    asserts += 1
    if not cond:
        bad += 1
        print('FAIL %s' % msg)


def compiler():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        'cc_mem', os.path.join(ENGINE, 'toolchain.py'))
    tc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tc)
    return tc.resolve_cc(), tc


SPEC, TC = compiler()

for name, case in CASES.items():
    dat, blob, move_rt, src, want_out, want_err = case
    path = os.path.join(TMP, name + '.brat')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    if dat is not None:
        with open(os.path.join(TMP, dat), 'wb') as f:
            f.write(blob)

    env = dict(os.environ)
    if move_rt:
        env['BRAT_MOVE_RT'] = '1'
    else:
        env.pop('BRAT_MOVE_RT', None)

    ip = subprocess.run([sys.executable, '-X', 'utf8', BRAT, path],
                        capture_output=True, text=True, env=env, timeout=120)

    em = subprocess.run([sys.executable, '-X', 'utf8', BRAT, 'emit-c', path],
                        capture_output=True, text=True, env=env, timeout=120)
    check(em.returncode == 0, '%s emit: %s' % (name, em.stderr[-300:]))
    if em.returncode != 0:
        continue
    cfile = os.path.join(TMP, name + '.c')
    exe = os.path.join(TMP, name + '.exe')
    with open(cfile, 'w', encoding='utf-8') as f:
        f.write(em.stdout)

    cmd = SPEC.argv(cfile, exe) + ['-fsanitize=address', '-g', '-O0',
                                   '-I', ENGINE, '-lm']
    rb = TC.cc_cached(cmd, cfile, exe)
    check(rb.returncode == 0, '%s build: %s' % (name, (rb.stdout + rb.stderr)[-300:]))
    if rb.returncode != 0:
        continue

    run_env = dict(env)
    run_env.update(ASAN)
    np = subprocess.run([exe], capture_output=True, text=True,
                        env=run_env, timeout=120)

    # Any sanitizer finding lands in stderr.
    check('AddressSanitizer' not in np.stderr,
          '%s native ASan: %s' % (name, np.stderr[:300]))
    check(ip.stdout == want_out, '%s interp out=%r want=%r'
          % (name, ip.stdout[:120], want_out))
    check(ip.stderr == want_err, '%s interp err=%r want=%r'
          % (name, ip.stderr[:120], want_err))
    check(np.stdout == want_out, '%s native out=%r want=%r'
          % (name, np.stdout[:120], want_out))
    check(np.stderr == want_err, '%s native err=%r want=%r'
          % (name, np.stderr[:120], want_err))

if bad:
    print('---- RUNTIME MEM FAIL (%d/%d)' % (bad, asserts))
    sys.exit(1)
print('RUNTIME MEM ALL GREEN (%d asserts)' % asserts)