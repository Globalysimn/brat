# BRAT

[![CI](https://github.com/Globalysimn/brat/actions/workflows/ci.yml/badge.svg)](https://github.com/Globalysimn/brat/actions/workflows/ci.yml)

BRAT is an experimental ahead-of-time compiled language for CLI tools, network
services, data pipelines, and C-heavy glue code that have outgrown scripting
languages without needing a kernel-grade borrow prover. A standalone C++20
compiler (`brat-cxx`) checks intra-function `move`/`ref`/`mut` ownership,
verifies `extern` C declarations against real C headers and DWARF debug info,
and emits portable C11.

Python is not required to build the compiler or to compile and run native BRAT
programs.

```brat run
import std.json

fn summarize_batch(raw: str) -> str {
  // Scratch allocations stay inside a 64 KiB bump arena; `own()` copies
  // only the surviving summary string out when the block resets.
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

Function signatures stay flat — no `'a` lifetime parameters or `Arc<Mutex<T>>`
type puzzles — while `brat-cxx check` still catches local use-after-move,
iterator invalidation, `arena` escapes, and unsynchronized `spawn` writes before
generating C:

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

Every `brat run` and `brat check` block in this README and across `docs/guide/`
is compiled and checked by `lang/tests/test_docs_examples.py` on every push.

## Build and run

You need `make`, a C++20 compiler (`g++` or `clang++`), and a C compiler (`cc`,
`gcc`, `clang`, or `$BRAT_CC`). Build time on a 4-core laptop is ~15 seconds:

```sh
git clone https://github.com/Globalysimn/brat.git brat && cd brat
make -j4
printf 'print(40 + 2)\n' > hello.brat
./lang/cxx/bin/brat-cxx run hello.brat
```

```text
42
```

Type-check without running, inspect generated C, or build a standalone binary:

```sh
./bin/brat check hello.brat
./bin/brat emit-c -O2 hello.brat
./bin/brat build -O2 hello.brat -o hello && ./hello
```

Compiled executables link `libc` and `libm` (`pthread` / `sqlite3` / `ssl` only
when your program actually touches them) and do not depend on `brat-cxx` or
Python. Python 3 is used only to run the repository test harness
(`lang/tests/run_all.py`) and benchmark script (`scripts/bench.py`).

## What is actually unusual here

Every language tutorial shows `if`, `for`, and `fib`, which have worked fine
since ALGOL 60. The parts of BRAT worth inspecting are the trade-offs:

1. **Ownership checks without lifetime contagion.** Heap values (`str`, `vec`,
   `map`, closures) are deterministically reference-counted, with `move`
   transfers and lexical `arena { ... }` bump regions for request-scoped trash.
   `borrow.cpp` tracks places (`x`, `x.f`, `x[i]`) inside each function and
   across `move`/`ref`/`mut` call boundaries. You pay one non-atomic refcount
   bump in single-threaded code and zero `'a` annotations in return.
2. **C headers are treated as rumors until proven.** `brat build` refuses to
   compile `extern fn` declarations unless you pass `--header` (which checks
   widths, mutability, and emits `_Static_assert(sizeof/offsetof)` probes),
   `--object` (which reads ELF/Mach-O/PE symbol tables and DWARF v2–v5 type DIEs
   without `dlopen`), a SHA-256 `.bratbind` manifest, or sign the `--unsafe-ffi`
   waiver yourself.
3. **Readable C11 output with dead-runtime surgery.** Instead of pulling in LLVM,
   `emit.cpp` lowers annotated `i64`/`f64` kernels to unboxed C scalars and
   patches a `BRAT_CRT_MASK` bitmask at the top of the emitted C file so unused
   HTTP, TLS, SQLite, regex, and thread-pool code disappears at the C
   preprocessor stage.
4. **Batteries in one ~4.5 MB binary.** `std.json` (RFC 8259 in C), `std.sqlite`,
   `std.http`, `std.crypto`, `std.csv`, linear-time Pike-VM regexes,
   deterministic `par for` folds, lockfile vendoring (`pkg`), `fmt`, `test`,
   `lsp`, and `mcp` ship in `./bin/brat`.

## Benchmarks

Microbenchmarks mostly measure how hard the author stared at `perf stat`, so
[`scripts/bench.py`](scripts/bench.py) is ~200 lines of plain Python you can
read and re-run locally with `make bench`.

Median of 3 runs on Linux x86-64 (AMD EPYC 7763, GCC 13.3.0 `-O2`, CPython 3.14.2):

| Workload | C (`gcc -O2`) | BRAT (`brat-cxx build -O2`) | CPython 3.14 |
|---|---:|---:|---:|
| `int_loop(100M)` (wrapping `i64` loop) | 0.126 s | 0.126 s | 17.075 s |
| `fib(35)` (29.9M recursive `i64` calls) | 0.025 s | 0.081 s | 1.155 s |
| `dyn_records(100k)` (`map` + `str` concat + RC drops) | n/a | 0.020 s | 0.047 s |
| `json_roundtrip(5k)` (`std.json` parse + stringify) | n/a | 0.010 s | 0.067 s |

Where BRAT wins and where it pays tax:

- **`int_loop(100M)` ties `-O2` C (`0.126 s` vs `0.126 s`)** because typed `i64`
  and `f64` loops lower to unboxed `int64_t` / `double` C scalars.
- **`fib(35)` is ~3x slower than unchecked C (`0.081 s` median / `0.061 s` best vs `0.025 s`)**
  because BRAT enforces a catchable 2,000-frame recursion budget and probes the
  pthread stack floor every 64 frames instead of segfaulting on stack overflow.
- **`dyn_records(100k)` (`0.020 s`) and `json_roundtrip(5k)` (`0.010 s`)** stay
  fast because `Val` fits in 16 bytes with 14-byte inline short strings (SSO),
  constant-key maps use pre-shaped layouts + 4-way inline caches (`BFieldIC`),
  and `std.json` runs directly in C (`crt/sql_proc.h`).

## Anti-features and who should use something else

BRAT is experimental single-author software, not a drop-in replacement for
production-hardened toolchains. Read [limits](docs/guide/limits.md) before
betting your weekend on it:

- **Kernel, embedded, or freestanding code → use C, Zig, or Rust.** Compiled
  programs link `libc` and `libm`. There is no `no_std` mode and no inline
  assembly.
- **Mathematically sound aliasing proofs → use Rust.** `brat-cxx check` is
  intra-procedural: it catches local move/borrow mistakes, iterator mutation,
  and direct `spawn` races, but it does not track lifetimes across arbitrary
  container graphs or opaque C pointers.
- **Unsigned 64-bit arithmetic in pure BRAT → write C via `extern fn`.** Inside
  BRAT, all integers are signed `i64` with two's-complement wrapping, `0x`/`0b`/`0o`
  literals, and `&`, `|`, `^`, `~`, `<<`, arithmetic `>>`. Unsigned widths
  (`u8`..`u64`, `usize`) exist only at FFI boundaries.
- **Cyclic pointer graphs (`a["next"] = b; b["prev"] = a`).** There is no
  tracing cycle collector. Intra-function cycles are rejected statically by
  `brat check`, and inter-procedural RC cycles leak unless built inside an
  `arena { ... }` block. Build long-lived graphs and LRU lists with integer
  indices into a `vec` (shown in [BRAT by Example](docs/guide/by-example.md)).
- **Hostile multi-tenant sandboxing.** `O_NOFOLLOW` path confinement, env
  denylists, and SQLite defensive mode are seatbelts against footguns, not a
  VM boundary. Put untrusted code in a container.

## Documentation

| Start here | Reference | Under the hood |
|---|---|---|
| [Language Tour](docs/guide/tour.md) | [Standard Library (`std.*`)](docs/guide/stdlib.md) | [Compiler & Runtime Architecture](docs/design/ARCHITECTURE.md) |
| [BRAT by Example](docs/guide/by-example.md) | [CLI, Env & Error Codes](docs/guide/cli.md) | [Bump Arenas (`arena`)](docs/design/ARENA.md) |
| [Ownership & Borrows](docs/guide/ownership.md) | [Modules & `pkg`](docs/guide/modules.md) | [Deterministic `par for`](docs/design/PARFOR.md) |
| [Verified C FFI](docs/guide/ffi.md) | [Language Spec (v0.7)](lang/spec/SPEC.md) | [Engineering Roadmap](docs/project/ROADMAP.md) |
| [Design Rationale & FAQ](docs/guide/faq.md) | [Limits & Sharp Edges](docs/guide/limits.md) | [Known Risks Ledger](docs/project/LEDGER.md) |

Additional material: [Quickstart Examples](examples/README.md),
[Security Policy](SECURITY.md), [Contributing](CONTRIBUTING.md).

## Repository layout

```text
examples/        curated quickstart programs (JSON+arena, SQLite, parallel fold, C FFI)
lang/cxx/src/    C++20 compiler, checker, C11 emitter and CLI tools (27 .cpp files)
lang/engine/     crt.h and crt/*.h modular C11 runtime headers
lang/std/        12 standard-library modules and .bri interfaces
lang/examples/   64 runnable language-feature programs
lang/tests/      169 test files; run_all.py drives local and sharded CI runs
scripts/         reproducible runtime and compile-time benchmarks (make bench)
editors/vscode/  VS Code syntax grammar and brat-cxx lsp client
docs/            language guides, stdlib/CLI manuals, rationale, and design notes
```

License: [Apache-2.0](LICENSE).
