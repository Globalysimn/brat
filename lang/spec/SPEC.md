# BRAT v0.7 Language Specification

This document specifies the syntax, types, ownership rules, error semantics,
concurrency model, and C ABI rules implemented by `brat-cxx` (`lang/cxx/src/`)
and `crt.h` (`lang/engine/`).

## 1. Lexical grammar

- Source files are UTF-8 without NUL (`0x00`) bytes.
- Comments start with `//` and run to `
`.
- Identifiers match `[A-Za-z_][A-Za-z0-9_]*`.
- Keywords:
  `fn`, `pub`, `let`, `mut`, `move`, `return`, `if`, `else`, `while`, `for`,
  `in`, `break`, `continue`, `match`, `enum`, `trait`, `impl`, `import`, `use`,
  `const`, `self`, `or`, `try`, `catch`, `defer`, `spawn`, `extern`, `export`,
  `unsafe`, `true`, `false`. `struct`, `scope`, `par`, `reduce` and `arena` mean
  something only where the parser expects them and are plain identifiers
  elsewhere. A keyword cannot name a binding, a fn or a parameter (`self`
  excepted).
- Numeric literals: decimal, `0x` hex, `0o` octal, and `0b` binary integers
  (`_` separators allowed) denote `i64`; decimals with `.` or `e`/`E` denote
  IEEE-754 `f64`.
- String literals `"..."` interpolate `{expr}`. The escapes are `\n`, `\t`, `\r`,
  `\\`, `\"`, `\'`, `\{` and `\}`. A brace pair that holds no expression stays
  text, and so does any other backslash sequence. A literal that holds NUL is an
  error.

## 2. Types and runtime representation

### 2.1 Core types

- Primitives (`Copy`): `i64`, `f64`, `bool`, `ptr` (when not `move ptr`).
- Heap-counted (`Move`): `str`, `vec[T]`, `map[K, V]`, nominal `struct`, payload
  `enum` variants, closures `fn(T1, ...) -> R`, channels `chan`, sockets, SQLite
  handles, plugin handles, and FFI `slice` views.
- Linear resources: `move ptr` / `move ptr[T]` (must be consumed on every exit
  path via `ptr_free` or `move` transfer) and sockets/SQLite/plugin handles
  tracked by the checker.
- Gradual boundary: unannotated bindings and parameters have type `Dyn` (`any`),
  checked at run time on typed boundaries.

### 2.2 Runtime `Val` layout (`crt/core.h`)

On 64-bit little-endian targets, `Val` is 16 bytes:
- Byte 0 encodes tag / inline short-string length (`<= 14` UTF-8 bytes + NUL
  stored in bytes `1..15` without heap allocation) / immortal literal bit.
- Non-SSO values store an 8-byte aligned payload (`int64_t`, `double`, or
  `BHead *` heap pointer).
- Heap objects (`VStr`, `Vec`, `Map`, `Tag`, `Clos`) carry a `BHead` reference
  count and `BHEAD_ARENA` bit. Reference counts are non-atomic until concurrency
  or dynamic plugins enable `BRAT_CRT_MT`.

## 3. Ownership, borrows, and arenas (`borrow.cpp`)

All ownership checking is intra-procedural over local places (`x`, `x.f`,
`x[i]`). Function and struct types carry no lifetime parameters.

1. Bindings (`let x = ...`) are immutable by default; reassignment and mutating
   method/field/index writes require `let mut x`.
2. Parameter modes:
   - `x: T` (`ref`): read-only borrow; caller retains ownership.
   - `mut x: T` (`mut`): mutable borrow; caller argument root must be `mut`.
   - `move x: T` (`move`): ownership transfer; caller binding becomes moved-out
     unless `T` is `Copy`.
3. Call-site and expression exclusivity (`E0499`):
   - Two mutable borrows of the same root (`f(x, x)` where both are `mut`), or a
     shared borrow paired with a `mut` or `move` parameter on the same root
     (`f(x, x)` where one is `ref` and one is `mut`/`move`, or passing `x` while
     also passing a closure that mutates `x`), is rejected.
   - Reading a place on one side of a binary operator while moving it on the
     other (`s + consume(move s)`) is rejected.
   - A borrow lasts for its call: `f(x) + f(x)` is two calls one after the
     other and is accepted, `f(x, g(x))` runs `g` inside the borrow of `f`.
4. Iteration lock: while `for el in v { ... }` runs, mutating or moving `v`
   directly or through a called closure is rejected.
5. Closures capture outer variables by shallow value copy. A closure body cannot
   assign to a capture (the write would be lost), `move` an outer non-`Copy`
   capture or take ownership of an outer `move ptr`. A top-level `let` is not
   captured: every fn and closure reads and writes the one value.
6. `unsafe { ... }` marks raw pointer / FFI sections for audit without disabling
   type checking, borrow exclusivity, or post-block `move` state propagation.
7. `arena { ... }` routes heap allocations inside its dynamic extent to a
   thread-local 64 KiB bump region reset on exit. Values allocated inside an
   `arena` block cannot be returned, stored into outer bindings, sent on
   channels, or written to raw pointers unless deep-copied to the regular heap
   via `own(expr)`.

## 4. Expressions, arithmetic, and strings

- Call arguments and the items of a vector, map or struct literal are separated
  by commas; a trailing comma is allowed.
- `i64` `+`, `-`, `*`, and unary `-` wrap in two's-complement (`uint64_t` casts
  plus `-fwrapv`).
- `/` and `%` raise a catchable `div by zero` error when the divisor is `0`, on
  `i64` and on `f64`. `INT64_MIN / -1` wraps to `INT64_MIN` and
  `INT64_MIN % -1` evaluates to `0`.
- A shift uses the low six bits of its count, so `1 << 64` is `1`; `>>`
  sign-extends.
- Otherwise `f64` follows IEEE-754: overflow gives `inf` and `NaN == NaN` is
  `false`.
- `i64(x)` and `f64(x)` convert a number to the other type. `i64` drops the
  fraction toward zero and raises `integer out of range` for a value that does
  not fit; both raise `not a number` for anything else.
- `==` and `!=` compare structural value equality on primitives, strings,
  vectors, maps, structs, and enums without implicit type coercion (`1 == "1"`
  is `false`, annotated or not).
- `+` adds two numbers or joins two strings. A string next to a number and a
  `bool` in arithmetic are compile errors where the types are known; a
  `"{expr}"` placeholder takes any value.
- `if` and `while` test a `bool`. A condition known to be a string, a number, a
  vector or a map is a compile error.
- `str` values are valid UTF-8. `len(s)`, `s[i]`, and `for ch in s` operate on
  Unicode scalar values (code points); `std.bytes` exposes raw UTF-8 byte views
  and `O(1)` byte indexing.

## 5. Control flow, `Option`/`Result`, and errors

- `if` / `else`, `while`, `for i in a..b`, `for x in coll`, and `match` are
  statements (`match` may also be used in return/value position when exhaustive
  over an `enum` or carrying a `_` wildcard arm).
- Built-in sum types: `Option[T] = Some(T) | None` and
  `Result[T, E] = Ok(T) | Err(E)`.
- Postfix `expr?`:
  - Inside a function returning `Result` or `Option`, unwraps `Ok(v)` / `Some(v)`
    and returns `Err(e)` / `None` early.
  - Otherwise unwraps `Ok(v)` and raises `e` into the thread-local error channel
    caught by `try { ... } catch err { ... }`.
- `raise(x)` unwinds to a `try` around it in the same fn. With none, the fn
  returns `Err(x)` to its caller; a call statement that drops the result of such
  a fn is a compile error.
- A fn that declares a result (`-> T`) leaves by `return` or `raise` on every
  path: a body that can run off its end is a compile error. An `if` with an
  `else`, a `match` over variants and a `while true` without `break` count when
  each of their branches leaves.
- `defer expr` statements run in LIFO order on every lexical scope exit (`normal`,
  `return`, `break`, `continue`, `?`, and `raise`).
- Call depth is capped at 2,000 frames alongside a 64-frame stack-floor probe,
  raising a catchable `recursion too deep` error before stack exhaustion.

## 6. Concurrency (`conc.h`, `borrow.cpp`, `parfor.cpp`)

- `scope { ... }` waits for all `spawn { ... }` tasks started inside it before
  exiting and re-raises the first uncaught worker error on the joining thread.
- Between `spawn` and `join_all()` (or `scope` exit), a binding written by a
  spawned task cannot be read or written by the parent or sibling tasks.
- `chan(cap)` creates a bounded MPMC queue (`cap >= 1`); `send` copies/moves
  values across threads under a mutex + condition variables.
- `par for x in xs reduce(op, unit) { ... }` requires both `op` (2 parameters)
  and the loop body to have an empty effect row (`raise`, `io`, `alloc`,
  `spawn`, `ffi` all absent) and zero writes to outer state, folding element
  results deterministically in index order `0..len(xs)`.

## 7. Modules and `.bri` interfaces

- `import a.b` resolves `std.*` modules from the `lang/std` directory next to
  `brat-cxx`, local `a/b.brat` relative to the entry root, or locked packages in
  `brat.toml` / `brat.lock`. Circular imports are rejected.
- `pub` marks functions, structs, enums, and traits exported across module
  boundaries. When a sibling `.bri` interface file is present, `check.cpp`
  verifies exported signatures against it.

## 8. C FFI and ABI verification (`ffi_cdecl.cpp`, `ffi_object.cpp`)

- Foreign widths (`i8`, `i16`, `i32`, `u8`, `u16`, `u32`, `u64`, `usize`, `f32`,
  `ptr`, `ptr[T]`) convert at `extern fn` / `export fn` boundaries with range
  checks where values narrow.
- `brat-cxx build` / `run` / `ffi` require `--header`, `--object`, or a
  SHA-256-pinned `.bratbind` manifest unless `--unsafe-ffi` is passed.
  `--header` emits `_Static_assert` checks on `extern struct` sizes and field
  offsets; `--object` inspects ELF, Mach-O, and PE/COFF symbol tables and DWARF
  v2-v5 DIEs without loading code via `dlopen`.
- `slice_of`, `slice_of_ptr`, `slice_of_struct`, and `slice_struct_of` produce
  runtime-tagged `slice` descriptors (`__brat_slice`); `slice_len`, `slice_get`,
  and `slice_set` reject plain vectors both statically and at run time and
  maintain reference counts on heap-allocated element payloads.
