# C interop

Both backends support the same syntax and agree on stdout and exit code.

## Declaring

```brat
extern fn puts(s: str) -> i32

extern "libm.so" {
  extern fn sin(x: f64) -> f64
  extern fn cos(x: f64) -> f64
}

extern struct Point { x: i64, y: f64 }
```

Types: `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `usize`, `f32`,
`f64`, `bool`, `str`, `ptr`, `ptr[T]`, `fn(...) -> T`, `none`, and names
declared by `extern struct`.

Every width has one C spelling, and one table decides it
(`ffi_scalar_c()` in `emit.cpp`): the declaration and the call site
read the same entry, so a `short` cannot be declared `int64_t` in one place and
called as two bytes in another. A width with no entry is refused at emit time
rather than spelled as a machine word. Narrow results widen on return, so a
`short` holding -1 arrives as -1 and not as 65535; narrow arguments are
range-checked before C sees them.

A signature copied out of a header may keep the header's spelling. The names
whose width BRAT already has are read as that width, so `size_t` is `usize`,
`ssize_t`/`ptrdiff_t`/`intptr_t` are `i64`, `uintptr_t` is `u64`, and
`int32_t`/`uint32_t`/`int64_t`/`uint64_t` are the same-width integer; the C
spelling still reaches the emitted C unchanged (`size_t` stays `size_t`, which
matters on a 32-bit target). A fixed-width type is the width it names:
`int8_t` is `i8`, `int16_t` is `i16`, `uint8_t`/`uint16_t` are `u8`/`u16`, and
`short`/`unsigned short` are `i16`/`u16` in every data model. The same
canonicalization accepts the other target-independent C spellings
(`unsigned long long`, `signed short int`, `float`, `double`, `_Bool` and
`const char *` for `str`), including inside `ptr[...]` and callback slots. It
does not guess C `long`, whose width belongs to the target: write the BRAT
width for that target, or let `bindgen --target` choose it. They used to be
widened to `i32`, which is two bytes short of the truth in both directions: a
generated `extern struct` holding an `int8_t` was refused by the layout proof
that generated it, and a narrow *result* was read as four bytes out of a
register where the ABI only promises one. C `bool`/`_Bool` bind as BRAT
`bool`, not as `u8`: the boundary converts between C's boolean and BRAT's
`true`/`false`, while `u8` remains the distinct `unsigned char` slot and is
refused by a header proof for a `_Bool` declaration.

The widths a declaration is compared against come from a **data model**, and a
`--target` triple picks it: `lp64` (Linux, macOS; `long` 8 bytes), `llp64`
(Windows; `long` 4, pointer 8), `ilp32` (i686, armv7, wasm32; `long` 4, pointer
4). So `extern fn f(x: i64)` beside a C `long f(long x)` is correct on LP64 and
a refused ABI bug on the other two — before anything is built for them.
`brat ffi --header`, `brat build --header` and `brat bindgen` all take
`--target`, and `bindgen` writes the target's width (`i32` for `long` on
ilp32). `size_t` binds as `usize` on every model, because BRAT emits `usize` as
`size_t` and C sizes that for the target.

Widths that belong to the target are confirmed by the target. `long` is eight
bytes on LP64 and four on i686 and on Windows/LLP64; `size_t`, `ssize_t`,
`ptrdiff_t`, `intptr_t`, `uintptr_t` and every pointer follow the target too.
The comparison table carries the usual answer so it has a number to compare, and
`brat ffi --header` / `brat build --header` then emit
`_Static_assert(sizeof(long) == 8, ...)` into the probe: the target's compiler
states the width or the binding is refused. A type whose width C fixes
(`int`, `short`, `int32_t`) needs no such confirmation. Under `--target` the
probe is only run when `zig` is available, because a host compiler's widths say
nothing about another machine.

A record the header declares as `struct Foo { ... };` has no shorter C name, so
the proof spells it `struct Foo`; a `typedef` gives the bare name. A probe that
guessed wrong would not compile, and a proof that cannot compile looks like a
header mismatch instead of a proof that never ran.

Mutability is carried by the type, not by a comment. `const char *` is a `str`:
a read-only, possibly shared buffer, which is exactly what `str` is. A writable
`char *` is a byte buffer (`ptr`), because handing C a `str` would let it write
into memory BRAT considers immutable -- `brat ffi --header` refuses the pairing
by name. `const unsigned char *` is bytes too, never text. On the calling side
`&x` is an **exclusive loan**: C receives a writable pointer, so two `&x` of the
same binding in one call is refused (the aliasing `f(&mut x, &mut x)` that Rust
refuses), and so is `&x` of a binding declared without `mut`. Both checkers word
it the same way.

A header is one C view, and `brat bindgen --lang` writes that view for another
language instead of inventing a second scanner: `--lang brat` (the default),
`--lang python` (a `ctypes` module), `--lang rust` (`extern "C"` plus
`#[repr(C)]` records) and `--lang go` (a cgo file that includes the header
rather than re-declaring it). The widths and the skipped set come from the same
parse, so a type that BRAT refuses is refused by name in every language, and
none of the four can disagree with another about a width. Python is the one that
is *checked by execution*: the generated `ctypes` module calls the same `.so` as
the BRAT program and must print the same numbers.

A C function that returns a number returns a **value**: `let n = compressBound(x)`
is a copy, so `let mut out_len = n` beside it -- the shape of every out-parameter
in C -- is not a borrow and is not refused. Before this, an unannotated `let`
stayed "dynamically typed, therefore a conservative borrow", and the idiom failed
with `cannot borrow immutable `. A returned `str` or record is still tracked as a
borrow: the rule is about scalar slots, and widening it further would quietly
switch borrow checking off at the boundary.

Ownership is proven, not guessed. A header does not say who owns a pointer, and
a wrong guess costs more than none (`getenv` marked owned, then freed). So
`brat bindgen` emits a plain `ptr` and the *program* states the ownership it
can be held to: `-> move ptr` for a result the caller must release, `move p: ptr`
for the parameter that releases it. That is a mode, not a type, so
`brat ffi --header` still proves the binding against C -- and the checker then
refuses a leak (`leaked owned pointer`) and a second release (`use of moved`) in
both checkers. `move` on a scalar is refused at the declaration: a copy has
nothing to own, and it used to be reported as a leak of a value with no storage.

Layout is proven, not assumed. For every `extern struct` the emitter writes
`_Static_assert(sizeof(...) == N)` and one `_Static_assert(offsetof(...) == M)`
per field, with the numbers it generated code against. A target whose `long` is
four bytes, a field spelled at the wrong width, or a record whose packing was
lost all stop the C compiler with the struct and the field named. The asserts
are compile time only: they add no code, no data and no run-time cost (the
`text`/`data`/`rodata` sections are byte-identical with and without them).

Every slot of an `extern` declaration is checked at `brat check`, not at build
time: a parameter, a result, a variadic tail and an `extern struct` field.
The check is recursive through a pointee and through a callback's parameters
and result. A type the emitter cannot spell is `F0001` with the line and a
list of what is supported. This catches unsupported FFI type spellings before
C generation. Passing `check` does not guarantee a successful build: headers,
libraries, symbols and target ABI compatibility still have to match.

`f32` is a boundary type, not a second float type: BRAT has one float, and the
value narrows once on the way into C and widens once on the way back. It exists
because a kernel or a graphics library declares `float`, and writing the cast in
BRAT would mean writing C in BRAT's clothes:

```brat
extern "libkern.so" fn scale(x: f32, k: f32) -> f32
extern "libkern.so" fn fill(p: ptr, n: i64)

let r = scale(2.5, 4.0)          // 10.0, rounded through float once
let buf = malloc(64)
fill(buf, 16)
print(ptr_read_f32(buf))         // a float * element, not a double
```

An `extern struct` field of type `f32` is a C `float` in the layout, and
`ptr_read_f32`/`ptr_write_f32` address a `float *` buffer. `slice_of_ptr` still
describes only `i64`, `f64` and `u8` windows: a byte window over floats would
have to guess an element size, so it refuses `"f32"` instead. Without a library string the symbol is
resolved from libc or the main handle.

`..T` marks a variadic tail: `extern fn printf(fmt: str, ..ptr) -> i32`. A `str`
may fill a `..ptr` slot — that is what `%s` reads — and reaches C as
`const char *`. A fixed `ptr` parameter and a `..i64` slot both refuse a `str`.
A variadic slot may also take an `extern struct` by value, which is the shape of
`printf`-style APIs that pass a record: `extern fn sum_pts(n: i64, ..Pt) -> i64`
called as `sum_pts(2, a, b)` passes two `Pt` records, and both backends do it.

The name in `extern fn NAME` and the names in `extern struct` are written into
the generated C as they are, because they are the library's names. They must
therefore be plain identifiers: a string token or a C keyword there is a parse
error, and FFI structs are emitted as `bf_<name>`, so a struct or field called
`Val` still compiles.

Integer arguments are range-checked at the boundary: out of range raises
`ffi: arg N of f wants i32`. A `u64`/`usize` result above `i64::MAX` raises
`ffi: result of f does not fit i64`, because BRAT integers are i64.

## Owned pointers

`move` on a parameter means the callee takes the pointer, `-> move ptr` means
the callee returns one.

```brat
extern fn malloc(size: i64) -> move ptr
extern fn free(move p: ptr)

fn main() -> i64 {
  let p = malloc(8)
  ptr_write_i64(p, 42)
  free(move p)
  return 0
}
```

The checker rejects a second free and any use after it (`use of moved`), and an
owned pointer that is neither freed nor returned (`leaked owned pointer`).
Storing a pointer in a container is not tracked and is reported as a leak; pass
it through a function instead.

One free per path is one free. Releasing on an error branch and returning is the
C idiom, and it is accepted:

```brat
let p = malloc(8)
if bad() {
  free(move p)
  return 1
}
free(move p)
```

A free that a `break` cuts short is not that case -- the loop ends and the
pointer is still the same one -- so it is refused.

## Strings

BRAT strings are NUL-terminated in both backends, so they pass to C as
`const char *`. A C string returned as `str` is copied into BRAT memory, so the C
side keeps owning its buffer. A NULL return is an error value, catchable with
`try`/`catch`, not a crash. The bytes must be valid UTF-8 and within BRAT's
string cap; otherwise the conversion raises instead of manufacturing a string
the interpreter cannot hold. That rule covers extern returns, `str` fields in
extern structs, dynamic `call` results, callback and exported-function
parameters, and `ptr_read_str`. A NULL `str` field in an extern struct becomes
`None`; the other non-nullable slots reject NULL. A string literal containing a
NUL byte is a parse error.

## Structs

`extern struct` fields are marshalled by value in both directions: a BRAT struct
literal becomes a C struct, a C struct return becomes a BRAT map.

## Callbacks

Pass a BRAT function by name in double quotes:

```brat
extern "libcb.so" fn apply_cb(x: i64, cb: fn(i64) -> i64) -> i64

fn double_it(x: i64) -> i64 { return x * 2 }

fn main() -> i64 {
  return apply_cb(21, "double_it")   // 42
}
```

The compiler writes a `brat_cb_<name>` wrapper. The argument must be a string
literal naming an existing function with the exact parameter count and ABI
types, including its return type (`unknown callback `x``, `callback `x` takes 1
parameter(s), fn(i64,i64)->i64 wants 2`, or a signature mismatch). `check` says
so, and so does everything downstream: `brat-cxx cc` and `brat emit-c` exit 1
with an empty stdout rather than write C for a wrapper that does not exist, and
the interpreter stops with the same sentence instead of letting a `ctypes`
traceback out of a call C is in the middle of. A closure or a bare fn name is
refused too -- the slot takes a name, and `arg N of `f`: callback must be a
function name` says which one. The literals `0` and `-1` in the same slot are
passed as the function-pointer bits themselves: NULL, and the all-ones sentinel
used for values such as `SQLITE_TRANSIENT`. No BRAT wrapper is made for either.
A trailing
`ptr` in the signature is the C `void *user_data` and is passed through to the
BRAT function. Callback names in imported modules are resolved to the same
flattened, mangled function name in both front ends.

Every width crosses a callback in both directions, `f32` included: a `float`
parameter arrives as a `float` and is widened once inside the wrapper, and an
`f32` result is read out of the float payload rather than out of the integer one
(a `float` read as an integer is 3.5 arriving as 4.6e18). A record may be a
parameter and a result too:

```brat
extern struct Pt { x: i32, y: i32 }
extern "libcb.so" fn apply_pt(cb: fn(i64) -> Pt, n: i64) -> Pt

fn mk(n: i64) -> Pt { return Pt { x: n, y: 2 } }
let p = apply_pt("mk", 7)     // 7, 2
```

The reference interpreter returns it too. CPython's `ctypes` refuses a struct as
a callback result outright (`invalid result type for callback function`), so for
exactly that shape the interpreter compiles a three-line C trampoline: the
trampoline keeps the real prototype, so the record really comes back where the
platform ABI puts it -- two registers for `{i32,i32}`, an SSE pair for a mixed
record, the hidden return pointer for a large one -- and Python only ever sees
`void *`. The trampoline is emitted with the same widths `ctypes` uses and
`_Static_assert`s the record's size and every field offset, so a disagreement is
a compile error, never a wrong number. It is compiled once per shape and cached
under `$TMPDIR/brat-cb-shim`; with no C compiler available the interpreter says
so (`... needs a C compiler ... run `brat build``) instead of guessing. The
native backend needs none of this: it emits the callback with its true prototype.

A `raise` inside a callback cannot unwind through the C frames in between: the
wrapper prints `unhandled: ('err', ...)` and exits 1, the same as an uncaught
error at the top level, in both backends. A `try`/`catch` around the extern call
does not see it. The same stop happens before the BRAT body runs if C passes
NULL or invalid UTF-8 for a callback's `str` parameter.

## Out-parameters

Most of libc and every driver ioctl wrapper returns its result by writing
through a pointer. `&x` at a `ptr[T]` argument is that pointer:

```brat
extern "libc.so.6" fn clock_gettime(clk: i32, ts: ptr[i64]) -> i32

let mut secs = 0
clock_gettime(0, &secs)      // the callee wrote it
print(secs)                  // as soon as the call returned
```

The binding holds what C wrote the moment the call returns, so it is an ordinary
value again: arithmetic, comparison, another call. Widths come from the pointee,
not from a machine word:

```brat
extern "libapi.so" fn narrow_down(small: ptr[u8], med: ptr[i16], big: ptr[i32])

let mut s = 0
let mut m = 0
let mut b = 0
narrow_down(&s, &m, &b)       // one byte, two bytes, four
```

An `i16 *` gets two bytes of storage. Everything narrower than a word used to be
written as a word, which is how an out-parameter corrupts the variable beside
it.

A handle factory takes `T **`; spell it `ptr[ptr]`. The storage is one pointer,
and the binding comes back holding the handle:

```brat
extern "libsqlite3.so.0" fn sqlite3_open(path: str, db: ptr[ptr]) -> i32

let mut db = 0
sqlite3_open("app.db", &db)
```

What `&` is not:

* not a general address-of. It is refused everywhere except a `ptr[T]` argument
  of an `extern`, in both checkers and at run time: `` `&` is an out-parameter:
  write it where the extern takes a `ptr[T]` ``. A BRAT program has no pointer
  arithmetic to do with a raw address, so an address is only ever the callee's.
* not applicable to an expression. `` `&` takes a binding: `let mut p = &x` ``.
* not a fresh binding. `let p = &x` needs `mut`, because the callee writes
  through it.

Two out-parameters in one call work, and the values come back in the order the
call wrote them: `divmod2(17, 5, &q, &r)` leaves `3` and `2`.

Memory a program allocates itself is ordinary FFI now:

```brat
extern "libc.so.6" fn malloc(n: usize) -> ptr[u8]
extern "libc.so.6" fn free(p: ptr[u8])

let mut buf = malloc(64)
let bytes = slice_of_ptr(buf, 64, "u8")
slice_set(bytes, 0, 42)       // the byte C will read
free(buf)
```

## Pointers and slices

`ptr[T]` says what a pointer points at; a bare `ptr` converts both ways, but
`ptr[i64]` and `ptr[f64]` do not mix.

```brat
let v = [10, 20, 30, 40]
let s: slice[i64] = slice_of(v, 1, 3)
print(slice_len(s))     // 2
print(slice_get(s, 0))  // 20
slice_set(s, 1, 99)
print(v[2])             // 99
```

A slice is a window, not a copy, checked at both ends: `slice_get(s, 2)` raises
`slice index 2 out of range 0..2`, and `slice_of(v, 1, 9)` raises
`slice range 1..9 out of range 0..3`. Both backends raise the same words and
`try`/`catch` handles them.

Builtins: `ptr_null`, `ptr_is_null`, `ptr_offset`, `ptr_read_i64`/`f64`/`ptr`,
`ptr_write_*`, `ptr_read_str`, `ptr_of_str`, `ptr_write_str`. Any of them given a
zero address raises `null pointer dereference` and a negative one raises
`pointer is negative`, in both backends. `ptr_read_str` also raises
`ptr_read_str: invalid UTF-8` for bytes that are not a BRAT string.

## C buffers

C hands out a pointer and a length; `strlen` is not that length.

```brat
print(str_from_ptr(get_name(), 5))   // hello
print(str_from_ptr(get_name(), 0))   // empty
print(str_from_ptr(with_nul(), 5))   // str_from_ptr: NUL byte at 2
print(str_from_ptr(bad_utf8(), 2))   // str_from_ptr: invalid UTF-8
```

`slice_of_ptr(p, n, "i64")` is the same idea for numeric arrays: a checked
window over C memory, no copy. The element kind is written out because the type
system cannot read it back from an address, and anything but `"i64"`, `"f64"` or
`"u8"` is an error.

## errno

Append `errno` to the result type and a failing call raises a BRAT error. The
failure value is NULL for `ptr` and -1 for `i32`/`i64`; any other result type is
rejected at parse time.

```brat
extern fn fopen(path: str, mode: str) -> ptr errno

try {
  let f = fopen("/no/such", "r")
} catch e {
  print(e)   // fopen: No such file or directory
}
```

Without `errno`, read it yourself: `errno()` and `strerror(errno())`.

## Effects

A declaration can say what the C function does, and every promise is used:

```brat
extern fn strlen(s: str) -> i64 pure nothrow
extern fn memcpy(dst: ptr, src: ptr, n: i64) -> ptr writes(dst) reads(src)
extern fn qsort(base: ptr, n: i64, sz: i64, cmp: ptr) -> none writes(base)
```

| effect | meaning | what uses it |
| --- | --- | --- |
| `pure` | writes no memory, returns the same answer for the same arguments | the C compiler (`const`), so repeated calls collapse and a call in a loop can be hoisted |
| `pure reads(p)` | reads memory behind `p`, writes none | the C compiler (`pure`), weaker than `const` but still hoistable |
| `writes(p)` | writes memory behind `p` | the sharing checker: a foreign write is a write, and races across `spawn` are reported |
| `reads(p)` | reads memory behind `p` | documentation for a reader; `reads` is implied by every argument |
| `nothrow` | never parks a failure in the error channel | the C compiler (`nothrow`); the emitter keeps the call out of a landing pad |

`p` must be a parameter that points at memory (`ptr`, `ptr[T]`, `str`, or a
slice); `writes(n)` on an `i64` is a promise about nothing and is refused.
Effects appear after the result type, in source order, and the order is part of
the tree both front ends print.

A declaration that contradicts itself is refused rather than believed:

```
pure writes(p)       `pure` contradicts `writes(p)` on f
pure + move result   `pure` contradicts a `move` parameter of f
pure + errno         `pure` contradicts `errno` on f
reads(p) writes(p)   `f` both reads and writes `p`
pure pure            `pure` declared twice on f
reads(q)             `reads(q)` names no parameter of f (params: p)
```

Why it matters, beyond tidiness: without `writes` the sharing checker treated
every foreign call as touching nothing, so

```brat
extern fn my_poke(p: ptr, v: i64) -> none writes(p)

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    my_poke(p, 1)        // child writes through p
  }
  my_poke(p, 2)          // parent writes through p, no join in between
  join_all()
  free(move p)
  return 0
}
```

was accepted, and is now refused with the same diagnostic any other racing write
gets. Drop the `writes(p)` and the program is accepted again — which is the
honest state of affairs: an effect not declared is a fact not known.

`pure` reaches the emitted C as `BRAT_ATTR_CONST` (or `BRAT_ATTR_PURE` when it
also reads through a parameter) and `nothrow` as `BRAT_ATTR_NOTHROW`. The macros
are empty on a compiler without `__attribute__`, so the promise is advice, not a
requirement. With the 16-byte `Val` representation operands pass in
two 64-bit registers on SysV x86-64/ARM64, and `borrows(p)` records that a
foreign callee does not retain `p` past the call return.

## Callback contracts

A callback parameter takes the name of a BRAT function, checked for arity and
widths at the call site (`test_ffi_callback_gate.py`). Two more things decide
whether that function is safe to call, and both are facts about the *callee*, so
they are written on the callback parameter and stored on the declaration:

```brat
extern "libuv.so" fn uv_read_start(h: ptr, alloc: fn(i64) -> str,
                                   read_cb: fn(str) -> none thread(same)) -> i32
extern "libsqlite.so" fn sqlite3_exec(db: ptr, sql: str,
                                      cb: fn(ptr, i64, ptr) -> i64 reenter(no),
                                      arg: ptr, err: ptr) -> i32
```

| contract | default | what the checker does with it |
| --- | --- | --- |
| `thread(same)` | yes | the callback runs on the thread that registered it, so touching the program's state is legal |
| `thread(any)` | no | the callee may call back from a thread the program never made; then the callback's body must not reach anything the caller's thread also owns -- no global, and no function that touches one |
| `reenter(no)` | no | the callee will not enter a callback while one of its own calls is on the stack, so a callback that calls its own library again is allowed |
| `reenter(yes)` | yes | re-entry is possible; a callback that calls its own library again is refused |

Both defaults are the dangerous half of the pair, and that is deliberate: an
unstated promise is a fact the checker cannot use, so a program that says nothing
is checked as if the callee may do the worst thing the language can express.

```
thread(any) on a callback that reads a global   `cb` is registered as a callback of `f`,
                                               which declares `thread(any)`, but it
                                               touches `total` ...
callback calls its own library, no `reenter`    `cb` is registered as a callback of `f`
                                               and calls `f` again; a callee may re-enter
                                               a callback unless the declaration says
                                               `reenter(no)`
```

The re-entry rule follows the call graph, so a helper counts. Refusals for a
declaration that cannot mean what it says are the parser's, and identical in both
front ends: an unknown contract (`frobnicate(x)`), a value outside the table
(`thread(sometimes)`), a contract on a parameter that is not a callback
(`v: i64 thread(any)`), the same contract twice, and a contract written at the
end of the declaration instead of on the callback parameter.

What is *not* here, and why: `lifetime(...)` was planned and dropped. It would
have said whether the callee may keep the callback after the call returns, and
the case that made it necessary -- a closure whose frame dies while C still holds
the pointer -- is already refused by the callback gate, which accepts a function
name and nothing else. A contract with nothing left to describe is a comment with
a grammar; adding it would be decoration.

## Plugins at run time

When the library is only known at run time, open it, look up a symbol and call
it through a literal signature:

```brat
let h = lib_open("/opt/plug/libplug.so")
let add = lib_sym(h, "add")
print(call(add, "i64(i64,i64)", 20, 22))   // 42
lib_close(h)
```

A signature is `ret(arg,arg,...)` over `i64`, `f64`, `str`, `ptr`, `none` and is
checked before the program runs:

```
call: arg 1 wants i64, got str
call: signature wants 2 argument(s), got 1
call: unsupported return type `vec`
call: signature must be a string literal
```

`lib_open` raises `lib_open: <dlerror>`, `lib_sym` raises `lib_sym: no symbol
<name>`. Runtime-issued positive IDs identify library handles and symbols;
`lib_close` invalidates every symbol from that handle. Closing an unknown or
already closed handle, looking up a symbol through a closed handle, and calling
a stale symbol raise catchable runtime errors. Native calls hold the library
open until the active call returns; the interpreter keeps the callable alive
for the duration of a call. A signature is checked against BRAT arguments but
cannot be checked against an arbitrary C function's real prototype. Supplying a
signature that disagrees with that prototype is still undefined behaviour. A
returned `ptr` is not owned or pinned by BRAT; its validity remains the
plugin's contract, especially after closing the library. On POSIX `lib_open`
uses `dlopen`/`dlsym`/`dlclose`; on Windows it uses
`LoadLibraryW`/`GetProcAddress`/`FreeLibrary`.

## Generating and exporting

```sh
brat bindgen header.h -o bindings.brat     # prototypes and structs -> extern blocks
```

A declaration whose name BRAT cannot read back — a C function called `fn`, a
struct called `while` — is skipped with a `// skipped: ...` comment instead of
being emitted broken, because those names are the C symbols and cannot be
renamed. C keywords in a struct or field (`int count`, `int do`) are fine: the
emitter writes them as `bf_count`, `bf_do`.

`export fn name(...)` in BRAT produces a C-callable function, and
`brat build lib.brat --shared -o libbrat.so` a shared library. Exported
signatures may use `i32`, `i64`, `f64`, `str`, `ptr` and `extern struct`.

## A direct SQLite binding

[examples/sqlite_ffi.brat](../../examples/sqlite_ffi.brat) is a small inventory
tool that binds the system `libsqlite3` directly. It does not use `std.sqlite`
or the `sql_*` builtins. The shapes that carry it are the ordinary ones:

```brat
extern "libsqlite3.so.0" {
  extern fn sqlite3_open(path: str, db: ptr[ptr]) -> i32
  extern fn sqlite3_prepare_v2(db: ptr, sql: str, n: i32, stmt: ptr[ptr], tail: ptr) -> i32
  extern fn sqlite3_bind_text(stmt: ptr, idx: i32, text: str, n: i32, destructor: fn(ptr)) -> i32
  extern fn sqlite3_column_text(stmt: ptr, col: i32) -> ptr[u8]
}
```

`sqlite3_bind_text` is called with `-1` for both the byte count and the
destructor: the second `-1` is `SQLITE_TRANSIENT`, so SQLite copies the text
during the call instead of retaining a borrowed BRAT string. `sqlite3_column_text`
returns `const unsigned char *`, so it is declared `ptr[u8]` and read back with
`ptr_read_str`.

```sh
brat ffi examples/sqlite_ffi.brat --header /usr/include/sqlite3.h
brat run --interp examples/sqlite_ffi.brat --header /usr/include/sqlite3.h
brat build examples/sqlite_ffi.brat --header /usr/include/sqlite3.h -o sqlite_ffi
./sqlite_ffi inventory.db add kiwi 1.5 30 sour
./sqlite_ffi inventory.db list
```

With no arguments the tool seeds an in-memory database and lists it. The native
verifier wants `--preprocess` on this header (`brat-cxx ffi ... --preprocess`);
the build gate falls back to the preprocessor when the raw header will not
parse. `test_ffi_sqlite.py` runs the whole path on both backends and under
ASan/UBSan/LSan.

## Safety

- Checked FFI misuse is a BRAT error rather than a segfault: a wrong argument
  type, a non-integer where an integer is declared, a negative or null pointer,
  a `str` with a NUL byte in the interpreter. A prototype that does not match
  the real C function is outside the check and is undefined behavior.
- `brat build` and `brat dist` refuse what the checker rejects, so the native
  path is not reached with a wrong type.
- `i32` returns are read as 32-bit ints in both backends: `strcmp` returning -1
  is `-1`, not `4294967295`.
- C receives BRAT `str` and borrowed pointer data for the duration of the call;
  it must not retain those addresses. In particular, arena-backed values are
  invalid after their arena block exits. Use the explicit owned-pointer
  conventions when C takes responsibility for a pointer.
- Pinned by `test_ffi.py`, `test_ffi_owned.py`, `test_ffi_hardening.py`,
  `test_cb_ctx.py`, `test_plugin.py`, `test_c_types.py`, `test_typed_ptr.py`.

## Proving a binding

A wrong prototype is not caught by anything downstream: the call compiles, runs,
and returns nonsense. The header knows better, so ask it:

```
brat ffi prog.brat --header /usr/include/curl/curl.h
```

```
  error: extern fn curl_easy_setopt: parameter 2 `opt` is declared `i64` (8 bytes), curl.h has `i32` (4 bytes)
  error: extern struct curl_slist: field 1 is `data` in BRAT, `next` in curl.h
prog.brat: 4 binding(s) proven, 2 error(s), 0 warning(s)
```

Three witnesses, and each one sees something the others cannot:

| stage | question | who answers |
|---|---|---|
| declarations | same width, arity, field order, variadic tail? | the header text |
| callbacks | same parameter and result widths *inside* the `(*)(...)`? | the header text |
| layout | same struct size and member offsets? | the C compiler, via `_Static_assert` (and DWARF DIEs under `--object`) |
| symbols | does the library export the name? | static ELF / Mach-O / PE-COFF symbol tables (no `dlopen` on file paths) |

A callback parameter is compared as a signature, not as a pointer: BRAT's
`fn(f32)->f32` against C's `float (*cb)(float)` is one argument and one result,
each compared by width. "Both are pointers" is not a proof, and a `double` where
the callback takes a `float` is a wrong register, not a rounding difference.

Width, not kind, is what protects the ABI: `long long` and `int64_t` are the
same eight bytes and BRAT calls them `i64`; `float` and `double` are not, and
BRAT calls them `f32` and `f64`. That is the distinction the command exists for.

`--json` prints the same verdict for a build gate in both front ends (`brat-cxx`
emits structured JSON directly via `JsonWriter`), and an unknown flag is a usage
error instead of a file named `--json`.

Without `--header` the symbol check still runs, and the output says the
comparison was not made. Passing `--object libfoo.so` (or `.o`, `.a`, `.dylib`,
`.dll`) additionally verifies every `extern fn`, `extern struct`, and
`extern union` directly against compiled ELF/Mach-O/PE-COFF symbols and DWARF
v2–v5 type DIEs (`brat ffi --header api.h --object libfoo.so --emit-manifest
foo.bratbind` writes a SHA-256-pinned `.bratbind` manifest).

## What the binary carries

A proof is worth what you can check about it later. `brat build` writes the extern
surface into the emitted C as data -- a `static const char brat_ffi_contract[]`,
marked `BRATFFI1`, so it survives into the executable -- and that record is what
`brat ffi --bin` reads:

```
brat ffi --bin prog.exe
```
```
binary: 2 extern fn(s), 1 extern struct(s)
target: host (ptr 8, long 8, wide align 8)
proved: against probe.h (sha256 a412fcff8b3f49a3)
```

It holds every `extern` declaration with its widths and arity, every
`extern struct` with its size, alignment and field offsets, the target's pointer
and `long` widths, and the sha256 of the header the build proved the declarations
against (`proved: against no header` when the build had none -- the binary then
says it carries an unproven surface instead of staying silent).

Give it the source and it re-proves the record against the declarations as they
are now:

```
brat ffi prog.brat --header probe.h --bin prog.exe
```
```
  drift: extern fn item_total: recorded f64(ptr, i32), now f64(ptr, i64)
prog.exe: the recorded proof no longer describes this source
```

Exit status is 1 on any drift, so a release step can refuse to ship a binary that
no longer matches its source, or one whose header changed under it. This is the
part no other C interop has: rustc, cgo, ctypes and bindgen compare declarations
once and the executable forgets, so the drift is found by the corrupted value it
causes.

What `--header` alone cannot see — a header that disagrees with the compiled
object beside it — `--object` checks directly against the binary's symbol table
and DWARF v2–v5 DIEs; stripped objects without DWARF still check exported symbol
names and header `_Static_assert` layouts.

`brat emit-c` writes the same block (`--header` proves it first), and an
`extern struct` is laid out by the target's rules -- `emit-c --target i686-linux-gnu`
asserts a 4-byte `ptr` -- because a layout is only meaningful for one data model.

## Flags the library needs

A library name covers the common case (`extern "libcudart.so"`), and `--link`
carries what a name cannot express:

```
brat build prog.brat -o prog --link -lcudart --link -L/usr/local/cuda/lib64
```

The flag is passed to the C compiler after the objects, once per occurrence, and
`--link=-Wl,-rpath,...` is the same thing without the space. This is the escape
hatch for a calling convention that is not the platform's default, too: BRAT
declares `extern` with the target's C convention and has no per-function
attribute, so a library that needs another one is called through a shim compiled
with `--link`.

## Generating, and why it is the same mechanism

```
brat ffi --header ./api.h --emit --emit-lib libapi.so > bindings.brat
brat ffi bindings.brat --header ./api.h          # proves what it just wrote
```

`--emit` reads the header through the same C view that `check` compares against,
so the two cannot drift: a generator that could write something its own proof
refuses would be a generator that lies. What it cannot represent exactly it
reports on stderr instead of rounding -- an opaque `struct Foo;` becomes a `ptr`,
and a struct holding another is emitted after it.

## Proving during a build

```
brat build prog.brat --header ./api.h -o prog
```

The proof runs first. A binding that disagrees with the header stops the build,
because the declaration in the file is the one the generated C calls through:
there is no later stage that would catch it.

A program that calls C and names no header is refused by the same gate, in every
command that produces or runs a binary:

```
brat build prog.brat            # refused
brat run prog.brat              # refused, before the exec
brat dist prog.brat             # refused
brat emit-c prog.brat           # refused
```

```
brat build: refusing prog.brat: it calls C and no header was given.
  A prototype that disagrees with the library is undefined behaviour, not a
  warning. Prove it, or say you are not going to:
      brat build prog.brat --header FILE     the declarations compared to C
      brat build prog.brat --unsafe-ffi      build it anyway, on your head
```

There is no "it probably matches". A program whose C surface was never compared
is the case the whole mechanism exists for, and an unproven build is not a
smaller claim than an unproven header -- it is the same claim with the evidence
thrown away. So the choice is made out loud:

- `--header FILE` proves the declarations, and the build carries the contract
  described above.
- `--unsafe-ffi` builds it, prints `prog.brat calls C without a proof
  (--unsafe-ffi)` on stderr, and records `proved: against no header` in the
  contract, so a reader of the binary learns the same thing.

`--interp` is gated too: it crosses the C ABI through `ctypes`, so a mismatched
prototype can still corrupt or terminate the process. Use `--header FILE` or a
verified `.bratbind`; `--unsafe-ffi` is the explicit escape hatch for test
fixtures and callers who accept that risk. The interpreter scans loaded source
for C-call sites; a declaration with no call site does not trigger this scan.
The scan is conservative and does not prove control-flow reachability, so a call
inside an uncalled function can still require a proof. FFI flags belong to
`brat` only up to `--`; after it an argument is the program's, even one called
`--header`.

Both `brat` and `lang/cxx/bin/brat-cxx` (`build`, `run`, `dist`, `emit-c`)
enforce the exact same `--header` / `.bratbind` / `--unsafe-ffi` gate and embed
the `BRATFFI1` contract block in compiled binaries.

## Structs behind a pointer

A C API normally hands out an opaque handle and owns the memory. The struct is
still described in the header, so BRAT can read and write it by name:

```brat
extern "libnode.so" fn node_new(id: i64) -> move ptr
extern "libnode.so" fn node_free(p: move ptr)
extern struct Node { id: i64, pos: Vec2 }
extern struct Vec2 { x: f32, y: f32 }

let h = node_new(7)
let n = ptr_read_struct(h, "Node")        // a copy, the library keeps its own
print(n.pos.x)
ptr_write_struct(h, Node { id: 99, pos: n.pos }, "Node")
node_free(move h)
```

## Typed windows: registers, descriptor tables, kernel buffers

A driver does not hand out `i64`. A register block is `u32`, a kernel buffer is
`float`, a descriptor table is an array of structs. A window carries its element
width, so the stride is the caller's choice and not a guess:

```brat
let dev = dev_open(4)                  // some opaque handle
let regs = slice_of_ptr(dev, 8, "i32")   // eight 32-bit registers
slice_set(regs, 2, 0x1F)               // range-checked: i32
print(slice_get(regs, 2))

let buf = slice_of_ptr(dev, 64, "f32")   // a float buffer
slice_set(buf, 3, 2.5)

let bytes = slice_of_ptr(dev, 256, "u8") // the same memory, byte by byte
```

## Packed records

A driver header is full of records that were declared packed because the
hardware reads the bytes and not the C struct:

```c
typedef struct __attribute__((packed)) { unsigned char kind; int len; } Hdr;
```

BRAT says `packed` where the header says the attribute, and the layout follows:

```brat
extern packed struct Hdr { kind: u8, len: i32 }

let mut p = hdr_make(7, 1234)   // the callee returns ptr[Hdr]
let d = p[0]                    // the record, read at the header's offsets
print(d.kind)                   // 7
print(d.len)                    // 1234 -- offset 1, not 4
```

Without `packed` the same declaration reads `len` at offset 4, which is what an
unpacked compiler would have written, and that is the difference the proof is
looking for: `brat ffi --header h.h` compiles `sizeof`/`offsetof` assertions
against the header itself, so a BRAT declaration that is packed when the header
is not (or the reverse) is an error, not a silently wrong read. Widths are part
of that check: a header's `unsigned char` matches `u8`, `unsigned short` matches
`u16`, and declaring `i32` there is refused.

## A pointer the callee types

The element of `ptr[T]` is what says how wide a step is, and an extern's return
type says it once for every caller:

```brat
extern "libapi.so" fn node_new(id: i64, x: f32, y: f32) -> ptr[Node]
extern "libapi.so" fn regs_get(n: ptr[i32]) -> ptr[Reg]

let mut n = node_new(7, 1.5, 2.0)
print(n[0].id)                  // the record, not its address
let tab = regs_get(0)           // ptr[Reg]: three records a C library owns
let t = slice_of_struct(tab, 3, "Reg")
print(slice_get(t, 1).count)    // 20
```

`slice_of_struct` is read-only: the table belongs to whoever allocated it, so
`slice_set` on it is refused (`read-only window`). A window built inside the
program with `slice_struct_of([...])` is writable, and its stores are visible to
whoever reads the values afterwards.

`let p = node_new(...)` needs no annotation for the same reason the annotation
exists: the callee already said `ptr[Node]`. Write `let p: ptr[Node]` when you
want to be explicit; both produce the same program.

Kinds: `"i64"`, `"i32"`, `"f64"`, `"f32"`, `"u8"`. Every kind is checked twice:
the index against the window's length, and the value against the kind's range —
`i32` refuses `2147483648`, `u8` refuses `300`, rather than truncating either.
Windows alias on purpose: writing the `f32` at element 1 of one window is
visible as the `i32` at element 1 of another over the same bytes.
