# Region Allocation (`arena` Blocks)

Regression suites: `test_arena_c.py` (ASan), `test_arena_diff.py`,
`test_arena_escape.py`, `test_arena_request.py`, `test_arena_pool.py`,
`test_arena_strings.py`.

## Purpose

Request handlers, parsers and batch transforms allocate many short-lived strings,
vectors and maps that become garbage together at the end of one step. Without
arenas, each value pays a separate `calloc`/`free` pair and reference-count
decrement cascade. `arena { ... }` routes heap allocations inside the block to a
thread-local bump allocator and resets the mark in one step on scope exit.

## Syntax and lowering

```brat
arena {
  let parts = raw.split(",")
  let line = parts.join(":")
  send_reply(line)
}
```

The parser lowers `arena { body }` to `if true { $arena_enter(); defer
$arena_leave(); body }`. User source cannot call `$arena_enter` or
`$arena_leave` directly, `emit-c --no-check` rejects programs containing `arena`
blocks, and nesting depth is capped at 64 levels. Because reset runs through
`defer`, unwinding an error out of the block still resets the arena mark.

## Static escape rules

An object allocated inside `arena { ... }` is invalidated when the mark resets,
so `check.cpp` rejects at compile time:

1. Returning a non-scalar value bound inside the arena from the block or
   enclosing function.
2. Assigning an arena-allocated value to an outer variable or mutating an outer
   container (`push`, index write, field write, `.set`) with one.
3. Capturing an arena-allocated value in `spawn` or in a closure that outlives
   the block.

To promote a value out of an arena onto the reference-counted heap, call
`own(x)`. `own(x)` suspends the active thread-local arena while deep-copying `x`
(for example, `return Err(own("bad record {n}"))`). At `-O2`, non-escaping
local `struct` bindings inside an `arena` block are scalarized by SRA without
allocating a `Map` in the arena at all; passing a whole `struct` to `own(s)`
marks `s` as escaping so `own(s)` deep-copies the full aggregate, while
`own(s.field)` promotes individual arena-backed fields cleanly before reset.

## Runtime layout (`crt.h`)

- Each OS thread owns a linked list of 64 KiB bump chunks plus an active mark
  stack. Objects larger than 64 KiB receive dedicated oversize chunks that are
  freed on reset.
- `BHead` carries an origin bit distinguishing arena objects from heap objects
  so `vrc_dec` never passes an arena pointer to `free()`.
- Resetting a mark recycles up to 4 chunks (256 KiB) in a per-thread pool before
  falling back to `malloc`. Size rounding and chunk growth use checked overflow
  arithmetic.
- Under AddressSanitizer (`__SANITIZE_ADDRESS__`), resetting a mark poisons the
  payload bytes while keeping `BHead` readable for frame cleanup, catching
  dangling arena reads immediately even when chunks stay pooled.

## Scope boundaries

- Foreign C code receives `str` and `ptr` arguments as borrowed addresses for
  the duration of the call; C code must never stash an arena-backed pointer
  past the end of the `arena` block.
- `spawn` inside an `arena` block is rejected statically; cross-thread region
  sharing is intentionally unsupported.
