# Language tour

Build the compiler with `make -j4` at the repository root, then:

```sh
./bin/brat run file.brat
./bin/brat check file.brat
./bin/brat build -O2 file.brat -o file
```

Every block below marked `brat run` or `brat check` is run in CI together with
the output printed under it.

## Values

```brat run
let name = "Ada"
let mut visits = 2
visits += 1
let scores = [90, 72, 85]
let ages = {"Ada": 36, "Alan": 41}
let p = Point { x: 3, y: 4 }

print("{name} came {visits} times, best score {scores.max()}")
print(ages["Alan"], p.x + p.y, len(scores))
print(7 / 2, -7 / 2, 7 % -2, 7 / 2.0, i64(3.9), f64(2))
print(len("naïve"), "naïve".upper(), ord("ï"), chr(960))
```

```output
Ada came 3 times, best score 90
41 7 3
3 -4 -1 3.5 3 2.0
5 NAÏVE 239 π
```

A binding is fixed unless it says `let mut`. The scalar types are `i64`, `f64`,
`bool` and `str`; the containers are vectors and maps, and a map keeps its keys
in insertion order. `Point { x: 3, y: 4 }` needs no declaration.

`"{expr}"` puts the value of an expression into a string. Braces that do not
hold an expression stay as they are, so `"{\"a\": 1}"` is the JSON it looks
like.

Integers are 64-bit and wrap on overflow. `/` on two integers rounds down and
`%` takes the sign of the divisor. An integer next to a float becomes a float;
a float becomes an integer only through `i64(x)`. Strings are UTF-8, and `len`,
indexing and `for ch in s` count code points.

Nothing is converted behind your back: `"a" + 5`, `true + 1` and `if "x"` are
compile errors.

```brat check
fn main() -> i64 {
  let n = 5
  print("items: " + n)
  if n {
    print("some")
  }
  return 0
}
```

```diagnostics
example.brat: 0 error(s), 0 warning(s)
example.brat: 2 type error(s)
example.brat:3: error: operator `+` joins two strings, got str and i64 (hint: `str(x)` makes the string)
  |   print("items: " + n)
example.brat:4: error: condition wants bool, got i64
  |   if n {
```

## Functions

```brat run
fn area(w: i64, h: i64) -> i64 {
  return w * h
}

fn describe(x) {
  if is_str(x) {
    return "text"
  }
  return "something else"
}

fn main() -> i64 {
  let twice = fn(n) { return n * 2 }
  print(area(3, 4), describe("hi"), describe(1.5), twice(21))
  print([1, 2, 3].map(twice).filter(fn(n) { return n > 2 }))
  return 0
}
```

```output
12 text something else 42
[4, 6]
```

Type annotations are optional. An annotated function is checked at its call
sites and compiles to plain C types; an unannotated one takes any value.

A function returns with `return`. The last expression is not returned for you,
and a function that declares `-> T` and can reach its end without a `return` is
a compile error.

A closure gets a copy of each binding it captures. It can read the copy but not
assign to it; a vector or a map behind the binding is shared, so that is where
a counter keeps its state:

```brat run
fn make_counter(step: i64) {
  let mut n = [0]
  return fn() {
    n[0] = n[0] + step
    return n[0]
  }
}

fn main() -> i64 {
  let count = make_counter(3)
  count()
  count()
  print(count())
  return 0
}
```

```output
9
```

A named function is not a value you can store. Pass it by name to `map`,
`filter`, `fold`, `any`, `all`, `find` or `sort_by`, or wrap it in a closure.

## Control flow

```brat run
fn main() -> i64 {
  let mut total = 0
  for i in 0..4 {
    total += i
  }
  for word in ["a", "b"] {
    print(word)
  }
  let mut n = 10
  while n > 1 {
    n = n / 3
  }
  if total == 6 && n == 1 {
    print("total {total}, n {n}")
  } else {
    print("unexpected")
  }
  return 0
}
```

```output
a
b
total 6, n 1
```

`if`, `while`, `for` and `match` are statements. `0..4` stops before 4. The
logical operators are `&&`, `||` and `!`.

## Structs, enums and `match`

```brat run
struct Rect { w: i64, h: i64 }

enum Shape {
  Box(r),
  Circle(radius: i64),
  Dot,
}

impl Rect {
  fn square(side: i64) {
    return Rect { w: side, h: side }
  }
  fn area(self) -> i64 {
    return self.w * self.h
  }
}

fn size(s) -> i64 {
  match s {
    Box(r) => { return r.area() },
    Circle(radius) => { return 3 * radius * radius },
    Dot => { return 0 },
  }
}

fn main() -> i64 {
  print(size(Box(Rect.square(4))), size(Circle(2)), size(Dot))
  return 0
}
```

```output
16 12 0
```

Declaring a `struct` fixes its fields and gives it a flat layout. `impl` adds
methods, which take `self`, and functions called on the type itself. A `match`
on an enum, an `Option`, a `Result` or a `bool` has to cover every case or end
in `_`; a missing case is a compile error.

## Errors

A function that can fail returns `Ok(value)` or `Err(reason)`. `?` after a call
takes the value out of an `Ok` and leaves the current function on an `Err`.
Lookups that can come back empty answer the value or `None`, and `or` supplies
a fallback.

```brat run
fn port(text: str) -> Result[i64, str] {
  let n = text.parse_int()?
  if n < 1 || n > 65535 {
    return Err("port out of range: {n}")
  }
  return Ok(n)
}

fn main() -> i64 {
  for text in ["8080", "99999", "http"] {
    match port(text) {
      Ok(n) => print("listening on {n}"),
      Err(why) => print("refused: {why}"),
    }
  }
  let limits = {"cpu": 4}
  print(limits.get("mem") or 512)
  return 0
}
```

```output
listening on 8080
refused: port out of range: 99999
refused: not a number
512
```

Run-time faults (an index out of range, a missing key, division by zero) and a
failed `?` in a function that does not return a `Result` unwind to the nearest
`try`. `defer` runs on the way out of its block whichever way the block ends.

```brat run
fn third(xs) {
  defer print("checked {len(xs)} items")
  return xs[2]
}

fn main() -> i64 {
  try {
    print(third([1, 2, 3]))
    print(third([1]))
  } catch e {
    print("caught: {e}")
  }
  return 0
}
```

```output
checked 3 items
3
checked 1 items
caught: index out of range
```

`raise(x)` is caught by a `try` around it in the same function. With no such
`try` it makes the function return `Err(x)`, so the caller sees a value and
decides with `?` or `match`. Calling such a function and dropping what it
returns is a compile error:

```brat check
fn save(n) {
  if n > 9 {
    raise("too big")
  }
  return n
}

fn main() -> i64 {
  save(12)
  return 0
}
```

```diagnostics
example.brat: 0 error(s), 0 warning(s)
example.brat: 1 type error(s)
example.brat:9: error: `save` can raise, and the `Err` it returns is dropped here (hint: `save(...)?` passes it on, `let _ = save(...)` ignores it)
  |   save(12)
```

An error nobody handles ends the program with status 1 and
`unhandled: Err('...')` on stderr. `brat run --trace-lines` adds the line it
came from. It is off by default because it costs recursive code its fast path.

## Ownership

Passing or assigning a string, vector or map lends it; `move` gives it away,
and the old name is dead afterwards (the README shows that error). Changing a
value through a parameter needs `mut` on the parameter, and one call cannot
both change a value and read it through another argument. Numbers and bools are
copied.

```brat check
fn append_all(mut dst, src) {
  for x in src {
    dst.push(x)
  }
}

fn main() -> i64 {
  let mut log = ["boot"]
  append_all(log, log)
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:9: error: law of exclusivity: `log` borrowed and mutated in one expression (clashes with `log` used by `dst`)
  |   append_all(log, log)
example.brat: 0 type error(s)
```

The check works on one function at a time, and a borrow ends at its last use.
There are no lifetime annotations. An element read from a vector without an
element type is a borrow of the vector, so a swap wants `vec[i64]`:

```brat run
fn main() -> i64 {
  let mut w: vec[i64] = [3, 1, 2]
  let tmp = w[0]
  w[0] = w[2]
  w[2] = tmp
  print(w)
  return 0
}
```

```output
[2, 1, 3]
```

The rules, the diagnostics and the known false positives are in
[ownership.md](ownership.md).

## Memory

Strings, vectors, maps and closures are reference counted and freed when the
last owner goes away. There is no cycle collector: the checker refuses a cycle
built inside one function, and one built across functions leaks.

`arena { ... }` allocates everything inside the block from a bump region and
frees it in one step at the end. `own(x)` copies a value out; letting one out
without it is a compile error.

```brat run
fn longest(text: str) -> str {
  arena {
    let words = text.split(" ")
    let mut best = ""
    for w in words {
      if len(w) > len(best) {
        best = w
      }
    }
    return own(best)
  }
}

fn main() -> i64 {
  print(longest("a quick brown fox"))
  return 0
}
```

```output
quick
```

## Threads

`spawn { ... }` runs a block on a worker thread and `scope { ... }` waits for
every task started inside it. While a task is running, a binding it writes
cannot be touched by anyone else; values move between threads through a bounded
channel. `recv` answers `None` once the channel is closed and empty.

```brat run
fn main() -> i64 {
  let ch = chan(4)
  scope {
    spawn {
      for i in 0..3 {
        ch.send(i * i)
      }
      ch.close()
    }
  }
  let mut sum = 0
  let mut got = ch.recv()
  while !is_none(got) {
    sum += got
    got = ch.recv()
  }
  print(sum)
  return 0
}
```

```output
5
```

`par for x in items reduce(f, unit) { ... }` runs a loop body on several
threads and folds the results in index order, see
[PARFOR.md](../design/PARFOR.md).

## The rest

- Built-in functions and the methods of strings, vectors and maps:
  [builtins.md](builtins.md).
- `import`, packages and the 12 `std.*` modules: [modules.md](modules.md),
  [stdlib.md](stdlib.md).
- Calling C: [ffi.md](ffi.md).
- Command-line flags and error codes: [cli.md](cli.md).
- Longer programs: [by-example.md](by-example.md).

## What is missing

- Generics are type-erased.
- No trait objects, macros or async.
- No unsigned integers outside the C boundary, and no `**`.
- No package registry: `brat pkg` reads local files, GitHub tarballs or an
  index URL you host.
- More in [limits.md](limits.md) and [faq.md](faq.md).
