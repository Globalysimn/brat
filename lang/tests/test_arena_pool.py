"""Arena chunk reuse: a request loop gets its chunks back instead of re-mapping them."""
import _scratch

import importlib.util
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))

_spec = importlib.util.spec_from_file_location("tc_pool", os.path.join(HERE, "_toolchain.py"))
tc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tc)

HARNESS = r'''
#include "crt.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    bint i, before;
    for (i = 0; i < 50; i++) {
        BMark m = barena_enter();
        char *p = (char *)balloc(1024);
        p[0] = (char)i;
        barena_leave(m);
    }
    before = barena_reuses();
    CHECK(before >= 45);

    for (i = 0; i < 50; i++) {
        BMark m = barena_enter();
        char *p = (char *)balloc(4096);
        p[0] = 'x';
        barena_leave(m);
    }
    CHECK(barena_reuses() - before >= 45);

    {
        BMark m = barena_enter();
        char *big = (char *)balloc(8 * 1024 * 1024);
        big[0] = 1;
        barena_leave(m);
        CHECK(barena_reuses() == before + 50);
    }

    {
        BMark m = barena_enter();
        Val s = vstr("kept");
        CHECK(strcmp(vstr_c(&s), "kept") == 0);
        barena_leave(m);
    }

    printf(fails ? "POOL FAILED %d\n" : "POOL OK\n", fails);
    return fails ? 1 : 0;
}
'''

tmp = _scratch.mkdtemp(prefix="brat-pool-")
cfile = os.path.join(tmp, "pool.c")
with open(cfile, "w", encoding="utf-8") as f:
    f.write(HARNESS)

exe = os.path.join(tmp, "pool.exe")
argv = tc.resolve_cc().argv(cfile, exe) + ["-O1", "-I", ENGINE, "-lm"]
r = subprocess.run(argv, capture_output=True, text=True, timeout=300)
assert r.returncode == 0, r.stdout + r.stderr
out = subprocess.run([exe], capture_output=True, text=True, timeout=120)
assert out.returncode == 0 and out.stdout == "POOL OK\n", out.stdout + out.stderr

print("ARENA POOL ALL GREEN")
