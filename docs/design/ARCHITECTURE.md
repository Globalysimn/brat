# Architecture

## Pipeline

```
source.brat
   |  lexer, parser                       (Python: brat/lexer.py, brat/parser.py)
   |  module loader, flatten, mangle       (brat/modules.py)
   v
one program: imports spliced in, every name qualified
   |            |                    |
   | typer      | borrow             | C emitter            (codegen_c.py + crt.h)
   | (typer.py) | (borrow.py)        v
   v            v                 prog.c -> cc -O2 -> native binary
diagnostics  diagnostics
```

The same front-end/checking/emission stages are ported to C++20 under
`lang/cxx/src/`: `lexer.cpp`, `parser.cpp`, `module.cpp`, `check.cpp`,
`borrow.cpp`, `ir.cpp`, `emit.cpp`, with `main.cpp` as the `brat-cxx` binary.
They produce the same tree, diagnostics and C as the Python reference, compared
byte for byte by `test_cxx_frontend.py`, `test_cxx_checker.py`,
`test_cxx_modules.py` and `test_cxx_emit.py`. The runtime differential is
Python interpreter vs generated native C; `brat-cxx` is another route to the
same native C, not an independent runtime.

`brat build` uses `brat-cxx build` when a fresh binary sits next to its sources,
and falls back to the Python backend otherwise. `brat-cxx build` checks,
emits and invokes the C compiler directly without Python. `brat emit-c` uses
the native emitter when fresh and otherwise falls back to Python.

`brat-cxx bundle DIR` creates a relocatable native-driver bundle containing
`brat-cxx` and `crt.h`. The bundle does not include a C compiler; building a
program still requires one on the host.

## Where Python is and is not

The emitted binary links `crt.h` and libc — nothing else. `brat run` is native
by default: it execs `brat-cxx`, so running a program needs a C compiler but not
Python. `--interp` and `BRAT_INTERP=1` fall back to the Python interpreter, which
stays because it is the oracle the C emitter is diffed against.

`brat check` and `brat run` both decide with the C++ checker now; `--py` and
`BRAT_INTERP=1` name the Python one, and the views `--json`, `--effects`,
`--proto`, `--caps` and `--ownership` stay in Python because the native checker
has no such table. `test_cxx_checker.py`, `test_cxx_borrow.py` and
`test_arena_escape.py` compare the two checkers' bytes, which is how the missing
arena closure rule in `check.cpp` was found the day the default moved.

The remaining Python CLI surface is filesystem glue (`pkg`, `dist`, `new`,
`bindgen`, `deps`, `api`, `fmt`, `test`, `explain`, `mcp`). The module docstring
of `cli.py` says which command runs where.

## Runtime

`lang/engine/crt.h` is a header-only runtime of about 250 functions over one
`Val` type: a tagged union (`V_NIL`, `V_BOOL`, `V_INT`, `V_FLOAT`, `V_STR`,
`V_VEC`, `V_STRUCT`, `V_TAG`, `V_MOVED`) with a refcounted `BHead` in front of
every heap value.

- Errors are values, not `longjmp`: `vraise` sets a thread-local pending error
  and unwinds through `vuwind`, so `-O2` is safe and no frame is skipped.
- Containers own their elements; `vrc_inc`/`vrc_dec`/`vdrop` release the tree.
- `spawn` uses pthreads (or `CreateThread`) with thread-local unwinding. `scope`
  joins its children on normal exit and on `return`/`break`/`continue` in both
  emitters; `test_scope_unwind.py` pins this behavior. Shared mutable captures
  are still not covered by a complete race-safety rule.
- Arenas are per-thread chunk lists with a mark, pooled after use.
- Regex is a Pike VM, so a pattern like `(a+)+b` does not backtrack
  exponentially.

## Fast path

A function whose parameters and body are provably integer (`i64`) compiles to
raw C `int64_t` without `Val` boxing; `i64v` functions take and return vectors
directly. The IR in `ir.cpp` proves the same kind of thing one level up and
removes the checks that cannot fire, behind `-O1`.

## Compiled-binary cache

`toolchain.cc_cached` keys a binary on the emitted C, `crt.h`, the exact compiler
command line and the compiler binary's identity (path, mtime, size), installs it
by rename, and never trusts a half-written entry. `brat build`, `brat-cxx run`
and the two heaviest test files share it, which is why a warm suite is minutes
shorter than a cold one. `prune_cache` holds the whole thing under 1 GiB
(`BRAT_CACHE_MAX_MB`), oldest entry first, and shrinks to a quarter of the
filesystem when free space drops under 128 MiB: an unbounded cache is a leak that
survives reboots, and a cache in `/tmp` on a tmpfs machine is a fake test failure
waiting to happen. The location is `$BRAT_CACHE`, else `~/.cache/brat`, and only
the temporary directory if `~` is not writable.

## Repository

```
lang/engine/     parser, checker, interpreter, C emitter, crt.h, CLI, LSP, MCP
lang/cxx/        the C++20 front end: same tree, diagnostics and C
lang/examples/   64 programs; the tests run most of them on both backends
lang/spec/       SPEC.md
lang/tests/ 112 files, one per area
docs/            this documentation
```

## Rules the code follows

- The two backends agree or the change is a bug. `test_agreement.py` runs one
  corpus on three backends and asserts identical stdout and exit code, and also
  that the two emitters write byte-identical C.
- An emitter change lands in both `codegen_c.py` and `emit.cpp`, or the byte
  identity test fails.
- `crt.h` changes come with the emitter side that uses them.
- No generated files in the tree: no `*.c`, `*.o`, `*.exe`, no `lang/cxx/bin`.
