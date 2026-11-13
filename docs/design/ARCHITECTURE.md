# Compiler and Runtime Architecture

BRAT has a single implementation: the native C++20 compiler (`brat-cxx` in
`lang/cxx/src/`, 26 `.cpp` and 27 `.h` files, ~56k lines) paired with the
modular C11 runtime (`lang/engine/crt.h` and `lang/engine/crt/*.h`, ~13k lines).

## Pipeline (`lang/cxx/src/`)

```text
source (.brat / .bri)
  -> lexer.cpp + parser.cpp      (tokens, AST, span table)
  -> module.cpp + driver_pkg.cpp (imports, .bri interfaces, brat.lock)
  -> check.cpp                   (gradual type inference, traits, exhaustiveness)
  -> borrow.cpp + parfor.cpp     (moves, local borrows, arenas, spawn/par-for rules)
  -> ffi_cdecl.cpp + ffi_object.cpp (C header, ELF/Mach-O/PE symbol & DWARF checks)
  -> ir.cpp                      (SSA-style analysis: SRA, range/div/RC elision)
  -> emit.cpp                    (two-pass C11 code generator + BRAT_CRT_MASK)
  -> system C11 compiler         (cc / gcc / clang / $BRAT_CC)
```

Key design choices in the compiler:

1. Gradual scalar specialization. Functions annotated with `i64`, `f64`, `bool`,
   or `vec[i64]`/`vec[f64]` emit unboxed scalar C signatures (`int64_t`,
   `double`) alongside thin `Val`-boxed wrappers for dynamic or closure callers.
   At `-O2`, `ir.cpp` performs scalar replacement of aggregates (SRA) on
   non-escaping `struct` locals and drops bounds, division-by-zero, and
   reference-count operations when static range or uniqueness proofs hold.
2. Separate BRAT and C struct tables. `Emitter::structs` tracks BRAT `struct`
   definitions (lowered to C structs of 16-byte `Val` fields unless scalarized
   by SRA), while `Emitter::native_structs` tracks `extern struct` layouts matching
   platform C ABI widths and alignments verified via `_Static_assert`.
3. Two-pass C emission and dead-runtime stripping:
   - Pass 1 (`emit.cpp`) lowers top-level functions, closures, traits, enums, and
     method tables into C function definitions while recording which runtime
     entry points and string literals are referenced.
   - Pass 2 prepends `#define BRAT_CRT_MASK ...`, `#include "crt.h"`, interned
     static literals, and forward declarations so unused runtime subsystems
     (HTTP/TLS, SQLite, Pike-VM regex, plugins, worker pool, `par for`) are
     compiled out by the C preprocessor.
4. Built-in tooling (`driver_tools.cpp`, `lsp.cpp`, `mcp.cpp`, `driver_pkg.cpp`).
   Formatting (`fmt`), test discovery (`test`), package resolution (`pkg`),
   header binding generation (`bindgen`), LSP (`lsp`), and MCP (`mcp`) run inside
   the same `brat-cxx` binary using the same parser and checker passes.

## Runtime (`lang/engine/crt.h`, `lang/engine/crt/*.h`)

`crt.h` includes five headers (`core.h`, `ops_str.h`, `conc.h`, `sql_proc.h`,
`net_tls.h`) gated by `BRAT_CRT_MASK`:

- `Val` (16 bytes on 64-bit): 1-byte tag/SSO/immortal header plus either up to
  14 inline UTF-8 bytes + NUL (SSO) or an 8-byte aligned payload (`int64_t`,
  `double`, or heap pointer).
- Heap objects (`VStr`, `Vec`, `Map`, `Tag`, `Clos`): intrusive `BHead` header
  holding the reference count (plain non-atomic increments/decrements in
  single-threaded programs, atomic operations when `BRAT_CRT_MT` is enabled) and
  the arena-allocation bit (`BHEAD_ARENA`).
- `arena` blocks: thread-local 64 KiB bump chunks reset on block exit (`defer`),
  retaining up to 4 chunks (256 KiB) per thread for reuse and marking freed
  regions under ASan.
- Error propagation: thread-local value channel (`verr_pending` / `verr_take`)
  checked across call boundaries, avoiding `setjmp`/`longjmp` or C++ exceptions
  across C frames.

## Test suite (`lang/tests/`)

All 175 test files in `lang/tests/` compile and execute against `brat-cxx`:

- `test_agreement.py` (`make test-native`) runs all 109 corpus programs in
  `lang/tests/corpus/agreement.json`.
- Dedicated suites (`test_cxx_frontend.py`, `test_cxx_checker.py`,
  `test_cxx_borrow.py`, `test_slice_borrow_http.py`, `test_lexer.py`,
  `test_front_ends_fuzz.py`, `test_caps_diff.py`, `test_docs_examples.py`,
  `test_runtime_json_enum.py`, `test_asan_examples.py`) cover parser/checker
  rules, ownership, FFI ABI verification, sanitizers, and documentation fences.
