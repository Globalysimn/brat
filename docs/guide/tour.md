# Language Tour

Build the native compiler with `make -j2` from the repository root. You need a
C++20 compiler and a C compiler; Python is not required.

```sh
./lang/cxx/bin/brat-cxx check file.brat
./lang/cxx/bin/brat-cxx run file.brat
./lang/cxx/bin/brat-cxx build -O2 file.brat -o file
./file
```

Blocks marked `brat run` and `brat check` below are compiled and checked by
`lang/tests/test_docs_examples.py`. Additional complete programs live in
`lang/examples/` (64 files).

## 1. Top level and `main`

Top-level statements execute in source order. A `main` function is optional;
when defined as `fn main() -> i64` or `fn main() -> Result[i64, str]`, its
returned integer (or `Ok(code)`) sets the process exit code, while `Err(e)`
prints to `stderr` and exits with status `1`.

```brat
let host = "127.0.0.1:9001"
let view: ref str = host
print("listening on {view}")
```

`"{expr}"` interpolates any BRAT expression inside a string literal. Braces that
do not parse as a single expression stay literal text, so JSON strings such as
`"{\"a\": 1}"` do not need brace escaping.

## 2. Scalars, containers and records

Scalar types are `i64`, `f64`, `bool` and `str`. Containers are `vec[T]` and
`map`. Ordinary struct literals require no `struct` declaration: `Point { x: 1,
y: 2 }` constructs a tagged map with tag `"Point"`.

```brat
let mut codes = [200, 404]
codes.push(500)
let routes = {"GET": 1, "POST": 2}
let p = Point { x: 10, y: 20 }
print(p.x + p.y)

for c in codes {
  print(c)
}
for i in 0..3 {
  print(i)          // 0, 1, 2
}
```

Integers are signed 64-bit (`i64`), support decimal, `0x` hex, `0b` binary and
`0o` octal literals, and wrap on `+`, `-`, `*` and unary `-`. Bitwise operators
`&`, `|`, `^`, `~`, `<<` and arithmetic `>>` work directly on `i64`.
Integer `/` rounds toward negative infinity and `%` takes the sign of the
divisor. `i64` and `f64` never convert implicitly; convert explicitly or write a
floating-point literal (`2.0`).

## 3. Ownership: `move`, `ref`, `mut`

Bindings are immutable by default; reassignment or mutating methods (`push`,
`pop`, `set`, `del`, `sort`) require `let mut` or a `mut` parameter.

Assigning a non-Copy value (`let r = s`) creates a borrow that ends at the last
read of `r` (non-lexical borrow lifetime within a function). `move s` transfers
ownership and invalidates `s`:

```brat
fn eat(x: move str) { print(x) }
fn peek(x: ref str) { print(x) }

fn main() -> i64 {
  let s = "hello"
  peek(s)           // borrow ends when peek returns
  eat(move s)       // transfers ownership
  return 0
}
```

See [ownership.md](ownership.md) for the exact place rules and checker limits,
or run `./bin/brat explain E0382` to inspect any diagnostic code in the terminal.

## 4. Errors as values, `Option[T]` and `Result[T, E]`

`Ok(v)`, `Err(e)`, `Some(v)` and `None` are built-in tagged values, typed as
`Result[T, E]` and `Option[T]` when annotated (or left dynamic in unannotated
functions). `?` unwraps `Ok(v)` / `Some(v)`, returning `Err(e)` early from
`-> Result[T, E]` functions and unwinding to `try` / `catch` otherwise;
`unwrap_or(x, fallback)` (or `x.unwrap_or(fb)`) evaluates `fallback` lazily only
on `Err`/`None`; `or` provides a fallback for `None` without swallowing `Err`.

`try`/`catch` catches both `Err` unwinding and runtime errors (division by zero,
out-of-bounds indexing, missing map keys):

```brat run
fn main() -> i64 {
  try {
    if true {
      defer print("cleanup")
      raise("boom")
    }
  } catch e {
    print("caught: {e}")
  }
  return 0
}
```

```output
cleanup
caught: boom
```

`defer` statements execute in reverse registration order (LIFO) at scope exit on
both normal control flow and error unwinding.

## 5. `match`, enums, structs and `impl` methods

BRAT does not require upfront type boilerplate: `Point { x: 3, y: 4 }` creates an
ad-hoc tagged record immediately, while `struct Point { x: i64, y: i64 }` opts
into fixed-slot layout and field checking. Likewise, `enum` variants can omit
types (`Done(ms)`) or declare payload types (`Done(ms: i64)`, `Click(x: i64, y: i64)`);
`match` checks exhaustiveness across local and imported `pub enum`s, `Option[T]`,
`Result[T, E]`, and `bool` unless a `_` catch-all arm is present.

`impl Type` attaches methods (`self`, `mut self`, `move self`) and associated
functions without `self` (`Type.make(...)`):

```brat run
impl Point {
  fn make(x: i64, y: i64) {
    return Point { x: x, y: y }
  }
  fn sum(self) -> i64 {
    return self.x + self.y
  }
}

fn main() -> i64 {
  let p = Point.make(3, 4)
  print(p.sum())
  return 0
}
```

```output
7
```

User `impl` methods always take precedence over built-in container or string
methods of the same name. Built-in methods are arity-checked during `brat-cxx
check`.

## 6. Closures and callbacks

Closures (`fn(x) { ... }`) capture outer bindings by shallow copy and can be
stored in variables or containers. Named top-level functions
(`fn double(x: i64) -> i64`) are not runtime values (`let f = double` is an
error): pass them directly as callbacks (`[1, 2, 3].map(double)`) to `map`,
`filter`, `fold`, `any`, `all`, `find` and `sort_by`, or wrap them in
`let f = fn(x) { return double(x) }`.

## 7. Memory and `arena` blocks

Strings, vectors, maps, tags and closures are reference-counted. For request
handlers or parsers that allocate many short-lived objects, `arena { ... }`
allocates from 64 KiB thread-local bump chunks and releases the entire region on
block exit:

```brat
arena {
  let parts = raw.split(",")
  let line = parts.join(":")
  print(line)
}
```

The checker rejects returning or storing an arena-allocated value outside the
block unless copied to the heap with `own(x)`. See [ARENA.md](../design/ARENA.md).

## 8. Threads, `scope` and bounded channels

`spawn { ... }` runs on an OS worker thread. `scope { ... }` joins all tasks
spawned inside it (including nested spawns) before exiting the block;
`join_all()` joins tasks spawned in the current frame.

While a `spawn` is live, the checker enforces single-writer / multiple-reader
access on captured bindings until a `join_all()` or `scope` boundary intervenes.
To pass values between threads, use a bounded channel (`chan(capacity)`).
`ch.recv()` returns the next sent value, or `None` once the channel is closed
and drained:

```brat run
fn main() -> i64 {
  let ch = chan(2)
  ch.send(41)
  ch.close()
  let first = ch.recv()
  let after_close = ch.recv()
  print(first + 1, is_none(after_close))
  return 0
}
```

```output
42 true
```

## 9. Built-in structured logging and spans

`log.info(...)`, `log.warn(...)` and `trace.span("name") { ... }` are built into
the parser and emit newline-delimited JSON records with timestamps and active
span stacks to stdout without external dependencies.

## 10. Modules, standard library and C FFI

- `import a.b` loads `a/b.brat` relative to the current file or `vendor/`, or
  from the 12 built-in `std.*` modules (`std.json`, `std.http`, `std.sqlite`,
  `std.crypto`, `std.fs`, `std.path`, `std.cli`, `std.csv`, `std.env`,
  `std.proc`, `std.bytes`, `std.time`). Details: [modules.md](modules.md) and
  [stdlib.md](stdlib.md).
- `extern fn`, `extern struct`, `extern union`, `ptr[T]`, `slice_of_ptr`,
  callbacks, `--header` / `--object` verification and `brat-cxx bindgen`:
  [ffi.md](ffi.md).
- Complete CLI flags, environment variables, and `brat explain` diagnostic
  codes: [cli.md](cli.md).
- Complete runnable recipes (JSON+`arena`, index-backed LRU list, Pike-VM
  regexes, SQLite, HMAC, `par for`): [by-example.md](by-example.md).

## 11. Omissions by design or current scope

- Generics are type-erased rather than monomorphized.
- No trait objects, macros, async/await or coroutines.
- No unsigned integer types outside FFI boundaries (`i64` right-shifts
  sign-extend) and no `**` exponentiation operator.
- No centralized package registry (`brat-cxx pkg` resolves against local files,
  GitHub release/ref tarballs or a JSON index URL you control).
- Detailed edge cases, measurements, and design Q&A: [limits.md](limits.md) and
  [faq.md](faq.md).
