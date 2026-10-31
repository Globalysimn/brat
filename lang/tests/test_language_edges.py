"""Crafted-program edges: defer/catch ordering, regex parity, the FS sandbox.

Parity alone cannot prove a case whose output is the point -- two backends can
be wrong the same way -- so every case here also asserts the exact stdout.
That exact stdout is what gives each FS-sandbox rejection a positive control:
the same program must still read a file inside the program directory, so a
guard that refused everything would fail the test, not pass it.

The recursion budget is pinned by test_recursion.py and is not duplicated here.
"""
import _scratch
import importlib.util
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENG = os.path.join(ROOT, 'lang', 'engine')
BASE = _scratch.mkdtemp(prefix='brat-edges-')
PROG = os.path.join(BASE, 'prog')
os.makedirs(PROG)

_spec = importlib.util.spec_from_file_location('cc_edges', os.path.join(ENG, 'codegen_c.py'))
cc = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(cc)

_tc_spec = importlib.util.spec_from_file_location('tc_edges', os.path.join(ENG, 'toolchain.py'))
_tc = importlib.util.module_from_spec(_tc_spec); _tc_spec.loader.exec_module(_tc)

# Sandbox fixtures: a file inside PROG, a link to it, and a link that escapes.
with open(os.path.join(PROG, 'inside.txt'), 'w', encoding='utf-8') as f:
    f.write('INSIDE-OK\n')
with open(os.path.join(BASE, 'outside.txt'), 'w', encoding='utf-8') as f:
    f.write('SECRET-OUTSIDE\n')
os.symlink(os.path.join(BASE, 'outside.txt'), os.path.join(PROG, 'escape_link'))
os.symlink(os.path.join(PROG, 'inside.txt'), os.path.join(PROG, 'inside_link'))

CASES = {
    # Defer drains LIFO, before catch, for a top-level try.
    'defer_try_toplevel': ('''fn main() -> i64 {
  try {
    defer print("def1")
    defer print("def2")
    raise("boom")
  } catch e {
    print("caught")
  }
  print("end")
  return 0
}
''', 'def2\ndef1\ncaught\nend\n'),
    # A defer registered inside catch runs at the end of that block.
    'defer_in_catch': ('''fn main() -> i64 {
  try {
    raise("boom")
  } catch e {
    defer print("catch-cleanup")
    print("in catch")
  }
  print("end")
  return 0
}
''', 'in catch\ncatch-cleanup\nend\n'),
    # A defer drains when its own nested block leaves.
    'defer_catch_nested': ('''fn main() -> i64 {
  try {
    raise("boom")
  } catch e {
    if true {
      defer print("nested-cleanup")
      print("body")
    }
    print("after-if")
  }
  return 0
}
''', 'body\nnested-cleanup\nafter-if\n'),
    # The deferred statement runs while the frame unwinds to the catch.
    'defer_stmt_raises': ('''fn main() -> i64 {
  try {
    defer print("will-print")
    raise("first")
  } catch e {
    print("caught {e}")
  }
  return 0
}
''', 'will-print\ncaught first\n'),
    # A re-raise from an inner catch reaches the outer one, with the payload.
    'double_raise_inner_catch': ('''fn main() -> i64 {
  try {
    try {
      raise("inner")
    } catch e {
      print("inner caught")
      raise("outer")
    }
  } catch e {
    print("outer caught {e}")
  }
  return 0
}
''', 'inner caught\nouter caught outer\n'),
    # Regex: both backends match raw UTF-8 bytes, so \w covers the whole string.
    'regex_unicode_word': ('print(re_match("h\u00e9llo", "\\\\w+"))\n', 'True\n'),
    # An empty alternation branch matches "", so "ac" fails but "abd" passes.
    'regex_empty_alt': ('print(re_match("ac", "a(b|c|)d"))\n'
                        'print(re_match("abd", "a(b|c|)d"))\n', 'False\nTrue\n'),
    # A nullable group under * never terminates the match loop early.
    'regex_nested_star': ('print(re_match("aaa", "(a*)*"))\n'
                          'print(re_match("b", "(a*)*"))\n', 'True\nTrue\n'),
    'regex_neg_class': ('print(re_match("abc", "[^xyz]+"))\n'
                        'print(re_match("xyz", "[^xyz]+"))\n', 'True\nFalse\n'),
    # `.` matches any byte, newline included -- both backends agree on this.
    'regex_dot_bytes': ('print(re_match("a\\nb", "a.b"))\n', 'True\n'),
    # `^` anchors the whole input, not each line.
    'regex_anchor_input': ('print(re_match("foo\\nbar", "^bar"))\n', 'False\n'),
    # read_file() resolves symlinks and refuses any target outside the program
    # directory; the inside_link and ../ reads are the positive controls.
    'fs_sandbox_links': ('''fn main() -> i64 {
  match read_file("escape_link") {
    Ok(t) => { print("ESCAPED [{t}]") },
    Err(r) => { print("refused {r}") }
  }
  match read_file("inside_link") {
    Ok(t) => { print("inside [{t}]") },
    Err(r) => { print("read-err {r}") }
  }
  match read_file("../outside.txt") {
    Ok(t) => { print("ESCAPED2 [{t}]") },
    Err(r) => { print("refused2 {r}") }
  }
  return 0
}
''', 'refused path outside the program directory\n'
     'inside [INSIDE-OK\n]\n'
     'refused2 path outside the program directory\n'),
}

bad = 0
for name, (src, want) in CASES.items():
    path = os.path.join(PROG, name + '.brat')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    cf = os.path.join(PROG, name + '.c')
    exe = os.path.join(PROG, name + '.exe')
    try:
        with open(cf, 'w', encoding='utf-8') as f:
            f.write(cc.emit(src, PROG))
        rb = subprocess.run(_tc.resolve_cc().argv(cf, exe) + ['-O1', '-lm', '-I', ENG],
                            capture_output=True, text=True, timeout=300)
        if rb.returncode != 0:
            print('%-22s BUILD FAIL %s' % (name, (rb.stdout + rb.stderr).strip().splitlines()[-1][:110]))
            bad += 1
            continue
    except Exception as e:
        print('%-22s EMIT FAIL %s' % (name, e))
        bad += 1
        continue
    a = subprocess.run([sys.executable, '-X', 'utf8', os.path.join(ENG, 'brat.py'), path],
                       capture_output=True, text=True, cwd=PROG, timeout=120)
    b = subprocess.run([exe], capture_output=True, text=True, cwd=PROG, timeout=120)
    if a.stdout != b.stdout or a.returncode != b.returncode:
        bad += 1
        print('%-22s DIFF rc i=%d n=%d' % (name, a.returncode, b.returncode))
        print('   interp: %r' % a.stdout[:150])
        print('   native: %r' % b.stdout[:150])
    elif a.stdout != want:
        bad += 1
        print('%-22s WRONG\n   got:  %r\n   want: %r' % (name, a.stdout[:150], want))
    else:
        print('%-22s ok   %r' % (name, a.stdout[:60]))
print('---- edge failures:', bad)
sys.exit(1 if bad else 0)