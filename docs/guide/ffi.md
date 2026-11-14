# C interop

BRAT compiles to C, so an `extern fn` is a plain C call with nothing in between.
A wrong prototype corrupts the process, so `build`, `run`, `dist` and `emit-c`
refuse a program that calls C until the declarations are checked against a
header or an object file (`--header`, `--object`, or a `.bratbind` manifest).
`--unsafe-ffi` skips the check.

## 1. Declarations and scalar widths

```brat
extern fn puts(s: str) -> i32

extern "libm.so.6" {
  extern fn sin(x: f64) -> f64
  extern fn cos(x: f64) -> f64
}

extern struct Point { x: i64, y: f64 }
extern packed struct WireHdr { kind: u8, len: i32 }
extern union Payload { iv: i64, fv: f64 }
```

Supported FFI types:

- Fixed integers: `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `usize`
- Floats and booleans: `f32`, `f64`, `bool` (C `_Bool`)
- Pointers and text: `str` (`const char *`), `ptr` (`void *`), `ptr[T]` (`T *`)
- Aggregates and functions: `extern struct`, `extern union`, `fn(...) -> T`, `none`
- Variadic tails: `..ptr`, `..i64`, `..f64`, or `..StructName`

The C names `size_t`, `ssize_t`, `ptrdiff_t`, `intptr_t`, `uintptr_t`,
`int8_t`..`uint64_t`, `short`, `float`, `double` and `_Bool` map to the BRAT
type of the same width. C `long` is 8 bytes on Linux and macOS and 4 on Windows,
so write the width you mean or let `brat-cxx bindgen --target TRIPLE` do it.

A narrow integer argument (`i8`..`u32`) is range-checked before the call. A
narrow result is extended to `i64`. A `u64` or `usize` result above the `i64`
maximum raises `ffi: result of f does not fit i64`. `f32` is narrowed on the way
in and widened on the way out.

## 2. Verifying bindings (`--header`, `--object`, `--bin`)

Check a program's `extern` declarations against a C header and compiled library:

```sh
./lang/cxx/bin/brat-cxx ffi prog.brat --header /usr/include/sqlite3.h --preprocess
./lang/cxx/bin/brat-cxx ffi prog.brat --header api.h --object libapi.so
./lang/cxx/bin/brat-cxx build prog.brat --header api.h --link -lapi -o prog
```

What is checked:

1. Against the header: parameter count, the variadic tail, integer against
   float, `str` against `const char *` (never a writable `char *`), and the
   signatures of callbacks.
2. Layout: the emitted C has a `_Static_assert` on the size of every
   `extern struct` and on the offset of every field.
3. Against an object (`--object`): the symbol table and the DWARF types of a
   `.o`, `.a`, `.so`, `.dylib` or `.dll`, read from the file without loading it.
4. Against a built binary (`--bin`): the binary carries its `extern`
   declarations, struct layouts and the SHA-256 of the header in a `BRATFFI1`
   section that survives `strip`. `brat-cxx ffi prog.brat --header api.h --bin ./prog`
   reports a header that changed since the build.

## 3. Generating bindings (`bindgen`) and exporting shared libraries

Generate verified bindings from C headers (or Rust shared libraries):

```sh
./lang/cxx/bin/brat-cxx bindgen api.h -o api.brat --lib libapi.so
./lang/cxx/bin/brat-cxx bindgen --preprocess --header-only /usr/include/zlib.h -o zlib.brat
```

`bindgen` parses function prototypes, structs, unions, C23 fixed-underlying
enums (`enum Tag : uint8_t`), `#define` constants and callback typedefs. It also
supports `--lang python`, `--lang rust` and `--lang go` from the same parsed C
model.

To expose BRAT functions to C callers, mark them `export fn` and build with
`--shared`:

```sh
./lang/cxx/bin/brat-cxx build lib.brat --shared -o libbrat_ext.so
```

## 4. Owned pointers, borrows and `ptr_of_str`

C headers do not express ownership, so pointer ownership is declared in BRAT
using `move` and `borrows(p)`:

```brat
extern fn malloc(size: usize) -> move ptr
extern fn free(move p: ptr)
extern fn c_view(p: ptr) -> ptr borrows(p)

fn main() -> i64 {
  let p = malloc(16)
  ptr_write_i64(p, 42)
  let v = c_view(p)
  print(ptr_read_i64(v))
  free(move p)
  return 0
}
```

`borrows(p)` needs a pointer result, and the view `v` ends at its last read.
Freeing the owner before that read is refused:

```brat check
extern fn malloc(size: usize) -> move ptr
extern fn free(move p: ptr)
extern fn c_view(p: ptr) -> ptr borrows(p)

fn main() -> i64 {
  let p = malloc(16)
  let v = c_view(p)
  free(move p)
  print(ptr_read_i64(v))
  return 0
}
```

```diagnostics
example.brat: 2 error(s), 0 warning(s)
example.brat:8: error: cannot move `p` while borrowed by `v`
  |   free(move p)
example.brat:7: note: borrowed by `v` here
  |   let v = c_view(p)
example.brat:10: error: leaked owned pointer `p`: free it or return it
  |   return 0
example.brat:6: note: `p` allocated here
  |   let p = malloc(16)
example.brat: 0 type error(s)
```

- A `move ptr` that is never freed is `leaked owned pointer`.
- A second `free(move p)`, or a read of `p` after it, is `use of moved`.
- `&x` at a `ptr[T]` parameter passes a slot of exactly the width of `T` for C
  to write. `x` has to be `let mut`, and `f(&x, &x)` is refused.
- A BRAT string goes to a `str` parameter as a `const char *` that is valid for
  the call. A `str` result is copied and checked to be UTF-8, and a `NULL`
  raises. `str_from_ptr(p, n)` reads a given length.
- `ptr_of_str(s)` is the address of the bytes, for C that takes `void *`.
  Returning `ptr_of_str(local)` is refused. A string of 14 bytes or less has no
  buffer of its own, so it is copied into a pool that is never freed, capped by
  `BRAT_PIN_BUDGET_BYTES` (default 64 MiB). The same pool holds strings returned
  to C from an `export fn` or a callback and strings written with
  `ptr_write_str`.

## 5. Typed slices, structs behind pointers and `errno`

- `slice_of_ptr(p, len, kind)` wraps C memory without copying for `kind` in
  `"u8"`, `"i32"`, `"i64"`, `"f32"`, `"f64"`. Slices carry a dedicated static
  `slice` type and runtime `__brat_slice` tag (plain `vec` values are rejected),
  check index bounds and element value ranges on `slice_get` / `slice_set`, and
  retain/release heap-backed element values across writes.
- `ptr_read_struct(p, "Node")` and `ptr_write_struct(p, val, "Node")` copy
  `extern struct` values to and from foreign addresses; `p[i]` indexes
  `ptr[Node]` directly; `slice_of_struct(p, n, "Node")` builds a read-only
  struct window.
- Adding `errno` after a return type (`extern fn fopen(path: str, mode: str) -> ptr errno`)
  turns a `NULL` pointer return (or `-1` on `i32`/`i64`) into a catchable BRAT
  error carrying `strerror(errno)`.

## 6. Effects and callback contracts

Effect annotations after the return type feed both the concurrency checker and C
optimizer attributes:

```brat
extern fn strlen(s: str) -> usize pure nothrow
extern fn memcpy(dst: ptr, src: ptr, n: usize) -> ptr writes(dst) reads(src)
```

- `writes(p)` tells the `spawn` race checker that the call mutates memory behind
  `p`, rejecting unsynchronized parent/child writes without a `join_all()`.
- `pure` and `nothrow` emit `__attribute__((const))` / `__attribute__((pure))`
  and `__attribute__((nothrow))` in generated C; contradictory combinations
  (`pure writes(p)`, `pure` with `errno` or `move`) are rejected at parse/check
  time.

Callbacks pass a named BRAT function as a string literal (`"my_cmp"`, or `0` for
`NULL` and `-1` for sentinels like `SQLITE_TRANSIENT`). `emit.cpp` generates a
typed C wrapper matching the exact C prototype, including struct-by-value
arguments and returns. Callback parameters carry two conservative contracts:

- `thread(same)` (default) vs `thread(any)`: `thread(any)` rejects callbacks
  that touch module globals or helpers that touch globals.
- `reenter(yes)` (default) vs `reenter(no)`: unless `reenter(no)` is declared, a
  callback that calls back into the same foreign library is rejected.

## 7. Runtime plugins (`lib_open`)

For shared libraries chosen at run time (`dlopen` on POSIX, `LoadLibraryW` on
Windows):

```brat
let h = lib_open("./libplug.so")
let sym = lib_sym(h, "add")
print(call(sym, "i64(i64,i64)", 20, 22))
lib_close(h)
```

`lib_close` invalidates all symbols derived from `h` and defers `dlclose` until
any in-flight worker-thread `call` on `h` finishes. Dynamic `call` checks BRAT
argument types against the literal signature string, not against C headers;
supplying the wrong C signature is undefined behavior.

See `examples/sqlite_ffi.brat` (`test_ffi_sqlite.py`) for a complete
header-verified `libsqlite3` CLI built directly on `extern` declarations.
