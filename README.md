# BRAT

BRAT is an experimental programming language with Python-like syntax, explicit
`move`, `ref` and `mut` at function boundaries, a checker that rejects
use-after-move and aliasing mistakes, and a compiler that emits C. There is also
an interpreter, written in Python, that defines the behaviour the compiler is
tested against.

It is not production software. The sections [What does not work](#what-does-not-work)
and [How far to trust this](#how-far-to-trust-this) are as much a part of this
README as the examples.

## Why it exists

The question is whether Rust's most useful error, "you used that after you gave
it away", can be had without lifetime annotations and without giving up a
scripting-style surface. BRAT's answer is to check ownership inside one function
and to read everything else from the signature: a parameter is `move`, `ref` or
`mut`, and the call site is checked against that. Types are optional; code
without annotations runs on tagged dynamic values.

The price is real. The checker is not a soundness proof, there is no way to
return a reference, generics are erased, and recursion is capped at 2000
frames. If you need memory safety you can rely on, use Rust or a garbage-collected
language. If you want a small compiled language with a C ABI and mature tooling,
use Zig, Odin or Go. If you want a scripting language with libraries, use
Python. BRAT is for people who want to look at this particular trade-off.

## What it looks like

Errors are values, and `?` propagates them. A number outside `i64` is an error,
not a wrapped number:

```brat run
fn parse_port(s: str) -> i64 {
  let p = s.parse_int()
  if is_err(p) {
    return 80
  }
  return unwrap(p)
}

print(parse_port("8080"))
print(parse_port("http"))
print(parse_port("99999999999999999999"))
```

```output
8080
80
80
```

The checker tracks `move` and reports where the value went:

```brat check
fn eat(x: move str) {
  print(x)
}

fn main() -> i64 {
  let s = "hi"
  eat(move s)
  print(s)
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:8: error: use of moved `s` (hint: the value was moved; use a copy or re-bind)
  |   print(s)
example.brat:7: note: `s` moved here
  |   eat(move s)
example.brat: 0 type error(s)
```

More in the [language tour](docs/guide/tour.md). Every program printed in this
README and in the guides marked `brat run` or `brat check` is executed on two
implementations by `lang/tests/test_docs_examples.py`.

## Running it

You need Python 3 and, to compile, a C compiler (`cc`, or `$BRAT_CC`). From a
checkout:

```sh
git clone https://github.com/Nefor229883984/d.git brat && cd brat
python3 lang/engine/cli.py run --interp hello.brat    # the Python interpreter
python3 lang/engine/cli.py build hello.brat -o hello  # emit C, call cc
./hello
```

`hello.brat` is any file, for example `print(40 + 2)`.

`cli.py run` without `--interp` runs native code only if the C++ compiler has
been built and is not older than its sources. Otherwise it says so on stderr and
runs the interpreter. Build it once:

```sh
mkdir -p lang/cxx/bin
c++ -std=c++20 -O2 lang/cxx/src/*.cpp -o lang/cxx/bin/brat-cxx
```

GCC 14 builds the sources. Clang 19 accepts all 24 files with `-fsyntax-only`;
nobody has linked and run a Clang build. A single `c++` command compiles the 24
files one after another: 134 and 148 seconds in two runs, and 540 MB for the largest compiler
process, on a 2-core 2.6 GHz Xeon (two parallel jobs took 69 seconds). Only
Linux x86-64 has been exercised. Nothing verifies the Windows and macOS code
paths the repository contains.

## What works

| Part | State |
|---|---|
| Interpreter (`lang/engine/brat/`) | Reference semantics. A tree walker on CPython, about 60 thousand calls a second; treat it as the specification you can step through, not as a way to run programs. |
| Native C emitter (C++) | The only C emitter; generated programs use the runtime `lang/engine/crt.h` (9.8k lines). The Python CLI delegates C generation and native builds to it. |
| Checker | Ownership, effects and optional static types. Intra-function, so it is a lint with teeth, not a proof. |
| Standard library | 12 modules: `bytes`, `cli`, `crypto`, `csv`, `env`, `fs`, `http`, `json`, `path`, `proc`, `sqlite`, `time`. Several need system libraries. |
| C interop | `extern fn`, `extern struct`, callbacks, header verification, `bindgen`, plugins. A trusted boundary: the verifier checks declarations, not the library behind them. |
| Tools | `lsp`, `mcp`, `pkg`, `dist`, `api`, `ffi`, `fmt`, `test`, `explain`. They exist and the repository tests them; the audit read the package code and fuzzed the archive reader, and did not go further. |

## What does not work

| Limit | Consequence |
|---|---|
| Recursion depth is 2000 BRAT frames in both implementations | Deeper recursion raises the catchable error `recursion too deep`. `BRAT_RECURSION_BUDGET` raises it; a compiled program on Linux also stops with that error when its stack is nearly used up (at 8 MiB, after 10,000 to 20,000 frames), elsewhere it dies with SIGSEGV past that depth. See [limits](docs/guide/limits.md). |
| The checker is not a soundness proof | A clean `brat check` means the program passes the checks that were implemented. See [ownership](docs/guide/ownership.md#what-is-not-checked). |
| Foreign code, raw pointers and `--unsafe-ffi` are trusted | A wrong signature corrupts memory. See [SECURITY.md](SECURITY.md). |
| No isolation | Running untrusted BRAT code, packages or plugins is not safe. Use an OS sandbox. |
| Two implementations can disagree | They are compared on the 109 programs of `lang/tests/corpus/agreement.json` and on a matrix of 9550 operator cases. Known differences are listed in [limits](docs/guide/limits.md#where-the-backends-differ); outside what was compared, assume nothing. |
| Integers are `i64` and wrap; there are no bitwise operators, shifts or `**`, and no `abs`, `min`, `max` or `sqrt` builtins | The lexer refuses `|`, `^` and `~` instead of skipping them. Do it with arithmetic or call C. |
| Generics are erased; no trait objects, macros or async | See [the tour](docs/guide/tour.md#13-not-implemented). |
| Named functions are not values | A function is passed to `map` and friends as a callback; `let g = f` is a checker error. |
| Compile time grows with the square of the size of one function | 200 string locals in one function take 26 seconds; the C compiler is killed after 120. See [limits](docs/guide/limits.md#compile-time). |
| No hosted package registry | `brat pkg` works against a local or HTTP index you provide. |
| No compatibility promise | Syntax, library and tools may change in any commit. |

## How far to trust this

`lang/tests/` holds 154 test files that pass on Linux x86-64 with GCC 14 and
Python 3.13. `run_all.py` counts skips separately from passes, and a test that
cannot run says so. Most behaviour is pinned by examples; the strongest
evidence is differential: `test_agreement.py` runs 109 cases through the
interpreter and the native C backend, and `test_ffi_front_ends_agree.py` feeds both
FFI verifiers thousands of generated declarations and requires identical
verdicts.

The tree has been audited once: by reading, by differential probes, with ASan,
UBSan and LSan builds of the C++ compiler and of generated programs, and with
four minutes of libFuzzer on each of the archive reader and the front end. That
produced 35 findings, among them a use-after-free in generated code. What came
out of it is kept in [known risks](docs/project/LEDGER.md). Four minutes of
fuzzing find shallow bugs only. Nothing here has been run in production, and
nobody outside the project has reviewed it.

## Documentation

- [Tour](docs/guide/tour.md), [language reference](lang/spec/SPEC.md)
- [Ownership and what it does not check](docs/guide/ownership.md),
  [limits](docs/guide/limits.md)
- [C interop](docs/guide/ffi.md), [modules and packages](docs/guide/modules.md)
- [Architecture](docs/design/ARCHITECTURE.md), [arenas](docs/design/ARENA.md),
  [parallel loops](docs/design/PARFOR.md)
- [Known risks](docs/project/LEDGER.md),
  [priorities](docs/project/ROADMAP.md)
- [Security](SECURITY.md), [contributing](CONTRIBUTING.md)

## Source tree

```
lang/engine/     Python implementation (interpreter/checker) and crt.h runtime
lang/cxx/src/    C++20 compiler, sole C emitter and tools
lang/std/        standard library
lang/examples/   64 example programs
lang/tests/      154 test files; run_all.py drives them
editors/         VS Code extension and grammar
docs/            guides, design notes, audit report
```

License: [Apache-2.0](LICENSE).
