"""Regressions for CRT and emitter lifetime, concurrency, SQLite, locale, and DWARF rules."""
import locale
import os
import platform
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
CRT = os.path.normpath(os.path.join(HERE, "..", "engine", "crt.h")).replace("\\", "/")
BRAT_BIN = os.path.join(
    ROOT, "lang", "cxx", "bin", "brat-cxx.exe" if platform.system() == "Windows" else "brat-cxx"
)

HARNESS = r'''
#include "%(crt)s"
#include <locale.h>
#include <stdio.h>
#include <string.h>
Val vcall_any(Val f, Vec *args) { (void)f; (void)args; return vnil(); }
int main(void) {
    vraise("first failure");
    Val saved = verr_take();
    Val msg = saved.as.tv->items[0];
    vraise("second failure");
    if (strcmp(vstr_c(&msg), "first failure") != 0) { printf("FAIL stale\n"); return 1; }
    Val second = verr_take();
    vraise("first");
    vraise("poisoned");
    if (strcmp(vstr_c(&vunwind.as.tv->items[0]), "first") != 0) { printf("FAIL first-wins\n"); return 1; }
    Val third = verr_take();
    vdrop(saved);
    vdrop(second);
    vdrop(third);

    /* Embedded NUL rejection in parse_int / parse_float */
    Val ni = vstrn("12\099", 5);
    Val pi = vparse_int(ni);
    if (pi.tag != V_TAG || strcmp(pi.as.tv->tag, "err") != 0) { printf("FAIL nul-int\n"); return 1; }
    vrc_dec(pi);
    vrc_dec(ni);

    Val nf = vstrn("1.5\0bad", 7);
    Val pf = vparse_float(nf);
    if (pf.tag != V_TAG || strcmp(pf.as.tv->tag, "err") != 0) { printf("FAIL nul-float\n"); return 1; }
    vrc_dec(pf);
    vrc_dec(nf);

    /* Locale-independent float formatting and parsing */
    const char *locs[] = {"de_DE.UTF-8", "fr_FR.UTF-8", "ru_RU.UTF-8", "es_ES.UTF-8", NULL};
    for (int li = 0; locs[li]; li++) {
        if (setlocale(LC_NUMERIC, locs[li])) break;
    }
    const char *fd = vfmt_double(1.5);
    if (strcmp(fd, "1.5") != 0) { printf("FAIL locale-fmt: %s\n", fd); return 1; }
    Val fstr = vstr_sso("1.5", 3);
    Val fp = vparse_float(fstr);
    if (fp.tag != V_TAG || strcmp(fp.as.tv->tag, "ok") != 0 || fp.as.tv->items[0].as.fval != 1.5) {
        printf("FAIL locale-parse\n");
        return 1;
    }
    vrc_dec(fp);
    setlocale(LC_NUMERIC, "C");

    /* > 512 distinct interned tag names must not leak */
    for (int i = 0; i < 600; i++) {
        char tname[32];
        snprintf(tname, sizeof tname, "dyn_tag_%d", i);
        Vec *tv = vvec();
        Val tagged = vtagv(tname, tv);
        Val tn = vtagof(tagged);
        if (strcmp(vstr_c(&tn), tname) != 0) { printf("FAIL tagof\n"); return 1; }
        vrc_dec(tn);
        vrc_dec(tagged);
    }

    /* SQLite column key safety: > 14B column names must not alias via vlit(),
     * and "$type" column names must be rejected. */
    Val db_res = vsql_open(vstr_sso(":memory:", 8));
    if (db_res.tag == V_TAG && strcmp(db_res.as.tv->tag, "ok") == 0) {
        Val db = db_res.as.tv->items[0];
        Vec *ep1 = vvec();
        Val q1 = vsql_query(db, vlit("SELECT 1 AS long_sqlite_column_alpha"), vvecv(ep1));
        vrc_dec(vvecv(ep1));
        Vec *ep2 = vvec();
        Val q2 = vsql_query(db, vlit("SELECT 2 AS long_sqlite_column_bravo"), vvecv(ep2));
        vrc_dec(vvecv(ep2));
        if (q1.tag != V_TAG || strcmp(q1.as.tv->tag, "ok") != 0 ||
            q2.tag != V_TAG || strcmp(q2.as.tv->tag, "ok") != 0) {
            printf("FAIL sql-long-col\n");
            return 1;
        }
        Val row1 = q1.as.tv->items[0].as.vec->items[0];
        Val row2 = q2.as.tv->items[0].as.vec->items[0];
        if (!map_get(row1.as.map, "long_sqlite_column_alpha") ||
            !map_get(row2.as.map, "long_sqlite_column_bravo")) {
            printf("FAIL sql-col-alias\n");
            return 1;
        }
        vrc_dec(q1);
        vrc_dec(q2);

        Vec *ep3 = vvec();
        Val q3 = vsql_query(db, vlit("SELECT 1 AS \"$type\""), vvecv(ep3));
        vrc_dec(vvecv(ep3));
        if (q3.tag != V_TAG || strcmp(q3.as.tv->tag, "err") != 0) {
            printf("FAIL sql-reserved-col\n");
            return 1;
        }
        vrc_dec(q3);

        Val cl = vsql_close(db);
        vrc_dec(cl);
    }
    vrc_dec(db_res);

    printf("REGRESS OK\n");
    return 0;
}
'''

BRAT_PROG = r'''import std.env

struct Box { s: str }

fn wipe(mut b: Box) -> str {
  b.s = "after_" + str(2)
  return "!"
}

fn grow(mut m: Dyn) -> str {
  let mut i = 0
  while i < 64 {
    m["k" + str(i)] = i
    i = i + 1
  }
  return "!"
}

fn nested_first() -> str {
  let xs = [["deep_" + str(42)]]
  return xs.first().first()
}

fn boom(z: i64) -> str {
  let q = 1 / z
  return str(q)
}

fn bad_idx_set(mut md: Dyn) -> bool {
  md[0]["k" + str(9)] = boom(0)
  return false
}

fn main() {
  let outer = [["keep_" + str(1), "two_" + str(2)]]
  let x = outer[0].first()
  assert(x == "keep_1")
  assert(outer[0][0] == "keep_1")
  assert(nested_first() == "deep_42")

  let mut b = Box { s: "before_" + str(1) }
  assert(b.s + wipe(b) == "before_1!")

  let mut m: Dyn = {"s": "dyn_" + str(1)}
  assert(m.s + grow(m) == "dyn_1!")

  let mut a = [[1, 2]]
  a[0].push(3)
  assert(len(a[0]) == 3)

  let mut md: Dyn = [{"a": "x_" + str(1)}]
  let mut caught = false
  try {
    bad_idx_set(md)
  } catch e {
    caught = true
  }
  assert(caught)
  assert(!md[0].has("k9"))

  let strs = ["cond_" + str(99)]
  if strs[0] {
    assert(!(!strs[0]))
  }
  if b.s {
    assert(true)
  }

  let mut i = 0
  while i < 64 {
    let t = tagof(Ok(i))
    assert(t == "ok")
    i = i + 1
  }

  let mut dm = {"k": "val_" + str(7)}
  assert(dm.get("k") == "val_7")
  assert(dm.set("k2", "val_" + str(8)) == "val_8")
  let mut sv = [3, 1, 2]
  let sv2 = sv.sort()
  assert(sv2[0] == 1)
  let found = strs.find(fn(el) { return len(el) > 0 })
  assert(found == "cond_99")

  let shared_v = [10, 20, 30, 40]
  let shared_m = {"a": 1, "b": 2, "c": 3}
  scope {
    spawn {
      let c = shared_v.copy()
      let z = shared_v.zip(c)
      assert(len(z) == 4)
      assert(shared_m.has("b"))
      assert(len(shared_m.keys()) == 3)
      assert(len(shared_m.vals()) == 3)
      assert(len(shared_m.items()) == 3)
    }
    spawn {
      assert(shared_v.first() == 10)
      assert(shared_v.last() == 40)
      assert(shared_v.min() == 10)
      assert(shared_v.max() == 40)
      assert(shared_m.get("c") == 3)
    }
  }

  assert(env.get_or("__BRAT_MISSING_ENV_VAR__", "fb_" + str(1)) == "fb_1")
  print("EMIT REGRESS OK")
}
'''

DBG_PROG = r'''let top_x = 10
fn helper(n: i64) -> i64 {
  defer print(n)
  let top_x = n + 1
  return top_x
}
let top_y = helper(top_x)
'''


def has_sanitizer(cc: str) -> bool:
    if platform.system() == "Windows":
        return False
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "t.c")
        e = os.path.join(td, "t")
        with open(p, "w", encoding="utf-8") as f:
            f.write("int main(void){return 0;}\n")
        r = subprocess.run(
            [cc, "-fsanitize=address,undefined", p, "-o", e],
            capture_output=True,
            text=True,
        )
        return r.returncode == 0


def run() -> int:
    cc = os.environ.get("CC", "gcc")
    use_san = has_sanitizer(cc)
    san_flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"] if use_san else []
    libs = [] if platform.system() == "Windows" else ["-lpthread", "-lm", "-ldl"]
    san_env = dict(os.environ)
    if use_san:
        # LSan requires ptrace; verify whether detect_leaks=1 works in this container.
        san_env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"
        san_env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"

    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "h.c")
        exe = os.path.join(d, "h.exe" if platform.system() == "Windows" else "h")
        with open(src, "w", encoding="utf-8") as f:
            f.write(HARNESS.replace("%(crt)s", CRT))
        b = subprocess.run(
            [cc, "-std=gnu11", "-O1", "-g", *san_flags, "-w", src, "-o", exe, *libs],
            capture_output=True,
            text=True,
        )
        if b.returncode != 0:
            print("build failed:", b.stderr[-800:])
            return 1
        r = subprocess.run([exe], capture_output=True, text=True, env=san_env)
        if use_san and r.returncode != 0 and "LeakSanitizer has encountered a fatal error" in r.stderr:
            san_env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
            r = subprocess.run([exe], capture_output=True, text=True, env=san_env)
        sys.stdout.write(r.stdout)
        if r.returncode != 0 or "REGRESS OK" not in r.stdout:
            print("STDERR:", r.stderr[-1200:])
            return 1

        if os.path.exists(BRAT_BIN):
            bsrc = os.path.join(d, "emit_reg.brat")
            with open(bsrc, "w", encoding="utf-8") as f:
                f.write(BRAT_PROG)
            for opt in ("-O0", "-O1", "-O2"):
                cpath = os.path.join(d, f"emit_{opt[1:]}.c")
                epath = os.path.join(
                    d, f"emit_{opt[1:]}.exe" if platform.system() == "Windows" else f"emit_{opt[1:]}"
                )
                em = subprocess.run([BRAT_BIN, "emit-c", opt, bsrc], capture_output=True, text=True)
                if em.returncode != 0:
                    print(f"emit-c {opt} failed:", em.stderr)
                    return 1
                with open(cpath, "w", encoding="utf-8") as f:
                    f.write(em.stdout)
                cb = subprocess.run(
                    [
                        cc,
                        "-std=gnu11",
                        "-O1",
                        "-g",
                        *san_flags,
                        "-w",
                        f"-I{os.path.join(ROOT, 'lang', 'engine')}",
                        cpath,
                        "-o",
                        epath,
                        *libs,
                    ],
                    capture_output=True,
                    text=True,
                )
                if cb.returncode != 0:
                    print(f"cc {opt} failed:", cb.stderr[-800:])
                    return 1
                er = subprocess.run([epath], capture_output=True, text=True, env=san_env)
                if er.returncode != 0 or "EMIT REGRESS OK" not in er.stdout:
                    print(f"run {opt} failed:", er.stdout, er.stderr[-1200:])
                    return 1

            dsrc = os.path.join(d, "dbg_line.brat")
            with open(dsrc, "w", encoding="utf-8") as f:
                f.write(DBG_PROG)
            dem = subprocess.run([BRAT_BIN, "emit-c", "-g", "-O0", dsrc], capture_output=True, text=True)
            if dem.returncode != 0:
                print("emit-c -g failed:", dem.stderr)
                return 1
            if "#line 3" not in dem.stdout or "#line 7" not in dem.stdout:
                print("missing #line directives for defer or shadowed top-level stmt:\n", dem.stdout)
                return 1

    return 0


if __name__ == "__main__":
    sys.exit(run())
