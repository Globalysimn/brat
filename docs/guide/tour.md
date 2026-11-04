# The language

Commands are from the repository root. Runnable programs live in
`lang/examples/`; the snippets below are fragments of those programs.

```
python3 lang/engine/cli.py check file.brat    # ownership + types
python3 lang/engine/cli.py run   file.brat    # interpreter
python3 lang/engine/cli.py build file.brat -o file && ./file   # native
```

`<ts>` in output is a timestamp and changes per run.

## 1. Top level and `main`

Top level is a script: statements run in order, no `main` required.

```
let host = "127.0.0.1:9001"
let r: ref str = host
log.info("hello {host}")
print(r)
```

`main` is optional; when it exists, its `i64` return value is the process exit
code. `?` unwraps a value and propagates an error; `ref str` is a view on the
string, not a copy.

## 2. Values

Base types: `i64`, `f64`, `bool`, `str`. Containers: `vec[T]`, `map`. Structs
and enums are values too, and a struct literal is no declaration — `Point { x:
1, y: 2 }` is a tagged map, and `Point` is only a tag.

```
let mut codes = [200, 404]
codes.push(500)
let by_name = {"get": 1, "put": 2}
let p = Point { x: 1, y: 2 }
print(p.x + p.y)
for c in codes {
  print(c)
}
for i in 0..3 {
  print(i)          // 0, 1, 2
}
```

Integer arithmetic is i64 and wraps on overflow like Rust release builds; `/`
is floor division like Python. `f64` is separate: no implicit conversion either
way.

## 3. Ownership

```
fn main() -> i64 {
  let s = "x"
  let r = s          // borrow: r is a view of s
  print(r)           // last use of r
  let m = move s     // legal: the borrow ended at its last use
  print(m)
  return 0
}
```

`x` printed twice is expected — `move` transfers ownership, not storage. The
checker is the gate: it is intra-function, it tracks paths (`x`, `x.f`, `x[i]`),
and it reports the move or the borrow that conflicts.

`let mut` is required to assign again. A parameter declared `mut`, `ref` or
`move` says what the callee does with it, and the call site is checked against
that:

```
fn eat(x: move str) { print(x) }
fn peek(x: ref str) { print(x) }

let s = "hi"
peek(s)
eat(move s)
```

## 4. Errors as values

`Ok`/`Err`/`Some`/`None` are runtime tags. `?` unwraps `Ok`/`Some` and
propagates `Err`.

```
fn parse_port(s: str) -> i64 {
  let p = s.parse_int()
  if is_err(p) {
    return 80
  }
  return unwrap(p)
}
```

`or` supplies a value for `None` only, not for `Err`. `try`/`catch` catches both
an `Err` and a runtime error — division by zero, an index out of range, a
missing key — in both backends:

```
try {
  let x = 10 / 0
} catch e {
  print(e)            // div by zero
}
```

An uncaught error at the top prints `unhandled: ('err', ...)` and exits 1.

## 5. `defer`

`defer` runs at the end of the block on every path, including while an error
unwinds, last registered first.

```
try {
  if true {
    defer print("cleanup")
    raise("boom")
  }
} catch e {
  print("caught")     // cleanup, then caught
}
```

## 6. Match, enums, methods

```
enum Color { Red, Blue }

fn tag_of(c) -> str {
  match c {
    Red => { return "red" },
    Blue => { return "blue" }
  }
  return "?"
}

match "nope".parse_int() {
  Ok(v) => { print(v) },
  Err(e) => { print(e) }
}
```

`match` arms take a literal, a binding, `_`, a nested pattern (`Has(Ok(v))`) or
`x if x > 0`. Missing a case is a runtime error, not a compile error, unless the
subject is a fixed tag set. Methods come from `impl`:

```
impl Point {
  fn sum(self) -> i64 { return self.x + self.y }
}
print(Point { x: 1, y: 2 }.sum())
```

A user method always beats a builtin of the same name, in both backends.

## 7. Closures

Capture is by copy; the heap a binding points at stays shared.

```
let f = fn(x) { return x * 2 }
print(f(21))
print([1, 2, 3].map(f))
```

A named function is a callback (`v.map(f)`, `v.filter(f)`, `v.fold(0, add)`,
`v.any/all/find/sort_by`), not a value: `let g = f` is a checker error, because
there are no first-class function values.

## 8. Memory

Containers own their elements, so leaving a scope or a loop frees the tree.
Strings, containers and tags are refcounted and a copy is never freed twice.

```
arena {
  let v = [1, 2, 3]
  // the whole block is released at the exit
}
```

`own(x)` copies a value out of an arena. The escape rules are checked: a value
made inside the block cannot leave it.

## 9. Concurrency

```
let ch = chan(2)
ch.send(Ok(41))
ch.close()
match ch.recv() {
  Ok(v) => { print(v + 1) },
  Err(e) => { print("closed") }
}

scope {
  spawn { print("joined") }
}
```

Real OS threads in both backends. A channel is bounded, `send` hands the value
over, and the checker reports using a binding after sending it. `scope { }`
joins every spawn started inside it, including spawns made by those spawns;
`join_all()` joins whatever the current frame started.

Sharing follows one rule: while a spawn runs, exactly one side may write the
value it shares with the parent.

```
let mut cell = [0]
spawn { print(cell[0]) }   // child reads
cell[0] = 5                // error: read inside `spawn`, written here
join_all()
cell[0] = 5                // fine: the join is the barrier
```

Two readers are fine, and so is a writer on one side after `join_all()`. The
error names the binding and the way out: `join_all()` first, or `send` the value
through a channel. `brat explain B0522` says the same thing offline.

## 10. Telemetry

`log`, `trace` and `span` are calls the parser knows, not a dependency.

```
log.info("up {1} ok")
trace.span("request") {
  print("inside span")
}
```

```
{"ts": <ts>, "lvl": "INFO", "msg": "up 1 ok"}
{"ts": <ts>, "span": ["request"]}
inside span
```

## 11. Modules

`import a.b` loads `a/b.brat` next to the file or from `vendor/`, flattens it
into the same program, and mangles its functions. `pub fn` is the export.
Details, interfaces and re-exports: `modules.md`.

## 12. C

`extern fn`, `extern struct`, `ptr`, `slice`, callbacks, `brat bindgen`,
`export fn`, run-time plugins: `ffi.md`.

## 13. Not implemented

Generics are erased rather than monomorphized; no trait objects; no macros; no
async; no first-class function values; no borrow across a thread; no bitwise
operators, shifts or `**` (the lexer refuses `|`, `^` and `~`). Packages
support a local index and exact transitive dependencies, but have no hosted
registry or version-range solving; see `modules.md`. [limits.md](limits.md) has
the rest, with programs that show each limit.

More programs: `lang/examples/` (64 files), each referenced by the test suite.
