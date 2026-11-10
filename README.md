# BRAT

[![CI](https://github.com/denionis23/brat/actions/workflows/ci.yml/badge.svg)](https://github.com/denionis23/brat/actions/workflows/ci.yml)

BRAT is an experimental ahead-of-time compiled language that emits C11. It is
aimed at CLI tools, small network services, batch jobs, and C glue code where
you want native binaries and checked C interop without a tracing garbage
collector or `'a` lifetime parameters on structs and functions.

```brat run
import std.json

fn summarize_batch(raw: str) -> str {
  // Temporary allocations stay in a 64 KiB bump arena; `own()` copies
  // only the final summary string to the heap before the block resets.
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

Before emitting C, `brat check` runs an intra-function ownership and concurrency
pass that catches local use-after-move, container mutation during iteration,
`arena` escapes, and unsynchronized `spawn` writes:

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

All `brat run` and `brat check` blocks in this README and under `docs/guide/`
are compiled and checked in CI by `lang/tests/test_docs_examples.py`.

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

Check a file without compiling, inspect the generated C11, or build a standalone
executable:

```sh
./bin/brat check hello.brat
./bin/brat emit-c -O2 hello.brat
./bin/brat build -O2 hello.brat -o hello && ./hello
```

Compiled binaries link `libc` and `libm`, adding `pthread`, `sqlite3`, or
OpenSSL only when the program references those modules.

## Design overview

### Memory: reference counting, local borrows, and `arena` blocks

Heap values (`str`, `vec`, `map`, closures) use deterministic reference counting
with plain non-atomic counters in single-threaded programs, switching to atomic
counts only when threads or dynamic plugins are used. Within each function, the
checker tracks `move`, `ref`, and `mut` access on local places (`x`, `x.f`,
`x[i]`) without putting lifetime parameters on function or struct signatures.
Request-scoped scratch allocations can be wrapped in `arena { ... }` and freed
in bulk at scope exit.

### Checked C interop

`brat build` refuses to compile `extern fn` declarations unless verified against
the C side: `--header` checks widths, signedness, and `const` qualifiers and
emits `_Static_assert(sizeof/offsetof)` checks into the C output; `--object`
reads ELF, Mach-O, and PE/COFF symbol tables plus DWARF v2-v5 debug info
directly without `dlopen`; `.bratbind` pins header digests in packages. Pass
`--unsafe-ffi` to opt out explicitly.

### Readable C11 output

The compiler lowers annotated `i64`, `f64`, `bool`, and local non-escaping
`struct` values to unboxed C scalars, and computes a feature bitmask so unused
runtime subsystems (HTTP, TLS, SQLite, regex, worker pool) are stripped by the C
preprocessor before compilation.

### Toolchain in one binary

`./bin/brat` (~4.5 MB) includes the compiler, `std.*` modules (`json`, `sqlite`,
`http`, `crypto`, `csv`, `fs`, `path`, `bytes`, `cli`, `time`, `env`, `proc`),
non-backtracking regexes, deterministic `par for` reductions, lockfile package
vendoring (`pkg`), `fmt`, `test`, `lsp`, and `mcp`.

## Benchmarks

[`scripts/bench.py`](scripts/bench.py) builds and runs every workload from
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

Notes on the numbers:

- `int_loop(100M)` (`0.126 s`) and `vec3_sra(50M)` (`0.141 s`) compile to raw C
  scalar loops; at `-O2`, scalar replacement splits non-escaping nested `struct`
  locals into C variables.
- `fib(35)` (`0.050 s` vs `0.024 s` in unchecked C and `0.055 s` in Go) is
  slower than C because BRAT enforces a catchable 2,000-frame recursion limit
  and checks the thread stack floor every 64 frames instead of segfaulting on
  stack overflow.
- `str_append(200k)` (`0.031 s`) and `dyn_records(100k)` (`0.020 s`) sit behind
  Go's `strings.Builder` (`0.015 s`) and typed structs (`0.017 s`). Unique
  (`rc == 1`) strings grow in place across `s = s + ...`, `Val` is 16 bytes with
  14-byte inline SSO, and `std.json` parses and formats directly in C (`0.010 s`
  vs `0.039 s` in Go `encoding/json`).

## When to use something else

BRAT is experimental single-author software. Details on edge cases and runtime
caps are in [limits](docs/guide/limits.md).

- Kernel, embedded, or freestanding code: use C, Zig, or Rust. Output requires
  `libc` and `libm`; there is no `no_std` mode or inline assembly.
- Full inter-procedural aliasing guarantees: use Rust. `brat check` operates one
  function at a time; it does not track lifetimes across container graphs or
  arbitrary C pointer stores.
- Unsigned 64-bit arithmetic in pure BRAT: call C via `extern fn`. All integers
  inside BRAT are signed `i64` with two's-complement wrapping (`u8`..`u64` and
  `usize` exist only at FFI boundaries).
- Pointer-linked cyclic graphs (`a["next"] = b; b["prev"] = a`): there is no
  cycle collector. Local cycles fail `brat check`, and cross-function cycles
  leak unless allocated inside an `arena` block. Long-lived graphs and LRU lists
  should store integer indices into a `vec` (see [BRAT by Example](docs/guide/by-example.md)).
- Untrusted multi-tenant code: path confinement (`O_NOFOLLOW`), env denylists,
  and defensive SQLite settings prevent accidental traversal and credential
  leaks, not hostile sandbox escapes. Use OS containers for untrusted inputs.

## Documentation

| Guide | Reference | Internals |
|---|---|---|
| [Language Tour](docs/guide/tour.md) | [Standard Library (`std.*`)](docs/guide/stdlib.md) | [Compiler & Runtime Architecture](docs/design/ARCHITECTURE.md) |
| [BRAT by Example](docs/guide/by-example.md) | [CLI, Env & Error Codes](docs/guide/cli.md) | [Bump Arenas (`arena`)](docs/design/ARENA.md) |
| [Ownership & Borrows](docs/guide/ownership.md) | [Modules & `pkg`](docs/guide/modules.md) | [Deterministic `par for`](docs/design/PARFOR.md) |
| [Verified C FFI](docs/guide/ffi.md) | [Language Spec (v0.7)](lang/spec/SPEC.md) | [Engineering Roadmap](docs/project/ROADMAP.md) |
| [Design FAQ](docs/guide/faq.md) | [Limits & Sharp Edges](docs/guide/limits.md) | [Known Risks Ledger](docs/project/LEDGER.md) |

See also [Quickstart Examples](examples/README.md), [Security Policy](SECURITY.md),
and [Contributing](CONTRIBUTING.md).

## Repository layout

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
