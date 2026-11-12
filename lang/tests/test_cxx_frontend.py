"""Native parser contracts and AST-preserving formatting of the source corpus."""
import argparse
import os
import subprocess
import sys
import time

import _cxx
import _scratch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
ENGINE = os.path.join(ROOT, "engine")
TMP = _scratch.mkdtemp(prefix="brat-cxx-")

# Hand-written trees: precedence, associativity, declarations and nesting.
CONTRACTS = [
    ("", []),
    ("1 + 2 * 3", [("expr", ("bin", "+", ("num", 1), ("bin", "*", ("num", 2), ("num", 3))))]),
    ("8 - 3 - 1", [("expr", ("bin", "-", ("bin", "-", ("num", 8), ("num", 3)), ("num", 1)))]),
    ("-(1 + 2)", [("expr", ("un", "-", ("bin", "+", ("num", 1), ("num", 2))))]),
    ("true || false && false", [("expr", ("bin", "||", ("bool", True), ("bin", "&&", ("bool", False), ("bool", False))))]),
    ("let mut n: i64 = 1", [("let", "n", ("num", 1), True, False, "i64")]),
    ("xs[1].push(move x)", [("expr", ("mcall", ("idx", ("var", "xs"), ("num", 1)), "push", [("move", ("var", "x"))]))]),
    ("fn add(x: i64) -> i64 { return x + 1 }",
     [("fn", "add", [("x", "ref", "i64", None)], [("return", ("bin", "+", ("var", "x"), ("num", 1)))], [], "i64")]),
    ("for i in 0..3 { print(i) }",
     [("forrange", "i", ("num", 0), ("num", 3), [("expr", ("call", "print", [("var", "i")]))])]),
    ("let f = fn(x) { return x }",
     [("let", "f", ("closure", None, [("x", "ref", None, None)], [("return", ("var", "x"))]), False, False, None)]),
    ("defer cleanup()", [("defer", ("call", "cleanup", []))]),
    ('let values = [None, true, 1.5, "é"]',
     [("let", "values", ("vec", [("none",), ("bool", True), ("num", 1.5), ("str", "é")]), False, False, None)]),
    ('let m = {"a": 1}', [("let", "m", ("maplit", [(("str", "a"), ("num", 1))]), False, False, None)]),
    ("if true { return 1 } else { return 0 }",
     [("if", ("bool", True), [("return", ("num", 1))], [("return", ("num", 0))])]),
    ("while false { break }", [("while", ("bool", False), [("break",)])]),
    ("const LIMIT = 3", [("let", "LIMIT", ("num", 3), False, True, None)]),
]

BROKEN = {
    "unclosed.brat": ("fn main() -> i64 {\n  let x = (1 + 2\n  return 0\n}\n", 3, "want ')'"),
    "nobraces.brat": ("fn main() -> i64\n  return 0\n", 2, "unexpected end of file"),
    "eof.brat": ("let x = 1 +\n", 1, "unexpected end of file"),
    "noname.brat": ("fn () -> i64 { return 0 }\n", 1, "want '('"),
}


def corpus():
    out = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in ("vendor", "bin", ".git", "__pycache__")]
        out.extend(os.path.join(base, name) for name in files if name.endswith(".brat"))
    return sorted(out)


def invoke(binary, *args):
    return subprocess.run([binary, *args], capture_output=True, text=True, encoding="utf-8", timeout=120)


def dumps(binary, paths):
    result = invoke(binary, "ast", *paths)
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert len(lines) == len(paths), (len(lines), len(paths))
    return lines


def main():
    binary = _cxx.build()
    start = time.perf_counter()
    contract_paths = []
    for i, (source, _tree) in enumerate(CONTRACTS):
        path = os.path.join(TMP, "contract-%d.brat" % i)
        with open(path, "w", encoding="utf-8") as stream:
            stream.write(source + "\n")
        contract_paths.append(path)
    expected = [_cxx.sx(("prog", tree)) for _source, tree in CONTRACTS]
    actual = dumps(binary, contract_paths)
    for path, want, got in zip(contract_paths, expected, actual):
        assert got == want, "%s: want %s, got %s" % (path, want, got)

    files = corpus()
    assert files, "no source corpus"
    original = dumps(binary, files)
    formatted_paths = []
    for i, path in enumerate(files):
        formatted = invoke(binary, "fmt", "--stdout", path)
        assert formatted.returncode == 0, (path, formatted.stderr)
        target = os.path.join(TMP, "formatted-%d.brat" % i)
        with open(target, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(formatted.stdout)
        formatted_paths.append(target)
    canonical = dumps(binary, formatted_paths)
    for path, before, after in zip(files, original, canonical):
        assert before == after, "%s: formatting changed the AST" % path
    checked = invoke(binary, "fmt", "--check", *formatted_paths)
    assert checked.returncode == 0, checked.stderr

    for name, (source, line, message) in BROKEN.items():
        path = os.path.join(TMP, name)
        with open(path, "w", encoding="utf-8") as stream:
            stream.write(source)
        result = invoke(binary, "check", path)
        assert result.returncode == 1, (name, result.stdout, result.stderr)
        assert "%s:%d: error:" % (path, line) in result.stderr, (name, result.stderr)
        assert message in result.stderr, (name, result.stderr)

    print("CXX FRONTEND ALL GREEN (%d syntax contracts, %d source round trips, %d refusals, %.2fs)"
          % (len(CONTRACTS), len(files), len(BROKEN), time.perf_counter() - start))


if __name__ == "__main__":
    main()
