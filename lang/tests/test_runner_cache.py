"""Regression tests for runner failures and cache pruning boundaries."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import signal
import time
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load("audit_runner", HERE / "run_all.py")
toolchain = load("audit_toolchain", HERE / "_toolchain.py")


class RunnerTests(unittest.TestCase):
    def test_empty_selection_is_not_success(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as exc:
                runner.main(["-k", "__no_such_test__"])
        self.assertEqual(exc.exception.code, 2)

    def test_invalid_arguments(self):
        for args in (["-j", "0"], ["-j", "-1"], ["-j"], ["--timeout", "nan"],
                     ["--timeout", "inf"], ["--timeout", "0"]):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as exc:
                    runner.main(args)
                self.assertEqual(exc.exception.code, 2)

    def test_timeout_is_a_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "slow.py"
            path.write_text("import time\nprint('started', flush=True)\ntime.sleep(60)\n")
            _, elapsed, rc, output = runner.one(str(path), dict(os.environ), timeout=0.5)
        self.assertEqual(rc, 124)
        self.assertLess(elapsed, 10)
        self.assertIn("started", output)
        self.assertIn("TIMEOUT", output)

    @unittest.skipIf(os.name == "nt", "POSIX process-group contract")
    def test_timeout_kills_child_holding_pipe(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "parent.py"
            path.write_text("import subprocess, sys, time\n"
                            "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
                            "print('child started', flush=True)\ntime.sleep(60)\n")
            _, elapsed, rc, output = runner.one(str(path), dict(os.environ), timeout=0.5)
        self.assertEqual(rc, 124)
        self.assertLess(elapsed, 10)
        self.assertIn("child started", output)

    @unittest.skipIf(os.name == "nt", "POSIX signal contract")
    def test_runner_cancellation_reaps_active_test(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / "pid"
            Path(tmp, "test_slow.py").write_text(
                "import os, time\nfrom pathlib import Path\n"
                "Path(%r).write_text(str(os.getpid()))\ntime.sleep(60)\n" % str(marker))
            wrapper = ("import sys; sys.path.insert(0, %r); import run_all; "
                       "run_all.HERE = %r; sys.exit(run_all.main(['-j', '1']))"
                       % (str(HERE), tmp))
            proc = subprocess.Popen([sys.executable, "-c", wrapper],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            child = None
            try:
                deadline = time.monotonic() + 10
                while not marker.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(marker.exists(), "test did not start")
                child = int(marker.read_text())
                proc.send_signal(signal.SIGTERM)
                proc.communicate(timeout=10)
                self.assertEqual(proc.returncode, 128 + signal.SIGTERM)
                with self.assertRaises(ProcessLookupError):
                    os.kill(child, 0)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate()
                if child is not None:
                    try:
                        os.killpg(child, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_invalid_utf8_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bytes.py"
            path.write_text("import os\nos.write(1, b'\\xff')\n")
            _, _, rc, output = runner.one(str(path), dict(os.environ))
        self.assertEqual(rc, 0)
        self.assertEqual(output, r"\xff")

    def test_spawn_failure_is_a_result(self):
        with patch.object(runner.subprocess, "Popen", side_effect=OSError("unavailable")):
            _, _, rc, output = runner.one("unused.py", {})
        self.assertEqual(rc, 1)
        self.assertIn("unavailable", output)

    def test_failure_does_not_hide_later_results(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "test_a.py").write_text("raise SystemExit(1)\n")
            Path(tmp, "test_b.py").write_text("print('ok')\n")
            stream = io.StringIO()
            with patch.object(runner, "HERE", tmp), contextlib.redirect_stdout(stream):
                rc = runner.main(["-j", "2"])
        self.assertEqual(rc, 1)
        self.assertIn("test_b.py", stream.getvalue())
        self.assertIn("2 files, 1 failed", stream.getvalue())


class CacheTests(unittest.TestCase):
    def test_pruning_preserves_unrelated_files_and_symlinks(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "cache"
            cache.mkdir()
            unrelated = cache / "important.txt"
            unrelated.write_bytes(b"keep me")
            entry = cache / ("a" * 64)
            entry.write_bytes(b"cached binary")
            pending = cache / (("b" * 64) + ".tmp123")
            pending.write_bytes(b"being written")
            directory = cache / ("d" * 64)
            directory.mkdir()
            link = cache / ("c" * 64)
            if os.name != "nt":
                link.symlink_to(unrelated)
            with patch.dict(os.environ, {"BRAT_CACHE": str(cache)}):
                self.assertEqual(toolchain.prune_cache(limit=0), len(b"cached binary"))
            self.assertFalse(entry.exists())
            self.assertEqual(unrelated.read_bytes(), b"keep me")
            self.assertTrue(pending.exists())
            self.assertTrue(directory.is_dir())
            if os.name != "nt":
                self.assertTrue(link.is_symlink())


if __name__ == "__main__":
    unittest.main()
