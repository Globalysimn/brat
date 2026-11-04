# Limits and sharp edges

What BRAT does not do, where its two backends disagree, and what nobody has specified. Everything
below was observed by running the code in this repository; where a number is a measurement it says
on what. The `brat run` and `brat check` blocks are executed by `lang/tests/test_docs_examples.py`:
the interpreter and the native compiler must both print exactly what the block under them shows.

If something bit you that is not listed here, this page is wrong. Please open an issue.

## Speed

One benchmark, recursive `fib(35)`, best of three on a 2-core Xeon at 2.6 GHz with GCC 14.2 and
CPython 3.13:

| | time |
|---|---|
| C, `gcc -O2` | 0.02 s |
| BRAT, compiled (`-O2`) | 1.17 s |
| CPython | 1.06 s |
| BRAT interpreter | `fib(22)` takes 0.96 s, about 60 thousand calls a second; `fib(35)` would take roughly eight minutes |

The compiled program is about as fast as CPython on call-heavy code and far from C. Every value is
a tagged, reference-counted cell and every call pushes a frame and checks for a pending error. The
emitter keeps some integer locals unboxed, so a tight integer loop should do better than this
benchmark; I did not measure one. The interpreter is a tree walker written in Python. It exists to
define the semantics and to run the tests, not to run your program.

## Recursion stops at 2000 frames

The limit counts BRAT frames, `main` included, and it is an error you can catch:

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

`BRAT_RECURSION_BUDGET=n` changes the limit in both backends. A value that is not an integer from 1
to 1000000 is ignored. The budget counts frames, not bytes, so on Linux a compiled program also
raises `recursion too deep` when its thread's stack is within 256 KiB of the end: with the budget at
1000000 and the default 8 MiB stack, a recursion of 900000 frames is that same catchable error. It
used to die with SIGSEGV at 20000 frames, out of reach of `try`. With `ulimit -s 262144` a compiled
program reached 400000 frames. Elsewhere the stack check does not exist and a budget above what the
stack holds still ends in SIGSEGV; I only ran this on Linux. The interpreter at a budget that large
was still running after three minutes and I stopped it.

Printing, comparing and copying nested values have their own limit of 2000 levels and fail with the
same `recursion too deep`.

## Integers are 64-bit and wrap

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
('err', 'number out of range')
('ok', 42)
('err', 'not a number')
```

`+`, `-` and `*` wrap around, in both backends. `/` on two integers rounds toward negative infinity
and `%` takes the sign of the divisor. `parse_int` is an `Err` for anything outside the `i64` range
and for anything but an optional sign and digits, with ASCII whitespace allowed around them.
It used to wrap silently. Floats are IEEE
doubles; `nan` and `inf` print as `nan` and `inf`.

## There are no bitwise operators

No `|`, `^`, `~`, `<<`, `>>` or `**`, and `&` only takes an address. The lexer used to skip any
character that starts no token, so `6 | 1` was read as `6` followed by the expression `1` and the
program ran with a different meaning. It now refuses the character:

```brat check
fn main() -> i64 {
  let flags = 6 | 1
  print(flags)
  return 0
}
```

```diagnostics
example.brat:2: error: unexpected character '|' at line 2
  |   let flags = 6 | 1
```

`<<` and `**` are refused as well, with `stuck at '<'` and `stuck at '*'`. Use arithmetic, or write
the operation in C and call it through [the FFI](ffi.md).

## A function name is not a value

A bare function name is a callback argument for `map`, `filter`, `fold`, `any`, `all`, `find` and
`sort_by` and nothing else. A closure is a value:

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

## Indexing and map keys

An index is an integer or a float with a whole value. A fraction, NaN, infinity and a bool are not
positions. A whole float too large for an `i64` is simply out of range:

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
    print(v[3])
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
index out of range
```

`1`, `1.0` and `true` are three different map keys. A vector or a map cannot be a key; reading with
one is `no key`, and storing with one is refused:

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

## Strings are code points

`len` and indexing count code points, not bytes and not grapheme clusters. Case mapping is the full
Unicode one. The checker refuses a NUL in a string literal and `utf8_str` refuses the bytes; I did
not look at every other way a string can be made.

```brat run
fn main() -> i64 {
  let s = "héllo"
  print(len(s))
  print(s[1])
  print(s.upper())
  print("ß".upper())
  return 0
}
```

```output
5
é
HÉLLO
SS
```

## Where the backends differ

The interpreter and the native C emitter run the same programs, and
`lang/tests/corpus/agreement.json` pins the answer for each of its programs on both.
Native emission is deterministic; that is tested separately. Differences I know about:

- An uncaught error prints `RuntimeError: ... at line N` from the interpreter and
  `unhandled: ('err', '...')` from a compiled program. The exit status is 1 in both. A `catch`
  binds the same message in both.
- A tag can be indexed, `Ok(1)[1]` is `1`, in compiled programs only. The interpreter says
  `index of non-indexable`. Do not rely on it. The runtime cannot simply refuse it: compiled `match`
  reads a variant's payload through the same index path, and when I removed it `match` failed on
  every enum and `Result` (15 test files went red before I reverted the change).
- A function in an `impl` without `self` cannot be called as `Point.make(x)`. Both checkers accept
  the call; the interpreter stops with `NameError: no fn Point` and the native compiler with
  `unresolved call Point`.
- The interpreter is Python, and an operation that nobody compared can still show CPython's own
  exception text where BRAT would show its own. I compared 15 operators and 7 unary operations or
  builtins over 25 values, 9550 cases, after fixing what that found; the 18 that still differ are
  the tag indexing above. Every string,
  vector and map method was then called with every argument count it accepts and eleven values,
  wrong ones included, on nine receivers. Maps agree. For strings and vectors the interpreter no
  longer shows CPython's words, but a wrong argument still gets different treatment:
  - the interpreter says ``method `repeat`: bad argument``, a compiled program says `not a number`
    (`repeat`, `chunks`, `del`, `ins`, `slice`) or `no method affix`, `no method index_of`,
    `no method replace`, `no method split_once`, `no method zip`, names that are internal;
  - the interpreter takes `true` for the number 1 (`v.del(true)` removes the element at 1) and
    zips with a string; a compiled program refuses both. A compiled program takes `"ab".repeat(2.5)`
    as 2 and `"a b".split(0)` as a split on whitespace; the interpreter refuses both;
  - a call with the wrong number of arguments is a catchable error in the interpreter and a compile
    error in the native compiler (`chars arity`), and both checkers accept it.

## `brat run` can run the interpreter

`brat run` (the Python CLI, `lang/engine/cli.py`) runs the compiled program only when
`lang/cxx/bin/brat-cxx` exists, can be executed and is not older than any file in `lang/cxx/src`.
After a fresh clone, or after a `git pull` that touches the sources, that is not so, and `brat run`
runs the interpreter instead: several hundred times slower on function calls (see [Speed](#speed))
and not always the same answer (see above). It used to do this without a word; it now prints one
line on stderr. A native build that fails is not a fallback: the C compiler's messages are printed
and the exit status is 1. `brat-cxx run FILE` never uses the interpreter and `brat run --interp
FILE` always does.

## Compile time

The C compiler is the slow part, and one function with many heap-valued locals is its worst case.
Locals that hold strings, one function, `-O2`, same machine:

| locals | emitted C | `brat build` |
|---|---|---|
| 50 | 29 KB | 2.5 s |
| 100 | 53 KB | 6.6 s |
| 200 | 102 KB | 26.3 s |

The C grows linearly and the compile time roughly with its square. `brat-cxx` kills the C compiler
after 120 seconds and reports `C compiler exited with status 124`; extrapolating the numbers above,
that happens a little over 400 such locals. `BRAT_CC_TIMEOUT_MS` raises the limit, up to 3600000.
Integer-only locals do not do this: 300 of them build in about a second.

`gcc -ftime-report` on 120 such locals (6.7 s) puts the time in the back end of the compiler on one
large function: instruction scheduling 23 %, post-reload CSE 13 %, alias walking 7 %, register
allocation 6 %. No flag removes the growth. `BRAT_CC` names the compiler and a wrapper script that
appends a flag is honoured: for those 120 locals the build took 6.5 s with the default `-O2`, 3.3 s
with `-O1` and 1.0 s with `-O0`, and the program printed the same. Splitting large functions helps
more.

## Platforms

Everything here was run on Linux x86-64 with GCC 14.2. The C++ sources also pass
`clang++ -fsyntax-only` with clang 19.1; nothing was linked or run with clang. The runtime uses
POSIX threads and `dlopen`. Nobody has checked macOS or Windows.

## What is not proven

- The ownership checker is a static approximation, not a proof. See
  [what is not checked](ownership.md#what-is-not-checked).
- The FFI is trusted. The verifier compares a declaration with a header; it cannot tell you the
  header is right, and C code runs with the full authority of the process. See [ffi.md](ffi.md).
- There is no isolation for hostile programs. `brat run` is not a sandbox.
