# CLI, Environment Variables, and Diagnostic Codes

`make` creates `./bin/brat` as a symlink to `./lang/cxx/bin/brat-cxx`; both
paths invoke the same binary and accept the same subcommands and flags.

## 1. Subcommands

### Compile and execute

| Command | Purpose |
|---|---|
| `brat check [--json] <file.brat>` | Parse, load modules, check ownership (`borrow.cpp`) and types (`check.cpp`) without invoking a C compiler. `--json` emits machine-readable diagnostics (`code`, `severity`, `line`, `col`, `message`, `hint`, `fix`). |
| `brat run [-O0\|-O1\|-O2] [-g\|--debug] [--trace-lines] [--mem SIZE] [FFI flags] <file.brat> [-- args...]` | Check, compile (using the content-addressed native cache), and execute immediately. Arguments after `--` become `args()` in the program. `--mem SIZE` (for example `16M` or `1G`) caps the address space of the program. |
| `brat build [-O0\|-O1\|-O2] [-g\|--debug] [--static\|--dynamic] [--shared] [--link FLAG] [--target TRIPLE] [--trace-lines] [--mem SIZE] [FFI flags] <file.brat> [-o out]` | Emit C11 and compile a standalone native binary or `--shared` dynamic library (`brat_plugin_abi_v1`). `-g` / `--debug` emits C11 `#line` source maps and compiles with DWARF debug info pointing back to `.brat` files. |
| `brat dist [--static\|--dynamic] [--shared] [--target TRIPLE[,TRIPLE...]] [FFI flags] <file.brat> [-o dir]` | Build the program once per target triple into `dir` (default `dist`). Takes the FFI flags of `build`. |
| `brat emit-c [-O0\|-O1\|-O2] [-g\|--debug] [--trace-lines] <file.brat> [-o out.c]` | Write the generated C11 translation unit to `stdout` or `-o out.c` so you can read it or feed a custom build system. |
| `brat new <name> [--kind service\|tool]` | Create `<name>/main.brat` and `<name>/README.md` for a TCP service (default) or a command-line tool. Refuses a directory that exists. |

### C FFI verification and binding generation

| Command | Purpose |
|---|---|
| `brat ffi [--json] [--preprocess] [-I dir] [-D K=V] [--target triple] [--abi-matrix] [--header h.h] [--object lib.o\|.a\|.so] <file.brat>` | Verify every `extern fn`, `extern struct`, and `extern union` in `<file.brat>` against C headers and/or compiled ELF/Mach-O/PE DWARF debug info. Without `--preprocess`, raw `#ifdef` directives in headers are rejected explicitly. |
| `brat bindgen <header.h\|lib.rs> [--preprocess] [-I dir] [-D K=V] [--target triple] [--lib name] [--lang brat\|python\|go\|rust] [-o out]` | Generate checked `.brat` `extern` declarations (plus an adjacent `.bratbind` SHA-256 verification manifest when `-o` is used) directly from a C header or a Rust source. `--lang` writes the bindings of another language from the same header. |

Build-time FFI flags accepted by `build` and `run`:

- `--header <h.h>` (one header; a second `--header` replaces the first)
- `--object <lib.o|.a|.so|.dylib|.dll>` (repeatable; inspects symbol tables + DWARF v2-v5 without `dlopen`)
- `--unsafe-ffi` (explicit opt-out allowing unverified `extern` declarations to compile)

`-I`, `-D`, `--target` and `--abi-matrix` are flags of `brat ffi`, and `brat bindgen` takes `-I`, `-D` and `--target`. `brat build` and `brat dist` take `--target`; `brat run` does not.

### Formatting, testing, packages, and introspection

| Command | Purpose |
|---|---|
| `brat fmt [--check\|--stdout] <file.brat...>` | Canonical formatter. Rewrites each file in place by default, prints the result with `--stdout`, and with `--check` exits `1` for a file that needs formatting. |
| `brat test [-j N] [--filter substr] <file_or_dir...>` | Discover `test_*.brat` files recursively in directories (or run explicit paths) in parallel and report pass/fail counts. |
| `brat pkg <add\|install\|verify\|list\|update\|publish>` | Vendored `.tar.gz` package manager backed by `brat.lock` and `./vendor/`. See [Modules & `pkg`](modules.md). |
| `brat deps [--dot\|--json] [-o out] <file.brat>` | Inspect the transitive import graph as text, Graphviz DOT, or JSON. |
| `brat api [--json] <file.brat>` | Extract the public `.bri` interface signature of a module. |
| `brat api --diff [--json] <old.bri> <new.bri>` | Compare two interfaces and classify the change as `none`, `minor`, or `major` (exits non-zero on `major`). |
| `brat doc [-o out.md\|out.html] <file.brat>` | Render Markdown or standalone HTML API documentation from module signatures and doc comments. |
| `brat bundle <out_dir>` | Copy this compiler, the runtime headers (`crt.h`, `crt/*.h`), `lang/std` and a `bin/brat` launcher into the new directory `<out_dir>`. |
| `brat explain [--json] <code>` | Print a worked explanation and fix example for diagnostic code `<code>` (listed below). |
| `brat lsp` | Start the JSON-RPC 2.0 Language Server on `stdio` (diagnostics, hover, go-to-definition, completions, formatting, code actions). |
| `brat mcp` | Start the Model Context Protocol tool server on `stdio`. |

## 2. Environment variables

### Compiler and build cache

| Variable | Default | Effect |
|---|---|---|
| `BRAT_CC` | `cc` (or platform `gcc`/`clang`) | C11 compiler invoked by `build`, `run`, `ffi`, and `bindgen --preprocess`. |
| `BRAT_CC_TIMEOUT_MS` | `120000` | Maximum wall time (`1..3600000` ms) allowed for a child C compiler invocation. |
| `BRAT_CACHE` | `~/.cache/brat` (or OS cache dir) | Directory for content-addressed `brat run` binaries and precompiled headers. |
| `BRAT_NATIVE_CACHE_DISABLE` | unset | Set to `1` to bypass the `brat run` binary cache (useful when benchmarking cold builds). |
| `BRAT_NO_PCH` | unset | Set to `1` to disable precompiled `crt.h` headers during C compilation. |
| `BRAT_STDLIB` | `<repo>/lang/std` | Override path searched for `import std.*`. |
| `BRAT_INDEX` | `./brat-index.json` | Path or URL to the JSON package index consulted by `brat pkg`. |
| `BRAT_PKG_ALLOW_INTERNAL` | unset | Set to `1` to let `brat pkg` connect to loopback, private and link-local addresses, which it refuses by default. |
| `BRAT_CASE_MAP_VERSION` | `1500` | Unicode version of the case tables behind `upper()` and `lower()`: `1500` is Unicode 15.0 and `1600` is 16.0. Read when the C is emitted. |
| `BRAT_MOVE_RT` | unset | `0` emits a program without ownership drops, so a change in drops can be bisected. Such a program leaks on purpose. |

### Runtime confinement and resource budgets

| Variable | Default | Effect |
|---|---|---|
| `BRAT_FILES` | unset (confined) | File and SQLite operations (`O_NOFOLLOW`) confine paths to the program's module root by default; set `BRAT_FILES=1` (`true`, `yes`, `on`) to opt out of module-root confinement for trusted CLI invocations. |
| `BRAT_EXEC` | `0` (disabled) | Must be set to `1` before `proc_run` / `std.proc.*` will spawn child processes. |
| `BRAT_EXEC_ALLOW` | unset (all programs when `BRAT_EXEC=1`) | Colon-separated allowlist of executable paths/names permitted under `BRAT_EXEC=1`. |
| `BRAT_PROC_TIMEOUT_MS` | unset | Default timeout in milliseconds applied to `proc_run` children before `SIGKILL`. |
| `BRAT_ENV_ALLOW` | unset | Comma/colon-separated allowlist of environment variable names visible to `env_get` / `std.env`, overriding the secret-name denylist. |
| `BRAT_ENV_DENY_ALL` | unset | Set to `1` to make `env_get` / `std.env` return `None` for every variable. |
| `BRAT_RECURSION_BUDGET` | `2000` | Maximum BRAT call depth (`1..1000000`) before raising catchable `recursion too deep`. |
| `BRAT_PIN_BUDGET_BYTES` | `67108864` (64 MiB) | Maximum slab memory used to pin short SSO strings passed across C FFI (`ptr_of_str` / `str`); `0` disables the cap (unlimited, not recommended for untrusted inputs). |
| `BRAT_THREADS` | hardware concurrency | Worker thread count for `par for` and the runtime task pool, at most 32. |
| `BRAT_TRACE` | unset | Set to `1` to enable line-number trace updates when compiled with `--trace-lines`. |

## 3. Diagnostic code index (`brat explain <code>`)

Run `./bin/brat explain E0382` (or `--json`) at any time to see a runnable
before/after example in your terminal.

### Ownership and lifetime (`E03xx` - `E05xx`)

| Code | Summary | Typical cause & fix |
|---|---|---|
| `E0382` | Use of moved value | Reading a binding after `move x` transferred ownership. Borrow with `ref`, pass `x.copy()`, or reorder the read before `move`. |
| `E0499` | Borrow | Law of exclusivity (`mut`+`mut` or `ref`+`mut`/`move` on one root in a call, binary read+`move`, or `let mut y = m[k]`) |
| `E0502` | Cannot mutate / borrow conflict | Mutating a container inside `for x in items { items.push(...) }`, mutating a binding while an active `ref` borrow is still read later, or direct self-insertion (`v.push(v)`). Collect changes in a second vector or finish reading the borrow first. |
| `E0505` | Cannot move out of borrowed value | Moving `x` (`eat(move x)`) while a local alias `let r = x` still has a later read. Move `eat(move x)` past the last read of `r`, or `r = x.copy()`. |
| `E0507` | Cannot move out of borrowed parameter | `move s` inside `fn f(s: ref str)`. Change the parameter to `s: move str` or copy (`let owned = s.copy()`). |
| `E0515` | Borrowed reference escapes via `return` | Returning a `ref` parameter, local borrow, or `ptr_of_str(local)` whose owner dies at function exit (`arena` escapes without `own(...)` are also rejected). Return an owned copy (`s.copy()` / `own(v)`). |
| `E0596` | Mutation of immutable binding | Reassigning `x = ...` or calling `.push()` / `.set()` / `.del()` / `.sort()` without `let mut x` or `mut` parameter mode. |

### Concurrency, scope, and plugins (`B0xxx`)

| Code | Summary | Typical cause & fix |
|---|---|---|
| `B0101` | Undeclared identifier | Variable or function name is not in scope (`brat check` suggests close spellings). |
| `B0521` | Borrowed value escapes via `spawn` | Spawning a worker closure that captures a `ref`/`mut` parameter without joining inside the function. Wrap the `spawn` in `scope { ... }` or call `join_all()` before returning. |
| `B0522` | Data race across concurrent `spawn` tasks | Mutating a captured binding while another `spawn` reads or writes it before `join_all()` / `scope` exit. Pass values over `chan(cap)` or use `par for ... reduce(...)`. |
| `B0530` | Plugin symbol/value used after `lib_close` | Calling a symbol or reading data from a handle opened with `lib_open` after `lib_close(lib)` ran. Keep `defer lib_close(lib)` in the outer scope. |
| `B0531` | Plugin signature mismatch | `call(sym, sig, ...)` signature string does not match the symbol exported in the plugin's `brat_plugin_abi_v1` table. |
| `B0000` | General check diagnostic | Catch-all code for diagnostics without a dedicated numbered rule (for example duplicate top-level definitions). |

### Syntax, types, FFI, and warnings (`P`, `T`, `F`, `W`)

| Code | Summary | Typical cause & fix |
|---|---|---|
| `P0001` | Syntax / parse error | Malformed token, unclosed delimiter, or non-exhaustive `match` on an in-file `enum`/`bool`. |
| `T0001` | Type or arity mismatch | Passing `str` or an `f64` to an `i64` parameter, narrowing an `f64` into an `i64` binding, wrong argument count, or `arena` escape. |
| `F0001` | Unverified or incompatible FFI declaration | `extern fn` lacks `--header`/`--object`/`.bratbind`, or C parameter/return width/constness disagrees with the header or DWARF DIE. |
| `W0001` | Unused variable | Local binding is never read; prefix intentionally ignored bindings with `_` (`let _unused = ...`). |
| `W0002` | Unnecessary `let mut` | Binding is declared `let mut` but never reassigned or mutated in place. |
| `W0003` | Unused `import` | Imported module has no symbols referenced in the file. |
| `W0004` | Literal `{...}` in string did not interpolate | Braces inside `"..."` failed to parse as an expression and stayed literal text (harmless for JSON literals, flagged when suspicious). |
