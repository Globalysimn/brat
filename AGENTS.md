# AGENTS.md — BRAT

## Project

BRAT is a small systems language with ownership checking, compiled to C.
Two backends: a Python interpreter and native C (`codegen_c.py` + `crt.h`).
Tests compare their output and exit codes byte-for-byte.

- Project root: `lang/`
- Engine: `lang/engine/` (parser, checker, interpreter, C codegen, runtime)
- C++ front end: `lang/cxx/` (lexer, parser, AST, module loader, checker, C
  emitter; same tree, same flattened program, same diagnostics and same C as the
  Python one)
- Examples: `lang/examples/`
- Spec: `lang/spec/SPEC.md`
- Tests: `lang/tests/test_*.py`
- Docs: `docs/`

## Commands

```sh
python3 lang/engine/cli.py run lang/examples/hello.brat
python3 lang/engine/cli.py check lang/examples/own_bad.brat
python3 lang/engine/cli.py build lang/examples/hello.brat -o hello
python3 lang/tests/test_lang.py
python3 lang/tests/test_agreement.py
c++ -std=c++20 -O2 lang/cxx/src/*.cpp -o lang/cxx/bin/brat-cxx
python3 lang/tests/test_cxx_frontend.py
python3 lang/tests/test_cxx_checker.py
python3 lang/tests/test_cxx_borrow.py
python3 lang/tests/test_cxx_modules.py
python3 lang/tests/test_cxx_emit.py
python3 lang/tests/test_cxx_opt.py          # S4: -O0 == S3, -O1 == -O0, --why-opt
python3 lang/tests/test_ffi_widths.py       # FFI widths, callbacks, struct by value
python3 lang/cxx/bin/brat-cxx cc -O1 --why-opt lang/examples/fast.brat
```

A test that cannot run exits 77 (`lang/tests/_skip.py`) and `run_all.py` reports
it as `skipped`, never as `ok`; a machine without a compiler shows the C++ tests
that way instead of passing them.

## Core rules

1. Honesty over percentages. No unsoundness claims. Known holes are listed in `docs/guide/ownership.md`.
2. No change without a run. Any change in `lang/engine/` runs at least `test_lang.py` + `test_native.py` + `test_agreement.py`.
3. No slop. No speculative abstractions, no new files without need, no refactoring without request. Smallest diff that solves the task.
4. Two backends must agree. Anything that works in the interpreter must work in native and vice versa.
5. Do not change `crt.h` silently. ~130 functions on tagged union `Val`. Change both sides.
6. No generated files in the repo: `*.c`, `*.o`, `*.exe`, `*.zip`, `/tmp`, `/out`, `/build`.

## Architecture

- `parser.py` — parser (BRAT -> AST)
- `typer.py` — static type checks
- `borrow.py` — ownership checker
- `brat/runtime.py` — interpreter (the tree walk and the builtins)
- `codegen_c.py` — C codegen
- `crt.h` — C runtime
- `cli.py` — CLI: run/check/build/pkg/test

## Known limitations

- `move` is checked inside one function; ownership passed across a call is a
  convention. Both backends also mark a moved binding at run time (the native
  one sets `V_MOVED`, the interpreter a sentinel that raises `use of moved
  value x`), so the marker is a backstop that only shows up under `--no-check`:
  the checker rejects the read first.
- A fn name is a callback (`v.map(f)`, `run("f")`), not a value: there are no
  first-class function values, and both checkers say so.
- Channels: `break` out of a `scope` does not join (only `return` does), and a
  channel slot is reused once its channel closes, so a handle held past that
  point names a closed channel.
- Ecosystem: `brat pkg add owner/name@1.2` resolves through an index file. No
  registry server, no `publish`, no transitive resolution.
- C interop is implemented: `extern fn`, `ptr[T]`, `slice[T]`, `str_from_ptr`,
  `brat bindgen`, `export fn`, and plugins at run time (`lib_open`, `lib_sym`,
  `call`). Tests: `test_ffi*.py`, `test_c_types.py`, `test_plugin.py`.

## Skills

- `ponytail` — simplest working solution.
- `grilling` — stress-test before a feature.
- `tdd` — red-green-refactor.
- `diagnosing-bugs` — reproduce first.
- `verification-before-completion` — evidence before claims.
- `code-review` — second eyes.
- `improve-codebase-architecture` — when design is bad.

## Red lines (do not touch without explicit request)

- `lang/engine/crt.h` — runtime, ~130 functions.
- `lang/spec/SPEC.md` — spec.
- `docs/project/LEDGER.md` — status ledger.
- `test_*.py` — do not weaken tests.

## Links

- `docs/project/LEDGER.md` — what is knowingly not fixed.
- `docs/guide/ownership.md` — ownership model, known holes.
- `docs/project/LEDGER.md` — Rust pain points vs BRAT.
- `docs/design/ARENA.md` — arenas.
- `CONTRIBUTING.md` — repo rules.