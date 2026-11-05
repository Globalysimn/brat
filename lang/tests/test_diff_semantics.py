"""Crafted-program interpreter/native byte agreement: defer capture and repr.

Each case runs through the interpreter and a compiled C binary; stdout and
exit code must match byte-for-byte. `test_agreement.py` keeps the shipped examples
green, this one pins the semantic seams those examples do not touch.
"""
import _cxx
import _scratch
import importlib.util, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENG = os.path.join(ROOT, 'lang', 'engine')
TMP = _scratch.mkdtemp(prefix='brat-sem-')


_tc_spec = importlib.util.spec_from_file_location('tc', os.path.join(os.path.dirname(os.path.abspath(__file__)), '_toolchain.py'))
_tc = importlib.util.module_from_spec(_tc_spec); _tc_spec.loader.exec_module(_tc)

# Reads back the bytes below; 0x00 stays out because C strings cannot hold it.
REPR_BIN = b"a\x01b\x0b\x1f\x7f\xc2\x85\xc2\xa0'\t\"\\\nz"

CASES = {
    # Defer: registration-time snapshot, block-scoped drain, LIFO.
    'defer_snapshot_rebind': ({}, '''fn main() -> i64 {
  let mut s = "abcdefghij"
  defer print(s)
  s = "zz"
  print("done")
  return 0
}
'''),
    'defer_lifo': ({}, '''fn main() -> i64 {
  defer print("first")
  defer print("second")
  print("body")
  return 0
}
'''),
    'defer_block_exit': ({}, '''fn main() -> i64 {
  if true {
    defer print("inner")
  }
  print("after")
  return 0
}
'''),
    'defer_nested_blocks': ({}, '''fn main() -> i64 {
  if true {
    defer print("outer")
    if true {
      defer print("inner1")
      defer print("inner2")
    }
    print("mid")
  }
  print("end")
  return 0
}
'''),
    'defer_loop_pass': ({}, '''fn main() -> i64 {
  let mut i = 0
  while i < 2 {
    defer print(i)
    i = i + 1
  }
  print("done")
  return 0
}
'''),
    'defer_continue': ({}, '''fn main() -> i64 {
  let mut i = 0
  while i < 3 {
    i = i + 1
    defer print(i)
    if i == 2 { continue }
  }
  print("end")
  return 0
}
'''),
    'defer_break': ({}, '''fn main() -> i64 {
  let mut i = 0
  while true {
    i = i + 1
    defer print(i)
    if i == 2 { break }
  }
  print("end")
  return 0
}
'''),
    'defer_on_return': ({}, '''fn f() -> i64 {
  defer print("cleanup")
  return 7
}
fn main() -> i64 {
  print(f())
  return 0
}
'''),
    'defer_unwind_block': ({}, '''fn main() -> i64 {
  try {
    if true {
      defer print("cleanup")
      raise("boom")
    }
  } catch e {
    print("caught")
  }
  return 0
}
'''),
    'defer_spawn': ({}, '''fn main() -> i64 {
  let mut s = "worker"
  spawn {
    defer print(s)
    s = "changed"
    print("in spawn")
  }
  join_all()
  print("main")
  return 0
}
'''),
    'defer_container_shared': ({}, '''fn main() -> i64 {
  let mut log = []
  let mut s = "old"
  defer log.push(s)
  s = "new"
  defer print(len(log))
  return 0
}
'''),
    # Repr: raw control bytes inside containers, quotes, tags, structs.
    'repr_control_bytes': ({'repr.bin': REPR_BIN}, '''fn main() -> i64 {
  let s = read_file("repr.bin")?
  print([s])
  print({"k": s})
  print(Ok(s))
  print(str([s]))
  print(s)
  print("in [{s}]")
  return 0
}
'''),
    'repr_quotes_backslash': ({}, r'''fn main() -> i64 {
  print(["plain", "it's", "say \"hi\"", "both '\" q", "back\\slash", "tab\tend", "nl\nend"])
  return 0
}
'''),
    'repr_quote_str_len': ({}, '''fn main() -> i64 {
  let s = str(["'"])
  print(len(s))
  print(s)
  return 0
}
'''),
    'repr_mixed_values': ({}, '''fn main() -> i64 {
  let p = Point { x: 1, y: "s" }
  print(p)
  print([1.5, 2.0, 1000000000000000.0, true, None, "x"])
  print(Ok(["n", 2]))
  print(Err(Some(1)))
  let mut m = {}
  m[true] = 1
  m[2] = "two"
  print(m)
  return 0
}
'''),
}

bad = 0
for name, (files, src) in CASES.items():
    path = os.path.join(TMP, name + '.brat')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    for fn, blob in files.items():
        with open(os.path.join(TMP, fn), 'wb') as f:
            f.write(blob)
    cf0 = os.path.join(TMP, name + '_o0.c')
    exe0 = os.path.join(TMP, name + '_o0.exe')
    cf = os.path.join(TMP, name + '.c')
    exe = os.path.join(TMP, name + '.exe')
    try:
        with open(cf0, 'w', encoding='utf-8') as f:
            f.write(_cxx.emit_source(src, TMP, flags=('-O0',)))
        with open(cf, 'w', encoding='utf-8') as f:
            f.write(_cxx.emit_source(src, TMP, flags=('-O2',)))
        rb0 = subprocess.run(_tc.resolve_cc().argv(cf0, exe0) + ['-O0', '-lm', '-lpthread', '-I', ENG],
                             capture_output=True, text=True, timeout=300)
        rb = subprocess.run(_tc.resolve_cc().argv(cf, exe) + ['-O2', '-lm', '-lpthread', '-I', ENG],
                            capture_output=True, text=True, timeout=300)
        if rb0.returncode != 0 or rb.returncode != 0:
            err = (rb0.stdout + rb0.stderr + rb.stdout + rb.stderr).strip().splitlines()[-1][:110]
            print('%-22s BUILD FAIL %s' % (name, err))
            bad += 1
            continue
    except Exception as e:
        print('%-22s EMIT FAIL %s' % (name, e))
        bad += 1
        continue
    a = subprocess.run([exe0], capture_output=True, timeout=120)
    b = subprocess.run([exe], capture_output=True, timeout=120)
    if a.stdout == b.stdout and a.returncode == b.returncode:
        print('%-22s ok   rc=%d %r' % (name, a.returncode, a.stdout[:60]))
    else:
        bad += 1
        print('%-22s DIFF rc i=%d n=%d' % (name, a.returncode, b.returncode))
        print('   interp: %r' % a.stdout[:150])
        print('   native: %r' % b.stdout[:150])
print('---- semantic diffs:', bad)
sys.exit(1 if bad else 0)