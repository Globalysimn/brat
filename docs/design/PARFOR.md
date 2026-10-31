# `par for ... reduce(op, unit)`

```
let total = par for x in v reduce(add, 0) { return cost(x) }
```

Every element of `v` is passed to the body, the results are folded left to right
with `add`, starting from `0`. The body runs on the thread pool; the fold runs in
index order on the calling thread.

## What the checker demands

A fold is only worth parallelising when three things hold, and each has one
message:

| requirement | refusal |
|---|---|
| `op` is a function of this program | ``` `nope` is not a function: `par for` needs the folding operation by name ``` |
| `op` takes exactly two parameters | ``` `add3` takes 3 parameter(s); a fold needs exactly two ``` |
| `op` has an empty effect row | ``` `shout` cannot fold in parallel: it can io ``` |
| the body has an empty effect row | ``` the `par for` body cannot io ``` |
| the body writes only places it binds | ``` the `par for` body writes `seen`, which lives outside the loop ``` |

The rows are the ones `brat check --effects` prints, so the error names what the
function actually does instead of saying "not pure". `alloc` counts as pure: a
body that builds a vector or a string is fine, because a fresh value cannot be
shared with another task.

Associativity is not checked, and it cannot be in v0.1 — `add` is only a monoid
if it really is one. What *is* guaranteed is determinism: the fold order is the
index order, so the parallel result equals the sequential one, element by
element. That is the difference from a goroutine with a shared counter, and from
`rayon`, which checks neither purity nor order.

## How it runs

* **Interpreter** — folds in index order on one thread. A pure body has no
  observable order, so the value is the native value; speed is the native
  backend's job and agreeing with it exactly is the interpreter's.
* **Native** — `vparfor(vec, body_closure, unit, op_closure)` in `crt.h`:
  one pool task per element writes into its own slot, `vjoin_all()` waits, then
  the slots are folded with `op`. Every task owns a private slot, so there is no
  shared mutable state to race on. An error in any task reaches `join_all` and
  propagates like a `spawn` error.

Measured on 2 cores, 400 elements of a 20 000-iteration body: 8 ms sequential,
4 ms with `par for`.

## Costs and limits

* One closure call and one `Val` per element; the result array is `n` values.
  For a trivially cheap body the fan-out costs more than it saves — the same
  trade as any task granularity, and the reason chunks are a later addition.
* The unit is used as given: an empty vector returns the unit, and the checker
  cannot prove that the unit is the identity of `op`.
* Nested `par for` inside a `par for` body is refused by the purity rule
  (`spawn` is an effect), which is also what keeps the pool from being
  oversubscribed.
