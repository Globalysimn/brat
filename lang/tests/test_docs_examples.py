"""Run documented examples, stdlib signatures, explain codes, and doc links."""
import _cxx
import _scratch

import glob
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-docs-")

DOCS = [
    "README.md",
    "docs/guide/tour.md",
    "docs/guide/by-example.md",
    "docs/guide/ownership.md",
    "docs/guide/ffi.md",
    "docs/guide/stdlib.md",
    "docs/guide/builtins.md",
    "docs/guide/limits.md",
    "docs/guide/faq.md",
]
FENCE = re.compile(r"```brat (run|check)\n(.*?)```\n\n```(output|diagnostics)\n(.*?)```", re.S)
PAIRING = {"run": "output", "check": "diagnostics"}


def execute(command, cwd):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=300,
                          env=dict(os.environ))


def verify_stdlib_signatures():
    stdlib_doc = os.path.join(ROOT, "docs", "guide", "stdlib.md")
    with open(stdlib_doc, encoding="utf-8") as f:
        doc_text = f.read()
    doc_sigs = set()
    cur_mod = None
    for chunk in re.split(r"(?m)^## `std\.([a-z0-9_]+)`", doc_text):
        if re.fullmatch(r"[a-z0-9_]+", chunk):
            cur_mod = chunk
            continue
        if not cur_mod:
            continue
        for block in re.findall(r"```brat\n(.*?)```", chunk, re.S):
            for line in block.splitlines():
                s = line.strip()
                if s.startswith("fn "):
                    doc_sigs.add((cur_mod, s))

    bri_sigs = set()
    for bri_path in sorted(glob.glob(os.path.join(ROOT, "lang", "std", "*.bri"))):
        mod = os.path.splitext(os.path.basename(bri_path))[0]
        with open(bri_path, encoding="utf-8") as f:
            for line in f:
                s = line.strip()
                if s.startswith("fn "):
                    assert (mod, s) in doc_sigs, "%s signature missing from stdlib.md: %s" % (
                        os.path.basename(bri_path), s
                    )
                    bri_sigs.add((mod, s))
    extra = doc_sigs - bri_sigs
    assert not extra, "stdlib.md lists signatures not in lang/std/*.bri: %s" % sorted(extra)


def verify_cli_explain_codes():
    cli_doc = os.path.join(ROOT, "docs", "guide", "cli.md")
    with open(cli_doc, encoding="utf-8") as f:
        text = f.read()
    codes = sorted(set(re.findall(r"\b(?:[BEPTFW]\d{4})\b", text)))
    assert len(codes) >= 19, "expected >=19 diagnostic codes in cli.md, found %d" % len(codes)
    for code in codes:
        done = execute([NATIVE, "explain", code], ROOT)
        assert done.returncode == 0, "brat explain %s failed: %s" % (code, done.stderr)


def verify_markdown_links():
    md_files = [os.path.join(ROOT, "README.md")]
    md_files.extend(sorted(glob.glob(os.path.join(ROOT, "docs", "**", "*.md"), recursive=True)))
    for md in md_files:
        base_dir = os.path.dirname(md)
        with open(md, encoding="utf-8") as f:
            text = f.read()
        for link in re.findall(r"\]\(([^)#]+)(?:#[^)]*)?\)", text):
            if link.startswith(("http://", "https://", "mailto:")):
                continue
            target = os.path.normpath(os.path.join(base_dir, link))
            assert os.path.exists(target), "%s links missing path: %s" % (
                os.path.relpath(md, ROOT), link
            )


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
                assert done.returncode == want_rc, "%s: %s exited %d\nstdout:\n%s\nstderr:\n%s" % (
                    where, label, done.returncode, done.stdout, done.stderr
                )
                got = done.stdout if kind == "run" else done.stdout + done.stderr
                for prefix in (cwd, os.path.realpath(cwd)):
                    got = got.replace(prefix + os.sep, "")
                assert got.strip() == expected.strip(), \
                    "%s: %s printed\n%s\nbut the document says\n%s" % (where, label, got, expected)
            count += 1

    assert count >= 30, "expected >=30 runnable doc examples, got %d" % count
    verify_stdlib_signatures()
    verify_cli_explain_codes()
    verify_markdown_links()
    print("DOCS EXAMPLES ALL GREEN (%d programs + stdlib + explain + links)" % count)


if __name__ == "__main__":
    main()
