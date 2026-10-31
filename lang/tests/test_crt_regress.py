"""Regressions for crt.h error handling: a caught error must not change when a later
raise happens on the same thread, and a poisoned follow-up must not replace the first
error. Compiled with -fsanitize so memory errors fail the test directly."""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CRT = os.path.normpath(os.path.join(HERE, '..', 'engine', 'crt.h')).replace('\\', '/')

HARNESS = r'''
#include "%(crt)s"
#include <stdio.h>
#include <string.h>
Val vcall_any(Val f, Vec *args) { (void)f; (void)args; return vnil(); }
int main(void) {
    vraise("first failure");
    Val saved = vunwind;
    Val msg = saved.as.tv.args->items[0];
    verr_take();
    vraise("second failure");
    if (strcmp(msg.as.str, "first failure") != 0) { printf("FAIL stale\n"); return 1; }
    verr_take();
    vraise("first");
    vraise("poisoned");
    if (strcmp(vunwind.as.tv.args->items[0].as.str, "first") != 0) { printf("FAIL first-wins\n"); return 1; }
    verr_take();
    vdrop(saved);
    printf("REGRESS OK\n");
    return 0;
}
'''

def run():
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 'h.c'); exe = os.path.join(d, 'h')
        with open(src, 'w', encoding='utf-8') as f:
            f.write(HARNESS % {'crt': CRT})
        cc = os.environ.get('CC', 'gcc')
        b = subprocess.run([cc, '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', '-w', src, '-o', exe, '-lm'],
                           capture_output=True, text=True)
        if b.returncode != 0:
            print('build failed:', b.stderr[-800:]); return 1
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        r = subprocess.run([exe], capture_output=True, text=True, env=env)
        sys.stdout.write(r.stdout)
        if r.returncode != 0 or 'REGRESS OK' not in r.stdout:
            print('STDERR:', r.stderr[-1200:]); return 1
    return 0

if __name__ == '__main__':
    sys.exit(run())