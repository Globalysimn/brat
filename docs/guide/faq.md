# Design FAQ

## Why make a new language instead of a library?

A library cannot enforce three things at once:

1. Rejecting local use-after-move, iterator invalidation, `arena` escapes, and
   unjoined `spawn` writes before the program runs.
2. Refusing to build `extern fn` declarations whose types disagree with the C
   header or the DWARF debug info in the linked `.o`/`.so`.
3. Reclaiming request-scoped allocations deterministically at scope exit
   (`arena` + non-atomic RC) without a tracing GC or `'a` lifetime parameters on
   structs.

If you do not need that combination, an existing language is a better choice.

---

## How does BRAT compare to Rust, Go, Zig, and Python?

| Question | Rust | Go | Zig | Python | BRAT |
|---|---|---|---|---|---|
| Heap reclamation | Static ownership (`Drop`) | Concurrent tracing GC | Manual `Allocator` | RC + cycle GC | RC + `move` + lexical `arena` (no cycle GC) |
| Lifetime annotations on structs/fns? | Yes (`'a`) | No | No (manual pointer discipline) | No | No (borrows stay intra-function) |
| C FFI type mismatches caught at build? | Via `bindgen` | Via `cgo` C compiler | `@cImport` translates headers | No (`ctypes` fails at run time) | Yes (`--header` `_Static_assert` + `--object` DWARF) |
| Freestanding / kernel code? | Yes | No | Yes | No | No (`libc` + `libm` required) |
| Ecosystem maturity | Large | Large | Growing | Large | Experimental single-author repo |

When to pick each:

- Use Rust when you need full inter-procedural aliasing guarantees,
  monomorphized generics, or `no_std`.
- Use Go when you want mature network libraries, fast team onboarding, and do
  not mind a concurrent GC.
- Use Zig when you want explicit allocators, low-level layout control, and
  cross-compilation without `libc`.
- Use BRAT for glue tools, batch pipelines, or C-backed services that would
  otherwise be Python + C extensions, when you want a single native binary,
  checked C ABI types, and scoped `arena` cleanup.

---

## Why reference counting + local borrow checks instead of full lifetimes or a tracing GC?

A moving or concurrent GC complicates C interop (pointer pinning, FFI call
barriers, memory headroom). Full static lifetimes solve both memory reclamation
and aliasing at compile time, at the cost of lifetime parameters spreading across
structs, closures, and trait signatures once data is shared.

BRAT uses reference counting for heap lifetimes (non-atomic on a single thread,
atomic once threads or plugins are active) and runs an intra-function place
checker over `x`, `x.f`, and `x[i]`. That catches local use-after-move,
iterator mutation, returning pointers to dead locals, and concurrent `spawn`
writes without requiring `'a` on data structures.

---

## What happens if I build a reference cycle or a doubly-linked list?

There is no background cycle collector.

Intra-function cycles (`v.push(v)` or `a["next"] = b; b["prev"] = a`) are
rejected statically during `brat check`:

```brat check
fn main() -> i64 {
  let mut a = {"id": 1}
  let mut b = {"id": 2}
  a["next"] = b
  b["prev"] = a
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:5: error: reference cycle: `a` goes into `b`, which already owns it (refcounting cannot reclaim a cycle)
  |   b["prev"] = a
example.brat: 0 type error(s)
```

Cycles wired across separate helper functions are not visible to intra-function
analysis and will stay alive until process exit unless freed by a region:

1. For short-lived cyclic structures (ASTs, dependency graphs, batch JSON
   transforms), build them inside `arena { ... }`. Exiting the block reclaims
   all arena chunks in bulk regardless of internal cycles, and `own(result)`
   copies out the surviving result.
2. For long-lived doubly-linked lists, trees with parent links, and LRU caches,
   store nodes in a `vec` and link them by `i64` index (`-1` for null). A full
   `LruList` implementation is in
   [BRAT by Example #5](by-example.md#5-doubly-linked-lists-and-lru-queues-without-pointer-cycles).

---

## Why compile to C11 instead of LLVM or Cranelift?

1. Small build footprint: `brat-cxx` has no third-party C++ dependencies and
   builds from source in ~15 seconds with `make -j4`.
2. Inspectable output: `./bin/brat emit-c -O2 file.brat` writes plain C11 you
   can read or debug in `gdb`/`lldb`.
3. Native C ABI matching: calling C from emitted C11 delegates struct passing
   and register rules to the platform C compiler instead of reimplementing
   System V AMD64, AAPCS64, and Win64 ABI lowering by hand.
4. Subsystem stripping: the emitter records a `BRAT_CRT_MASK` bitmask of used
   runtime features (`NET`, `TLS`, `SQL`, `PROC`, `RE`, `CONC`, `JSON`) so
   programs do not compile or link unused runtime code.

The trade-off is C compiler time on cold builds (~0.7s for a minimal program at
`-O2`), offset during development by `brat run`'s content-addressed binary cache.

---

## Why does `brat build` refuse `extern fn puts(s: str) -> i32` by default?

Handwritten C prototypes go stale easily: writing `i64` where C expects `int` or
dropping `const` silently corrupts registers or memory at run time.

`brat build` requires one of four inputs:

1. `--header stdio.h`: parses the C header, checks argument counts, integer
   widths, signedness, and pointer mutability, and emits `_Static_assert`
   `sizeof`/`offsetof` checks into the C11 output.
2. `--object libfoo.a` (or `.o`/`.so`/`.dylib`/`.dll`): reads ELF, Mach-O, or
   PE/COFF symbol tables and DWARF v2-v5 `.debug_info` DIEs directly without
   calling `dlopen`.
3. A `.bratbind` manifest generated by `brat bindgen`, pinned to the header's
   SHA-256 digest.
4. `--unsafe-ffi` on the command line when you intentionally bypass checks.

---

## How are strings represented? Bytes, UTF-16, or UTF-8?

Strings are immutable, NUL-terminated UTF-8:

- Strings up to 14 bytes are stored inline inside the 16-byte `Val`
  representation (no heap allocation or reference-count traffic).
- Longer strings use reference-counted heap buffers with cached byte and
  code-point metadata (ASCII strings index in $O(1)$).
- `len(s)`, `s[i]`, and `s.slice(a, b)` count Unicode scalar values (code
  points), so `"héllo"[1]` is `"é"`. Extended grapheme clusters (such as
  multi-codepoint emoji sequences) are not segmented.
- Raw binary buffers and non-UTF-8 OS arguments/paths use `vec[i64]` byte slices
  via `utf8_bytes`, `utf8_str`, `args_bytes`, `env_get_bytes`, and `std.bytes`.

---

## What happens on integer overflow, division by zero, and deep recursion?

All three have defined behavior:

- Integer overflow: `+`, `-`, `*`, and unary `-` on `i64` emit unsigned C11
  operations and wrap in two's complement
  (`9223372036854775807 + 1 == -9223372036854775808`). Shift counts mask to
  `0..63`.
- Division and modulo by zero: checked before `/` or `%`; raises a catchable
  `div by zero` error instead of `SIGFPE`.
- Deep recursion: capped at `2000` frames by default (`BRAT_RECURSION_BUDGET`,
  range `1..1000000`) plus a 256 KiB thread stack-floor check on Linux, macOS,
  and Windows 8+, raising catchable `recursion too deep` instead of `SIGSEGV`.

```brat run
fn main() -> i64 {
  print(0x7fffffffffffffff + 1)
  try {
    let x = 10 / 0
    print(x)
  } catch e {
    print("caught:", e)
  }
  return 0
}
```

```output
-9223372036854775808
caught: div by zero
```

---

## Why does BRAT have both `Result`/`?` and `try`/`catch`?

Expected I/O and parsing errors (`json.parse`, `sqlite.query`, `"abc".parse_int()`,
`read_file`) return `Ok(v)` / `Err(e)` values and compose with `?` and `match`.

Runtime checks (division by zero, out-of-bounds indexing, missing map keys, or
recursion limits) raise unwindable errors rather than forcing `Result` onto every
arithmetic operation or aborting a long-running service on one bad request. A
request boundary can wrap work in `try { ... } catch e { ... }`, which runs
active `defer` blocks and drops all local RC and `arena` allocations as it
unwinds.

---

## Why aren't top-level `fn` names runtime values?

Top-level functions compile directly to static C functions (often with unboxed
`int64_t` or `double` parameters) without generating a closure object or uniform
boxing wrapper for every function. Bare `fn` names work directly with collection
methods (`[1, 2, 3].map(double)`); when you want a closure value to
store in a variable or container, write `fn(x) { return double(x) }`.

---

## Is the compiler self-hosted?

No. `brat-cxx` is ~57k lines of C++20 (`lang/cxx/src/`, 26 `.cpp` and 25 `.h`
files) with no external C++ libraries, emitting C11 against `lang/engine/crt.h`.
Keeping the compiler in plain C++20 avoids bootstrap cycles while the language
is still changing.

---

## What is rough or unfinished right now?

1. Type-erased generics: container element types are checked statically when
   annotated, but generic code uses 16-byte `Val` slots rather than
   monomorphized C structs per type argument.
2. Runtime representation of `move p.f`: `brat check` rejects reading `p.f`
   after `move p.f`, while at run time the parent container slot keeps a counted
   reference until overwritten or dropped.
3. Signed-only integers inside BRAT: bitwise operators and `0x`/`0b`/`0o`
   literals work on `i64` with arithmetic `>>`, while unsigned types (`u8`..`u64`,
   `usize`) exist only on `extern` FFI declarations.
4. Non-Linux runtime test coverage: `brat-cxx`, the static checkers, `bindgen`,
   and `bundle` run in CI on Linux, macOS arm64, and Windows (MinGW-w64 LLP64),
   while POSIX socket/process and sanitizer suites run primarily on Linux.

Additional implementation risks are tracked in
[`docs/project/LEDGER.md`](../project/LEDGER.md).
