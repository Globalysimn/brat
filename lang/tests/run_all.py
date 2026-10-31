"""Run every lang/tests/test_*.py once, with per-file timing.

Sets BRAT_NESTED=0 so test_lang.py does not re-run edge tests that this runner
already executes on their own.

Usage: python3 lang/tests/run_all.py [-k substring] [-j N]
`-j N` runs N files at once, default one per core; the files own their temp
dirs and ports, so they do not collide. Exit status: 0 when every file passes,
1 otherwise.

A file that exits 77 (`_skip.SKIP`) could not run here — no C compiler on the
PATH, say. It is counted as `skipped`, printed as SKIP with its reason, and it
is neither `ok` nor `FAILED`: a suite that quietly passed by not running is the
thing this line exists to prevent.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP = 77


def one(path, env):
    t0 = time.perf_counter()
    p = subprocess.run([sys.executable, path], capture_output=True, text=True,
                       timeout=1800, env=env)
    return path, time.perf_counter() - t0, p.returncode, p.stdout + p.stderr


def main():
    only = sys.argv[sys.argv.index('-k') + 1] if '-k' in sys.argv else None
    jobs = (int(sys.argv[sys.argv.index('-j') + 1]) if '-j' in sys.argv
            else os.cpu_count() or 1)
    env = dict(os.environ, BRAT_NESTED='0')
    files = sorted(glob.glob(os.path.join(HERE, 'test_*.py')))
    if only:
        files = [f for f in files if only in os.path.basename(f)]
    failed, skipped, t_all = [], [], time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        for path, dt, rc, out in pool.map(lambda f: one(f, env), files):
            name = os.path.basename(path)
            why = ''
            if rc == SKIP:
                skipped.append(name)
                why = next((ln.split(':', 1)[1].strip() for ln in out.splitlines()
                            if ln.startswith('SKIPPED:')), '')
            elif rc != 0:
                failed.append(name)
                sys.stdout.write('FAIL %s\n%s\n' % (name, out[-1200:]))
            status = 'SKIP' if rc == SKIP else ('ok' if rc == 0 else 'FAILED')
            sys.stdout.write('%6.1fs  %-28s %s%s\n'
                             % (dt, name, status, ('  ' + why) if why else ''))
            sys.stdout.flush()
    print('-' * 50)
    print('%d files, %d failed, %d skipped, %.1fs total (jobs=%d)'
          % (len(files), len(failed), len(skipped), time.perf_counter() - t_all, jobs))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
