# Limits

What the compiler and the runtime bound, what a representation costs, and where
behaviour may surprise. The programs on this page run in CI.

## Speed

Reproduce on your machine with `make bench` (`python3 scripts/bench.py`).
Median of 5 runs on Linux x86-64 (AMD EPYC 7763, GCC 13.3.0 `-O2`, Go 1.27.3, CPython 3.14.2),
except the two long CPython loops, which run once. Two runs of the same build on that
CPU model differ by up to 20% in the rows under 0.1 s (BRAT `fib(35)`: 0.050 s and 0.059 s):

| Workload | C (`gcc -O2`) | Go 1.27 (`go build`) | BRAT (`brat-cxx build -O2`) | CPython 3.14 |
|---|---:|---:|---:|---:|
| `int_loop(100M)` (wrapping `i64` arithmetic loop) | 0.126 s | 0.129 s | 0.126 s | 16.795 s |
| `vec3_sra(50M)` (loop-carried nested `struct` step) | 0.134 s | 0.155 s | 0.141 s | 90.569 s |
| `fib(35)` (29.9M recursive `i64` calls) | 0.024 s | 0.055 s | 0.050 s | 1.167 s |
| `str_append(200k)` (chained `+` vs `Builder`) | 0.014 s | 0.015 s | 0.031 s | 0.057 s |
| `dyn_records(100k)` (`map` + `str` concat + RC drops) | n/a | 0.017 s | 0.020 s | 0.047 s |
| `json_roundtrip(5k)` (`std.json` parse + stringify) | n/a | 0.039 s | 0.010 s | 0.067 s |

Where the numbers come from:

- A fn annotated with `i64`, `f64`, `bool`, `vec[i64]` or `vec[f64]` compiles to
  a C function over `int64_t` and `double`, so a tight loop is the loop `gcc`
  would get from C. A recursive one also counts frames and checks the stack
  floor every 64 calls, which is the gap to C in `fib(35)`.
- A local `struct` that does not escape is split into C locals at `-O2`.
  `brat-cxx cc -O2 --why-opt` says which were.
- Everything else is a 16-byte tagged value. A string of up to 14 bytes is
  stored inline, `s = s + ...` grows in place when `s` has one owner, and a
  constant map key is cached at the use site.
- `std.json` is written in C.

## Recursion budget (2000 frames)

Call depth is capped at 2000 frames, `main` included. Going past it is an error
a `try` can catch:

```brat run
fn depth(n: i64) -> i64 {
  if n == 0 {
    return 0
  }
  return 1 + depth(n - 1)
}

fn main() -> i64 {
  print(depth(1998))
  try {
    print(depth(1999))
  } catch e {
    print(e)
  }
  return 0
}
```

```output
1998
recursion too deep
```

`BRAT_RECURSION_BUDGET=N` (`1..1000000`) changes the cap. On Linux, macOS and
Windows 8+ the program also raises `recursion too deep` when less than 256 KiB
of stack is left, so a large budget ends in an error and not in a crash. `print`,
`==`, `<` and `own` walk nested values under the same 2000-level nesting cap.

## Nesting in source

The parser refuses a block or a parenthesized expression nested 200 levels deep
(`block nested too deeply`, `expression nested too deeply`) and an expression
tree more than 1000 levels tall (`expression tree too deep`), which a chain such
as `1 + 1 + ... + 1` reaches at about 1000 operators. Every later pass walks the
tree recursively, and these bounds keep that inside an 8 MiB stack.

## Signed 64-bit wrapping integers

All BRAT integers are `i64`. `+`, `-`, `*` and unary `-` wrap in two's
complement; `/` rounds toward negative infinity and `%` takes the divisor's
sign. `parse_int()` rejects overflow instead of wrapping:

```brat run
fn main() -> i64 {
  let top = 9223372036854775807
  print(top + 1)
  print(-7 / 2)
  print(-7 % 2)
  print(7 / 2.0)
  print("9223372036854775808".parse_int())
  print("+42".parse_int())
  print("1e3".parse_int())
  return 0
}
```

```output
-9223372036854775808
-4
1
3.5
Err('number out of range')
Ok(42)
Err('not a number')
```

## Integer operators

Bitwise operators work on `i64`: `&`, `|`, `^`, `~`, `<<` and `>>`.
Shift counts use their low six bits; right shift sign-extends. Hexadecimal,
binary and octal literals use `0x`, `0b` and `0o` prefixes. Unary `&x` is an
extern C out-parameter. There is no `**` operator.

```brat run
fn main() -> i64 {
  let flags = 6 | 1
  print(flags)
  return 0
}
```

```output
7
```

## Named functions are callbacks, not runtime values

A top-level `fn` name can be passed directly to `map`, `filter`, `fold`, `any`,
`all`, `find` and `sort_by` (or as `"fn_name"` to a C callback parameter), but
cannot be stored in a variable. Use a `fn(x) { ... }` closure when you need a
stored function value:

```brat check
fn double(x: i64) -> i64 {
  return x * 2
}

fn main() -> i64 {
  let f = double
  print(f(2))
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:6: error: use of `double` as a value: a fn name is a callback, not a value (hint: call it: `double()`)
  |   let f = double
example.brat: note: help: a bare fn name is a callback argument to `map`, `filter`, `fold`, `any`, `all`, `find` and `sort_by` only
example.brat: 0 type error(s)
```

```brat run
fn double(x: i64) -> i64 {
  return x * 2
}

fn main() -> i64 {
  print([1, 2, 3].map(double))
  let triple = fn(x) { return x * 3 }
  print(triple(4))
  return 0
}
```

```output
[2, 4, 6]
12
```

A `fn` declared inside another function is a name of that function only, and
it does not capture: its body sees its own parameters and the top-level names.
Reading a local of the enclosing function is a compile error. Bind a closure
with `let name = fn(...) { ... }` when the body needs one. This holds for a
`fn` declared in a closure body or in a `spawn` block too.

A block takes one `fn` per name. Sibling blocks can each declare their own:

```brat check
fn main() -> i64 {
  fn step(n: i64) -> i64 {
    return n + 1
  }
  fn step(n: i64) -> i64 {
    return n + 2
  }
  print(step(1))
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:5: error: duplicate fn `step`
  |   fn step(n: i64) -> i64 {
example.brat:2: note: first declared here
  |   fn step(n: i64) -> i64 {
example.brat: 0 type error(s)
```

Top-level statements are not a function body. A `fn` declared inside one (in
the block of an `if` or a loop, or in a closure bound there) is an error.
Declare it at the top level.

## Indexing, variants and map keys

Vector and string indices accept `i64` or integral `f64` values (`2.0`), while
fractional floats, `bool`, `nan` and `inf` raise `index must be int`. Enum and
`Result`/`Option` variants (`Ok(42)`) are not indexable by `[]`; extract their
payloads with `match`, `if let`, `?` or `unwrap`:

```brat run
fn main() -> i64 {
  let v = [10, 20, 30]
  print(v[2.0])
  for i in [1.5, true] {
    try {
      print(v[i])
    } catch e {
      print(e)
    }
  }
  try {
    let r = Ok(42)
    print(r[1])
  } catch e {
    print(e)
  }
  return 0
}
```

```output
30
index must be int
index must be int
index of non-indexable
```

Map keys distinguish tag and value: `1`, `1.0` and `true` are three distinct
keys in the same map. Vectors and maps cannot be used as map keys:

```brat run
fn main() -> i64 {
  let m = {1: "int", 1.0: "float", true: "bool"}
  print(len(m))
  print(m[1])
  print(m[1.0])
  print(m[true])
  try {
    print(m[[1]])
  } catch e {
    print(e)
  }
  return 0
}
```

```output
3
int
float
bool
no key [1]
```

## UTF-8 code-point strings and raw OS bytes

`len(s)`, `s[i]`, `s.slice(a, b)`, `s.chars()` and `for ch in s` count Unicode
scalar values (code points), not UTF-8 bytes and not extended grapheme clusters.
NUL bytes are rejected in string literals and across FFI string boundaries. For
non-UTF-8 OS arguments, environment variables or paths, use `args_bytes()`,
`env_get_bytes()` and byte-vector paths in
`read_file`/`write_file`/`sql_open`/`proc_run`:

```brat run
fn main() -> i64 {
  let s = "héllo"
  print(len(s))
  print(s[1])
  for ch in "né" {
    print(ch)
  }
  print(s.upper())
  print("ß".upper())
  return 0
}
```

```output
5
é
n
é
HÉLLO
SS
```

## Errors nobody caught

An error that reaches the top prints `unhandled: Err('...')` on `stderr` and the
program exits 1. `--trace-lines` adds `at line N`. Inside `catch e`, `e` is the
payload itself.

`brat check` counts the arguments of a built-in method (`"abc".chars(1)` is
``method `chars` wants 0 args``) and of the callback it takes. When the receiver
has no known type the same mistake is a run-time error.

## No implicit conversions

`+` joins two strings or adds two numbers. `"n=" + 5`, `true + 1` and `if n` on
an `i64` are compile errors where the types are known. Write `str(n)`, or a
`"n={n}"` placeholder, and `n != 0`.

## Compile time and caching

The runtime is C headers compiled into each program. The compiler strips the
subsystems a program does not use, so a cold `brat-cxx build -O2` of
`fn main() -> i64 { return 0 }` takes `0.85 s` (median of 7 on the machine of the
speed table, `BRAT_NATIVE_CACHE_DISABLE=1`). Build time grows linearly with the
number of locals in a function (`make bench`, one cold build per row):

| Heap string locals (`N`) | Emitted C | Cold `brat-cxx build -O2` |
|---:|---:|---:|
| 50 | 28.3 KB | 1.32 s |
| 100 | 48.4 KB | 1.65 s |
| 200 | 89.2 KB | 2.63 s |
| 400 | 170.6 KB | 4.72 s |

`brat-cxx run` caches the binary. The key covers the compiler, the environment,
the emitted C, the runtime, the flags and the contents of the linked libraries.
`BRAT_NATIVE_CACHE_DISABLE=1` forces a cold build, `BRAT_CC` picks the C
compiler, and `BRAT_CC_TIMEOUT_MS` (default `120000`) bounds its run time.

## Platform status

CI runs all 175 test files on Linux x86-64 with GCC and Clang, plus an
ASan/UBSan/LSan build, on every push. On macOS arm64 and Windows x86-64 (MinGW-w64)
it builds the compiler and runs the frontend, checker, borrow, module, header
verification and `bundle` tests. The process and socket tests are Linux-only.
