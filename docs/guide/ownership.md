# Ownership and Borrow Checking

`brat-cxx check` (`lang/cxx/src/borrow.cpp`) runs an intra-function ownership,
mutability and concurrency pass over every BRAT function without lifetime
annotations (`'a`), `RefCell` wrappers, or `Arc<Mutex<T>>` signatures. By
design, BRAT relies on deterministic reference counting, value moves, and
scoped `arena` blocks for memory reclamation rather than inter-procedural
lifetime solving. Passing `check` catches common aliasing, use-after-move,
iterator invalidation and thread-sharing bugs, while keeping ordinary function
signatures flat; it is **not** a complete soundness proof. Residual gaps are
listed below and in [LEDGER.md](../project/LEDGER.md).

```sh
./lang/cxx/bin/brat-cxx check file.brat
python3 lang/tests/test_ownership.py
python3 lang/tests/test_cxx_borrow.py
python3 lang/tests/test_defer_try_soundness.py
```

## Place-based model

Every non-Copy value has one logical owner. Parameter signatures declare how a
callee accesses each argument:

- `ref` (the default when omitted): read-only borrow for the duration of the call.
- `mut`: exclusive read-write borrow for the duration of the call; requires a
  mutable place (`let mut`) with no other live borrows in the same call.
- `move`: transfers ownership into the callee and invalidates the caller's place.

Places are tracked at path granularity (`x`, `x.f`, `x.f.g`, `x[i]`, where all
indices of `x` map to the same element place). Moving `p.x` invalidates `p.x`
while leaving `p.y` readable. Whole-variable `move x` marks `x` moved both
statically and at run time (`V_MOVED`); projection moves (`move p.x`) are
checked statically and increment the underlying reference count at run time
because the parent map or vector retains its slot.

Borrows end at the last read of the borrowing binding (non-lexical lifetime
within straight-line code, merged conservatively across `if`/`match`/`while`
branches):

```brat
let s = "payload"
let view = s        // borrow starts
print(view)         // last read of view; borrow ends here
let owned = move s  // accepted
```

## Checked rules

| Condition | Diagnostic |
|---|---|
| Read after `move` | `use of moved \`x\`` + `note: \`x\` moved here` |
| `move` while borrowed | `cannot move \`x\` while borrowed by \`y\`` |
| Reassign without `let mut` | `assign to immutable \`x\` (want \`let mut\` or mut param)` |
| Reassign `const` | `assign to const \`x\`` |
| Mutating method on immutable receiver | `cannot mutate immutable \`x\`: \`x.push\` writes through it` |
| Overlapping mutable borrows | `double mut borrow of \`x\`` |
| Aliasing `mut` arguments in one call | `law of exclusivity: ...` |
| Move out of a borrowed parameter | `cannot move borrowed parameter ...` |
| Immutable binding passed to `mut` param | `pass immutable \`x\` as mut param \`p\`` |
| Local borrow returned from function | `ref \`x\` escapes via return` |
| Local `ptr_of_str(s)` returned | `` `ptr_of_str` borrows local `s`, which dies on return `` |
| Local borrow captured by `spawn` | `ref \`x\` escapes via spawn` |
| Mutate container during `for` loop | `cannot mutate \`v\` while iterating it` |
| Unfreed `move ptr` from FFI | `leaked owned pointer \`p\`` |
| Missing trait method in `impl` | `impl Shout for Point missing loud` |

### Container iteration lock

A `for x in v` loop holds an immutable view over `v` for the entire loop body:

```brat check
fn main() -> i64 {
  let mut v = [1, 2, 3]
  for x in v {
    v.push(x)
  }
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:4: error: cannot mutate `v` while iterating it (hint: collect the writes and apply them after the loop)
  |     v.push(x)
example.brat:3: note: iterated here by this `for`
  |   for x in v {
example.brat: 0 type error(s)
```

Iterating over an integer range (`for i in 0..len(v)`) or a shallow copy
(`for x in v.copy()`) does not hold a view on `v`.

### Early-return cleanup paths

Branches that exit the function (`return` or `raise`) do not merge their moves
into the fallthrough state, so standard C error-cleanup idioms pass without
false double-free errors:

```brat
let p = malloc(64)
if bad_header() {
  free(move p)
  return 1
}
free(move p)        // accepted: the early-return branch never reaches here
```

`break` and `continue` stay inside the enclosing function, so moving an outer
binding before `break` still marks it moved after the loop. `defer` extends
borrow liveness to scope exit (`defer free(move p)` allows reading `p` in the
body while rejecting a second `free(move p)`), and `try`/`catch` propagates any
move that could occur before an error inside `try` into `catch`.

## Reference-cycle prevention at run time

Because heap containers use reference counting without a cycle collector,
container mutations (`v[i] = x`, `v.push(x)`, `v.ins(i, x)`, `m.set(k, x)`)
maintain an owner-parent forest (`child -> parent`) and reject insertions that
would close a cycle (`v.push(v)`, or `a.push(b); b.push(a)`). Graphs that need
cycles must represent edges as integer indices into a vector or allocate nodes
inside an `arena` block.

## Thread sharing and channel sessions

`spawn` copies the local frame bindings while sharing heap allocations behind
them. Until `join_all()`, `join(h)` or the end of the enclosing `scope { ... }`:

- Multiple concurrent readers of a binding are allowed.
- A write inside `spawn` conflicts with any read or write in the parent (and
  vice versa), including foreign writes declared via `writes(p)`, out-parameters
  (`&x`), `ptr_write_*`, `slice_set`, and `move` transfers (`free(move p)`).

Exchange mutable values across threads through `chan(cap)`: `ch.send(x)` moves
non-Copy `x` into the channel (`use of sent \`x\`` on reuse), and `session.cpp`
rejects straight-line channel protocols that overflow `cap` or block forever
without a matching `recv`/`close`.

## What is not checked

1. **Inter-procedural aliasing.** Signatures declare `move`, `ref` and `mut` per
   parameter, not relational lifetimes between two returned views or nested
   containers.
2. **Projection move invalidation at run time.** Under `--no-check`, whole-name
   `move x` still trips `V_MOVED` at run time, whereas `move p.field` does not
   poison `p.field`.
3. **Opaque foreign code.** If a C function mutates through a `ptr` without
   declaring `writes(p)` or retains a borrowed pointer past return without
   taking `move p: ptr`, the checker cannot see it.
4. **JSON diagnostics.** Pass `brat-cxx check --json file.brat` to consume
   machine-readable error codes (such as `E0382` and `B0522`), spans and fix
   hints without parsing terminal text.
