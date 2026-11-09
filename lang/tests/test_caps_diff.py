"""Capability lattice regression sweep across the corpus, C++ borrow fixtures,
and ownership tables. Runs natively against `brat-cxx check` by default.
"""
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
sys.path.insert(0, HERE)

import _cxx      # noqa: E402
import _scratch  # noqa: E402

CXX = _cxx.build()

EXTRA = {
    ("own_paths_bad.brat", 5, "use of moved `p` (hint: the value was moved; use a copy or re-bind)"),
}


def verdicts(text):
    """[(file, line, message)] from one `check` run."""
    out = []
    for raw in text.splitlines():
        if ": error:" not in raw and ": warning:" not in raw:
            continue
        head, _, rest = raw.partition(": ")
        path, _, line = head.rpartition(":")
        msg = rest.split(": ", 1)[1] if rest.startswith(("error: ", "warning: ")) else rest
        out.append((os.path.basename(path), int(line) if line.isdigit() else 0, msg))
    return out


def tables_without_running(filename):
    path = os.path.join(HERE, filename)
    with open(path, encoding="utf-8") as f:
        src = f.read()
    lines = src.rstrip().split("\n")
    if lines and lines[-1].startswith("main()"):
        src = "\n".join(lines[:-1])
    ns = {"__name__": "fixtures", "__file__": path}
    exec(compile(src, path, "exec"), ns)
    return ns


def sources(tmp):
    for path in sorted(glob.glob(os.path.join(HERE, "..", "examples", "*.brat"))) + \
            sorted(glob.glob(os.path.join(HERE, "fixtures", "*.brat"))):
        with open(path, encoding="utf-8") as f:
            yield os.path.basename(path), f.read(), os.path.abspath(path), None

    for name, src in tables_without_running("test_cxx_borrow.py")["OWNERSHIP"].items():
        yield name, src, os.path.join(tmp, name), None

    own = tables_without_running("test_ownership.py")
    for want_ok, table in ((True, own["ACCEPT"]), (False, own["REJECT"])):
        for entry in table:
            case_name, files = entry[0], entry[1]
            root = os.path.join(tmp, case_name)
            for inner, src in files.items():
                p = os.path.join(root, inner)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w", encoding="utf-8") as f:
                    f.write(src)
            first_rel = next(iter(files.keys()))
            yield "%s/%s" % (case_name, first_rel), files[first_rel], os.path.join(root, first_rel), want_ok


def main():
    tmp = _scratch.mkdtemp(prefix="caps-diff")
    bad = []
    checked = 0
    saw_extra = False


    for name, src, path, expect_accept in sources(tmp):
        if not os.path.exists(path):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(src)
        ast_probe = subprocess.run([CXX, "ast", path], capture_output=True, text=True)
        if ast_probe.returncode != 0 and expect_accept is None:
            continue
        checked += 1

        cx_run = subprocess.run([CXX, "check", path], capture_output=True, text=True)
        cx_all = cx_run.stdout + "\n" + cx_run.stderr
        cx = {(ln, msg) for _f, ln, msg in verdicts(cx_all)}

        if name == "own_paths_bad.brat" and (5, "use of moved `p` (hint: the value was moved; use a copy or re-bind)") in cx:
            saw_extra = True
        if expect_accept is True and cx_run.returncode != 0:
            bad.append("ACCEPT REJECTED  %s  %r" % (name, cx_all[-200:]))
        if expect_accept is False and cx_run.returncode == 0:
            bad.append("REJECT ACCEPTED  %s" % name)

    if not saw_extra:
        bad.append("MISSING EXTRA  own_paths_bad.brat:5")

    shutil.rmtree(tmp, ignore_errors=True)
    if bad:
        for line in bad:
            print(line)
        print("\nCAPS DIFF FAILED (%d problems, %d programs)" % (len(bad), checked))
        sys.exit(1)
    print("CAPS DIFF ALL GREEN (%d programs, %d reviewed addition)" % (checked, len(EXTRA)))


if __name__ == "__main__":
    main()
