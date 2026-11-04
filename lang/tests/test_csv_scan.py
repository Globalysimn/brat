"""Unicode-safe CSV scanning is byte-linear and remains equivalent in both runtimes."""
import _cxx
import _scratch

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "lang", "engine")
CLI = os.path.join(ENGINE, "cli.py")
TMP = _scratch.mkdtemp(prefix="brat-csv-")


def run(argv, cwd=TMP, timeout=180):
    return subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=timeout)


def main():
    native = _cxx.build()
    data_path = os.path.join(TMP, "rows.csv")
    row = 'café,שלום🙂,"hello, ""world"""\r\n'
    with open(data_path, "w", encoding="utf-8", newline="") as stream:
        stream.write(row * 6000)
        stream.write('"","tail"\r\n')
    source = os.path.join(TMP, "csv.brat")
    with open(source, "w", encoding="utf-8") as stream:
        stream.write('''import std.csv

fn main() -> i64 {
  let text = unwrap(read_file("rows.csv"))
  let rows = unwrap(std.csv.parse(text))
  assert(len(rows) == 6001)
  assert(rows[0][0] == "café" && rows[0][1] == "שלום🙂")
  assert(rows[0][2] == "hello, \\"world\\"")
  assert(rows[6000][0] == "" && rows[6000][1] == "tail")
  let reparsed = unwrap(std.csv.parse(std.csv.dump(rows)))
  assert(reparsed == rows)
  print("csv ok")
  return 0
}
''')

    interp = run([sys.executable, CLI, "run", "--interp", source])
    assert interp.returncode == 0 and interp.stdout == "csv ok\n", interp.stdout + interp.stderr

    exe = os.path.join(TMP, "csv-native")
    built = run([native, "build", source, "-o", exe])
    assert built.returncode == 0, built.stdout + built.stderr
    result = run([exe])
    assert (result.returncode, result.stdout, result.stderr) == \
           (interp.returncode, interp.stdout, ""), result.stdout + result.stderr

    emitted = run([native, "cc", source])
    assert emitted.returncode == 0, emitted.stderr
    c_path = os.path.join(TMP, "csv.c")
    asan_exe = os.path.join(TMP, "csv-asan")
    with open(c_path, "w", encoding="utf-8") as stream:
        stream.write(emitted.stdout)
    compiled = run(["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I", ENGINE, c_path, "-o", asan_exe,
                    "-lm", "-pthread"])
    assert compiled.returncode == 0, compiled.stderr
    sanitized = run([asan_exe])
    assert sanitized.returncode == 0 and sanitized.stdout == "csv ok\n", \
           sanitized.stdout + sanitized.stderr

    print("CSV SCANNER ALL GREEN")


if __name__ == "__main__":
    main()
