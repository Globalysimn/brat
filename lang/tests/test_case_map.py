"""Native case mapping must match the interpreter (Python str.upper / str.lower).

Every code point from U+0000 to U+10FFFF is checked for
single-character simple mappings. Code points where Python maps to several
characters (ß -> SS) or depends on context (final sigma) are skipped; those
are covered by their own tests.
"""
import _scratch
import os
import subprocess
import sys
import tempfile
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
CRT = os.path.normpath(os.path.join(HERE, '..', 'engine', 'crt.h')).replace('\\', '/')
MAX_CP = 0x10FFFF

HARNESS = r'''
#define BRAT_CASE_MAP_VERSION %(version)d
#include "%(crt)s"
#include <stdio.h>
Val vcall_any(Val f, Vec *args) { (void)f; (void)args; return vnil(); }
int main(void) {
    for (unsigned cp = 0; cp <= %(max)d; cp++) {
        printf("%%u %%u %%u\n", cp, vcp_upper(cp), vcp_lower(cp));
    }
    return 0;
}
'''


def build_harness(tmp):
    src = os.path.join(tmp, 'case.c')
    exe = os.path.join(tmp, 'case')
    with open(src, 'w', encoding='utf-8') as f:
        version = tuple(int(part) for part in unicodedata.unidata_version.split('.'))
        case_version = 1600 if version >= (16, 0) else 1500
        f.write(HARNESS % {'crt': CRT, 'max': MAX_CP, 'version': case_version})
    cc = os.environ.get('CC', 'gcc')
    b = subprocess.run([cc, '-std=gnu11', '-O1', src, '-o', exe],
                       capture_output=True, text=True)
    assert b.returncode == 0, 'harness build failed:\n' + b.stderr[-800:]
    return exe


def expected(cp):
    ch = chr(cp)
    up = ch.upper()
    low = ch.lower()
    want_up = None
    want_low = None
    if len(up) == 1:
        want_up = 0 if up == ch else ord(up)
    if len(low) == 1 and cp != 0x3A3:   # final sigma is context-dependent
        want_low = 0 if low == ch else ord(low)
    return want_up, want_low


def main():
    tmp = _scratch.mkdtemp(prefix='brat-casemap-')
    exe = build_harness(tmp)
    out = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, 'harness failed rc=%d' % out.returncode
    bad = []
    checked = 0
    for line in out.stdout.splitlines():
        cp, up, low = (int(x) for x in line.split())
        want_up, want_low = expected(cp)
        if want_up is not None:
            checked += 1
            if up != want_up:
                bad.append('upper U+%04X: native %s, want %s' % (cp, hex(up), hex(want_up)))
        if want_low is not None:
            checked += 1
            if low != want_low:
                bad.append('lower U+%04X: native %s, want %s' % (cp, hex(low), hex(want_low)))
    assert not bad, '%d mismatches, first:\n%s' % (len(bad), '\n'.join(bad[:12]))
    print('CASE MAP ALL GREEN (%d asserts)' % checked)


if __name__ == '__main__':
    main()
