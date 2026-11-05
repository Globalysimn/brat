"""The binary cache must observe compiler inputs, not just generated C."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import _skip

ENGINE = Path(__file__).resolve().parents[1] / "engine"
CC = shutil.which("cc") or shutil.which("gcc")
if not CC:
    _skip.skip("a C compiler is required")
spec = importlib.util.spec_from_file_location("dependency_toolchain", Path(__file__).resolve().parent / "_toolchain.py")
tc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tc)


class CacheDependencies(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="brat-cache-inputs-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / "main.c"
        self.exe = self.root / ("main.exe" if os.name == "nt" else "main")
        self.cache = self.root / "cache"
        self.env = dict(os.environ, BRAT_CACHE=str(self.cache))
        self.scope = patch.dict(os.environ, self.env)
        self.scope.start()
        self.addCleanup(self.scope.stop)

    def build(self, flags=(), env=None, compiler=CC):
        args = [compiler, str(self.src), "-o", str(self.exe), *flags]
        return tc.cc_cached(args, str(self.src), str(self.exe), timeout=30, env=env)

    def result(self):
        return subprocess.run([str(self.exe)], timeout=10).returncode

    def test_header_and_transitive_header_changes(self):
        self.src.write_text('#include "outer.h"\nint main(void){return VALUE;}\n')
        (self.root / "outer.h").write_text('#include "inner.h"\n')
        header = self.root / "inner.h"
        header.write_text('#define VALUE 3\n')
        self.assertEqual(self.build().returncode, 0)
        self.assertEqual(self.result(), 3)
        header.write_text('#define VALUE 7\n')
        self.assertEqual(self.build().returncode, 0)
        self.assertEqual(self.result(), 7)

    def test_deleted_header_cannot_hit_cache(self):
        self.src.write_text('#include "value.h"\nint main(void){return VALUE;}\n')
        header = self.root / "value.h"
        header.write_text('#define VALUE 3\n')
        self.assertEqual(self.build().returncode, 0)
        header.unlink()
        self.assertNotEqual(self.build().returncode, 0)

    def test_new_header_shadows_previous_include(self):
        early, late = self.root / "early", self.root / "late"
        early.mkdir()
        late.mkdir()
        self.src.write_text('#include <value.h>\nint main(void){return VALUE;}\n')
        (late / "value.h").write_text('#define VALUE 3\n')
        flags = ["-I", str(early), "-I", str(late)]
        self.assertEqual(self.build(flags).returncode, 0)
        self.assertEqual(self.result(), 3)
        (early / "value.h").write_text('#define VALUE 7\n')
        self.assertEqual(self.build(flags).returncode, 0)
        self.assertEqual(self.result(), 7)

    def test_include_environment_is_part_of_build(self):
        dirs = [self.root / "a", self.root / "b"]
        self.src.write_text('#include <value.h>\nint main(void){return VALUE;}\n')
        for directory, value in zip(dirs, [3, 7]):
            directory.mkdir()
            (directory / "value.h").write_text('#define VALUE %d\n' % value)
            env = dict(self.env, CPATH=str(directory))
            self.assertEqual(self.build(env=env).returncode, 0)
            self.assertEqual(self.result(), value)

    def test_library_replacement_is_not_cached(self):
        ar = shutil.which("ar")
        if not ar:
            self.skipTest("ar is required for the static-library fixture")
        self.src.write_text('extern int value(void);\nint main(void){return value();}\n')
        source, obj, library = [self.root / n for n in ("value.c", "value.o", "libvalue.a")]
        for value in (3, 7):
            source.write_text('int value(void){return %d;}\n' % value)
            subprocess.run([CC, "-c", str(source), "-o", str(obj)], check=True, timeout=30)
            subprocess.run([ar, "rcs", str(library), str(obj)], check=True, timeout=30)
            self.assertEqual(self.build([str(library)]).returncode, 0)
            self.assertEqual(self.result(), value)

    def test_unknown_flags_bypass_key(self):
        self.src.write_text('int main(void){return 0;}\n')
        for extra in (["@flags.rsp"], ["-fplugin=plugin.so"], ["-Wl,-T,link.ld"]):
            args = [CC, str(self.src), "-o", str(self.exe), *extra]
            self.assertIsNone(tc.cache_key(str(self.src), args, str(self.exe)))

    @unittest.skipIf(os.name == "nt", "POSIX compiler wrapper fixture")
    def test_compiler_replacement_at_same_path(self):
        import shlex
        wrapper = self.root / "cc"
        self.src.write_text('int main(void){return VALUE;}\n')
        for value in (3, 7):
            wrapper.write_text('#!/bin/sh\nexec %s -DVALUE=%d "$@"\n' % (shlex.quote(CC), value))
            wrapper.chmod(0o700)
            self.assertEqual(self.build(compiler=str(wrapper)).returncode, 0)
            self.assertEqual(self.result(), value)

    def test_preprocessor_failure_falls_back_to_compile(self):
        self.src.write_text('int main(void){return 3;}\n')
        original = tc._run_cc
        def run(args, timeout, env, **options):
            if "-E" in args:
                return subprocess.CompletedProcess(args, 1, b"", b"preprocessor failed")
            return original(args, timeout, env, **options)
        with patch.object(tc, "_run_cc", side_effect=run):
            self.assertEqual(self.build().returncode, 0)
        self.assertEqual(self.result(), 3)
        self.assertFalse(self.cache.exists())

    def test_concurrent_stores_leave_no_temporary_entries(self):
        from concurrent.futures import ThreadPoolExecutor
        self.src.write_text('int main(void){return 3;}\n')
        def build(index):
            target = str(self.root / ("binary-%d" % index))
            args = [CC, str(self.src), "-o", target]
            return tc.cc_cached(args, str(self.src), target, timeout=30).returncode
        with ThreadPoolExecutor(max_workers=4) as pool:
            self.assertEqual(list(pool.map(build, range(4))), [0, 0, 0, 0])
        self.assertEqual(len(list(self.cache.iterdir())), 1)

    def test_header_edit_between_hash_and_codegen(self):
        header = self.root / "value.h"
        header.write_text('#define VALUE 3\n')
        self.src.write_text('#include "value.h"\nint main(void){return VALUE;}\n')
        original = tc._run_cc
        def run(args, timeout, env, **options):
            result = original(args, timeout, env, **options)
            if "-E" in args:
                header.write_text('#define VALUE 7\n')
            return result
        with patch.object(tc, "_run_cc", side_effect=run):
            self.assertEqual(self.build().returncode, 0)
        self.assertEqual(self.result(), 3)
        self.assertEqual(self.build().returncode, 0)
        self.assertEqual(self.result(), 7)
        header.write_text('#define VALUE 3\n')
        self.assertEqual(self.build().returncode, 0)
        self.assertEqual(self.result(), 3)

    def test_preprocessed_bytes_are_not_text_decoded(self):
        self.src.write_bytes(b'int main(void){return (unsigned char)"' + bytes([255]) + b'"[0];}\n')
        self.assertEqual(self.build().returncode, 0)
        self.assertEqual(self.result(), 255)

    def test_forced_include_survives_snapshot_compilation(self):
        header = self.root / "value.h"
        header.write_text('#define VALUE 7\n')
        self.src.write_text('int main(void){return VALUE;}\n')
        self.assertEqual(self.build(["-include", str(header)]).returncode, 0)
        self.assertEqual(self.result(), 7)

    def test_explicit_bypass_does_not_preprocess_or_store(self):
        self.src.write_text('int main(void){return 3;}\n')
        env = dict(self.env, BRAT_CC_CACHE_DISABLE="1")
        with patch.object(tc, "_run_cc", wraps=tc._run_cc) as run:
            self.assertEqual(self.build(env=env).returncode, 0)
            self.assertEqual(len(run.call_args_list), 1)
            self.assertNotIn("-E", run.call_args_list[0].args[0])
        self.assertFalse(self.cache.exists())

    def test_preprocessor_diagnostics_are_kept_on_miss(self):
        self.src.write_text('#warning cache_warning\nint main(void){return 3;}\n')
        result = self.build()
        self.assertEqual(result.returncode, 0)
        self.assertIn("cache_warning", result.stderr)

    def test_output_cannot_replace_input_via_snapshot(self):
        content = 'int main(void){return 3;}\n'
        self.src.write_text(content)
        args = [CC, str(self.src), "-o", str(self.src)]
        self.assertIsNone(tc.cache_key(str(self.src), args, str(self.src)))
        self.assertNotEqual(tc.cc_cached(args, str(self.src), str(self.src)).returncode, 0)
        self.assertEqual(self.src.read_text(), content)

    def test_unavailable_cache_does_not_break_compilation(self):
        self.src.write_text('int main(void){return 3;}\n')
        self.cache.write_text("not a directory")
        self.assertEqual(self.build().returncode, 0)
        self.assertEqual(self.result(), 3)
        self.assertEqual(self.cache.read_text(), "not a directory")

    def test_warm_hit_does_not_invoke_codegen(self):
        self.src.write_text('int main(void){return 3;}\n')
        self.assertEqual(self.build().returncode, 0)
        with patch.object(tc, "_run_cc", wraps=tc._run_cc) as run:
            self.assertEqual(self.build().returncode, 0)
            self.assertEqual(len(run.call_args_list), 1)
            self.assertIn("-E", run.call_args_list[0].args[0])
        self.assertEqual(self.result(), 3)


if __name__ == "__main__":
    unittest.main()
