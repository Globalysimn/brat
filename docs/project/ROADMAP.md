# Engineering Roadmap

Planned work is ordered strictly after correctness and diagnostic quality:

1. Compiler and runtime soundness: fuzzing lexer, parser, checker, Cheader/DWARF
   readers, and `std.json`/`std.http` parsers under ASan/UBSan/TSan across all
   175 test suites.
2. Diagnostics and editor feedback: tighter error spans and fix hints in
   `check.cpp`/`borrow.cpp`, plus incremental workspace indexing in `brat-cxx
   lsp`.
3. Code generation: selective monomorphization for small generic `struct` and
   `fn` definitions where boxed `Val` fields currently prevent unboxed layout
   across function boundaries.
4. C header binding coverage: broader C preprocessor macro evaluation in `brat-cxx
   bindgen` (bitfields, function-like integer macros, nested anonymous unions).
5. Targets and packaging: Linux arm64 CI runners and signed release archives in
   `.github/workflows/release.yml`.

Explicit non-goals: adding `'a` lifetime annotations to function/struct types,
adding a tracing garbage collector, or embedding an async green-thread runtime.
