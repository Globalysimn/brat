"""Native module contracts."""

import _cxx
import _scratch

import os
import subprocess
import time

HERE = os.path.dirname(os.path.realpath(__file__))
ENGINE = os.path.realpath(os.path.join(HERE, "..", "engine"))
ROOT = os.path.realpath(os.path.join(HERE, ".."))
BIN = _cxx.BIN
TMP = _scratch.mkdtemp(prefix="brat-modules-")
def corpus():
    out = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in ("vendor", "bin", ".git", "__pycache__")]
        out.extend(os.path.join(base, name) for name in files if name.endswith(".brat"))
    return sorted(out)


def flat(path):
    result = subprocess.run([BIN, "flat", path], capture_output=True, text=True, encoding="utf-8", timeout=120)
    return result.stdout, result.stderr, result.returncode


def declaration(name, expression, public=False):
    node = ("fn", name, [], [("return", expression)], [], "i64")
    return ("pub", node) if public else node


ANSWER = "pub fn answer() -> i64 { return 42 }\n"
# These trees are specified directly, not recorded from either loader.
ACCEPTED = {
    "standalone": (
        {"app.brat": "fn main() -> i64 { return 1 }\n"},
        [declaration("main", ("num", 1))]),
    "qualified": (
        {"app.brat": "import util\nfn main() -> i64 { return util.answer() }\n",
         "util.brat": "pub fn answer() -> i64 { return helper() }\nfn helper() -> i64 { return 42 }\n"},
        [declaration("util__answer", ("call", "util__helper", []), True),
         declaration("util__helper", ("num", 42)),
         declaration("main", ("call", "util__answer", []))]),
    "duplicate": (
        {"app.brat": "print(0)\nimport util\nprint(util.answer())\nimport util\nprint(util.answer())\n",
         "util.brat": ANSWER},
        [("expr", ("call", "print", [("num", 0)])),
         declaration("util__answer", ("num", 42), True),
         ("expr", ("call", "print", [("call", "util__answer", [])])),
         ("expr", ("call", "print", [("call", "util__answer", [])]))]),
    "diamond": (
        {"app.brat": "import left\nimport right\nfn main() -> i64 { return right.answer() }\n",
         "left.brat": "import util\npub use util.answer\n",
         "right.brat": "import util\npub use util.answer\n",
         "util.brat": ANSWER},
        [declaration("util__answer", ("num", 42), True),
         declaration("main", ("call", "util__answer", []))]),
    "nested": (
        {"app.brat": "import pkg.util\nfn main() -> i64 { return pkg.util.answer() + util.answer() }\n",
         "pkg/util.brat": ANSWER},
        [declaration("pkg__util__answer", ("num", 42), True),
         declaration("main", ("bin", "+", ("call", "pkg__util__answer", []),
                              ("call", "pkg__util__answer", [])))]),
    "cycle": (
        {"app.brat": "import left\nfn main() -> i64 { return left.a() }\n",
         "left.brat": "import right\npub fn a() -> i64 { return right.b() + 1 }\n",
         "right.brat": "import left\npub fn b() -> i64 { return 1 }\n"},
        [declaration("right__b", ("num", 1), True),
         declaration("left__a", ("bin", "+", ("call", "right__b", []), ("num", 1)), True),
         declaration("main", ("call", "left__a", []))]),
    "shadow": (
        {"app.brat": "import util\nfn answer() -> i64 { return 7 }\nfn main() -> i64 { return answer() }\n",
         "util.brat": ANSWER},
        [declaration("util__answer", ("num", 42), True),
         declaration("answer", ("num", 7)), declaration("main", ("call", "answer", []))]),
    "interface": (
        {"app.brat": "import util\nfn main() -> i64 { return util.answer() }\n",
         "util.brat": "fn answer() -> i64 { return 42 }\n",
         "util.bri": "fn answer() -> i64\n"},
        [declaration("util__answer", ("num", 42)),
         declaration("main", ("call", "util__answer", []))]),
}

REJECTED = {
    "private": (
        {"app.brat": "import util\nprint(util.secret())\n",
         "util.brat": "fn secret() -> i64 { return 1 }\n"},
        ("function `secret` is private in module `util`",)),
    "missing_function": (
        {"app.brat": "import util\nprint(util.missing())\n", "util.brat": ANSWER},
        ("module `util` has no function `missing`",)),
    "missing_module": (
        {"app.brat": "import absent_fixture_module\n"},
        ("no module absent_fixture_module",)),
    "ambiguous": (
        {"app.brat": "import left\nimport right\nprint(answer())\n",
         "left.brat": ANSWER, "right.brat": ANSWER},
        ("ambiguous unqualified name `answer`", "left.brat", "right.brat", "qualify the call")),
    "reexport": (
        {"app.brat": "import facade\nprint(facade.answer())\n",
         "facade.brat": "import left\nimport right\npub use left.answer\npub use right.answer\n",
         "left.brat": ANSWER, "right.brat": ANSWER},
        ("`answer` is re-exported from two modules: `left` and `right`",)),
    "interface": (
        {"app.brat": "import util\nprint(util.answer())\n", "util.brat": ANSWER,
         "util.bri": "fn answer(x: i64) -> i64\n"},
        ("does not match its interface", "`answer` takes 0 parameter(s) in the implementation, 1 in the interface")),
}
REFUSED = {"examples/own_priv.brat": "function `helper` is private in module `modapi`"}


def write_case(name, sources):
    directory = os.path.join(TMP, name)
    for relative, source in sources.items():
        path = os.path.join(directory, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as stream:
            stream.write(source)
    return os.path.join(directory, "app.brat")


def assert_refused(path, messages):
    out, err, rc = flat(path)
    assert rc == 2 and out.startswith(path + ": load error ") and not err, (path, rc, out, err)
    assert all(message in out for message in messages), (path, messages, out)


def main():
    _cxx.build()
    start = time.perf_counter()
    for name, (sources, tree) in ACCEPTED.items():
        path = write_case("accepted-" + name, sources)
        out, err, rc = flat(path)
        expected = _cxx.sx(("prog", tree)) + "\n"
        assert rc == 0 and not err and out == expected, (name, rc, expected, out, err)
    for name, (sources, messages) in REJECTED.items():
        assert_refused(write_case("rejected-" + name, sources), messages)

    files = corpus()
    assert files, "no source corpus"
    assert set(REFUSED) <= {os.path.relpath(path, ROOT).replace(os.sep, "/") for path in files}
    for path in files:
        name = os.path.relpath(path, ROOT).replace(os.sep, "/")
        if name in REFUSED:
            assert_refused(path, (REFUSED[name],))
            continue
        out, err, rc = flat(path)
        assert rc == 0 and not err, (path, rc, out, err)
        assert out.startswith('("prog" [') and out.endswith("])\n"), (path, out)
        assert '("import" ' not in out and '("pubuse" ' not in out, path
        assert flat(path) == (out, err, rc), "non-deterministic flattening: " + path
    print("CXX MODULES ALL GREEN (%d trees, %d refusals, %d corpus files, %.2fs)"
          % (len(ACCEPTED), len(REJECTED), len(files), time.perf_counter() - start))


if __name__ == "__main__":
    main()
