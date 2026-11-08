# Design Rationale & Hostile Q&A

Every new language gets asked roughly the same fifteen questions in the first
twenty minutes. Here are the direct answers, including the parts where another
language is a better fit.

---

## Why make a new language instead of a library?

Libraries cannot enforce three things BRAT cares about at the same time:

1. Rejecting local use-after-move, iterator invalidation, `arena` escapes, and
   unjoined `spawn` captures *before* the code runs.
2. Refusing to link `extern fn` declarations whose types disagree with the C
   header or DWARF debug info in the `.o`/`.so` you actually linked against.
3. Dropping scratch allocations deterministically at scope exit (`arena` +
   non-atomic RC) without a tracing garbage collector or `'a` lifetime
   parameters on every struct.

If you don't want that specific combination, you genuinely do not need BRAT.

---

## How does BRAT compare to Rust, Go, Zig, and Python?

| Question | Rust | Go | Zig | Python | BRAT |
|---|---|---|---|---|---|
| Heap reclamation | Static ownership (`Drop`) | Concurrent tracing GC | Manual `Allocator` | RC + cycle GC | RC + `move` + lexical `arena` (no cycle GC) |
| Lifetime annotations on structs/fns? | Yes (`'a`) | No | No (manual pointer discipline) | No | No (borrows stay intra-function) |
| C FFI type mismatches caught at build? | Only via `bindgen` | Via `cgo` C compiler | `@cImport` translates headers | No (`ctypes` crashes at run time) | Yes (`--header` `_Static_assert` + `--object` DWARF) |
| Freestanding / kernel code? | Yes | No | Yes | No | No (`libc` + `libm` required) |
| Ecosystem maturity | Huge | Huge | Growing | Huge | Tiny (experimental single-author repo) |

In one sentence each:

- **Pick Rust** when you need ironclad inter-procedural aliasing guarantees,
  zero-cost generic monomorphization, or `no_std`.
- **Pick Go** when you want mature goroutine network servers, fast team
  onboarding, and a massive library ecosystem, and GC pauses are fine.
- **Pick Zig** when you want explicit allocators, bit-exact control over memory
  layout, and cross-compiling C without `libc` assumptions.
- **Pick BRAT** when you would otherwise write Python + C extensions (or Go
  glue around C libraries) and wish the script compiled to a single small
  binary, checked C ABI widths automatically, and cleaned up request-scoped
  junk in an `arena`.

---

## Why reference counting + local borrow checks instead of full Rust lifetimes or GC?

Tracing GC makes C FFI awkward (pinning, moving collectors, cgo stack switches,
unpredictable tail latency on tight memory budgets). Full Rust borrow checking
solves both memory and concurrency statically, at the cost of lifetime
parameters infecting structs, closures, and async traits whenever data is
shared.

BRAT splits the job down the middle:

- Heap values carry a deterministic reference count (non-atomic on a single
  thread; upgraded to atomic ops only when concurrency is compiled in).
- `borrow.cpp` runs an intra-procedural dataflow pass over places (`x`, `x.f`,
  `x[i]`). That catches the bugs people write every afternoon — reading `x`
  after `move x`, mutating a vector while iterating over it, returning a
  pointer into a dead local, or letting a `ref` escape into an unjoined `spawn`
  — while keeping every struct definition free of `'a`.

---

## What happens if I build a reference cycle or a doubly-linked list?

There is **no background cycle collector**.

Intra-function reference cycles (`v.push(v)` or closing a multi-hop loop such
as `a["next"] = b; b["prev"] = a`) are caught statically by `capability.cpp`
during `brat check`:

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

Because the check is intra-procedural, cycles wired together across opaque
helper calls can still slip past static analysis and will not reach refcount
zero until process exit. You have two clean idioms:

1. **Short-lived cyclic graphs (ASTs, dependency graphs, JSON transforms):**
   build them inside `arena { ... }`. When the arena exits, every chunk is
   reclaimed in bulk regardless of internal cycles, and `own(result)` copies out
   only the acyclic summary.
2. **Long-lived doubly-linked lists, trees with parent links, and LRU caches:**
   store nodes in a `vec` and use `i64` indices (`-1` for null) instead of raw
   object references. A full runnable `LruList` implementation is in
   [BRAT by Example #5](by-example.md#5-doubly-linked-lists-and-lru-queues-without-pointer-cycles).

---

## Why compile to C11 instead of LLVM or Cranelift?

1. **Zero 400 MB compiler dependency.** Anyone with `g++`/`clang++` and `make`
   builds `brat-cxx` from source in ~15 seconds.
2. **Inspectable codegen.** When you suspect the compiler did something dumb,
   `./bin/brat emit-c -O2 file.brat` hands you plain C11 you can read or step
   through in `gdb`/`lldb`.
3. **Free C ABI parity.** Calling C from emitted C11 uses the host C compiler's
   own calling convention rules rather than re-implementing System V AMD64 /
   AAPCS64 / Win64 struct-splitting logic by hand.
4. **Dead-runtime stripping (`BRAT_CRT_MASK`).** A second pass over the emitted
   AST computes a bitmask of used runtime subsystems (`NET`, `TLS`, `SQL`,
   `PROC`, `RE`, `CONC`, `JSON`) so `hello.brat` doesn't pay compile time or
   binary size for HTTP, SQLite, or worker threads.

The downside is C compiler wall time on cold builds (~0.7s for `hello.brat` at
`-O2`), mitigated in day-to-day workflows by `brat run`'s content-addressed
binary cache and `crt.h` precompiled headers.

---

## Why does `brat build` refuse `extern fn puts(s: str) -> i32` by default?

Because handwritten FFI declarations lie constantly — someone writes `i64`
where C wrote `int`, drops `const`, or upgrades `libfoo` on Tuesday and
discovers the ABI drift via `SIGSEGV` on Friday.

`brat build` requires one of four proofs:

1. `--header stdio.h` — parses the C header, verifies argument count, integer
   widths, signedness, and pointer mutability, and emits compile-time
   `_Static_assert(sizeof(...) == ...)` / `offsetof(...)` checks into the C11
   unit.
2. `--object libfoo.a` (or `.o`/`.so`/`.dylib`/`.dll`) — parses ELF, Mach-O, or
   PE/COFF headers plus DWARF v2–v5 `.debug_info` DIEs directly (never calling
   `dlopen`, so static constructors cannot execute during `brat check`).
3. A `.bratbind` manifest produced by `brat bindgen`, pinned by SHA-256 of the
   originating header.
4. `--unsafe-ffi` — an explicit flag on the command line admitting you are on
   your own.

---

## How are strings represented? Bytes, UTF-16, or UTF-8?

Strings are **immutable, NUL-clean UTF-8** in memory:

- Strings of **14 bytes or fewer** live inline inside the 16-byte `Val` value
  (Small String Optimization — zero heap allocation, zero refcount traffic).
- Longer strings are reference-counted heap buffers with cached byte and
  code-point metadata (`SWAR` ASCII fast paths keep pure-ASCII indexing $O(1)$).
- `len(s)`, `s[i]`, and `s.slice(a, b)` count **Unicode scalar values (code
  points)**, so `"héllo"[1]` is `"é"`, never half of a UTF-8 sequence. They do
  *not* segment Unicode extended grapheme clusters (such as multi-codepoint
  family emoji).
- Arbitrary binary data and non-UTF-8 Unix filenames/argv use `vec[i64]` byte
  slices via `utf8_bytes`, `utf8_str`, `args_bytes`, `env_get_bytes`, and
  `std.bytes`.

---

## What happens on integer overflow, division by zero, and deep recursion?

None of them cause C undefined behavior:

- **Integer overflow:** `+`, `-`, `*`, and unary `-` on `i64` lower to
  unsigned casts in C11 and wrap deterministically in two's complement
  (`9223372036854775807 + 1 == -9223372036854775808`). Shift amounts mask to
  `0..63`.
- **Division / modulo by zero:** checked before executing `/` or `%`; raises a
  catchable runtime error (`div by zero`) instead of `SIGFPE`.
- **Deep recursion:** capped at `2000` frames by default (`BRAT_RECURSION_BUDGET`,
  range `1..1000000`),
  plus a 256 KiB thread stack-floor check on Linux, macOS, and Windows 8+,
  raising catchable `recursion too deep` instead of `SIGSEGV`.

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

## Why does BRAT have *both* `Result`/`?` and `try`/`catch`?

Because pretending division by zero or out-of-bounds indexing must either
return `Result` on every `+`/`[]` or tear down the entire process is a false
dichotomy in CLI and service glue code:

- **Expected domain failures** (`json.parse`, `sqlite.query`, `"abc".parse_int()`,
  `read_file`) return `Ok(v)` / `Err(e)` values and compose with `?` and `match`.
- **Invariant / input boundary guards** (isolating one malformed request or
  plugin call) can wrap a handler in `try { ... } catch e { ... }`, which
  unwinds cleanly through `defer` blocks and releases all RC/arena allocations
  along the way.

---

## Why aren't top-level `fn` names first-class values?

Direct top-level functions compile straight to static C functions (often with
unboxed `int64_t` / `double` signatures), without synthesizing a heap closure
object or uniform `Val(Val*, int)` trampoline for every function in the
program. You can pass bare `fn` names directly to collection higher-order
methods (`[1, 2, 3].map(double)`); when you actually want a heap value to store
in a variable or map, write `fn(x) { return double(x) }` so the boxing cost is
visible.

---

## Is the compiler self-hosted?

No. `brat-cxx` is ~55k lines of C++20 (`lang/cxx/src/`, 26 `.cpp` and 25 `.h`
files) with zero third-party C++ dependencies, emitting C11 against
`lang/engine/crt.h`.
Self-hosting a young language too early freezes bad syntax decisions in amber
and doubles build bootstrap complexity; keeping the compiler in dependency-free
C++20 makes changing the language cheap.

---

## What is rough or unfinished right now?

In decreasing order of annoyance:

1. **Type-erased generics.** Container element types check statically when
   annotated, but polymorphic code boxes values into 16-byte `Val` slots rather
   than monomorphizing specialized C structs per type parameter.
2. **Projection moves (`move p.f`).** Moving a field out of a map/record copies
   a counted reference rather than invalidating only that single field slot.
3. **Signed-only integers inside BRAT.** Bitwise `&`, `|`, `^`, `~`, `<<`, `>>`
   and `0x`/`0b`/`0o` literals work on `i64`, while `>>` is arithmetic
   (sign-extending). Unsigned `u8`..`u64` exist at `extern fn` / `extern struct`
   boundaries, not yet as distinct arithmetic types inside pure BRAT.
4. **POSIX runtime bias outside Linux.** The compiler, checker, `bindgen`, and
   `bundle` run natively on Linux, macOS arm64, and Windows (MinGW-w64 LLP64) in
   CI, while full socket/process/sanitizer runtime coverage is Linux-first.

Full risk accounting lives in [`docs/project/LEDGER.md`](../project/LEDGER.md).
