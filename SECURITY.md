# Security

BRAT is a compiler and a runtime, so its attack surface is both the code it
compiles and the C it generates.

## In scope

- Generated C that does something the source does not say. Every splice of
  source text into the emitted C is either a C string literal (escaped by
  `_clit`) or a C identifier, and the identifier positions are validated by the
  parser: `extern fn`, `extern struct` and its fields, `export fn`. A string
  token or a C keyword there is a parse error, not C text. FFI structs and
  members are emitted as `bf_<name>`, so a name the runtime already declares
  (`Val`) cannot collide with a typedef. A program where `brat check` is clean
  and the generated C fails to compile, or does something else, is a report we
  want.
- Ownership-checker bypass. A way to silence the checker is a vulnerability,
  not a feature.
- Memory safety in `lang/engine/crt.h`: out-of-bounds, use-after-free, data
  races in the runtime helpers.
- Runtime sandbox: `read_file`, `write_file` and `env_get` stay inside the
  program's directory, checked on the resolved path and again on the open file
  descriptor. `BRAT_FILES=1` is the runner's own opt-out and the only value that
  disables the sandbox; anything else leaves it on.
- Dependency fetching: a `file://` URL is accepted from the command line and
  from a local lock file, never from an index or a lock fetched over the
  network -- otherwise publishing an index would be a way to read the local
  disk.
- Denial of service: unbounded allocation, catastrophic backtracking,
  exponential parsing of untrusted input, and deep values. `print`, `str()`
  and comparison walk a value graph recursively, so a program that builds a
  deeply nested value used to overflow the stack in the C backend and die with
  SIGSEGV (~100 000 levels, cheap to build). Both walkers now stop at
  `BRAT_MAX_VALUE_DEPTH` (2000, the interpreter's frame budget) with
  `recursion too deep`; `test_depth_guard.py` pins it, including the 120 000
  level case that used to crash. The regex engine is a Pike VM in the
  native backend, so `(a+)+b` does not explode; the parser has a recursion
  limit and a fuzzer (`test_fuzz.py`). JSON parsing stops at 200 levels, source
  nesting at 200 levels per kind, and both the LSP and MCP servers cap a header
  line, the header count and the body size they will read from a client.

## Out of scope

- Language limits documented in `docs/guide/ownership.md` and
  `docs/project/LEDGER.md`, and anything `lang/spec/SPEC.md` says is not in
  v0.1.
- Data races that go through memory the compiler cannot see: FFI pointers and
  `unsafe` blocks. A `spawn` body is covered by the one-writer rule (the
  checker refuses a race before it reaches the runtime), not by a `Send`/`Sync`
  trait. A foreign function whose write is *declared* (`writes(p)`) is inside
  that rule; one that writes and does not say so is not, and no checker can
  know without the declaration.
- A wrong signature passed to `call(fn, "i64(i64)", ...)`: the string is
  checked against the argument types, not against the callee's real prototype.
- Anything that needs root or an existing shell on the target.

## Reporting

Send a description and a minimal `.brat` file that reproduces it, plus the
command you ran. The most useful payload proves itself: writes a marker file in
a temp directory and exits, with no network access and no destruction.

A fix ships with a test that fails before it and passes after. Findings that
cannot be reproduced are said to be unreproducible, plainly.

## Status

Fixed and pinned by tests:

- C text injection through names and string keys: `test_inject.py` (35 asserts)
  feeds a `touch` payload through struct-literal keys, enum and `impl` type
  names and post-dot field and method names, then requires `check`, `emit-c` and
  `build` to refuse it and the marker file never to appear; every case carries a
  positive control so a payload that merely broke parsing cannot pass.
- C text injection through FFI and export names — `test_ffi_hardening.py`
  (12 asserts), both emitters byte-identical: `extern fn "abc"`, a keyword in a
  name position and a struct member the lexer refuses are parser errors before
  any C exists; `brat bindgen` skips such declarations with the reason instead of
  emitting a binding that cannot compile (`test_bindgen_c.py`).
- Negative and null pointer arguments to FFI and to `ptr_read_*`/`ptr_write_*`
  raise instead of faulting — `test_ffi_hardening.py`.
- Filesystem sandbox: path traversal, absolute paths and a path that is a file
  descriptor — `test_security_fixes.py`.
- NUL bytes in string literals are a parse error, so a `str` cannot silently
  truncate at the C boundary.
- `??` in a literal is split into adjacent literals, so a trigraph cannot
  change what a string means under `-std=c11` (both emitters) —
  `test_hardening.py`.
- `unsafe { }` does not switch the ownership checker off: a borrowed parameter
  still cannot be moved inside one. `unsafe` is for raw pointers and FFI.
- `brat run` gates `arena { }`, `spawn` and `lib_open` programs through the same
  checks `brat check` runs, and the type-checker diagnostics are shown only when
  they exist. A warning never refuses a run.
- A plugin value belongs to its library: a symbol or a `ptr` made through a
  handle is dead after `lib_close`, and a literal `call` signature is compared
  with the `extern fn` of the same name (`test_plugin.py`).
- Shared runtime state is thread-safe: refcounts are atomic, the string and tag
  interning tables, the print buffer and `vset_at` take locks. Checked with
  ThreadSanitizer on a program that shares strings, vectors and tags across
  eight workers: no warnings.
- A path with more components than the normaliser holds is refused, not
  truncated to a different path.
### Resource limits

A program that asks for a large object gets an `err` value, never an abort.
The bounds below are shared by both backends, so neither one dies where the
other one works:

| Limit | Value | Where |
|---|---|---|
| nesting of a value walked by `print`, `str()`, `==` | 2000 | `BRAT_MAX_VALUE_DEPTH`, `_MAX_VALUE_DEPTH` |
| channel capacity | 1 048 576 slots | `BRAT_MAX_CHAN_CAP`, `runtime.py` |
| nesting of an expression in the parser | 200 | both front ends, `expression nested too deeply` |
| depth of an expression tree (a flat `1+1+...` chain counts) | 1000 | both front ends, `expression tree too deep` |
| interpreter call depth | 2000 frames | `runtime.py` |
| package download | 16 MiB | `pkg` |
| MCP/LSP header line, header count, body | 8 KiB, 64, 16 MiB | `mcp.py` |

`test_depth_guard.py`, `test_tree_depth.py` and `test_resource_bounds.py` hold
these to the letter.

### Checker coverage is part of the security story

An ownership or escape rule that only one checker implements is a hole, not a
style question: `brat run` decides with the C++ checker, so a rule missing there
is a program that builds and runs with a dangling pointer. Every escape rule in
`typer.py` has a counterpart in `check.cpp`, and `test_arena_escape.py`,
`test_cxx_checker.py` and `test_cxx_borrow.py` compare the two checkers' output
byte for byte rather than trusting one of them. The 2026-10-28 round found the
arena closure rule missing exactly this way.

Open and known, listed rather than hidden:

- Shared mutable state that arrives through FFI pointers or `unsafe` (above).
- A plugin prototype that the program never declares: when the name is not an
  `extern fn` in the source, the literal signature in `call` cannot be compared
  with anything, and a mismatch is undefined behaviour. Declared names are
  checked (arity, parameter and return types, `B0531`), and a value made through
  a handle is tied to that handle, so use after `lib_close` is refused
  (`B0530`). A `ptr` remains unowned by the runtime: nothing stops foreign code
  from freeing memory BRAT still points at, or from keeping a pointer after
  `lib_close` in a way the checker cannot see (a handle passed as a parameter).
