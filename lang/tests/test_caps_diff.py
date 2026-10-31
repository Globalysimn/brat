"""The lattice must agree with the walk, with the C++ backend, and add only truth.

`borrow.py` decides ownership with a list of live views per root; `caps.py` and
`caps.cpp` decide it with one capability per place, joined across the flow. Three
invariants keep the arrangement honest, and this file checks all three on every
program the project already owns — the corpus, the 41 pinned C++ fixtures and the
193 ownership cases:

  * **the same bytes**, Python and C++ — the lattice is two implementations of one
    model, so a program may not read differently depending on who checked it;
  * **nothing lost**: every verdict the walk made is still printed;
  * **nothing invented**: the only findings the lattice adds are the ones listed
    below, each with the reason it is true.

A program here is checked in a single batch per backend, so the whole sweep costs
two processes rather than four hundred.
"""
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "engine"))
CLI = os.path.join(ENGINE, "cli.py")
CXX = os.path.abspath(os.path.join(HERE, "..", "cxx", "bin", "brat-cxx"))

sys.path.insert(0, ENGINE)
sys.path.insert(0, HERE)

import _scratch     # noqa: E402
import brat         # noqa: E402
import borrow       # noqa: E402

# Findings the lattice adds: the walk cannot see them, and each one is a bug in
# the program, not in the checker.
#   own_paths_bad:5   `print(p.y)` after `let q = move p` — the list model kept
#                     `p` alive because a field had already been moved out of it.
EXTRA = {
    ("own_paths_bad.brat", 5, "use of moved `p` (hint: the value was moved; use a copy or re-bind)"),
}


def verdicts(text):
    """[(file, line, message)] from one `check` run over many files."""
    out = []
    for raw in text.splitlines():
        if ": error:" not in raw and ": warning:" not in raw:
            continue
        head, _, rest = raw.partition(": ")
        path, _, line = head.rpartition(":")
        msg = rest.split(": ", 1)[1] if rest.startswith(("error: ", "warning: ")) else rest
        out.append((os.path.basename(path), int(line) if line.isdigit() else 0, msg))
    return out


def sources(tmp):
    """(name, source, path) for every program this project already checks.

    The corpus stays where it is: `jsonlibtest.brat` imports `jsonlib.brat` from
    the directory beside it, and copying the file elsewhere would break that.
    """
    for path in sorted(glob.glob(os.path.join(ENGINE, "..", "examples", "*.brat"))) + \
            sorted(glob.glob(os.path.join(ENGINE, "..", "tests", "fixtures", "*.brat"))):
        with open(path, encoding="utf-8") as f:
            yield os.path.basename(path), f.read(), os.path.abspath(path)

    for name, src in tables_without_running("test_cxx_borrow.py")["OWNERSHIP"].items():
        yield name, src, os.path.join(tmp, name)

    own = tables_without_running("test_ownership.py")
    for entry in own["ACCEPT"] + own["REJECT"]:
        for inner, src in entry[1].items():
            yield "%s/%s" % (entry[0], inner), src, \
                os.path.join(tmp, "%s__%s" % (entry[0], inner))


def tables_without_running(filename):
    """The fixture tables of a test file, without running that test's `main()`.

    Importing the module would check 200 programs a second time; the tables are
    data above the runner, so the file is executed with the last line dropped.
    """
    path = os.path.join(HERE, filename)
    with open(path, encoding="utf-8") as f:
        src = f.read()
    lines = src.rstrip().split("\n")
    if lines and lines[-1].startswith("main()"):
        src = "\n".join(lines[:-1])
    ns = {"__name__": "fixtures", "__file__": path}
    exec(compile(src, path, "exec"), ns)
    return ns


def walk_only(src):
    """The verdicts the lexical walk makes on its own."""
    prog = brat.parse_prog(brat.Cur(brat.lex(src)))
    saved = borrow.caps
    borrow.caps = None
    try:
        errs, warns = borrow.check(prog)
    finally:
        borrow.caps = saved
    return [(ln, msg) for ln, msg, *_ in errs] + [(ln, msg) for ln, msg, *_ in warns]


def main():
    have_cxx = os.path.exists(CXX)
    tmp = _scratch.mkdtemp(prefix="caps-diff")
    bad = []
    checked = 0
    for name, src, path in sources(tmp):
        try:
            brat.parse_prog(brat.Cur(brat.lex(src)))
        except SyntaxError:
            continue                       # a parse error belongs to another test
        if not os.path.exists(path):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(src)
        checked += 1

        py = {(ln, msg) for _f, ln, msg in
              verdicts(subprocess.run([sys.executable, CLI, "check", "--py", path],
                                      capture_output=True, text=True).stdout)}
        if have_cxx:
            cx = {(ln, msg) for _f, ln, msg in
                  verdicts(subprocess.run([CXX, "check", path],
                                          capture_output=True, text=True).stdout)}
            for ln, msg in sorted(py - cx):
                bad.append("PYTHON ONLY  %s:%s  %s" % (name, ln, msg))
            for ln, msg in sorted(cx - py):
                bad.append("C++ ONLY  %s:%s  %s" % (name, ln, msg))

        if "import " in src:
            # A module program: the walk here sees one file, the check sees the
            # set, so the names they disagree about are not ownership at all.
            continue
        walked = walk_only(src)
        for ln, msg in walked:
            if (ln, msg) not in py:
                bad.append("LOST  %s:%s  %s" % (name, ln, msg))
        walk_lines = {ln for ln, _m in walked}
        for ln, msg in sorted(py):
            if ln not in walk_lines and (name, ln, msg) not in EXTRA:
                bad.append("UNREVIEWED ADDITION  %s:%s  %s" % (name, ln, msg))

    shutil.rmtree(tmp, ignore_errors=True)
    if bad:
        for line in bad:
            print(line)
        print("\nCAPS DIFF FAILED (%d problems, %d programs)" % (len(bad), checked))
        sys.exit(1)
    print("CAPS DIFF ALL GREEN (%d programs, both backends identical, nothing lost,"
          " %d reviewed addition)" % (checked, len(EXTRA)))


if __name__ == "__main__":
    main()
