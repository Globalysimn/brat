"""Run documented examples against their expected output and diagnostics."""
import _cxx
import _scratch

import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-docs-")

DOCS = ["README.md", "docs/guide/tour.md", "docs/guide/ownership.md", "docs/guide/limits.md"]
FENCE = re.compile(r"```brat (run|check)\n(.*?)```\n\n```(output|diagnostics)\n(.*?)```", re.S)
PAIRING = {"run": "output", "check": "diagnostics"}


def execute(command, cwd):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=300,
                          env=dict(os.environ, BRAT_NO_NATIVE_CLI="1"))


def main():
    count = 0
    for doc in DOCS:
        path = os.path.join(ROOT, doc)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for kind, program, tag, expected in FENCE.findall(text):
            assert PAIRING[kind] == tag, "%s: a `brat %s` fence is followed by `%s`" % (doc, kind, tag)
            where = "%s, example %d" % (doc, count + 1)
            cwd = os.path.join(TMP, "e%d" % count)
            os.makedirs(cwd)
            with open(os.path.join(cwd, "example.brat"), "w", encoding="utf-8") as f:
                f.write(program)
            runs = [("native", [NATIVE, kind, "example.brat"])]
            for label, command in runs:
                done = execute(command, cwd)
                want_rc = 0 if kind == "run" else 1
                assert done.returncode == want_rc, "%s: %s exited %d\n%s" % (where, label, done.returncode, done.stderr)
                got = done.stdout if kind == "run" else done.stdout + done.stderr
                # The checker prints the absolute path of the file; the page shows it as `example.brat`.
                for prefix in (cwd, os.path.realpath(cwd)):
                    got = got.replace(prefix + os.sep, "")
                assert got.strip() == expected.strip(), \
                    "%s: %s printed\n%s\nbut the document says\n%s" % (where, label, got, expected)
            count += 1

    assert count > 0, "no runnable example found in %s" % DOCS
    print("DOCS EXAMPLES ALL GREEN (%d programs)" % count)


if __name__ == "__main__":
    main()
