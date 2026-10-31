# Arenas

Status: implemented. Tests: `test_arena_c.py` (runtime under ASan),
`test_arena_diff.py` (both backends), `test_arena_escape.py` (escape rules),
`test_arena_request.py` (rollback on error, memory limit), `test_arena_pool.py`
(chunk reuse), `test_arena_strings.py` (string shapes).

## Why

A request handler allocates many small values that die together. Each is
otherwise a separate `calloc`/`free` pair through the refcount. An arena
replaces that with a bump pointer and one reset per block. It pays for
short-lived work and does nothing for long-lived structures.

## Syntax

```
arena {
  let req = parse_request(raw)
  let body = join(req.parts, ",")
  send(body)
}
```

The parser desugars the block into
`if true { __arena_enter(); defer __arena_leave(); body }`, so every backend
reuses the existing defer handling and an error that unwinds out of the block
still resets the arena. Nesting is capped at 64.

Inside the block every heap allocation comes from the arena; on exit, normal or
error, it resets to the mark taken at entry.

## Escape rule

A value allocated in the block must not outlive it. `typer.py` rejects, at
compile time:

- returning an arena value from the block or from the enclosing function;
- assigning one to a binding declared outside the block;
- storing one in a container that lives outside the block (`push`, index, field,
  method);
- capturing one in a `spawn` or a closure that outlives the block.

A value escapes when it mentions an inner name and its static type is not a
scalar. Region ids are tracked, so mutation through a path is not a blind spot
except through a called function that receives an outer container.

`own(x)` is the way out: a heap deep copy, with arena allocation suspended while
the copy is made. The escape rule does not look inside `own()`, so
`return Err(own("bad {len(parts)}"))` is accepted and survives the reset.

`emit-c --no-check` refuses a program containing an arena block: the reset is
only safe with the check.

## Runtime

- Thread-local arena: a chunk list plus the current mark. `balloc` takes memory
  from the arena when one is active and falls back to `calloc` otherwise.
- `BHead` records where the object came from, so release never `free()`s arena
  memory.
- Chunks are 64 KiB, with a dedicated allocation for a value larger than a chunk
  and doubling for medium ones. Freed chunks go to a per-thread pool of at most
  4 chunks (256 KiB) and are reused before `malloc` is called again.
- Allocation-size rounding and chunk growth check for `size_t` overflow before
  arithmetic; impossible sizes fail through the runtime's out-of-memory path
  instead of wrapping into a smaller allocation.
- AddressSanitizer builds poison each allocation's data when its mark is reset,
  while leaving its small runtime header readable for deferred refcount cleanup.
  Reused space is unpoisoned before it is handed out. This catches stale arena
  data references even when the backing chunk stays cached. Sanitizer builds
  retain reset chunks for deferred frame drops and may cache up to their peak
  arena footprint; regular builds keep the bounded 4-chunk/256 KiB pool and
  have no sanitizer calls or poisoning overhead.
- Strings are arena allocations too (`split`, `join`, `repeat`, `replace`, the
  case helpers), so a block that parses and joins text is freed in one reset.
- An arena object still carries a refcount; the reset drops the whole chunk
  instead of decrementing, which is exactly why the escape rule has to hold.

## Measured

A request-shaped workload -- 3000 requests, 600 of them failing -- stays under a
16 MiB hard limit with an arena and exceeds it without one, because a failed
request releases its block whole instead of walking what it allocated.
LeakSanitizer reports 2.3 MB against 25 MB for the same pair. The chunk pool
moved peak RSS by a third and wall clock not at all.

`test_arena_strings.py`, five string operations per round, 50k rounds, best of 3:

| shape | wall | peak RSS |
|---|---|---|
| no arena | 0.03 s | 14 MB |
| one block per round | 0.04 s | 14 MB |
| one block around the loop | 0.05 s | 49 MB |

A long block keeps every string it built, so peak memory grows, and a bump
allocator is not faster than `malloc` for a few dozen bytes. Use an arena where
a request-shaped workload justifies it, not by default.

## Not covered

- `spawn` inside an arena block is rejected by the escape rule rather than by a
  runtime check; sharing arena values across threads is out of scope.
- The escape checker cannot see pointers retained by foreign C code. Treat
  strings and pointers passed to C as borrowed for the call only; never retain an
  arena-backed address past the block.
- A WASM chunk source would use the same runtime API over linear memory; not
  started.
