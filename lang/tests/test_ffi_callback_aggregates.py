"""Aggregate callbacks agree with C across compiler and interpreter boundaries."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import _cxx
import _skip

ENGINE_DIR = Path(__file__).resolve().parents[1] / "engine"
COMPILERS = [cc for cc in (shutil.which("gcc"), shutil.which("clang")) if cc]
if os.name == "nt" or not COMPILERS:
    _skip.skip("POSIX callback fixtures require GCC or Clang")
BIN = _cxx.build()


class AggregateCallbacks(unittest.TestCase):
    def check_case(self, c_source, declarations, body, expected):
        for compiler in COMPILERS:
            with self.subTest(compiler=compiler), tempfile.TemporaryDirectory(prefix="brat-cb-abi-") as tmp:
                root = Path(tmp)
                library = root / "callback.so"
                built = subprocess.run([compiler, "-std=c11", "-shared", "-fPIC", "-O2", "-x", "c", "-",
                                        "-o", str(library)], input=c_source, capture_output=True,
                                       text=True, timeout=60)
                self.assertEqual(built.returncode, 0, built.stderr)
                program = root / "main.brat"
                program.write_text(declarations.replace('"LIB"', '"%s"' % library) + body)
                env = dict(os.environ, CC=compiler, BRAT_CC=compiler,
                           BRAT_CACHE=str(root / "cache"), TMPDIR=str(root))
                commands = ([BIN, "run", "--unsafe-ffi", str(program)],
                            _cxx.cli_args("run", "--interp", "--unsafe-ffi", str(program)))
                for command in commands:
                    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=120)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stdout, expected)

                if Path(compiler).name == "gcc":
                    # Instrument both sides: an uninstrumented C reader can hide a dangling result.
                    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g1"]
                    built = subprocess.run([compiler, "-shared", "-fPIC", *flags, "-x", "c", "-",
                                            "-o", str(library)], input=c_source, capture_output=True,
                                           text=True, timeout=60)
                    self.assertEqual(built.returncode, 0, built.stderr)
                    emitted = subprocess.run([BIN, "emit-c", "--unsafe-ffi", str(program)],
                                             env=env, capture_output=True, text=True, timeout=60)
                    self.assertEqual(emitted.returncode, 0, emitted.stderr)
                    source, exe = root / "main.c", root / "main.asan"
                    source.write_text(emitted.stdout)
                    built = subprocess.run([compiler, "-std=gnu11", "-O1", *flags, "-I", str(ENGINE_DIR),
                                            str(source), str(library), "-lm", "-o", str(exe)],
                                           capture_output=True, text=True, timeout=120)
                    self.assertEqual(built.returncode, 0, built.stderr)
                    # String results have process lifetime. Numeric-result leaks are checked separately.
                    san_env = dict(env, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
                    result = subprocess.run([str(exe)], env=san_env, capture_output=True,
                                            text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stdout, expected)

    def test_distinct_argument_record(self):
        self.check_case(
            'typedef struct { int x, y; } Pt;\n'
            'typedef struct { double a; long long b; } Mix;\n'
            'long long apply(Pt (*cb)(Mix)) { Mix m = {1.5, 7}; Pt p = cb(m); return p.x * 1000 + p.y; }\n',
            'extern struct Pt { x: i32, y: i32 }\n'
            'extern struct Mix { a: f64, b: i64 }\n'
            'extern "LIB" fn apply(cb: fn(Mix) -> Pt) -> i64\n',
            'fn make(m: Mix) -> Pt { return Pt { x: m.b, y: 2 } }\nprint(apply("make"))\n',
            '7002\n')

    def test_bool_field(self):
        self.check_case(
            'typedef struct { _Bool on; int n; } Flags;\n'
            'long long apply(Flags (*cb)(long long)) { Flags f = cb(7); return f.on * 100 + f.n; }\n',
            'extern struct Flags { on: bool, n: i32 }\n'
            'extern "LIB" fn apply(cb: fn(i64) -> Flags) -> i64\n',
            'fn make(n: i64) -> Flags { return Flags { on: true, n: n } }\nprint(apply("make"))\n',
            '107\n')

    def test_bool_argument(self):
        self.check_case(
            'typedef struct { int x, y; } Pt;\n'
            'long long apply(Pt (*cb)(_Bool)) { Pt a = cb(1), b = cb(0); return a.x * 100 + b.x; }\n',
            'extern struct Pt { x: i32, y: i32 }\n'
            'extern "LIB" fn apply(cb: fn(bool) -> Pt) -> i64\n',
            'fn make(on: bool) -> Pt { if on { return Pt { x: 7, y: 0 } }\n'
            '  return Pt { x: 3, y: 0 } }\nprint(apply("make"))\n',
            '703\n')

    def test_scalar_array(self):
        self.check_case(
            'typedef struct { int values[3]; double weight; } Array;\n'
            'long long apply(Array (*cb)(long long)) { Array a = cb(3);\n'
            ' return a.values[0] + a.values[1] + a.values[2] + (long long)(a.weight * 2); }\n',
            'extern struct Array { values: i32[3], weight: f64 }\n'
            'extern "LIB" fn apply(cb: fn(i64) -> Array) -> i64\n',
            'fn make(n: i64) -> Array { return Array { values: [n, n + 1, n + 2], weight: 1.5 } }\n'
            'print(apply("make"))\n',
            '15\n')

    def test_record_array(self):
        self.check_case(
            'typedef struct { int x, y; } Pt;\n'
            'typedef struct { Pt values[2]; } Pairs;\n'
            'long long apply(Pairs (*cb)(long long)) { Pairs a = cb(3);\n'
            ' return a.values[0].x * 1000 + a.values[1].y; }\n',
            'extern struct Pt { x: i32, y: i32 }\n'
            'extern struct Pairs { values: Pt[2] }\n'
            'extern "LIB" fn apply(cb: fn(i64) -> Pairs) -> i64\n',
            'fn make(n: i64) -> Pairs { return Pairs { values: [Pt { x: n, y: 1 }, Pt { x: 2, y: 7 }] } }\n'
            'print(apply("make"))\n',
            '3007\n')

    def test_string_array(self):
        self.check_case(
            'typedef struct { const char *values[2]; } Names;\n'
            'long long apply(Names (*cb)(long long)) { Names a = cb(3);\n'
            ' return a.values[0][0] * 1000 + a.values[1][0]; }\n',
            'extern struct Names { values: str[2] }\n'
            'extern "LIB" fn apply(cb: fn(i64) -> Names) -> i64\n',
            'fn make(n: i64) -> Names { return Names { values: ["alpha", "beta"] } }\n'
            'print(apply("make"))\n',
            '97098\n')

    def test_long_string_result_survives_next_callback(self):
        self.check_case(
            '#include <string.h>\n'
            'typedef struct { const char *s; long long n; } Named;\n'
            'long long apply(Named (*cb)(long long)) { Named first = cb(7), second = cb(8);\n'
            ' return strcmp(first.s, "a-long-callback-string-7") == 0 &&\n'
            '        strcmp(second.s, "a-long-callback-string-8") == 0; }\n',
            'extern struct Named { s: str, n: i64 }\n'
            'extern "LIB" fn apply(cb: fn(i64) -> Named) -> i64\n',
            'fn make(n: i64) -> Named { return Named { s: "a-long-callback-string-{n}", n: n } }\n'
            'print(apply("make"))\n',
            '1\n')

    def test_null_pointer_arguments(self):
        for annotation in ('ptr', 'ptr[i32]'):
            with self.subTest(annotation=annotation):
                self.check_case(
                    'typedef struct { int x, y; } Pt;\n'
                    'long long apply(Pt (*cb)(void *)) { Pt p = cb(0); return p.x; }\n',
                    'extern struct Pt { x: i32, y: i32 }\n'
                    'extern "LIB" fn apply(cb: fn(%s) -> Pt) -> i64\n' % annotation,
                    'fn make(p: %s) -> Pt { if p == 0 { return Pt { x: 7, y: 0 } }\n'
                    ' return Pt { x: 3, y: 0 } }\nprint(apply("make"))\n' % annotation,
                    '7\n')

    def test_typed_pointer_result(self):
        self.check_case(
            'long long apply(int *(*cb)(int *)) { int n = 7; return cb(&n) == &n; }\n',
            'extern "LIB" fn apply(cb: fn(ptr[i32]) -> ptr[i32]) -> i64\n',
            'fn keep(p: ptr[i32]) -> ptr[i32] { return p }\nprint(apply("keep"))\n',
            '1\n')

    def test_record_argument_identity(self):
        self.check_case(
            'typedef struct { int x, y; } Pt;\n'
            'long long apply(Pt (*cb)(Pt)) { Pt p = {7, 3}; p = cb(p); return p.x * 1000 + p.y; }\n',
            'extern struct Pt { x: i32, y: i32 }\n'
            'extern "LIB" fn apply(cb: fn(Pt) -> Pt) -> i64\n',
            'fn keep(p: Pt) -> Pt { return p }\nprint(apply("keep"))\n',
            '7003\n')

    def test_numeric_results_do_not_accumulate_keepalives(self):
        with tempfile.TemporaryDirectory(prefix="brat-cb-refs-") as tmp:
            td = Path(tmp)
            c_src = td / "pair_loop.c"
            c_so = td / "libpairloop.so"
            c_src.write_text(
                "typedef struct { long long x, y; } Pair;\n"
                "long long run100(Pair (*cb)(long long)) {\n"
                "  long long acc = 0;\n"
                "  for (long long i = 0; i < 100; i++) {\n"
                "    Pair p = cb(i);\n"
                "    acc += p.x * 10 + p.y;\n"
                "  }\n"
                "  return acc;\n"
                "}\n",
                encoding="utf-8",
            )
            cc = COMPILERS[0]
            subprocess.run([cc, "-shared", "-fPIC", "-O2", str(c_src), "-o", str(c_so)],
                           check=True, timeout=30)
            prog = td / "app.brat"
            prog.write_text(
                "extern struct Pair { x: i64, y: i64 }\n"
                f'extern "{c_so}" fn run100(cb: fn(i64) -> Pair) -> i64\n'
                "fn make(i: i64) -> Pair { return Pair { x: 7, y: 3 } }\n"
                "fn main() -> i64 { print(run100(\"make\")); return 0 }\n",
                encoding="utf-8",
            )
            emitted = subprocess.run([BIN, "emit-c", "--unsafe-ffi", str(prog)],
                                     capture_output=True, text=True, check=True, timeout=30)
            c_out = td / "app.c"
            exe = td / "app_asan"
            c_out.write_text(emitted.stdout, encoding="utf-8")
            cc_asan = subprocess.run(
                [cc, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                 "-I", str(ENGINE_DIR), str(c_out), str(c_so), "-lm", "-pthread", "-o", str(exe)],
                capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(cc_asan.returncode, 0, cc_asan.stderr)
            rn = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30,
                                env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"))
            self.assertEqual(rn.returncode, 0, rn.stderr)
            self.assertEqual(rn.stdout, "7300\n")

    def test_concurrent_shim_builds_have_unique_outputs(self):
        from concurrent.futures import ThreadPoolExecutor

        with tempfile.TemporaryDirectory(prefix="brat-cb-builds-") as tmp:
            td = Path(tmp)
            prog = td / "same.brat"
            prog.write_text("fn main() -> i64 { print(42); return 0 }\n", encoding="utf-8")

            def build_one(k):
                out_exe = td / f"out_{k}"
                r = subprocess.run([BIN, "build", str(prog), "-o", str(out_exe)],
                                   capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stderr)
                return out_exe

            with ThreadPoolExecutor(max_workers=2) as pool:
                exes = list(pool.map(build_one, range(2)))
            self.assertEqual(len(set(exes)), 2)
            for ex in exes:
                rn = subprocess.run([str(ex)], capture_output=True, text=True, timeout=15)
                self.assertEqual((rn.returncode, rn.stdout), (0, "42\n"))
            leftover = [p for p in td.iterdir() if p.name.startswith(".brat-build-")]
            self.assertEqual(leftover, [], f"staging directories leaked: {leftover}")


if __name__ == "__main__":
    unittest.main()
