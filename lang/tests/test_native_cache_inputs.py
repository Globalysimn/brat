"""Native run and package C caches must observe changed compiler inputs."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

import _cxx
import _skip

CC = shutil.which("cc") or shutil.which("gcc")
if os.name == "nt" or not CC:
    _skip.skip("POSIX compiler-wrapper fixtures require a C compiler")
BIN = _cxx.build()


class NativeCacheInputs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="brat-native-cache-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.app = self.root / "main.brat"
        self.app.write_text('print("program")\n')
        self.env = dict(os.environ, BRAT_CACHE=str(self.root / "cache"))
        self.env.pop("BRAT_CC", None)

    def run_app(self, env=None, flags=()):
        result = subprocess.run([BIN, "run", str(self.app), *flags], cwd=self.root,
                                env=env or self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def wrapper(self, name, marker):
        header = self.root / (name + ".h")
        header.write_text('#include <stdio.h>\n'
                          '__attribute__((constructor)) static void cache_marker(void)'
                          '{puts("%s");}\n' % marker)
        wrapper = self.root / name
        wrapper.write_text('#!/bin/sh\nexec %s -include %s "$@"\n' %
                           (shlex.quote(CC), shlex.quote(str(header))))
        wrapper.chmod(0o700)
        return str(wrapper)

    def package(self):
        pkg = self.root / "pkg"
        pkg.mkdir()
        (pkg / "brat.pkg.json").write_text(json.dumps({"name": "pkg", "version": "1.0",
                                                       "c_sources": ["value.c"]}))
        (pkg / "value.c").write_text('#include "value.inc"\nlong long value(void){return VALUE;}\n')
        (pkg / "value.inc").write_text('#define VALUE 3\n')
        self.app = pkg / "main.brat"
        self.app.write_text('extern fn value() -> i64\nprint(value())\n')
        return pkg

    def test_compiler_change_does_not_reuse_program(self):
        first = dict(self.env, BRAT_CC=self.wrapper("first", "first"))
        second = dict(self.env, BRAT_CC=self.wrapper("second", "second"))
        self.assertEqual(self.run_app(first), "first\nprogram\n")
        self.assertEqual(self.run_app(second), "second\nprogram\n")

    def test_forced_header_edit_does_not_reuse_program(self):
        compiler = self.wrapper("compiler", "first")
        env = dict(self.env, BRAT_CC=compiler)
        self.assertEqual(self.run_app(env), "first\nprogram\n")
        self.wrapper("compiler", "second")
        self.assertEqual(self.run_app(env), "second\nprogram\n")

    def test_package_include_with_non_header_extension(self):
        pkg = self.package()
        self.assertEqual(self.run_app(flags=["--unsafe-ffi"]), "3\n")
        (pkg / "value.inc").write_text('#define VALUE 7\n')
        self.assertEqual(self.run_app(flags=["--unsafe-ffi"]), "7\n")

    def test_deleted_package_include_is_not_hidden_by_cache(self):
        pkg = self.package()
        self.assertEqual(self.run_app(flags=["--unsafe-ffi"]), "3\n")
        (pkg / "value.inc").unlink()
        result = subprocess.run([BIN, "run", str(self.app), "--unsafe-ffi"], cwd=self.root,
                                env=self.env, capture_output=True, text=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)

    def test_package_include_environment_changes(self):
        pkg = self.package()
        (pkg / "value.c").write_text('#include <external.inc>\nlong long value(void){return VALUE;}\n')
        for name, value in (("first", 3), ("second", 7)):
            directory = self.root / name
            directory.mkdir()
            (directory / "external.inc").write_text('#define VALUE %d\n' % value)
            env = dict(self.env, CPATH=str(directory))
            self.assertEqual(self.run_app(env, ["--unsafe-ffi"]), "%d\n" % value)

    def test_path_compiler_header_edit(self):
        directory = self.root / "bin"
        directory.mkdir()
        self.wrapper("bin/cc", "first")
        env = dict(self.env, PATH=str(directory) + os.pathsep + self.env.get("PATH", ""))
        self.assertEqual(self.run_app(env), "first\nprogram\n")
        self.wrapper("bin/cc", "second")
        self.assertEqual(self.run_app(env), "second\nprogram\n")

    def test_external_package_header_changes_at_same_path(self):
        pkg = self.package()
        (pkg / "value.c").write_text('#include <external.inc>\nlong long value(void){return VALUE;}\n')
        directory = self.root / "include"
        directory.mkdir()
        env = dict(self.env, CPATH=str(directory))
        for value in (3, 7):
            (directory / "external.inc").write_text('#define VALUE %d\n' % value)
            self.assertEqual(self.run_app(env, ["--unsafe-ffi"]), "%d\n" % value)

    def test_package_compiler_macro_change(self):
        pkg = self.package()
        (pkg / "value.c").write_text('long long value(void){return VALUE;}\n')
        wrapper = self.root / "compiler"
        env = dict(self.env, BRAT_CC=str(wrapper))
        for value in (3, 7):
            wrapper.write_text('#!/bin/sh\nexec %s -DVALUE=%d "$@"\n' % (shlex.quote(CC), value))
            wrapper.chmod(0o700)
            self.assertEqual(self.run_app(env, ["--unsafe-ffi"]), "%d\n" % value)

    def static_library(self, value):
        ar = shutil.which("ar")
        if not ar:
            self.skipTest("ar is required")
        source, obj, library = [self.root / name for name in ("value.c", "value.o", "libvalue.a")]
        source.write_text('long long value(void){return %d;}\n' % value)
        subprocess.run([CC, "-c", str(source), "-o", str(obj)], check=True, timeout=30)
        subprocess.run([ar, "rcs", str(library), str(obj)], check=True, timeout=30)
        self.app.write_text('extern "%s" fn value() -> i64\nprint(value())\n' % library)
        return library

    def test_static_library_replacement(self):
        self.static_library(3)
        self.assertEqual(self.run_app(flags=["--unsafe-ffi"]), "3\n")
        self.static_library(7)
        self.assertEqual(self.run_app(flags=["--unsafe-ffi"]), "7\n")

    def test_missing_static_library_cannot_hit_cache(self):
        library = self.static_library(3)
        self.assertEqual(self.run_app(flags=["--unsafe-ffi"]), "3\n")
        library.unlink()
        result = subprocess.run([BIN, "run", str(self.app), "--unsafe-ffi"], cwd=self.root,
                                env=self.env, capture_output=True, text=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)

    def test_package_warm_objects_are_reused(self):
        self.package()
        self.assertEqual(self.run_app(flags=["--unsafe-ffi"]), "3\n")
        objects = list((self.root / "cache" / "c_obj").glob("*.o"))
        self.assertEqual(len(objects), 1)
        before = objects[0].stat().st_mtime_ns
        self.assertEqual(self.run_app(flags=["--unsafe-ffi"]), "3\n")
        self.assertEqual(objects[0].stat().st_mtime_ns, before)

    def test_explicit_bypass_rebuilds_program(self):
        env = dict(self.env, BRAT_NATIVE_CACHE_DISABLE="1")
        self.assertEqual(self.run_app(env), "program\n")
        binaries = [p for p in (self.root / "cache" / "run").iterdir()
                    if p.is_file() and "." not in p.name]
        self.assertEqual(len(binaries), 1)
        before = binaries[0].stat().st_mtime_ns
        self.assertEqual(self.run_app(env), "program\n")
        self.assertNotEqual(binaries[0].stat().st_mtime_ns, before)

    def test_default_warm_run_does_not_recompile(self):
        self.assertEqual(self.run_app(), "program\n")
        entries = list((self.root / "cache" / "run").iterdir())
        binaries = [p for p in entries if p.is_file() and "." not in p.name]
        self.assertEqual(len(binaries), 1)
        before = binaries[0].stat().st_mtime_ns
        self.assertEqual(self.run_app(), "program\n")
        self.assertEqual(binaries[0].stat().st_mtime_ns, before)


if __name__ == "__main__":
    unittest.main()
