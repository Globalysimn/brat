"""Crafted-program edges: defer/catch ordering, regex parity, the FS sandbox.

Parity alone cannot prove a case whose output is the point -- -O0 and -O2 can
be wrong the same way -- so every case here also asserts the exact stdout.
That exact stdout is what gives each FS-sandbox rejection a positive control:
the same program must still read a file inside the program directory, so a
guard that refused everything would fail the test, not pass it.

The recursion budget is pinned by test_recursion.py and is not duplicated here.
"""
import _cxx
import _scratch
import importlib.util
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENG = os.path.join(ROOT, 'lang', 'engine')
BASE = _scratch.mkdtemp(prefix='brat-edges-')
PROG = os.path.join(BASE, 'prog')
os.makedirs(PROG)


_tc_spec = importlib.util.spec_from_file_location('tc_edges', os.path.join(os.path.dirname(os.path.abspath(__file__)), '_toolchain.py'))
_tc = importlib.util.module_from_spec(_tc_spec); _tc_spec.loader.exec_module(_tc)

# Sandbox fixtures: a file inside PROG, a link to it, and a link that escapes.
with open(os.path.join(PROG, 'inside.txt'), 'w', encoding='utf-8') as f:
    f.write('INSIDE-OK\n')
with open(os.path.join(BASE, 'outside.txt'), 'w', encoding='utf-8') as f:
    f.write('SECRET-OUTSIDE\n')
os.symlink(os.path.join(BASE, 'outside.txt'), os.path.join(PROG, 'escape_link'))
os.symlink(os.path.join(PROG, 'inside.txt'), os.path.join(PROG, 'inside_link'))

CASES = {
    # `\{` and `\}` write the braces, a brace pair with no expression stays text,
    # and a backslash sequence the language does not know stays as written.
    'string_escapes': (r'''fn main() -> i64 {
  let x = 5
  print("a \{x\} {x} { }")
  print("t\tq \"q\" \\ \q")
  return 0
}
''', 'a {x} 5 { }\nt\tq "q" \\ \\q\n'),
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
    # `\w` is ASCII only; the search is unanchored, so the `h` is enough.
    'regex_unicode_word': ('print(re_match("h\u00e9llo", "\\\\w+"))\n'
                           'print(re_match("h\u00e9llo", "^\\\\w+$"))\n', 'true\nfalse\n'),
    # A pattern works on characters: `.` used to hand back the first byte of
    # `\u00e9`, which is not a string, and `\u00e9+` repeated the second byte.
    'regex_code_points': ('print(unwrap(re_find("\u00e9t\u00e9", ".")) == "\u00e9")\n'
                          'print(re_match("\u00e9\u00e9\u00e9", "^\u00e9+$"), re_match("\u00e9", "^..$"))\n'
                          'print(unwrap(re_find("\u043f\u0440\u0438\u0432\u0435\u0442 x", "[\u0430-\u044f]+")))\n'
                          'print(re_match("\u00e9", "^[^a]$"), re_match("\u20ac", "^\\\\W$"))\n'
                          'print(re_replace("a\u00e9b", ".", "<$0>"))\n',
                          'true\ntrue false\n\u043f\u0440\u0438\u0432\u0435\u0442\ntrue true\n<a><\u00e9><b>\n'),
    # An empty alternation branch matches "", so "ac" fails but "abd" passes.
    'regex_empty_alt': ('print(re_match("ac", "a(b|c|)d"))\n'
                        'print(re_match("abd", "a(b|c|)d"))\n', 'false\ntrue\n'),
    # An empty group or an empty first branch matches "" and the pattern goes
    # on: it used to jump to the end, so `b()$` accepted "ba".
    'regex_empty_group': ('print(re_match("ba", "b()$"), re_match("b", "b()$"))\n'
                          'print(re_match("ac", "^a(|b)d$"), re_match("ad", "^a(|b)d$"))\n'
                          'print(re_match("x", "^(|a)$"), re_match("", "^(|a)$"))\n'
                          'print(re_match("a1", "^a()\\{2\\}1$"), re_match("a", "^a()\\{2\\}1$"))\n'
                          'print(unwrap(re_captures("ab", "a()(|x)b")))\n',
                          'false true\nfalse true\nfalse true\ntrue false\n[\'ab\', \'\', \'\']\n'),
    # A nullable group under * never terminates the match loop early.
    'regex_nested_star': ('print(re_match("aaa", "(a*)*"))\n'
                          'print(re_match("b", "(a*)*"))\n', 'true\ntrue\n'),
    'regex_neg_class': ('print(re_match("abc", "[^xyz]+"))\n'
                        'print(re_match("xyz", "[^xyz]+"))\n', 'true\nfalse\n'),
    # `.` matches any character, newline included -- -O0 and -O2 agree on this.
    'regex_dot_bytes': ('print(re_match("a\\nb", "a.b"))\n', 'true\n'),
    # `^` anchors the whole input, not each line.
    'regex_anchor_input': ('print(re_match("foo\\nbar", "^bar"))\n', 'false\n'),
    # A float in the vector makes the sum a float. The integers used to be added
    # in 64 bits first, so two large ones wrapped and the total came out -1.75.
    'sum_mixed_large': ('fn id(v) {\n  return v\n}\n'
                        'print(id([0.25, 9223372036854775807, 9223372036854775807]).sum())\n'
                        'print(id([1, 0.1, 2, 0.2]).sum())\n'
                        'print(id([9223372036854775807, 1]).sum())\n',
                        '1.8446744073709552e+19\n3.3\n-9223372036854775808\n'),
    # `?` on None leaves a function that returns Option, running its defers.
    # It used to fall through with None in hand and fail on the next line.
    # A Result function keeps the None, and a `try` around it catches.
    'quest_none': ('''fn find(k: i64) -> Option[i64] {
  if k > 0 { return Some(k * 10) }
  return None
}
fn twice(k: i64) -> Option[i64] {
  defer print("left", k)
  let v = find(k)?
  return Some(v + 1)
}
fn res(k: i64) -> Result {
  let v = find(k)?
  return Ok(v)
}
fn guarded(k: i64) -> Option[i64] {
  try {
    let v = find(k)?
    return Some(v)
  } catch e {
    return Some(-1)
  }
}
fn main() -> i64 {
  print(twice(1), twice(0))
  print(res(1), res(0))
  print(guarded(2), guarded(0))
  return 0
}
''', 'left 1\nleft 0\nSome(11) None\nOk(10) Ok(None)\nSome(20) Some(-1)\n'),
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
            f.write(_cxx.emit_source(src, PROG))
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
    a = subprocess.run(_cxx.run_args(path),
                       capture_output=True, text=True, cwd=PROG, timeout=120)
    b = subprocess.run([exe], capture_output=True, text=True, cwd=PROG, timeout=120)
    if a.stdout != b.stdout or a.returncode != b.returncode:
        bad += 1
        print('%-22s DIFF rc i=%d n=%d' % (name, a.returncode, b.returncode))
        print('   run_o0: %r' % a.stdout[:150])
        print('   native: %r' % b.stdout[:150])
    elif a.stdout != want:
        bad += 1
        print('%-22s WRONG\n   got:  %r\n   want: %r' % (name, a.stdout[:150], want))
    else:
        print('%-22s ok   %r' % (name, a.stdout[:60]))
print('---- edge failures:', bad)
sys.exit(1 if bad else 0)