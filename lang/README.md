# `lang/` Directory Layout

- `cxx/src/`: C++20 compiler, module loader, ownership/type/effect checker,
  C11 emitter, FFI/DWARF verifier, LSP/MCP servers and CLI tools (`brat-cxx`,
  26 `.cpp` and 25 `.h` files).
- `engine/crt.h`, `engine/crt/*.h`: modular C11 runtime headers (`core.h`,
  `ops_str.h`, `conc.h`, `sql_proc.h`, `net_tls.h`) included by emitted C programs.
- `std/`: 12 standard-library modules and `.bri` interface files.
- `examples/`: 64 runnable `.brat` programs exercised by the test suite.
- `tests/`: regression, sanitizer and fuzz test files driven by `run_all.py`.
- `spec/SPEC.md`: language reference.

See the [root README](../README.md) for build and test instructions.
