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
| `op` must take 2 parameters | `` `add3` takes 3 parameter(s); a fold needs exactly two `` |
| `op` must have an empty effect row | `` `shout` cannot fold in parallel: it can io `` |
| Loop body must have an empty effect row | `` the `par for` body cannot io `` |
| Loop body cannot mutate outer places | `` the `par for` body writes `seen`, which lives outside the loop `` |

Inferred effect rows track `io`, `fs`, `net`, `ffi`, `err` and `spawn`. Local
heap allocation (`alloc`) is permitted inside the body because newly allocated
values are private to each task until returned into its result slot. Nested
`par for` loops are rejected because `spawn` is an effect, preventing worker
pool deadlock or oversubscription.

## Execution and determinism

- **Runtime (`vparfor` in `crt/conc.h`):** allocates an `N`-slot result array,
  submits one task per element writing exclusively to slot `i`, waits with
  `vjoin_all()` (propagating the first worker error if one occurs), and folds
  slots `0..N-1` sequentially with `op`.

Because the reduction step runs in index order (`0..N-1`), floating-point or
non-associative folds produce deterministic, run-to-run identical results
regardless of thread scheduling. The checker does not prove algebraic
associativity or verify that `unit` is a mathematical identity for `op`; an
empty input vector returns `unit` directly.

## Overhead

Each element currently dispatches one closure invocation and writes one `Val`
slot (`O(N)` temporary result storage). Use `par for` when per-element work is
coarse enough to amortize thread-pool wakeup and task queuing.
