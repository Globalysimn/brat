# C Interop (FFI)

BRAT compiles to C11, so `extern` functions and structs map directly to the
platform C ABI without a marshalling VM. Because a mismatched C prototype
corrupts the process, `brat-cxx build`, `run`, `dist` and `emit-c` refuse to
compile any program that calls C unless you supply a header or object proof
(`--header`, `--object`, or a `.bratbind` package manifest) or opt out
explicitly with `--unsafe-ffi`.

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

Standard C typedef names (`size_t`, `ssize_t`, `ptrdiff_t`, `intptr_t`,
`uintptr_t`, `int8_t`..`uint64_t`, `short`, `unsigned short`, `float`, `double`,
`_Bool`, `const char *`) are canonicalized to their exact BRAT width. C `long`
is target-dependent (`8` bytes on LP64 Linux/macOS, `4` bytes on Windows LLP64
and 32-bit ILP32), so write the target width explicitly or generate bindings
with `brat-cxx bindgen --target TRIPLE`.

Narrow integer arguments (`i8`..`u32`) are range-checked before entering C;
narrow integer returns sign- or zero-extend to `i64`, and `u64`/`usize` returns
above `i64::MAX` raise a catchable `ffi: result of f does not fit i64` error.
`f32` narrows once on call entry and widens back to `f64` on return.

## 2. Verifying bindings (`--header`, `--object`, `--bin`)

Check a program's `extern` declarations against a C header and compiled library:

```sh
./lang/cxx/bin/brat-cxx ffi prog.brat --header /usr/include/sqlite3.h --preprocess
./lang/cxx/bin/brat-cxx ffi prog.brat --header api.h --object libapi.so
./lang/cxx/bin/brat-cxx build prog.brat --header api.h --link -lapi -o prog
```

Verification runs four checks:

1. Header signature match: parameter count, variadic tail, integer vs float
   register class (`i32` != `f32`, `i64` != `f64`), mutability (`str` matches
   `const char *`, never writable `char *`), and nested callback signatures.
2. Layout assertions: emitted C includes compile-time
   `_Static_assert(sizeof(...) == N)` and `_Static_assert(offsetof(...) == M)`
   for every `extern struct` and field, catching packing or data-model drift at
   C compile time.
3. Object and DWARF inspection (`--object`): reads ELF, Mach-O 64, and PE/COFF
   x64 symbol tables plus DWARF v2-v5 type DIEs directly from `.o`, `.a`, `.so`,
   `.dylib`, and `.dll` files without `dlopen`.
4. Post-build binary audit (`--bin`): compiled binaries embed a `BRATFFI1`
   section recording their `extern` declarations, struct layouts, and header
   SHA-256 (preserved across `strip`). Run `brat-cxx ffi --bin ./prog` or
   `brat-cxx ffi prog.brat --header api.h --bin ./prog` in release builds to
   catch header drift.

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
extern fn c_inspect(p: ptr) -> i64 borrows(p)

fn main() -> i64 {
  let p = malloc(16)
  ptr_write_i64(p, 42)
  print(c_inspect(p))
  free(move p)
  return 0
}
```

- Forgetting `free(move p)` fails `brat-cxx check` with `leaked owned pointer`.
- Calling `free(move p)` twice or reading `p` afterwards fails with `use of moved`.
- Passing `&x` at an `extern` `ptr[T]` parameter passes a width-exact writable
  out-parameter slot (`u8` = 1 byte, `i16` = 2, `i32`/`f32` = 4, `i64`/`f64`/`ptr`
  = 8), requires `let mut x`, and enforces exclusivity (`f(&x, &x)` is rejected).
- BRAT strings are NUL-terminated UTF-8 and pass to `str` parameters as borrowed
  `const char *` valid for the call. Returning a `str` from C copies and
  validates UTF-8 (a `NULL` return raises a catchable error; `str_from_ptr(p, n)`
  reads explicit lengths and rejects embedded NUL bytes).
- `ptr_of_str(s)` yields a raw address for C APIs taking `void *` / `uintptr_t`:
  long heap strings (`>= 15` bytes) point to their owned buffer (returning
  `ptr_of_str(local)` is rejected statically), while inline SSO strings (`<= 14`
  bytes) are interned in a demand-paged slab pool bounded by
  `BRAT_PIN_BUDGET_BYTES` (default 64 MiB). Strings returned to C from
  `export fn` or callbacks (directly or inside an `extern struct`) and strings
  written via `ptr_write_str` / `ptr_write_struct` are deduplicated and pinned
  under the same `BRAT_PIN_BUDGET_BYTES` cap so ownership of the temporary BRAT
  value can be released without dangling pointers.

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
