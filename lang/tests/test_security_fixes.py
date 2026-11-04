"""Regression pins for the security fixes.

FS sandbox type rejection, `defer` vs `catch` order, regex parity (>=65
alternatives and UTF-8 `.`), and nested `join_all` termination. Interpreter and
C binary must agree on stdout and exit code; nested-spawn output is compared as
a sorted line set because the two backends schedule workers differently.
"""
import _cxx
import _scratch
import importlib.util, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENG = os.path.join(ROOT, 'lang', 'engine')
TMP = _scratch.mkdtemp(prefix='brat-sec-')

_tc_spec = importlib.util.spec_from_file_location('tc', os.path.join(ENG, 'toolchain.py'))
_tc = importlib.util.module_from_spec(_tc_spec); _tc_spec.loader.exec_module(_tc)

_ALT65 = '|'.join(['a'] * 65)

# name -> (source, expected stdout, expected exit code)
CASES = {
    'fs_read_fd': ("fn main() -> i64 {\n  print(read_file(0))\n  0\n}\n",
                   b"('err', 'read_file wants a string path')\n", 0),
    'fs_read_int': ("fn main() -> i64 {\n  print(read_file(5))\n  0\n}\n",
                    b"('err', 'read_file wants a string path')\n", 0),
    'fs_write_fd': ('fn main() -> i64 {\n  print("BEFORE")\n'
                    '  print(write_file(1, "RAW\\n"))\n  print("AFTER")\n  0\n}\n',
                    b"BEFORE\n('err', 'write_file wants string path and text')\nAFTER\n", 0),
    'fs_write_list': ('fn main() -> i64 {\n  print(write_file([1,2], "x"))\n  0\n}\n',
                      b"('err', 'write_file wants string path and text')\n", 0),
    'fs_write_int': ('fn main() -> i64 {\n  print(write_file("o.txt", 5))\n  0\n}\n',
                     b"('err', 'write_file wants string path and text')\n", 0),
    'fs_roundtrip': ('fn main() -> i64 {\n  print(write_file("round.txt", "hi"))\n'
                     '  print(read_file("round.txt"))\n  0\n}\n',
                     b"('ok', 2)\n('ok', 'hi')\n", 0),
    'defer_unwind': ('fn main() -> i64 {\n  try {\n    if true {\n'
                     '      defer print("cleanup")\n      raise("boom")\n    }\n'
                     '  } catch e {\n    print("caught")\n  }\n  return 0\n}\n',
                     b"cleanup\ncaught\n", 0),
    'regex_alt65': ('fn main() -> i64 {\n  print(re_match("a", "%s"))\n  0\n}\n' % _ALT65,
                    b"True\n", 0),
    'regex_dot_utf8': ('fn main() -> i64 {\n  print(re_match("\u00e9", ".."))\n  0\n}\n',
                       b"True\n", 0),
}

NEST = '''fn worker(n: i64) -> i64 {
  spawn {
    print("child")
  }
  join_all()
  print("parent")
  return 0
}
fn main() -> i64 {
  let mut i = 0
  while i < 8 {
    spawn {
      worker(i)
    }
    i = i + 1
  }
  join_all()
  print("done")
  return 0
}
'''
NEST_LINES = sorted([b"child"] * 8 + [b"parent"] * 8 + [b"done"])

bad = 0


def build(name, src):
    cf = os.path.join(TMP, name + '.c')
    exe = os.path.join(TMP, name + '.exe')
    with open(cf, 'w', encoding='utf-8') as f:
        f.write(_cxx.emit_source(src, TMP))
    rb = subprocess.run(_tc.resolve_cc().argv(cf, exe) + ['-O1', '-lm', '-I', ENG],
                        capture_output=True, text=True, timeout=300)
    return rb, exe


def run_interp(path, timeout):
    return subprocess.run([sys.executable, '-X', 'utf8', os.path.join(ENG, 'brat.py'), path],
                          input=b"stdin-should-not-be-read\n",
                          capture_output=True, timeout=timeout)


for name, (src, want, want_rc) in CASES.items():
    path = os.path.join(TMP, name + '.brat')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    try:
        rb, exe = build(name, src)
        if rb.returncode != 0:
            print('%-16s BUILD FAIL %s' % (name, (rb.stdout + rb.stderr).strip().splitlines()[-1][:110]))
            bad += 1
            continue
    except Exception as e:
        print('%-16s EMIT FAIL %s' % (name, e))
        bad += 1
        continue
    a = run_interp(path, 60)
    b = subprocess.run([exe], capture_output=True, timeout=60)
    if a.stdout == b.stdout and a.returncode == b.returncode and a.stdout == want \
            and a.returncode == want_rc:
        print('%-16s ok   rc=%d %r' % (name, a.returncode, a.stdout[:50]))
    else:
        bad += 1
        print('%-16s FAIL i=(%d,%r) n=(%d,%r) want=(%d,%r)'
              % (name, a.returncode, a.stdout, b.returncode, b.stdout, want_rc, want))

# Nested join_all must terminate in both backends (sorted output, same rc).
path = os.path.join(TMP, 'nested_join_all.brat')
with open(path, 'w', encoding='utf-8') as f:
    f.write(NEST)
try:
    rb, exe = build('nested_join_all', NEST)
    if rb.returncode != 0:
        print('nested_join_all  BUILD FAIL')
        bad += 1
    else:
        a = run_interp(path, 45)
        b = subprocess.run([exe], capture_output=True, timeout=45)
        ok = (sorted(a.stdout.splitlines()) == NEST_LINES
              and sorted(b.stdout.splitlines()) == NEST_LINES
              and a.returncode == 0 and b.returncode == 0)
        if ok:
            print('%-16s ok   rc=%d lines=%d' % ('nested_join_all', a.returncode,
                                                 len(a.stdout.splitlines())))
        else:
            bad += 1
            print('nested_join_all  FAIL i=(%d,%r) n=(%d,%r)'
                  % (a.returncode, a.stdout[:80], b.returncode, b.stdout[:80]))
except subprocess.TimeoutExpired as e:
    bad += 1
    print('nested_join_all  TIMEOUT %s' % e)

print('---- security diffs:', bad)
sys.exit(1 if bad else 0)