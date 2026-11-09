# Ownership and Borrow Checking

`brat-cxx check` runs an intra-function ownership, mutability, and concurrency
pass over every BRAT function without `'a` lifetime annotations or `RefCell` /
`Arc<Mutex<T>>` wrappers. Memory reclamation relies on deterministic reference
counting, value moves, and lexical `arena` blocks rather than inter-procedural
lifetime solving. Passing `check` catches common local aliasing, use-after-move,
iterator invalidation, and thread-sharing mistakes while keeping function
signatures flat; it is not a complete soundness proof. Remaining gaps are listed
at the end of this page and in [LEDGER.md](../project/LEDGER.md).

```sh
./lang/cxx/bin/brat-cxx check file.brat
python3 lang/tests/test_ownership.py
python3 lang/tests/test_cxx_borrow.py
python3 lang/tests/test_defer_try_soundness.py
```

## Place-based model

Every non-Copy value has one logical owner. Parameter annotations declare how a
callee accesses each argument:

- `ref` (default when omitted): read-only borrow for the duration of the call.
- `mut`: exclusive read-write borrow for the duration of the call; requires a
  mutable place (`let mut`) with no overlapping live borrows in the same call.
- `move`: transfers ownership into the callee and invalidates the caller's place.

Places are tracked at path granularity (`x`, `x.f`, `x.f.g`, `x[i]`, where all
indices of `x` map to one element place).

### Whole-binding moves vs projection moves (`move p.x`)

Whole-variable `move x` invalidates `x` both statically and at run time
(`V_MOVED`). Projection moves (`move p.x`) work differently from Rust partial
moves:

- Statically, `move p.x` marks `p.x` moved while leaving sibling fields such as
  `p.y` readable; reading or moving `p.x` a second time before re-assigning it
  is rejected (`E0382`).
- At run time, container and struct slots never hold uninitialized poison holes,
  so `move p.x` increments the field value's reference count when transferring
  it out while the parent slot retains its reference until `p.x` is overwritten
  or `p` itself is dropped.

```brat check
struct Pair { x: str, y: str }

fn consume(s: move str) {
  print(s)
}

fn main() -> i64 {
  let p = Pair { x: "left", y: "right" }
  consume(move p.x)
  print(p.y)
  consume(move p.x)
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:11: error: use of moved `p.x`
  |   consume(move p.x)
example.brat: 0 type error(s)
```

### Comparison with Rust

| Aspect | Rust | BRAT |
|---|---|---|
| Heap reclamation | Static drop (`Drop`) | Reference counting + `move` + lexical `arena` |
| Lifetimes in signatures | Explicit (`'a`) | None; borrows stay intra-function |
| Moving a struct field (`move p.x`) | Partial move invalidates whole `p` | `p.x` marked moved statically; `p.y` stays live; RC `+1` at run time |
| Reference cycles | Allowed (`Rc` leaks silently) | Intra-function cycles rejected statically; cross-function cycles leak unless in `arena` |
| Thread `spawn` | `move` closure + `Send` / `Sync` | Copies frame bindings; concurrent readers share RC heap; concurrent writes rejected |
| Foreign pointer writes | `*mut T` inside `unsafe` | Declared `move ptr` ownership plus `writes(p)` / `&x` checked across `spawn` and borrows |

## Borrow liveness, branches, and known false positives

Borrows end at the last read of the borrowing binding, including reads inside
`if`, `match`, and `while` blocks:

```brat
let s = "payload"
let view = s
if verbose {
  print(view)       // last read of view; borrow ends at the end of the branch
}
let owned = move s  // accepted
```

Branch joins are path-insensitive: the checker merges move states across
branches without proving relationships between separate boolean conditions. Two
`if` statements guarded by `cond` and `!cond` therefore trigger a conservative
false positive because `s` looks possibly moved after the first `if`:

```brat check
fn consume(s: move str) {
  print(s)
}

fn send_once(cond: bool, s: move str) {
  if cond {
    consume(move s)
  }
  if !cond {
    consume(move s)
  }
}

fn main() -> i64 {
  send_once(true, "pkt")
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:10: error: use of moved `s` (hint: the value was moved; use a copy or re-bind)
  |     consume(move s)
example.brat:7: note: `s` moved here
  |     consume(move s)
example.brat: 0 type error(s)
```

Write mutually exclusive moves as `if cond { ... } else { ... }` (or `match`) so
the checker sees both arms together.

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

### Container iteration lock and `.copy()`

A `for x in v` loop holds a read-only view over `v` for the entire loop body:

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

Iterating over an index range (`for i in 0..len(v)`) or a shallow copy
(`for x in v.copy()`) does not lock `v`. For `vec[T]`, `v.copy()` allocates a
new vector buffer and copies each element `Val` (copying scalar `i64`/`f64`/`bool`
values directly and incrementing reference counts for heap `str` or nested
container elements), so `v.push(...)` reallocating `v` cannot invalidate `x`.

### Early returns, `defer`, and cleanup across `try`/`catch`

Branches that exit the function (`return` or `raise`) do not merge their moves
into the fallthrough path, so C-style guard clauses work without false
double-free errors:

```brat
let p = malloc(64)
if bad_header() {
  free(move p)
  return 1
}
free(move p)        // accepted: the early-return branch never reaches here
```

`break` and `continue` stay inside the current function, so moving an outer
binding before `break` marks it moved after the loop.

Inside `try { ... } catch e { ... }`, any move that may happen before an error
in `try` propagates into `catch`. Freeing `move p` in both `try` and `catch` is
rejected because an error raised after the first `free(move p)` would double-free
`p` in `catch`:

```brat check
extern fn malloc(n: i64) -> move ptr;
extern fn free(p: move ptr);

fn parse_buf(p: ptr) -> i64 {
  if p == 0 {
    raise("bad ptr")
  }
  return 1
}

fn bad(n: i64) -> i64 {
  let p = malloc(n)
  try {
    let r = parse_buf(p)
    free(move p)
    return r
  } catch _e {
    free(move p)
    return 0
  }
}

fn main() -> i64 {
  return bad(64)
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:18: error: use of moved `p` (hint: the value was moved; use a copy or re-bind)
  |     free(move p)
example.brat:15: note: `p` moved here
  |     free(move p)
example.brat: 0 type error(s)
```

Bind cleanup with `defer free(move p)` before entering `try` and borrow `p` inside
the body (`defer` keeps `p` readable throughout the scope while preventing a
second `free(move p)`). If a callee itself takes `p: move ptr`, that callee owns
freeing `p` on all of its internal error paths:

```brat
let p = malloc(n)
defer free(move p)
try {
  return parse_buf(p)
} catch _e {
  return 0
}
```

Details on declaring `-> move ptr`, `move p: ptr`, and `borrows(p)` on C
functions are in [C Interop (FFI)](ffi.md#4-owned-pointers-borrows-and-ptr_of_str).

## Static reference-cycle rejection

Because heap containers do not have a cycle collector, the checker tracks
intra-function ownership edges across container writes (`v[i] = x`, `v.push(x)`,
`v.ins(i, x)`, `m.set(k, x)`, `m[k] = x`) and rejects mutations that close a
local cycle (`v.push(v)`, or `a["next"] = b; b["prev"] = a`). Cycles assembled
across separate functions are not caught; represent long-lived cyclic graphs
with integer indices into a `vec` or build short-lived cyclic structures inside
an `arena` block.

## Thread sharing and channel sessions

`spawn` copies the local frame bindings while sharing the reference-counted heap
objects behind them (upgrading reference counts to atomic operations). Until
`join_all()`, `join(h)`, or the end of the enclosing `scope { ... }`, multiple
concurrent readers are allowed:

```brat run
fn main() -> i64 {
  let banner = "ready"
  let ch = chan(2)
  scope {
    spawn { ch.send(len(banner)) }
    spawn { ch.send(len(banner)) }
  }
  print(ch.recv() + ch.recv())
  return 0
}
```

```output
10
```

Any write inside `spawn` conflicts with concurrent reads or writes in sibling
tasks or the parent frame, including foreign writes declared via `writes(p)`,
out-parameters (`&x`), `ptr_write_*`, `slice_set`, and `move` transfers:

```brat check
fn main() -> i64 {
  let mut v = [1, 2]
  scope {
    spawn { v.push(3) }
    spawn { print(len(v)) }
  }
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:5: error: `v` is written inside `spawn` and touched again with no `join_all()` in between (hint: send the value through a channel)
  |     spawn { print(len(v)) }
example.brat: 0 type error(s)
```

Pass mutable values across threads over `chan(cap)`: `ch.send(x)` moves non-Copy
`x` into the channel (`use of sent \`x\`` on reuse), and straight-line channel
protocols that exceed `cap` or block without a matching `recv`/`close` are
rejected statically.

## Machine-readable diagnostics (`--json` and error codes)

`brat-cxx check --json file.brat` emits structured JSON diagnostics with error
codes (`E0382`, `E0502`, `B0522`, `F0001`, `W0001`, ...), line/column spans,
notes, and fix hints used by `brat lsp` and CI scripts. Run `brat explain E0382`
for a terminal example, or see the full code table in
[CLI & Diagnostic Codes](cli.md#3-diagnostic-code-index-brat-explain-code).

## What is not checked

1. Inter-procedural aliasing: signatures declare `move`, `ref`, and `mut` per
   parameter, not lifetime relationships between returned views or nested
   containers.
2. Runtime projection poisoning under `--no-check`: whole-name `move x` still
   trips `V_MOVED` at run time when `--no-check` is passed, whereas `move p.f`
   is enforced only by static checking.
3. Unannotated foreign mutations: if a C function mutates memory through `ptr`
   without `writes(p)` or stores a borrowed pointer past return without taking
   `move p: ptr`, the checker cannot see it.
