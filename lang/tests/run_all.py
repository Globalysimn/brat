"""Run test_*.py files with bounded concurrency and per-file timing.

Exit status: 0 when selected tests pass (skips are reported separately),
1 for test failures, and 2 for invalid arguments or an empty selection.
BRAT_NESTED=0 prevents test_lang.py from running the same edge tests twice.
"""
import argparse
import concurrent.futures
import contextlib
import threading
import glob
import math
import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP = 77
_ACTIVE = set()
_ACTIVE_LOCK = threading.Lock()
_CANCELLED = threading.Event()


def kill_test(proc):
    try:
        if os.name != "nt":
            os.killpg(proc.pid, signal.SIGKILL)
        else:
            proc.kill()
    except ProcessLookupError:
        pass


@contextlib.contextmanager
def cancellation_guard():
    # Test processes own sessions so deadlines can kill their descendants.
    # Forward runner cancellation explicitly; the shell's group no longer does.
    def cancel(signum, frame):
        _CANCELLED.set()
        with _ACTIVE_LOCK:
            for proc in _ACTIVE:
                kill_test(proc)
        raise SystemExit(128 + signum)

    previous = {sig: signal.signal(sig, cancel) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        yield
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        _CANCELLED.clear()


def one(path, env, timeout=1800):
    with _ACTIVE_LOCK:
        if _CANCELLED.is_set():
            return path, 0, 130, "Runner cancelled"
        return_start = time.perf_counter()
        try:
            proc = subprocess.Popen(
                [sys.executable, path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL, text=True, encoding="utf-8", errors="backslashreplace",
                env=env, start_new_session=(os.name != "nt"))
        except OSError as exc:
            return path, time.perf_counter() - return_start, 1, "Cannot start test: %s" % exc
        _ACTIVE.add(proc)
    try:
        return collect(proc, path, return_start, timeout)
    finally:
        with _ACTIVE_LOCK:
            _ACTIVE.discard(proc)


def collect(proc, path, t0, timeout):
    try:
        output, _ = proc.communicate(timeout=timeout)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        # Tests spawn compilers and programs. Killing only Python leaves those
        # processes running, sometimes still holding the output pipe open.
        kill_test(proc)
        # Bound the drain too: a descendant may have detached from the group.
        try:
            output, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired as exc:
            output = exc.output or b""
            if isinstance(output, bytes):
                output = output.decode("utf-8", "backslashreplace")
            proc.stdout.close()
            proc.wait()
        output += "\nTIMEOUT after %g seconds\n" % timeout
        rc = 124
    return path, time.perf_counter() - t0, rc, output


def positive_int(value):
    n = int(value)
    if n < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return n


def positive_timeout(value):
    n = float(value)
    if not math.isfinite(n) or n <= 0:
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return n


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-k", metavar="SUBSTRING", help="select test file names")
    parser.add_argument("-j", type=positive_int, default=os.cpu_count() or 1,
                        metavar="N", help="concurrent files (default: CPU count)")
    parser.add_argument("--timeout", type=positive_timeout, default=1800,
                        metavar="SECONDS", help="per-file deadline (default: 1800)")
    args = parser.parse_args(argv)
    files = sorted(glob.glob(os.path.join(HERE, "test_*.py")))
    if args.k:
        files = [f for f in files if args.k in os.path.basename(f)]
    if not files:
        parser.error("no test files matched")
    env = dict(os.environ, BRAT_NESTED="0")
    failed, skipped, t_all = [], [], time.perf_counter()
    with cancellation_guard(), concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as pool:
        pending = [pool.submit(one, f, env, args.timeout) for f in files]
        for future in concurrent.futures.as_completed(pending):
            path, dt, rc, out = future.result()
            name = os.path.basename(path)
            why = ""
            if rc == SKIP:
                skipped.append(name)
                why = next((ln.split(":", 1)[1].strip() for ln in out.splitlines()
                            if ln.startswith("SKIPPED:")), "")
            elif rc != 0:
                failed.append(name)
                sys.stdout.write("FAIL %s\n%s\n" % (name, out[-1200:]))
            status = "SKIP" if rc == SKIP else ("ok" if rc == 0 else "FAILED")
            sys.stdout.write("%6.1fs  %-28s %s%s\n"
                             % (dt, name, status, ("  " + why) if why else ""))
            sys.stdout.flush()
    print("-" * 50)
    print("%d files, %d failed, %d skipped, %.1fs total (jobs=%d)"
          % (len(files), len(failed), len(skipped), time.perf_counter() - t_all, args.j))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
