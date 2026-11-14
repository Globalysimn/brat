# BRAT

[![CI](https://github.com/denionis23/brat/actions/workflows/ci.yml/badge.svg)](https://github.com/denionis23/brat/actions/workflows/ci.yml)

BRAT is a small language that compiles to C11. It is meant for command-line
tools, small services and glue around C libraries, where you want one native
binary and no garbage collector.

The idea it is built on: ownership is checked inside one function at a time,
and whatever crosses a function boundary is reference counted. You get `move`,
`ref` and `mut`, and a checker that refuses use after move, but no lifetime
parameters in any signature.

```brat run
fn count_words(text: str) {
  let mut seen = {}
  for w in text.lower().split() {
    seen[w] = seen.get_or(w, 0) + 1
  }
  return seen
}

fn main() -> i64 {
  let seen = count_words("The cat saw the other cat")
  for w in seen.keys() {
    print("{w}: {seen[w]}")
  }
  return 0
}
```

```output
the: 2
cat: 2
saw: 1
other: 1
```

Every `brat run` and `brat check` block in this README and under `docs/guide/`
is run in CI by `lang/tests/test_docs_examples.py`, with the output compared
byte for byte.

## What the checker refuses

```brat check
fn consume(buf: move str) {
  print(buf)
}

fn main() -> i64 {
  let payload = "once"
  consume(move payload)
  print(payload)
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:8: error: use of moved `payload` (hint: the value was moved; use a copy or re-bind)
  |   print(payload)
example.brat:7: note: `payload` moved here
  |   consume(move payload)
example.brat: 0 type error(s)
```

The same pass refuses two `mut` arguments that alias, a container changed
while a loop walks it, a value written in one `spawn` and read in another, and
a value that leaves the `arena { ... }` block it was allocated in. The full
list, with the cases it gets wrong, is in [ownership](docs/guide/ownership.md).

Because the check stops at the function boundary, it proves less than Rust
does. Two functions can still build a reference cycle between them, and that
cycle leaks.

## Calling C

`brat build` refuses an `extern fn` until it has been compared with the C
header (`--header`) or with the symbols of the library (`--object`), so a wrong
width or a missing `const` is a build error. A pointer the C side hands over
with `-> move ptr` has to be freed or returned on every path:

```brat check
extern fn malloc(size: usize) -> move ptr
extern fn free(move p: ptr)

fn load_header(bad: bool) -> i64 {
  let buf = malloc(64)
  if bad {
    return -1
  }
  free(move buf)
  return 0
}

fn main() -> i64 {
  return load_header(true)
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:7: error: leaked owned pointer `buf`: free it or return it
  |     return -1
example.brat:5: note: `buf` allocated here
  |   let buf = malloc(64)
example.brat: 0 type error(s)
```

More in [C interop](docs/guide/ffi.md).

## Build and run

You need `make`, a C++20 compiler and a C11 compiler. Python is not required to
build the compiler or to run programs; the test runner and the benchmark script
use Python 3.

```sh
git clone https://github.com/denionis23/brat.git brat && cd brat
make -j4
printf 'print(40 + 2)\n' > hello.brat
./lang/cxx/bin/brat-cxx run hello.brat
```

```text
42
```

`make` also links `./bin/brat` to that binary:

```sh
./bin/brat check hello.brat
./bin/brat emit-c -O2 hello.brat
./bin/brat build -O2 hello.brat -o hello && ./hello
```

## Where to look first

- [Language tour](docs/guide/tour.md): the whole language on one page.
- [By example](docs/guide/by-example.md): a hash table with inserts and
  deletes, a doubly linked list and an LRU queue, JSON of unknown size, regexes,
  SQLite.
- [Limits](docs/guide/limits.md): overflow, division by zero, recursion depth,
  what `len` of a string counts.
- [Built-ins](docs/guide/builtins.md), [standard library](docs/guide/stdlib.md),
  [modules](docs/guide/modules.md), [CLI and error codes](docs/guide/cli.md).
- [FAQ](docs/guide/faq.md) and the [specification](lang/spec/SPEC.md).
- How the compiler works: [architecture](docs/design/ARCHITECTURE.md),
  [arenas](docs/design/ARENA.md), [`par for`](docs/design/PARFOR.md).

## Speed

`make bench` runs [`scripts/bench.py`](scripts/bench.py), which compiles and
times every workload from source.

Median of 5 runs on Linux x86-64 (AMD EPYC 7763, GCC 13.3.0 `-O2`, Go 1.27.3,
CPython 3.14.2); the two long CPython loops run once. Rows under 0.1 s move by
up to 20% between two runs of the same build.

| Workload | C (`gcc -O2`) | Go 1.27 | BRAT (`build -O2`) | CPython 3.14 |
|---|---:|---:|---:|---:|
| `int_loop(100M)` | 0.126 s | 0.129 s | 0.126 s | 16.795 s |
| `vec3_sra(50M)` | 0.134 s | 0.155 s | 0.141 s | 90.569 s |
| `fib(35)` | 0.024 s | 0.055 s | 0.050 s | 1.167 s |
| `str_append(200k)` | 0.014 s | 0.015 s | 0.031 s | 0.057 s |
| `dyn_records(100k)` | n/a | 0.017 s | 0.020 s | 0.047 s |
| `json_roundtrip(5k)` | n/a | 0.039 s | 0.010 s | 0.067 s |

These are six microbenchmarks. Annotated integer and float code compiles to
plain C loops and runs like C. Code that builds strings and maps is slower than
Go, and `fib` pays for the recursion-depth check that turns a stack overflow
into an error you can catch. The JSON row is fast because `std.json` is written
in C.

## What it is not for

- Kernels and bare metal. The generated C needs `libc` and `libm`.
- Programs where a reference cycle must never leak. There is no cycle
  collector; a long-lived doubly linked list is written with indices into a
  vector, see [by example](docs/guide/by-example.md#5-doubly-linked-lists-and-lru-queues-without-pointer-cycles).
- Tight generic code. Generics are type-erased: a container holds 16-byte
  boxed values, except `vec[i64]` and `vec[f64]`.
- Unsigned arithmetic. Every integer is a wrapping `i64`; other widths exist
  only at the C boundary.
- Running code you do not trust. File access is confined to the program's
  directory by default, which guards against mistakes and is not a sandbox.

## How it was built

Most of the code here was written by an AI model. I decide what the language
does and what gets merged. The commit history shows it: the first commits are
large.

The project is experimental and has one maintainer. CI runs on Linux x86-64
(GCC, Clang, ASan and UBSan), macOS arm64 and Windows x86-64 (MinGW-w64).

```text
examples/        programs to start from
lang/cxx/src/    the compiler, C++20 (26 .cpp files)
lang/engine/     the runtime, C11 headers
lang/std/        12 standard-library modules
lang/examples/   64 small programs, one feature each
lang/tests/      175 test files
scripts/         benchmark runner
editors/vscode/  syntax grammar and LSP client
docs/            guides and design notes
```

See also [examples](examples/README.md), [security policy](SECURITY.md),
[contributing](CONTRIBUTING.md), the [roadmap](docs/project/ROADMAP.md) and the
[known limits](docs/project/LEDGER.md).

License: [Apache-2.0](LICENSE).
