lang — BRAT language

Engine (`engine/`)

| File | Role |
|------|------|
| `brat.py` | CLI entry (thin shim, run + transpile-c) |
| `brat/lexer.py` | Tokens: `lex`, `Cur` |
| `brat/parser.py` | Parser: `parse_prog`, match/for desugar |
| `brat/brat/modules.py` | Module loading, `.bri` interfaces, import graph |
| `brat/runtime.py` | Tree-walk eval, ownership runtime, `run`, `to_c` |
| `borrow.py` | Ownership checker, `check(prog, extra)` |
| `codegen_c.py` | Full C99 emitter |
| `crt.h` | Runtime behind the emitted C: tagged values, refcounts, arena, `defer`, plugins |
| `cli.py` | `brat run/check/fmt/emit-c/build/dist/deps/api/pkg/new/bindgen` |
| `toolchain.py` | C compiler discovery, targets, static/dynamic |
| `scaffold.py` | `brat new` project layout |

Examples (`examples/`)

`hello.brat`, `control.brat` (match/for), `result.brat` + `lib.brat` (import, Ok/Err),
`span.brat` (trace.span block + path interpolation), `own_good.brat`,
`own_bad.brat` (must fail with 4 errors).

Tests (`tests/`)

`run_all.py` — the whole suite, one line per file: 78 files. `test_lang.py` is
the single entry for examples, ownership/typer counts and the
codegen_c/language-edges checks; `test_agreement.py` and `test_native.py` compare the
interpreter with generated C; `test_ownership.py` runs the checker against its
reject/accept corpus; `test_scope_unwind.py` checks that `break` and `continue`
join scoped tasks in both C emitters; `test_reclaim_calls.py` and `test_arena_*.py`
check freeing under AddressSanitizer; `test_plugin.py` builds
a real `.so` and calls it from both backends.

C++ front end (`cxx/`)

`c++ -std=c++20 -O2 src/*.cpp -o bin/brat-cxx` — no dependencies, and the same
compiler `brat build` already needs. `brat-cxx ast FILE` prints the tree,
`brat-cxx flat FILE` the tree with imports resolved and names qualified,
`brat-cxx check FILE` runs both checker passes, and `brat-cxx cc FILE` prints
the C `codegen_c.py` would print for it; `brat-cxx build FILE -o OUTPUT` also
checks, emits and links without starting Python. `tests/test_cxx_frontend.py`,
`tests/test_cxx_modules.py`, `tests/test_cxx_checker.py` and
`tests/test_cxx_emit.py` hold all four to the Python side on the whole corpus,
and `tests/test_cxx_borrow.py` pins every ownership rule on its own fixture.
When the binary is present and newer than its sources, `brat build` delegates
to its native `build` command; otherwise it uses the Python backend. For memory
safety, build it once with `-fsanitize=address,undefined` and run the corpus
through it.

`run_all.py` runs one test file per core; scratch directories are stable per
test (`tests/_scratch.py`), which is what makes the compiler cache hit and the
suite 3x faster warm.

Spec (`spec/SPEC.md`)

Spec follows code. New syntax lands with a runnable example first.
