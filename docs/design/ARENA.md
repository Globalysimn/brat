# Lexical Bump Arenas (`arena` / `own`)

```brat
fn summarize_line(line: str) -> str {
  arena {
    let parts = line.split(",")
    let tag = parts[0].trim().upper()
    return own("{tag}:{len(parts)}")
  }
}
```

## Runtime mechanism (`lang/engine/crt/core.h`)

Entering `arena { ... }` increments the thread-local arena depth (`varena_enter`)
and saves a `(chunk, used)` cursor. While active, heap allocations (`VStr` > 14
bytes, `Vec`, `Map`, `Tag`, `Clos` together with backing storage) bump a pointer
inside 64 KiB chunks (`BHEAD_ARENA`) instead of calling `malloc`/`free` per
value.

- `b_inc` and `b_dec` check `BHEAD_ARENA` and return without touching `rc`.
- Exiting the block (`normal`, `return`, `break`, `continue`, `?`, or `raise`)
  runs `varena_leave` via `defer`, rewinding the cursor in `O(chunks)` time and
  retaining up to 4 chunks (`256 KiB`) per thread for the next `arena` entry.
  A request that does not fit the current chunk opens a new one of at least
  64 KiB and at least twice the size of the previous chunk; a chunk that would
  take the kept pool past 256 KiB is freed on exit.
- `own(x)` deep-copies `x` onto the regular reference-counted heap outside the
  active arena (with a 2000-level recursion guard). Scalars, inline short strings
  (`<= 14` bytes), and objects already on the regular heap return unchanged.
- Under AddressSanitizer, `varena_leave` poisons rewound bytes so any bug in the
  static escape pass faults on the first read.

## Static escape rules (`lang/cxx/src/borrow.cpp`)

`borrow.cpp` tags values allocated inside an `arena` block and rejects four
escape paths unless wrapped in `own(...)`:

1. Returning an arena-tainted expression (`return x` inside `arena`).
2. Assigning or mutating a binding declared outside the `arena` (`outer = x`,
   `outer.push(x)`, `outer[k] = x`, `outer.f = x`).
3. Sending an arena-tainted value over a channel (`ch.send(x)`).
4. Spawning tasks (`spawn`), nested `par for`, or storing into raw C pointers
   (`ptr_write_*`, `slice_set`) inside `arena`.

Functions called inside `arena` allocate from the active thread-local region as
well, and any value returned from such a call is treated as arena-tainted by the
caller until passed through `own(...)`.
