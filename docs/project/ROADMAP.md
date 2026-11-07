# Engineering Priorities

Product priority order:

1. **Ecosystem**: standard library (`std.*`), `brat-cxx pkg` reproducible vendor
   workflows, `.bri` module contracts, LSP/MCP editor tooling and documentation.
2. **Speed**: unboxed numeric codegen, compact C11 output, low-overhead `Val`
   containers/strings and fast cold `brat-cxx build` compilation.
3. **Safety and security**: static `move`/`ref`/`mut` and `spawn` checks,
   bounded runtime resource budgets, sanitizer cleanliness and hardened
   filesystem/SQLite confinement defaults.
4. **C FFI**: header and DWARF `--object` ABI verification, `.bratbind`
   manifests, `bindgen` accuracy and explicit pointer ownership contracts.

Correctness is a baseline requirement across all four areas: optimizations and
syntax shortcuts do not ship if they silently miscompile or corrupt memory.

## Active work tracks

1. **Close checker and codegen edge cases** as differential and fuzzer tests
   uncover them, keeping `brat-cxx` diagnostics specific and actionable without
   claiming full inter-procedural soundness.
2. **Broaden macOS and Windows test coverage.** Native `macos-15` (arm64) and
   `windows-2022` (MinGW-w64 LLP64) CI jobs now build `brat-cxx` and verify the
   frontend, ownership/type checkers, C preprocessor + FFI ABI probes and
   portable bundles on every push; expand runtime stdlib test coverage on
   non-Linux hosts over time.
3. **Grow package and C-binding workflows** around `brat.pkg.json`, `c_sources`
   and `.bratbind` manifests so multi-file BRAT + C libraries build without
   `--unsafe-ffi` or external build scripts.
4. **Expand sanitizer, 32-bit (`ILP32`) and fuzzing coverage** across `-O0`,
   `-O1` and `-O2` emitted C across all 174 native test suites.

See [LEDGER.md](LEDGER.md) for current technical limitations and known risks.
