"""Arena runtime in engine/crt.h, checked under AddressSanitizer.

Checks chunk release and reuse, nested marks, and ASan poisoning of reset memory.
"""
import _scratch
import _skip
import os
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'engine')

HARNESS = r'''
#include "crt.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static Val str_val(char *s) { Val v; v.tag = V_STR; v.as.str = s; return v; }

int main(void) {
    /* 1. outside an arena, balloc stays on the heap */
    char *h = (char *)balloc(8);
    strcpy(h, "heap");
    CHECK(!BIS_ARENA((BHead *)h - 1));
    CHECK(strcmp(h, "heap") == 0);
    vrc_dec(str_val(h));

    /* 2. many small objects across several chunks, all readable before leave */
    BMark m = barena_enter();
    char *slots[20000];
    for (int i = 0; i < 20000; i++) {
        char *s = (char *)balloc(100);
        memset(s, 'a' + (i % 26), 99);
        s[99] = 0;
        slots[i] = s;
        CHECK(BIS_ARENA((BHead *)s - 1));
    }
    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        if (slots[i][0] != 'a' + (i % 26) || slots[i][98] != 'a' + (i % 26)) bad++;
    }
    CHECK(bad == 0);
    barena_leave(m);
    CHECK(barena_top == NULL);
    CHECK(barena_depth == 0);

    /* 3. releasing an arena object must not free it: no free() on arena memory */
    m = barena_enter();
    char *a = (char *)balloc(16);
    strcpy(a, "dropped");
    vrc_dec(str_val(a));
    vrc_dec(str_val(a));
    barena_leave(m);

    /* 4. nested arenas: an outer object survives the inner leave */
    BMark outer = barena_enter();
    char *o = (char *)balloc(32);
    strcpy(o, "outer");
    BMark inner = barena_enter();
    char *in = (char *)balloc(4096 * 3);
    strcpy(in, "inner");
    CHECK(strcmp(in, "inner") == 0);
    barena_leave(inner);
    CHECK(strcmp(o, "outer") == 0);
    char *o2 = (char *)balloc(8);
    strcpy(o2, "after");
    CHECK(strcmp(o2, "after") == 0);
    barena_leave(outer);

    /* 5. bgrow_rc on an arena object copies the old contents */
    m = barena_enter();
    char *p = (char *)balloc(4);
    strcpy(p, "abc");
    char *q = (char *)bgrow_rc(p, 200);
    CHECK(strcmp(q, "abc") == 0);
    CHECK(BIS_ARENA((BHead *)q - 1));
    barena_leave(m);

    /* 6. vdrop on an arena container returns without touching its memory */
    m = barena_enter();
    Vec *v = (Vec *)balloc(sizeof(Vec));
    v->items = NULL; v->len = 0; v->cap = 0;
    Val vv; vv.tag = V_VEC; vv.as.vec = v;
    vdrop(vv);
    barena_leave(m);

    if (fails == 0) printf("ARENA C OK\n");
    return fails ? 1 : 0;
}
'''

UAF_HARNESS = r'''
#include "crt.h"

int main(void) {
    BMark m = barena_enter();
    volatile char *stale = (volatile char *)balloc(8);
    stale[0] = 'x';
    barena_leave(m);
    return stale[0];
}
'''

OVERFLOW_HARNESS = r'''
#include "crt.h"

int main(void) {
    BMark m = barena_enter();
    (void)m;
    balloc(SIZE_MAX);
    return 0;
}
'''


def cc():
    for name in ('gcc', 'clang', 'cc'):
        if shutil.which(name):
            return name
    return None


def main():
    compiler = cc()
    if compiler is None:
        _skip.skip('no C compiler, the arena runtime cannot be built')
    tmp = _scratch.mkdtemp(prefix='brat-arena-')
    try:
        src = os.path.join(tmp, 'arena.c')
        exe = os.path.join(tmp, 'arena')
        with open(src, 'w', encoding='utf-8') as f:
            f.write(HARNESS)
        base = [compiler, '-O1', '-g', '-Wall', '-I', ENGINE, src, '-o', exe, '-lm', '-lpthread']
        p = subprocess.run(base + ['-fsanitize=address', '-fno-omit-frame-pointer'],
                           capture_output=True, text=True, timeout=180)
        sanitized = p.returncode == 0
        if not sanitized:
            p = subprocess.run(base, capture_output=True, text=True, timeout=180)
            assert p.returncode == 0, 'arena harness build failed:\n' + (p.stdout + p.stderr)[-2000:]
        env = dict(os.environ)
        env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
        r = subprocess.run([exe], capture_output=True, text=True, timeout=120, env=env)
        out = r.stdout + r.stderr
        assert r.returncode == 0 and 'ARENA C OK' in out, 'arena run failed (rc=%d):\n%s' % (r.returncode, out[-3000:])
        overflow_src = os.path.join(tmp, 'arena_overflow.c')
        overflow_exe = os.path.join(tmp, 'arena_overflow')
        with open(overflow_src, 'w', encoding='utf-8') as f:
            f.write(OVERFLOW_HARNESS)
        overflow_cmd = [compiler, '-O1', '-g', '-Wall', '-I', ENGINE,
                        overflow_src, '-o', overflow_exe, '-lm', '-lpthread']
        p = subprocess.run(overflow_cmd, capture_output=True, text=True, timeout=180)
        assert p.returncode == 0, 'arena overflow harness build failed:\n' + (p.stdout + p.stderr)[-2000:]
        overflow = subprocess.run([overflow_exe], capture_output=True, text=True,
                                  timeout=120, cwd=tmp)
        assert overflow.returncode != 0 and 'brat: out of memory' in overflow.stderr, \
            'oversized arena request was not rejected safely: rc=%d err=%s' % (
                overflow.returncode, overflow.stderr[-1200:])
        if sanitized:
            uaf_src = os.path.join(tmp, 'arena_uaf.c')
            uaf_exe = os.path.join(tmp, 'arena_uaf')
            with open(uaf_src, 'w', encoding='utf-8') as f:
                f.write(UAF_HARNESS)
            uaf_cmd = [compiler, '-O1', '-g', '-Wall', '-I', ENGINE, uaf_src,
                       '-o', uaf_exe, '-lm', '-lpthread', '-fsanitize=address',
                       '-fno-omit-frame-pointer']
            p = subprocess.run(uaf_cmd,
                               capture_output=True, text=True, timeout=180)
            assert p.returncode == 0, 'arena UAF harness build failed:\n' + (p.stdout + p.stderr)[-2000:]
            uaf = subprocess.run([uaf_exe], capture_output=True, text=True,
                                 timeout=120, env=env)
            assert uaf.returncode != 0 and 'use-after-poison' in uaf.stderr, \
                'ASan did not catch a read after arena reset: rc=%d err=%s' % (
                    uaf.returncode, uaf.stderr[-1200:])
        mode = 'AddressSanitizer' if sanitized else 'plain build (no ASan here)'
        print('ARENA C OK (%s)' % mode)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()