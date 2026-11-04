"""Measure the unique-owner string append loop at three input sizes."""
import os
import statistics
import subprocess
import tempfile
import time

import _cxx

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")


def source_for(count):
    return '''fn main() -> i64 {
  let mut text = ""
  for i in 0..%d { text = text + "x" }
  print(len(text))
  return 0
}
''' % count


def main():
    native = _cxx.build()
    counts = (100000, 500000, 1000000)
    with tempfile.TemporaryDirectory(prefix="brat-string-bench-") as tmp:
        executables = {}
        for count in counts:
            source = os.path.join(tmp, "append-%d.brat" % count)
            with open(source, "w", encoding="utf-8") as stream:
                stream.write(source_for(count))
            emitted = subprocess.run([native, "cc", source], cwd=ROOT,
                                     capture_output=True, text=True, timeout=60)
            if emitted.returncode:
                raise RuntimeError(emitted.stderr)
            if "vadd_self(" not in emitted.stdout:
                raise RuntimeError("unique-owner append path was not selected")
            c_path = os.path.join(tmp, "append-%d.c" % count)
            exe = os.path.join(tmp, "append-%d" % count)
            with open(c_path, "w", encoding="utf-8") as stream:
                stream.write(emitted.stdout)
            compiled = subprocess.run(
                ["cc", "-std=gnu11", "-O2", "-fwrapv", "-I", ENGINE, c_path,
                 "-o", exe, "-lm", "-pthread"],
                cwd=ROOT, capture_output=True, text=True, timeout=60)
            if compiled.returncode:
                raise RuntimeError(compiled.stderr)
            executables[count] = exe

        for count, exe in executables.items():
            samples = []
            for _ in range(7):
                start = time.perf_counter()
                result = subprocess.run([exe], cwd=ROOT, capture_output=True,
                                        text=True, timeout=30)
                elapsed = time.perf_counter() - start
                if result.returncode or result.stdout != "%d\n" % count:
                    raise RuntimeError(result.stderr or "benchmark result mismatch")
                samples.append(elapsed)
            median = statistics.median(samples)
            print("%d appends: %.6f s (%.1f ns/append)" %
                  (count, median, median * 1e9 / count))


if __name__ == "__main__":
    main()
