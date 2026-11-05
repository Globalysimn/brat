"""Amortized string append stays linear and owns only a logarithmic number of buffers."""
import os
import re
import subprocess
import sys

import _cxx
import _scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")

TMP = _scratch.mkdtemp(prefix="brat-string-builder-")


def source_for(count):
    return '''fn main() -> i64 {
  let mut text = ""
  for i in 0..%d { text = text + "x" }
  print(len(text))
  return 0
}
''' % count


def run(argv, timeout=180, env=None):
    return subprocess.run(argv, cwd=ROOT, capture_output=True, text=True,
                          timeout=timeout, env=env)


def main():
    native = _cxx.build()
    small_n = 2000
    small_path = os.path.join(TMP, "small.brat")
    with open(small_path, "w", encoding="utf-8") as stream:
        stream.write(source_for(small_n))

    cpp = run([native, "cc", small_path])
    assert cpp.returncode == 0, cpp.stderr
    assert "vadd_self(" in cpp.stdout, "unique-owner append lowering was not selected"

    interp = run(_cxx.cli_args("run", "--interp", small_path))
    assert (interp.returncode, interp.stdout, interp.stderr) == (0, "%d\n" % small_n, ""), \
           (interp.returncode, interp.stdout, interp.stderr)

    large_n = 200000
    large_path = os.path.join(TMP, "large.brat")
    with open(large_path, "w", encoding="utf-8") as stream:
        stream.write(source_for(large_n))
    large = run([native, "cc", large_path])
    assert large.returncode == 0, large.stderr
    assert "vadd_self(" in large.stdout, "large loop missed unique-owner append lowering"

    c_path = os.path.join(TMP, "large.c")
    exe = os.path.join(TMP, "large-asan")
    instrument = '''#include <stddef.h>
#include <stdlib.h>
static size_t brat_calloc_count;
static void *(*brat_real_calloc)(size_t, size_t) = calloc;
static void *brat_counted_calloc(size_t n, size_t s) {
  brat_calloc_count++;
  return brat_real_calloc(n, s);
}
#define calloc brat_counted_calloc
#include "crt.h"
#undef calloc
__attribute__((destructor)) static void brat_report_callocs(void) {
  fprintf(stderr, "brat_calloc_count=%zu\\n", brat_calloc_count);
}
'''
    c_source = large.stdout.replace('#include "crt.h"', instrument, 1)
    with open(c_path, "w", encoding="utf-8") as stream:
        stream.write(c_source)
    compiled = run(["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-fwrapv", "-I", ENGINE, c_path,
                    "-o", exe, "-lm", "-pthread"])
    assert compiled.returncode == 0, compiled.stderr
    sanitized = run([exe], env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"))
    assert sanitized.returncode == 0 and sanitized.stdout == "%d\n" % large_n, \
           sanitized.stdout + sanitized.stderr
    match = re.fullmatch(r"brat_calloc_count=(\d+)\n", sanitized.stderr)
    assert match, sanitized.stderr
    allocations = int(match.group(1))
    assert allocations < 100, "append made too many heap buffers: %d" % allocations

    print("STRING BUILDER ALL GREEN (%d appends, %d calloc calls)" %
          (large_n, allocations))


if __name__ == "__main__":
    main()
