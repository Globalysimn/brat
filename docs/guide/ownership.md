# Ownership

**Status:** BRAT is experimental. A clean check is not a proof of memory safety or
data-race freedom. The checker is intra-function plus the call modes a signature
declares; FFI pointers and `unsafe` blocks stay outside it. Holes are listed in
`../project/LEDGER.md`.

Single source of truth: `lang/engine/borrow.py`; the C++ port is
`lang/cxx/src/borrow.cpp`, and `test_cxx_borrow.py` requires the same messages in
the same order.

```sh
python3 lang/engine/cli.py check file.brat   # ownership + types
python3 lang/tests/test_ownership.py         # the checker cases
python3 lang/tests/test_cxx_borrow.py        # checker against checker
```

## Model

One owner, linear aliasing (Vale-style): `move` transfers ownership, `ref` and
`mut` are views for the duration of a call, and there are no lifetime
annotations.

Ownership is tracked per place, not per name. A place is a path — `x`, `x.f`,
`x.f.g`, `x[i]` (any index is the same place) — so `move p.x` invalidates `p.x`
only and the sibling fields stay alive. A projection move is counted rather than
emptied: the slot still belongs to the parent. Whole-value `move x` empties `x`
in both runtimes.

Borrows follow NLL-lite: a borrow ends at the last use of the borrower, not at
the end of the block.

```brat
let s = "x"
let _r = s          // borrow, never used again
let m = move s      // legal
```

## What is checked

| Rule | Diagnostic |
|---|---|
| use after move | `use of moved `x`` + `note: `x` moved here` |
| move under a live borrow | `cannot move `x` while borrowed by `y`` |
| assign to an immutable binding | `assign to immutable `x` (want `let mut` or mut param)` |
| assign to `const` | `assign to const `x`` |
| mutate through an immutable receiver | `cannot mutate immutable `x`: `x.push` writes through it` |
| two mutable borrows | `double mut borrow of `x`` |
| `mut self` method on an immutable receiver | `cannot mutate immutable `x`: `f` takes mut self` |
| exclusive borrow at a call | `law of exclusivity: ...` |
| move out of a non-Copy borrowed parameter | `cannot move borrowed parameter ...` |
| immutable argument to a `mut` parameter | `pass immutable `x` as mut param `p`` |
| `ref` escapes via `return` | `ref `x` escapes via return` |
| `ref` escapes via `spawn` | `ref `x` escapes via spawn` |
| write through a mutable alias of the owner | `write through `y` mutates immutable `x`` |
| undeclared name | `use of undeclared var `x`` |
| fn name read as a value | `use of `f` as a value: a fn name is a callback, not a value` |
| mutate the container a `for` is walking | `cannot mutate `v` while iterating it` |
| incomplete trait implementation | `impl Shout for Point missing loud` |

A `for` holds a view of the container for the whole of its body, so a write into
that container inside the loop is refused: `v.push(x)`, `v[0] = x`,
`m.set(k, 1)`, or handing `v` to a `mut` parameter. The rule follows the place,
so `for i in 0..3 { v.push(i) }` (a range iterates nothing) and
`for x in v.copy() { v.push(x) }` (a fresh container) are both legal. To grow
what a loop reads, collect into a second vector and apply the writes after it.

Branches and loops are walked twice and merged conservatively: a move in one
branch counts as a move after the `if`. A binding declared inside the loop body
is fresh at the top of every iteration, so
`for i in 0..3 { let s = ...; let t = move s }` is accepted while
`let s = ...; for i in 0..3 { move s }` is refused.

A branch that returns is the exception, and it is the shape C programmers write
all day:

```brat
let p = malloc(8)
if bad() {
  free(move p)      // this path leaves the function here
  return 1
}
free(move p)        // ...so this is the only free that reaches the end
```

A path that returns has no "after", so its move is not merged into the state that
follows the `if`; without that, the cleanup idiom was reported as a second free.
`break` and `continue` are *not* this case: they leave the body, not the
function, and a free on the way out of a loop is still the same pointer at the
end of it.

## What is not checked

1. **Across functions.** A call site is checked against the modes a signature
   declares (`move`, `mut`, `ref`). Consuming non-Copy parameter storage
   requires a `move` parameter; consuming a borrowed parameter or an alias
   derived from it is rejected. Foreign code is a trusted contract.
2. **Moves at run time.** Both runtimes mark a simple moved binding — the C
   backend sets `V_MOVED`, the interpreter raises `use of moved value x` — but
   the checker rejects the bad read first, so the marker is a backstop that
   shows under `--no-check`. Projection moves have no complete runtime
   invalidation path.
3. **`spawn` and data races.** A `ref` escaping into `spawn`, a rebound captured
   local, and any value both sides touch while they run are refused: until
   `join_all()` or `join(handle)`, a write in the child plus any touch in the
   parent is an error, and so is a read in the child plus a write in the parent.
   Two readers are fine. Exchange between threads goes through a channel.
4. **Returning a parameter** is accepted: it is a view into the caller's
   storage, which outlives the call. There is no `-> ref` type.
5. **`ref` escaping through a closure** is rejected by `escaping_refs`. A `ref`
   to a parameter is allowed.
6. **Writes through a field or element** are checked on the root path, so
   `p.items[0] = 9` and `p.items = [3]` both need `let mut p`.
7. **Iteration is a view, not a borrow.** A write into `v` through foreign code
   (a C function handed a `ptr`, a plugin) is invisible to the rule, which reads
   the program, not the callee.

## Cycles

Containers own their elements and refcounting frees a value at zero, so the
ownership graph is a forest and a cycle is refused rather than leaked. An insert
is `v[i] = x`, `c.push(x)`, `c.ins(i, x)` and `c.set(k, x)`; with the owner
forest `parent: value -> container`, an insert closes a cycle exactly when the
container already reaches the value:

```
v.push(v)                      parent[v] = v                 refused
a.push(b); b.push(a)           parent[b] = a, a under b       refused
a.push(b); let c = a; b.push(c)  c resolves to a             refused
outer.push(inner)              parent[inner] = outer         accepted
```

The check is one walk up one chain per insert, O(inserts x depth). A graph with
a genuine cycle has to be built out of an index or an `arena` pool.

## Threads and channels

`spawn` copies the frame, not the object graph: the bindings are private, the
heap objects behind them are shared, and the rule above orders the writes. The
safe exchange is a channel — `chan(n)`, `send`, `recv`, `close` — because a value
in a channel has one holder at a time.

What counts as a write includes the two shapes no `let` in the program spells
out: `ptr_write_i64(p, v)`, `slice_set(s, i, v)` and `fill(&x, v)` write the
place their argument names, because C does; and `move` hands the storage away, so
`free(move p)` ends `p` here. A pointer written in a `spawn` and written in the
parent is two writers, and a pointer read in a `spawn` and freed in the parent is
a reader and a writer — both are refused until a `join_all()` is between them.

A channel is a bounded queue with two ends in one scope. `send` blocks while the
queue is full, `recv` blocks while it is empty, `recv` on a closed and drained
channel gives `None`, `send` on a closed channel returns an `err`, `close` twice
is idempotent. The checker derives its rule from those five facts:

```
let ch = chan(1)
ch.send(1); ch.send(2); ch.send(3)   # refused: the third send blocks
print(ch.recv())

let ch = chan(1)
ch.send(1); ch.close(); print(ch.recv()); print(ch.recv())   # fine: None
```

The session pass infers the channel shape (`!i64` for a send, `?i64` for a
receive, `dynamic` when a loop or a branch makes the count unknowable) and
refuses unbalanced straight-line protocols during `brat check`.

## Which mechanism, and why

The borrow and capability passes classify each binding from four counters:
reads, writes, escapes and thread crossings.

| counters | mechanism | why |
|---|---|---|
| Copy type | `own` | passed by value, nothing to free |
| used in `spawn` | `transfer` | handed over, never shared mutably |
| never read | `drop` | freed at the value, no binding needed |
| read, never written | `view` | a pointer, no refcount traffic |
| read once, leaves the frame | `move` | hand it over, no clone |
| written, no live reader | `own` | mutate in place |
| written while a reader is live | `cow` | clone at the first write — **refused** |

Clone-on-write is refused rather than applied: a shallow clone of a container
shares its elements, so enabling it would change what the live reader sees.

## Rendered diagnostics

```
$ python3 lang/engine/cli.py check lang/examples/own_move.brat
lang/examples/own_move.brat: 2 error(s), 3 warning(s)
lang/examples/own_move.brat:7: error: use of moved `h` (hint: the value was moved; use a copy or re-bind)
  |   print(h)
lang/examples/own_move.brat:6: note: `h` moved here
  |   let m = move h
```

The same message reaches `--json` with the code `E0382` and a `helps` line, so a
tool does not parse the prose.
