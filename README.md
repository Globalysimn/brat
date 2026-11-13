# BRAT

[![CI](https://github.com/denionis23/brat/actions/workflows/ci.yml/badge.svg)](https://github.com/denionis23/brat/actions/workflows/ci.yml)

BRAT is an ahead-of-time compiled language that emits C11. It is built for CLI
tools, small network services, batch jobs, and C glue where you want a single
native binary and header-checked C interop without a tracing garbage collector
or `'a` lifetime parameters on types and functions.

[Language tour](docs/guide/tour.md) | [By example](docs/guide/by-example.md) |
[Ownership](docs/guide/ownership.md) | [C interop](docs/guide/ffi.md) |
[Standard library](docs/guide/stdlib.md) | [CLI & error codes](docs/guide/cli.md) |
[Specification](lang/spec/SPEC.md) | [Limits](docs/guide/limits.md) |
[FAQ](docs/guide/faq.md)

Heap values use reference counting, local borrows stay inside one function, and
`arena { ... }` routes request-scoped allocations to a bump region reset at
scope exit:

```brat run
import std.json

fn summarize_batch(raw: str) -> str {
  // Scratch strings, vectors, and maps stay in a 64 KiB bump chunk;
  // `own()` copies only the final summary string to the heap on exit.
  arena {
    let items = json.parse(raw)?
    let mut ok_ms = 0
    let mut failed = 0
    for row in items {
      if json.get_bool(row, "ok", false) {
        ok_ms += json.get_int(row, "ms", 0)
      } else {
        failed += 1
      }
    }
    return own("ok={ok_ms}ms failed={failed}")
  }
}

fn main() -> i64 {
  let payload = "[{\"ok\":true,\"ms\":18},{\"ok\":false,\"ms\":0},{\"ok\":true,\"ms\":24}]"
  print(summarize_batch(payload))
  try {
    print(summarize_batch("{truncated"))
  } catch e {
    print("bad input: {e}")
  }
  return 0
}
```

```output
ok=42ms failed=1
bad input: expected string key at byte 1
```

Every `brat run` and `brat check` block in this README and under `docs/guide/`
is compiled and checked in CI by `lang/tests/test_docs_examples.py`.

## How it works

### Local ownership, borrows, and `arena` blocks

Strings, vectors, maps, tagged values, and closures are freed deterministically
by reference counting (plain non-atomic counts on one thread, upgraded to atomic
operations only when threads or plugins are active). Before emitting C, `brat
check` tracks `move`, `ref`, and `mut` access on local places (`x`, `x.f`,
`x[i]`), rejecting use-after-move, aliasing mutable arguments in a call,
mutating a container while iterating it, and returning values allocated inside
an `arena` block without `own(...)`:

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

Because borrows are checked within one function rather than encoded into types,
structs and function signatures never carry lifetime parameters. The trade-off
is that cross-function aliasing is managed by reference counting instead of
static proofs; see [ownership](docs/guide/ownership.md).

### Checked C interop and owned pointers

`brat build` refuses to compile `extern fn` declarations unless verified against
the C side: `--header` checks widths, signedness, and `const` qualifiers and
emits `_Static_assert(sizeof/offsetof)` checks into the C output; `--object`
reads ELF, Mach-O, and PE/COFF symbol tables plus DWARF v2-v5 debug info
without calling `dlopen`; `.bratbind` pins header SHA-256 digests in packages.
Pass `--unsafe-ffi` only when opting out deliberately.

Foreign allocations marked `-> move ptr` are tracked across branches and early
returns so leaked or double-freed C pointers fail `brat check`:

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

### Structured threads, channels, and deterministic `par for`

`spawn { ... }` runs on the OS worker pool, `scope { ... }` joins every task
spawned inside the block before exiting, and bounded `chan(cap)` queues transfer
ownership between threads. While a `spawn` is active, captured bindings obey
single-writer / multiple-reader exclusivity:

```brat check
fn main() -> i64 {
  let mut log = ["boot"]
  scope {
    spawn { log.push("worker-a") }
    spawn { print(len(log)) }
  }
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:5: error: `log` is written inside `spawn` and touched again with no `join_all()` in between (hint: send the value through a channel)
  |     spawn { print(len(log)) }
example.brat: 0 type error(s)
```

For pure data-parallel loops, `par for x in items reduce(op, unit)` evaluates
elements across worker threads and folds results in index order (`0..len(items)`)
so output does not depend on thread scheduling:

```brat run
fn add_i64(a: i64, b: i64) -> i64 {
  return a + b
}

fn range_sq(n: i64) -> i64 {
  let mut acc = 0
  for i in 0..n {
    acc += i * i
  }
  return acc
}

fn main() -> i64 {
  let jobs = [10, 20, 30, 40]
  let total = par for n in jobs reduce(add_i64, 0) {
    return range_sq(n)
  }
  print(total)
  return 0
}
```

```output
31850
```

### Unboxed scalars, struct SRA, and C11 output

Type annotations are optional. Functions annotated with `i64`, `f64`, `bool`, or
`vec[i64]`/`vec[f64]` compile to unboxed C scalar signatures alongside boxed
wrappers for dynamic callers. At `-O2`, scalar replacement splits non-escaping
local `struct` values into C locals (`--why-opt` prints which bindings were
scalarized and which bounds or division checks were proved redundant). A
feature bitmask (`BRAT_CRT_MASK`) strips unused runtime subsystems (HTTP, TLS,
SQLite, regex, worker pool) before invoking the C compiler.

## Build and run

Building requires `make`, a C++20 compiler (`g++` or `clang++`), and a C11
compiler (`cc`, `gcc`, `clang`, or `$BRAT_CC`). Python is not required to build
the compiler or compiled programs; Python 3 is only used by the test runner
(`lang/tests/run_all.py`) and benchmark script (`scripts/bench.py`). A clean
build takes ~15 seconds on 4 cores:

```sh
git clone https://github.com/denionis23/brat.git brat && cd brat
make -j4
printf 'print(40 + 2)\n' > hello.brat
./lang/cxx/bin/brat-cxx run hello.brat
```

```text
42
```

`make` also creates a `./bin/brat` symlink to `./lang/cxx/bin/brat-cxx`. Check a
file without invoking the C compiler, inspect the generated C11, or build a
standalone binary:

```sh
./bin/brat check hello.brat
./bin/brat emit-c -O2 hello.brat
./bin/brat build -O2 hello.brat -o hello && ./hello
```

Compiled binaries link `libc` and `libm`, adding `pthread`, `sqlite3`, or
OpenSSL only when referenced. `./bin/brat` (~5 MB) includes the compiler, 12
`std.*` modules (`json`, `sqlite`, `http`, `crypto`, `csv`, `fs`, `path`,
`bytes`, `cli`, `time`, `env`, `proc`), Pike-VM regexes, `pkg`, `fmt`, `test`,
`lsp`, and `mcp`.

## Benchmarks

[`scripts/bench.py`](scripts/bench.py) compiles and times every workload from
source (`make bench`).

Median of 5 runs on Linux x86-64 (AMD EPYC 7763, GCC 13.3.0 `-O2`, Go 1.27.3, CPython 3.14.2):

| Workload | C (`gcc -O2`) | Go 1.27 (`go build`) | BRAT (`brat-cxx build -O2`) | CPython 3.14 |
|---|---:|---:|---:|---:|
| `int_loop(100M)` (wrapping `i64` loop) | 0.126 s | 0.129 s | 0.126 s | 16.795 s |
| `vec3_sra(50M)` (nested `struct` step) | 0.134 s | 0.155 s | 0.141 s | 90.569 s |
| `fib(35)` (29.9M recursive `i64` calls) | 0.024 s | 0.055 s | 0.050 s | 1.167 s |
| `str_append(200k)` (chained `+` vs `Builder`) | 0.014 s | 0.015 s | 0.031 s | 0.057 s |
| `dyn_records(100k)` (`map` + `str` + drops) | n/a | 0.017 s | 0.020 s | 0.047 s |
| `json_roundtrip(5k)` (`std.json` vs stdlib) | n/a | 0.039 s | 0.010 s | 0.067 s |

Where BRAT matches C/Go and where it loses:

- `int_loop(100M)` (`0.126 s`) and `vec3_sra(50M)` (`0.141 s`) lower to plain C
  scalar loops; at `-O2`, nested `struct` locals split into C variables without
  heap allocation.
- `fib(35)` (`0.050 s` vs `0.024 s` in unchecked C and `0.055 s` in Go) pays
  for BRAT's 2,000-frame recursion cap and 64-frame stack-floor probe, which
  turn stack overflow into a catchable error instead of `SIGSEGV`.
- `str_append(200k)` (`0.031 s`) and `dyn_records(100k)` (`0.020 s`) trail Go's
  `strings.Builder` (`0.015 s`) and typed structs (`0.017 s`). Unique (`rc == 1`)
  strings grow in place across `s = s + ...`, `Val` is 16 bytes with 14-byte
  inline SSO, and `std.json` runs directly in C (`0.010 s` vs `0.039 s` in Go
  `encoding/json`).

## Limitations

Details on runtime caps and edge cases are in [limits](docs/guide/limits.md) and
[LEDGER.md](docs/project/LEDGER.md).

- No freestanding or kernel mode: emitted C requires `libc` and `libm`, and
  there is no inline assembly. Use C, Zig, or Rust for bare-metal work.
- Intra-procedural checker only: `brat check` analyzes one function at a time
  and does not track lifetimes across container graphs or unannotated C pointer
  stores. Use Rust when you need full inter-procedural aliasing guarantees.
- Type-erased generics and signed-only integers: generic containers hold 16-byte
  `Val` slots rather than monomorphized layouts, and all integers inside BRAT
  are signed `i64` with two's-complement wrapping (`u8`..`u64` and `usize` exist
  only at C FFI boundaries).
- No cycle collector: intra-function container cycles are rejected statically,
  while cycles wired across separate functions leak unless allocated inside an
  `arena` block. Long-lived doubly-linked lists and LRU queues use `i64` indices
  into a `vec` (see [BRAT by Example](docs/guide/by-example.md#5-doubly-linked-lists-and-lru-queues-without-pointer-cycles)).
- Not a sandbox for hostile code: default `O_NOFOLLOW` path confinement, secret
  env denylists, and defensive SQLite settings guard against accidental bugs in
  local tools, not untrusted code execution. Isolate untrusted workloads in an
  OS container.

## Documentation

| Guide | Reference | Internals |
|---|---|---|
| [Language Tour](docs/guide/tour.md) | [Standard Library (`std.*`)](docs/guide/stdlib.md) | [Compiler & Runtime Architecture](docs/design/ARCHITECTURE.md) |
| [BRAT by Example](docs/guide/by-example.md) | [CLI, Env & Error Codes](docs/guide/cli.md) | [Bump Arenas (`arena`)](docs/design/ARENA.md) |
| [Ownership & Borrows](docs/guide/ownership.md) | [Modules & `pkg`](docs/guide/modules.md) | [Deterministic `par for`](docs/design/PARFOR.md) |
| [Verified C FFI](docs/guide/ffi.md) | [Language Spec (v0.7)](lang/spec/SPEC.md) | [Engineering Roadmap](docs/project/ROADMAP.md) |
| [Design FAQ](docs/guide/faq.md) | [Limits & Sharp Edges](docs/guide/limits.md) | [Known Engineering Limits Ledger](docs/project/LEDGER.md) |

See also [Quickstart Examples](examples/README.md), [Security Policy](SECURITY.md),
and [Contributing](CONTRIBUTING.md).

## Status and layout

BRAT is experimental single-author software (~56k lines of C++20 in `brat-cxx`
and ~13k lines of C11 runtime headers). CI builds and tests Linux x86-64 (GCC,
Clang, ASan/UBSan/LSan), macOS arm64, and Windows x86-64 (MinGW-w64 LLP64) on
every push.

```text
examples/        quickstart programs (JSON+arena, SQLite, parallel fold, C FFI)
lang/cxx/src/    C++20 compiler, checker, C11 emitter, and CLI tools (26 .cpp files)
lang/engine/     crt.h and crt/*.h modular C11 runtime headers
lang/std/        12 standard-library modules and .bri interfaces
lang/examples/   64 runnable feature programs
lang/tests/      175 test files; run_all.py drives local and sharded CI runs
scripts/         runtime and compile-time benchmark runner (make bench)
editors/vscode/  VS Code syntax grammar and brat-cxx lsp client
docs/            guides, stdlib/CLI manuals, and design notes
```

License: [Apache-2.0](LICENSE).
