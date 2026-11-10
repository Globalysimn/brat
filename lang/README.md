# `lang/`

Compiler, C11 runtime, standard library, specification, and test suite:

- `cxx/src/` - C++20 ahead-of-time compiler, ownership/type checker, C11
  emitter, and CLI subcommands (`check`, `build`, `run`, `emit-c`, `ffi`, `pkg`,
  `fmt`, `test`, `lsp`, `mcp`).
- `engine/` - `crt.h` and `crt/*.h` modular C11 runtime headers linked into
  compiled programs.
- `std/` - 12 standard-library modules (`*.brat`) and interface files (`*.bri`).
- `spec/SPEC.md` - BRAT v0.7 language specification.
- `examples/` - 64 small feature programs (`newcomer.brat`, `logstat.brat`,
  ownership, arenas, FFI, concurrency).
- `tests/` - 175 test files executed by `python3 lang/tests/run_all.py`.
