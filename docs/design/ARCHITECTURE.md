# Compiler and Runtime Architecture

`lang/cxx/bin/brat-cxx` is the standalone C++20 compiler, checker and tool
driver. Building or running native BRAT programs requires `brat-cxx` and a host
C compiler (`cc`, `gcc`, `clang`, or `$BRAT_CC`); Python is not used in the
native compilation or execution pipeline.

## Compilation pipeline

1. **Lexing and parsing (`lexer.cpp`, `parser.cpp`).** UTF-8 source files are
   tokenized and parsed into an explicit AST (`ast.h`). String interpolation
   (`"{expr}"`) re-enters the expression parser at lex/parse time.
2. **Module resolution and flattening (`module.cpp`).** Reachable `import`
   statements are resolved relative to the entry file, `vendor/` (anchored by
   `brat.lock`), and `lang/std/` (`$BRAT_STDLIB`). `.bri` interface contracts
   and `pub` visibility rules (including `pub struct` and `pub enum`) are
   enforced before flattening imported functions under mangled `module__name`
   symbols into one whole-program AST.
3. **Static checking (`borrow.cpp`, `caps.cpp`, `check.cpp`, `capability.cpp`, `session.cpp`, `parfor.cpp`).**
   - `borrow.cpp` and `caps.cpp` check per-place `move`/`ref`/`mut` ownership,
     NLL-lite borrow lifetimes and capability transitions, container iteration
     locks, `defer`/`try` unwinding paths, `spawn` read/write exclusivity and
     `ptr_of_str` local return escapes.
   - `check.cpp` type-checks annotated functions, nominal `struct` fields, typed
     `enum` variants, `Option[T]` / `Result[T, E]` flow narrowing and `match`
     exhaustiveness, built-in method arity, `arena` region escapes, callback
     signatures, callback thread/reentrancy contracts and FFI scalar/struct
     types.
   - `session.cpp` and `parfor.cpp` check straight-line bounded channel balance
     and pure `par for ... reduce(op, unit)` effect rows.
4. **FFI verification (`ffi_cdecl.cpp`, `ffi_object.cpp`, `ffi_native.cpp`).**
   Programs calling `extern` C functions must pass `--header` verification
   (header width/arity comparison plus C compiler `_Static_assert` probes for
   `sizeof`/`offsetof`), a SHA-256-verified `.bratbind` manifest, or explicit
   `--unsafe-ffi`. `--object` inspects ELF, Mach-O and PE/COFF symbol tables and
   DWARF v2-v5 DIEs directly without `dlopen`.
5. **IR optimization and C11 emission (`ir.cpp`, `emit.cpp`, `lsp.cpp`).**
   - At `-O2`, `emit.cpp` runs multi-level Escape Analysis + Scalar
     Replacement of Aggregates (`run_sra`), decomposing non-escaping, unaliased
     local `struct` bindings into scalar or `Val` locals before `compute_fast()`,
     while `-O1` and `-O2` run `ir.cpp` (interval, bitwise known-bits and Granger congruence analysis
     proving division/shift/index checks redundant; inspectable via `--why-opt`).
   - Annotated numeric kernels (`i64`, `f64`, `bool`, `vec[i64]`, `vec[f64]`)
     emit unboxed C scalar functions alongside boxed `Val` entry wrappers. Heap-allocated nominal
     `struct` instances use a canonical declaration-order `BShape` layout and
     constant-slot loads/stores (`vstruct_slot_ptr` / `vstruct_slot_set`).
   - General BRAT functions emit C11 against `lang/engine/crt.h`. Functions with
     24 or more boxed locals pack them into a single `Val __fv[N]` array to keep
     GCC/Clang `-O2` SSA construction linear. `lsp.cpp` and `mcp.cpp` expose the
     same frontend/checker pipeline to editors and agent tooling over JSON-RPC.
   - At the end of Pass 2, `emit.cpp` scans emitted C identifiers and patches
     `#define BRAT_CRT_MASK` at the top of the translation unit so unused runtime
     subsystems (HTTP/TLS, SQLite, regex, plugins, thread pool, parallel loops)
     are compiled out by the C preprocessor.

## Runtime representation (`lang/engine/crt.h`, `lang/engine/crt/*.h`)

`crt.h` pulls in five modular C11 headers (`core.h`, `ops_str.h`, `conc.h`,
`sql_proc.h`, `net_tls.h`), gated by `BRAT_CRT_MASK`:

- **`Val` (16 bytes on 64-bit):** 1-byte tag/SSO/immortal header plus either up
  to 14 inline UTF-8 bytes + NUL (SSO) or an 8-byte aligned payload (`int64_t`,
  `double`, or heap pointer).
- **Heap objects (`VStr`, `Vec`, `Map`, `Tag`, `Clos`):** intrusive `BHead`
  header storing reference count (atomic only when `BRAT_CRT_MT` is enabled,
  plain non-atomic increments/decrements in single-threaded programs) and arena
  origin bit.
- **`arena` blocks:** thread-local 64 KiB bump-allocated chunks reset in bulk on
  block exit (`defer`), with 4-chunk (256 KiB) per-thread reuse and ASan
  poisoning/unpoisoning hooks.
- **Unwinding:** value-based thread-local error channel (`verr_pending` /
  `verr_take`), never `setjmp`/`longjmp` or C++ exceptions across C frames.

## Test suite (`lang/tests/`)

All 174 test files in `lang/tests/` execute natively against `brat-cxx`:

- `test_agreement.py` (`make test-native`) verifies all 109 corpus programs in
  `lang/tests/corpus/agreement.json` directly against `brat-cxx`.
- Frontend, checker, borrow, FFI, sanitizer and documentation tests
  (`test_cxx_frontend.py`, `test_cxx_checker.py`, `test_cxx_borrow.py`,
  `test_lexer.py`, `test_front_ends_fuzz.py`, `test_caps_diff.py`,
  `test_docs_examples.py`, `test_runtime_json_enum.py`) test `brat-cxx` and
  `crt.h` contracts directly.
