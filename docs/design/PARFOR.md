# Deterministic Parallel Fold (`par for`)

```brat
let total = par for x in items reduce(add, 0) {
  return cost(x)
}
```

`par for` evaluates its body across `items` on the OS worker pool and folds the
per-element results left-to-right in original index order (`0..len(items)`) on
the calling thread using `op(acc, elem_result)` starting from `unit`.

## Static requirements (`parfor.cpp`)

`brat-cxx check` enforces five conditions before accepting a `par for` loop:

| Condition | Diagnostic |
|---|---|
| `op` must name a declared function | `` `nope` is not a function: `par for` needs the folding operation by name `` |
| `op` must take 2 parameters | `` `add3` takes 3 parameter(s); a fold needs exactly two (the accumulated value and the next one) `` |
| `op` must have an empty effect row | `` `shout` cannot fold in parallel: it can io (a monoid operation has to be pure, or the result depends on the thread order) `` |
| Loop body must have an empty effect row | `` the `par for` body cannot io: every element is computed at the same time `` |
| Loop body and `op` cannot mutate outer places | `` the `par for` body writes `seen`, which lives outside the loop (a parallel fold may not touch outer mutable state) `` |

`parfor.cpp` infers five effect tags transitively across the call graph:
`raise` (`?`, `raise`, `unwrap`, `assert`, `exit`), `io` (console, files,
network, SQLite, channels, clocks, env), `alloc` (container/struct/closure
literals and allocating container/string methods), `spawn` (`spawn` and
`scope`), and `ffi` (`extern fn`, plugins, raw pointers, slices). Both the loop
body and the reduction function must have an empty effect row.

Functions are looked up by name. A `fn` declared inside another `fn` counts
too, and two functions with the same name share one row: a pure nested `add`
is refused when a top-level `add` prints.

## Execution and determinism

`vparfor` (`crt/conc.h`) allocates an `N`-slot result array and cuts the input
into at most `4 * workers` contiguous chunks (capped at 128). Each chunk is one
task that writes the body's result for element `i` to slot `i`. The calling
thread waits for all chunks, propagates the first worker error if there is one,
and folds slots `0..N-1` sequentially with `op`.

Because the reduction step runs in index order (`0..N-1`), floating-point or
non-associative folds produce identical results across runs regardless of thread
scheduling. The checker does not verify algebraic associativity or check that
`unit` is an identity element for `op`; an empty input vector returns `unit`.

## Overhead

Each element costs one closure call and one `Val` slot (`O(N)` temporary result
storage); each chunk costs one task on the queue. Use `par for` when the body
does enough work per element to outweigh the closure call.
