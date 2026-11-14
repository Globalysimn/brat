# FAQ

## Why a language and not a library?

What BRAT adds happens before the program runs. The checker refuses use after
move, a container changed during its own loop and a value that escapes its
`arena`. The build refuses an `extern fn` that disagrees with the C header. A
library can report those at run time at best.

If you do not need that, an existing language is the better choice.

## How is it different from Rust, Go and Zig?

Rust proves more. Its borrow checker follows references across functions and
through data structures, which is what lifetime parameters are for. BRAT checks
borrows inside one function and counts references everywhere else, so there is
nothing to annotate and a cycle built across two functions leaks. Pick Rust when
that is not acceptable, or when you need generics without boxing.

Go has a garbage collector and the libraries. BRAT frees memory at the last use
and compiles to C, and has no ecosystem to speak of.

Zig gives you allocators, layout control and freestanding targets. BRAT needs
`libc` and decides the layout for you.

Where BRAT fits is the program that would otherwise be Python with a C
extension: glue, a batch job, a small service around a C library.

## Why reference counting and not a garbage collector?

A collector that moves or scans memory makes C interop harder: pointers handed
to C have to be pinned and calls need barriers. Counting keeps a value where it
is and frees it at a known point. The count is not atomic until a thread or a
plugin is started.

## What about a doubly linked list?

There is no cycle collector. A cycle built inside one function is a compile
error:

```brat check
fn main() -> i64 {
  let mut a = {"id": 1}
  let mut b = {"id": 2}
  a["next"] = b
  b["prev"] = a
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:5: error: reference cycle: `a` goes into `b`, which already owns it (refcounting cannot reclaim a cycle)
  |   b["prev"] = a
example.brat: 0 type error(s)
```

A cycle built across functions is not seen and leaks. There are two ways to
write such a structure:

- Short-lived (a parse tree, a graph for one request): build it inside
  `arena { ... }`. The block frees everything at its end, cycles included.
- Long-lived (a list, a tree with parent links, an LRU cache): keep the nodes in
  a vector and link them by index, with `-1` for no node. A complete LRU list is
  in [by example](by-example.md#5-doubly-linked-lists-and-lru-queues-without-pointer-cycles).

## Why compile to C?

- The compiler has no dependencies and builds with `make`.
- `brat emit-c file.brat` prints C you can read and step through in a debugger.
- Calling C from C leaves struct passing and calling conventions to the C
  compiler.

The price is the C compiler's time on every cold build. `brat run` caches the
binary by content.

## Why does `brat build` refuse my `extern fn`?

A prototype written by hand goes stale: an `i64` where C has `int`, or a dropped
`const`, corrupts memory at run time without a word. So the build wants one of:

- `--header file.h`: the declaration is compared with the header.
- `--object lib.a` (or `.o`, `.so`, `.dylib`, `.dll`): it is compared with the
  symbol table and the debug info.
- a `.bratbind` file written by `brat bindgen`.
- `--unsafe-ffi`, when you mean to skip the check.

## Are strings bytes or characters?

UTF-8, immutable. `len(s)`, `s[i]` and `s.slice(a, b)` count code points, so
`"héllo"[1]` is `"é"`. Grapheme clusters are not handled: an emoji made of
several code points has that many characters. Raw bytes are a `vec[i64]`, see
`std.bytes`.

## What happens on overflow, division by zero and deep recursion?

Integer overflow wraps. Division by zero and recursion past 2000 frames are
errors a `try` can catch.

```brat run
fn main() -> i64 {
  print(0x7fffffffffffffff + 1)
  try {
    let x = 10 / 0
    print(x)
  } catch e {
    print("caught:", e)
  }
  return 0
}
```

```output
-9223372036854775808
caught: div by zero
```

## Why both `Result` and `try`?

Failures you expect (a file that is not there, text that is not a number) come
back as `Ok` or `Err` and you handle them with `?` and `match`. Faults in the
program (an index out of range, division by zero) unwind to a `try`, so that a
service can fail one request and keep running without every `a / b` returning a
`Result`.

`raise(x)` sits between the two. A caller that takes the result gets `Err(x)`.
A caller that does not, as in a bare `save(n)`, has to be inside a `try`, which
then catches it. Outside a `try` that line is a compile error.

## Why can't I store a named function in a variable?

A top-level function compiles to a plain C function, often with unboxed
arguments, and has no closure object. Pass it by name to `map`, `filter` and the
other methods that take a callback, or write `fn(x) { return double(x) }` when
you need a value.

## Is the compiler self-hosted?

No. It is C++20 with no libraries, and it will stay that way while the language
is still changing.

## Was it written by an AI?

Most of the code, yes. The README says so.

## What is unfinished?

- Generics are type-erased: a container holds boxed 16-byte values.
- After `move p.f` the checker refuses to read `p.f`, but at run time the slot
  keeps its reference until `p` is dropped or the field is overwritten.
- Integers are signed 64-bit only. Unsigned types exist on `extern`
  declarations.
- The socket, process and sanitizer tests run on Linux. macOS and Windows run
  the compiler and checker tests.

The [limits](limits.md) page and [LEDGER.md](../project/LEDGER.md) have the
rest.
