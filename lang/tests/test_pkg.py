import _cxx
import _scratch
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(os.path.dirname(HERE), 'engine')

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


tmp = _scratch.mkdtemp(prefix='brat-pkg-')
lib = os.path.join(tmp, 'mymod.brat')
with open(lib, 'w', encoding='utf-8') as f:
    f.write('pub fn twice(x: i64) -> i64 {\n  return x * 2\n}\n')
url = __import__('pathlib').Path(lib).as_uri()

r = subprocess.run(_cxx.cli_args('pkg', 'add', url), capture_output=True,
                   text=True, timeout=60, cwd=tmp)
check(r.returncode == 0, 'pkg add: ' + (r.stdout + r.stderr)[-300:])
check(os.path.isfile(os.path.join(tmp, 'vendor', 'mymod.brat')), 'vendored')
lock = json.load(open(os.path.join(tmp, 'brat.lock'), encoding='utf-8'))
check(len(lock) == 1 and len(lock[0]['sha256']) == 64, 'lock: %r' % (lock,))

main = os.path.join(tmp, 'app.brat')
with open(main, 'w', encoding='utf-8') as f:
    f.write('import mymod\nfn main() -> i64 {\n  print(twice(21))\n  return 0\n}\n')
r = subprocess.run(_cxx.cli_args('run', '-O0', main), capture_output=True,
                   text=True, timeout=60, cwd=tmp)
check(r.returncode == 0 and '42' in r.stdout, 'vendor import: ' + (r.stdout + r.stderr)[-300:])

os.unlink(os.path.join(tmp, 'vendor', 'mymod.brat'))
r = subprocess.run(_cxx.cli_args('pkg', 'install'), capture_output=True,
                   text=True, timeout=60, cwd=tmp)
check(r.returncode == 0 and os.path.isfile(os.path.join(tmp, 'vendor', 'mymod.brat')),
      'pkg install: ' + (r.stdout + r.stderr)[-300:])

verified = subprocess.run(_cxx.cli_args('pkg', 'verify'), capture_output=True,
                          text=True, timeout=60, cwd=tmp)
check(verified.returncode == 0 and verified.stdout == 'verified 1 package(s)\n',
      'pkg verify: ' + (verified.stdout + verified.stderr)[-300:])

vendor = os.path.join(tmp, 'vendor')
saved_vendor = os.path.join(tmp, 'vendor-saved')
outside = tmp + '-outside'
lock_path = os.path.join(tmp, 'brat.lock')
lock_before = open(lock_path, 'rb').read()
os.replace(vendor, saved_vendor)
os.mkdir(outside)
shutil.copy2(lib, os.path.join(outside, 'mymod.brat'))
try:
    os.symlink(outside, vendor)
    removed = subprocess.run(_cxx.cli_args('pkg', 'remove', 'mymod.brat'),
                             capture_output=True, text=True, timeout=60, cwd=tmp)
    check(removed.returncode == 1 and 'real directory' in removed.stderr,
          'pkg remove rejects symlink vendor: ' + removed.stderr)
    check(os.path.isfile(os.path.join(outside, 'mymod.brat')),
          'pkg remove through symlink did not touch outside package')
    check(open(lock_path, 'rb').read() == lock_before,
          'pkg remove symlink failure preserved lockfile')
finally:
    if os.path.islink(vendor):
        os.unlink(vendor)
    os.replace(saved_vendor, vendor)
    shutil.rmtree(outside)

removed = subprocess.run(_cxx.cli_args('pkg', 'remove', 'mymod.brat'),
                         capture_output=True, text=True, timeout=60, cwd=tmp)
check(removed.returncode == 0 and not os.path.exists(os.path.join(vendor, 'mymod.brat')),
      'pkg remove deletes a locked package: ' + removed.stderr)
check(json.load(open(lock_path, encoding='utf-8')) == [], 'pkg remove updates lock')
r = subprocess.run(_cxx.cli_args('pkg', 'add', url), capture_output=True,
                   text=True, timeout=60, cwd=tmp)
check(r.returncode == 0, 'reinstall after remove: ' + r.stderr)

with open(os.path.join(tmp, 'vendor', 'mymod.brat'), 'w', encoding='utf-8') as f:
    f.write('modified package\n')
verified = subprocess.run(_cxx.cli_args('pkg', 'verify'), capture_output=True,
                          text=True, timeout=60, cwd=tmp)
check(verified.returncode == 1 and 'HASH MISMATCH mymod.brat' in verified.stderr,
      'pkg verify detects local modification: ' + verified.stderr)

lock[0]['sha256'] = '0' * 64
json.dump(lock, open(os.path.join(tmp, 'brat.lock'), 'w', encoding='utf-8'))
r = subprocess.run(_cxx.cli_args('pkg', 'install'), capture_output=True,
                   text=True, timeout=60, cwd=tmp)
check(r.returncode == 1 and 'HASH MISMATCH' in (r.stdout + r.stderr), 'mismatch detected')

escape_name = os.path.basename(tmp) + '-escape.brat'
escape_path = os.path.join(os.path.dirname(tmp), escape_name)
unsafe_lock = [{'file': '../' + escape_name, 'url': url, 'sha256': lock[0]['sha256']}]
json.dump(unsafe_lock, open(os.path.join(tmp, 'brat.lock'), 'w', encoding='utf-8'))
r = subprocess.run(_cxx.cli_args('pkg', 'install'), capture_output=True,
                   text=True, timeout=60, cwd=tmp)
check(r.returncode == 1 and 'unsafe package filename' in r.stderr,
      'path traversal rejected: ' + r.stderr)
check(not os.path.exists(escape_path),
      'path traversal wrote outside vendor')

malformed_lock = [{'file': 'mymod.brat', 'url': url, 'sha256': '0' * 64,
                   'name': None, 'version': '1.0'}]
json.dump(malformed_lock, open(os.path.join(tmp, 'brat.lock'), 'w', encoding='utf-8'))
r = subprocess.run(_cxx.cli_args('pkg', 'list'), capture_output=True,
                   text=True, timeout=60, cwd=tmp)
check(r.returncode == 1 and 'invalid package identity' in r.stderr
      and 'Traceback' not in r.stderr, 'malformed lock rejected cleanly: ' + r.stderr)

with open(os.path.join(tmp, 'vendor', 'mymod.brat'), 'w', encoding='utf-8') as f:
    f.write('old package\n')
source_hash = hashlib.sha256(open(lib, 'rb').read()).hexdigest()
staged_lock = [
    {'file': 'mymod.brat', 'url': url, 'sha256': source_hash},
    {'file': 'other.brat', 'url': 'file:///missing-brat-package',
     'sha256': source_hash},
]
json.dump(staged_lock, open(os.path.join(tmp, 'brat.lock'), 'w', encoding='utf-8'))
r = subprocess.run(_cxx.cli_args('pkg', 'install'), capture_output=True,
                   text=True, timeout=60, cwd=tmp)
check(r.returncode == 1 and 'fetch' in r.stderr,
      'fetch failure rejected: ' + r.stderr)
check(open(os.path.join(tmp, 'vendor', 'mymod.brat'), encoding='utf-8').read()
      == 'old package\n', 'failed multi-package install changed existing vendor file')
check(not os.path.exists(os.path.join(tmp, 'vendor', 'other.brat')),
      'failed multi-package install left a partial package')

limit = 16 * 1024 * 1024
with open(os.path.join(tmp, 'brat.lock'), 'wb') as f:
    f.truncate(limit + 1)
r = subprocess.run(_cxx.cli_args('pkg', 'list'), capture_output=True,
                   text=True, timeout=60, cwd=tmp)
check(r.returncode == 1 and 'exceeds 16 MiB' in r.stderr,
      'oversized local lockfile must be refused before JSON parsing: ' + r.stderr)

large_index = os.path.join(tmp, 'brat-index.json')
with open(large_index, 'wb') as f:
    f.truncate(limit + 1)
env = dict(os.environ, BRAT_INDEX=large_index)
r = subprocess.run(_cxx.cli_args('pkg', 'add', 'owner/package'),
                   capture_output=True, text=True, timeout=60, cwd=tmp, env=env)
check(r.returncode == 1 and 'exceeds 16 MiB' in r.stderr,
      'oversized local package index must be bounded: ' + r.stderr)

shutil.rmtree(tmp, ignore_errors=True)
print('PKG ALL GREEN (%d asserts)' % n)