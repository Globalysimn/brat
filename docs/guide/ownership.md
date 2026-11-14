# Ownership

`brat-cxx check` follows who owns each value inside one function. It needs no
lifetime annotations, because it does not follow borrows across calls: between
functions a value is kept alive by its reference count. That makes it a checker
for the common local mistakes (use after move, a container changed during its
loop, a write shared between threads) and not a proof. What it misses is listed
at the end and in [LEDGER.md](../project/LEDGER.md).

```sh
./lang/cxx/bin/brat-cxx check file.brat
python3 lang/tests/test_ownership.py
python3 lang/tests/test_cxx_borrow.py
python3 lang/tests/test_defer_try_soundness.py
```

## Parameters

A parameter says how the function takes its argument:

- `ref`, the default: reads it for the length of the call.
- `mut`: reads and writes it. The argument has to be a `let mut` that nothing
  else in the same call uses.
- `move`: takes it over. The caller's name is dead afterwards.

The checker tracks paths: `x`, `x.f`, `x.f.g`, and `x[i]` with every index of
`x` counted as one place.

## Moving a field

`move x` kills the whole name. `move p.x` kills only that field: `p.y` stays
readable, and a second read of `p.x` is `E0382`.

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

At run time the slot of `p.x` is not emptied. The field's count goes up by one
and the slot keeps its reference until `p.x` is overwritten or `p` is dropped.

## Next to Rust

| | Rust | BRAT |
|---|---|---|
| Freeing | `Drop` at a point known statically | Reference count, `move`, `arena` |
| Lifetimes in signatures | `'a` | None. Borrows do not cross a call |
| `move p.x` | The whole `p` is partly moved | `p.x` is dead, `p.y` lives |
| Cycles | `Rc` cycles leak | Refused inside a function, leak across functions unless in an `arena` |
| Threads | `move` closure, `Send`, `Sync` | `spawn` copies the frame. Shared reads are allowed, shared writes refused |
| Raw pointers | `*mut T` in `unsafe` | `move ptr` ownership and declared `writes(p)`, checked across `spawn` |

## When a borrow ends

A borrow ends at the last read of the name that holds it, wherever that read is:

```brat
let s = "payload"
let view = s
if verbose {
  print(view)       // last read of view; borrow ends at the end of the branch
}
let owned = move s  // accepted
```

Branches are merged without reasoning about their conditions. After
`if cond { move s }` the checker treats `s` as possibly moved, so a second `if`
on `!cond` is refused even though only one of them runs:

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

Write it as `if cond { ... } else { ... }` or a `match`.

## Checked rules

| Condition | Diagnostic |
|---|---|
| Read after `move` | `use of moved \`x\`` + `note: \`x\` moved here` |
| `move` while borrowed | `cannot move \`x\` while borrowed by \`y\`` |
| Reassign without `let mut` | `assign to immutable \`x\` (want \`let mut\` or mut param)` |
| Reassign `const` | `assign to const \`x\`` |
| Mutating method on immutable receiver | `cannot mutate immutable \`x\`: \`x.push\` writes through it` |
| Overlapping mutable borrows | `double mut borrow of \`x\`` |
| Aliasing `mut`+`mut` or `ref`+`mut`/`move` in one call, or binary read+`move` | `law of exclusivity: ...` (`E0499`) |
| Move out of a borrowed parameter | `cannot move borrowed parameter ...` |
| Immutable binding passed to `mut` param | `pass immutable \`x\` as mut param \`p\`` |
| Local borrow returned from function | `ref \`x\` escapes via return` |
| Local `ptr_of_str(s)` returned | `ref \`s\` escapes via return` |
| Local borrow captured by `spawn` | `ref \`x\` escapes via spawn` |
| Mutate container during `for` loop | `cannot mutate \`v\` while iterating it` |
| Unfreed `move ptr` from FFI | `leaked owned pointer \`p\`` |
| Missing trait method in `impl` | `impl Shout for Point missing loud` |

### Call-site exclusivity (`E0499`)

One call cannot take the same value twice as `mut`, or as `ref` next to a `mut`
or a `move`. The same holds for reading a value on one side of an operator and
moving it on the other, as in `s + consume(move s)`.

```brat check
fn drain_and_len(mut dst: vec[str], src: vec[str]) -> i64 {
  dst.pop()
  return len(src)
}

fn main() -> i64 {
  let mut items = ["a", "b"]
  return drain_and_len(items, items)
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:8: error: law of exclusivity: `items` borrowed and mutated in one expression (clashes with `items` used by `dst`)
  |   return drain_and_len(items, items)
example.brat: 0 type error(s)
```

A borrow lasts for its call, so `fib(n - 1, memo) + fib(n - 2, memo)` with a
`mut memo` is fine. In `f(memo, g(memo))` the inner call runs while `f` already
holds `memo`, and that is refused.

## Loops and closures

`for x in v` holds `v` for the whole body. Changing `v` there is an error,
directly or through a closure:

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

`for i in 0..len(v)` and `for x in v.copy()` do not hold `v`. `v.copy()` is a
new vector with the same elements.

A closure gets a shallow copy of the names it uses. It cannot assign to them,
move them, or take over a `move ptr` from outside.

## Early return, `try` and `defer`

A branch that leaves the function keeps its moves to itself, so a guard clause
works:

```brat
let p = malloc(64)
if bad_header() {
  free(move p)
  return 1
}
free(move p)        // accepted: the early-return branch never reaches here
```

`break` and `continue` stay in the function: a move before `break` counts after
the loop.

A move in `try` counts in `catch` too, because the error may come after it.
Freeing in both is refused:

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

Put the cleanup in a `defer` before the `try` and only borrow inside:

```brat
let p = malloc(n)
defer free(move p)
try {
  return parse_buf(p)
} catch _e {
  return 0
}
```

The C side of this (`-> move ptr`, `move p: ptr`, `borrows(p)`) is in
[C interop](ffi.md#4-owned-pointers-borrows-and-ptr_of_str).

## Cycles

There is no cycle collector. The checker follows writes into containers inside
one function and refuses the one that closes a cycle, such as `v.push(v)`. A
cycle built across functions is not seen and leaks. Keep long-lived graphs as
indices into a vector, and build short-lived ones inside an `arena`.

## Threads

`spawn` copies the names it uses and shares the values behind them. Any number
of tasks may read a value until `join_all()`, `join(h)` or the end of the
`scope`:

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

A write inside `spawn` conflicts with any other use of the value before the
join. Writes through C count: `writes(p)`, `&x`, `ptr_write_*`, `slice_set`.

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

To hand a value to another thread, send it. `ch.send(x)` moves `x` into the
channel, and using `x` afterwards is ``use of sent `x` ``.

## Error codes

`brat-cxx check --json file.brat` prints the same diagnostics as JSON with
codes, spans, notes and hints. `brat explain E0382` shows an example for a code.
The table of codes is in
[CLI](cli.md#3-diagnostic-code-index-brat-explain-code).

## What is not checked

1. Aliasing across calls. A signature says `move`, `ref` or `mut` for each
   parameter and nothing about how a returned value relates to them.
2. `move p.f` under `--no-check`. `move x` is also caught at run time, a moved
   field is caught only by the checker.
3. C that does more than it declares: a write through a `ptr` without
   `writes(p)`, or a pointer kept past the call without `move p: ptr`.
