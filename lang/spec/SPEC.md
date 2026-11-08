# BRAT Language Reference (v0.7)

This document specifies the syntax, runtime semantics and static checks of BRAT
as implemented by `lang/cxx/src/` (`brat-cxx`) and `lang/engine/crt.h`. Known
checker boundaries and operational limits are summarized in Section 11,
[SECURITY.md](../../SECURITY.md) and [LEDGER.md](../../docs/project/LEDGER.md).

## 1. Execution model

BRAT source files (`.brat`) are UTF-8 text without embedded NUL bytes. The
compiler resolves reachable `import` declarations into one flattened program,
runs static ownership, type, effect and FFI checks, emits GNU C11 against
`lang/engine/crt.h`, and invokes a host C compiler to produce an executable or
shared library.

Top-level statements execute in order. Defining `fn main() -> i64` or
`fn main() -> Result[i64, E]` is optional; when present, the returned integer
(or `Ok(code)`) sets the process exit status, and `Err(e)` reports to `stderr`
and exits with status `1`.

## 2. Values and types

### 2.1 Scalar and composite types

- `i64`: signed 64-bit two's-complement integer (`+`, `-`, `*` and unary `-`
  wrap on overflow; `/` floors toward negative infinity; `%` takes the sign of
  the divisor; division or modulo by zero raises a catchable runtime error;
  decimal, `0x`, `0b` and `0o` integer literals; bitwise `&`, `|`, `^`, `~`,
  `<<` and arithmetic `>>` masking shift counts to `0..63`).
- `f64`: IEEE-754 binary64 floating-point number (`nan` is unequal to everything
  including itself and prints as `nan`; infinities print as `inf` and `-inf`).
  `i64` and `f64` do not convert implicitly.
- `bool`: `true` or `false`. Short-circuit logical operators are `&&`, `||` and `!`.
- `str`: immutable, NUL-terminated UTF-8 string. `len(s)`, `s[i]`, `s.slice(a, b)`
  and `s.chars()` index by Unicode code points, not bytes. String literals
  support `\n`, `\t`, `\r`, `\\`, `\"`, `\'` and `"{expr}"` expression
  interpolation (braces that do not parse as a single expression remain literal
  characters). Raw string literals disable escape and interpolation processing.
- `vec[T]`: dynamically sized, bounds-checked sequence (`[1, 2, 3]`).
- `map`: insertion-ordered hash/linear table (`{"a": 1}`). Valid keys are `str`,
  `i64`, `f64` and `bool`, distinguished by both runtime tag and value (`1`,
  `1.0` and `true` are three distinct keys). Indexing a missing key with `m[k]`
  raises `no key`; `m.get(k)` returns `None` when absent and `m.get_or(k, def)`
  returns `def`.
- Nominal `struct`s, tagged records, `enum`s, `Option[T]` and `Result[T, E]`:
  `[pub] struct Name { field: Type, ... }` declares a nominal struct with
  checked field names/types, shorthand literals (`Point { x, y }`), canonical
  declaration-order slot layout, and closed or open (`Point { x, .. }`) pattern
  destructuring; undeclared `Point { x: 1, y: 2 }` still creates an ad-hoc map
  tagged `"Point"`. `[pub] enum Name { A, B(x), C(i64), D(id: i64, msg: str) }`
  defines untyped, positional typed, or named typed variant constructors.
  `Option[T]` (`Some(v)`, `None`) and `Result[T, E]` (`Ok(v)`, `Err(e)`) are
  predeclared in the prelude. Variants cannot be indexed with `[]`; destructure
  them with `match`, `if let`, `?` or `unwrap` / `unwrap_or`.

### 2.2 Equality and ordering

`==` and `!=` compare values structurally across all types (`nan != nan`).
Relational operators (`<`, `<=`, `>`, `>=`) and `sort` / `min` / `max` order
values within the same value class:

1. Numbers (`i64`, `f64`, `bool`) compare by exact mathematical value
   (`9007199254740993 > 9007199254740992.0` is `true`, and `i64` pairs compare at
   full 64-bit precision). Comparing `nan` with `<`, `<=`, `>` or `>=` returns
   `false`.
2. Strings compare lexicographically by Unicode code point.
3. Vectors compare lexicographically element-by-element, then by length.
4. Maps compare by struct tag, then pairwise key/value entries, then length.
5. Enum tags compare by tag name, then payload items, then arity.
6. `None` orders below other values; comparing across incompatible classes
   raises `type error in <` (or the written operator).

### 2.3 Built-in methods

User `impl` methods always shadow built-in methods of the same name. Built-in
method arity is checked statically during `brat-cxx check`:

- `str`: `split([sep])`, `split_once(sep)`, `lines()`, `trim()`, `lower()`,
  `upper()`, `starts_with(p)`, `ends_with(s)`, `contains(sub)`, `index_of(sub)`,
  `replace(old, new)`, `repeat(n)`, `chars()`, `copy()`, `parse_int()`,
  `parse_float()`. ASCII whitespace (`' '`, `\t`, `\n`, `\v`, `\f`, `\r`) is used
  by `trim`, zero-arg `split()`, `parse_int` and `parse_float`. `parse_int`
  returns `Err("number out of range")` outside `i64` rather than wrapping.
- `vec`: `push(x)`, `pop()`, `ins(i, x)`, `del(i)`, `first()`, `last()`,
  `slice(a, b)`, `join(sep)`, `sort()`, `sort_by(fn)`, `rev()`, `sum()`,
  `min()`, `max()`, `enumerate()`, `zip(other)`, `chunks(n)`, `pos(x)`, `copy()`,
  `map(fn)`, `filter(fn)`, `fold(init, fn)`, `any(fn)`, `all(fn)`, `find(fn)`.
- `map`: `get(k)`, `get_or(k, default)`, `has(k)`, `set(k, v)`, `del(k)`,
  `keys()`, `vals()`, `items()`, `copy()`.
- `Option[T]` and `Result[T, E]` tags: `unwrap()`, `unwrap_or(default)` (evaluates
  `default` lazily only on `None` / `Err`), `is_ok()`, `is_err()`, `is_some()`,
  `is_none()`.

## 3. Ownership, mutability and borrows

Scalars (`i64`, `f64`, `bool` and FFI numeric widths) are `Copy`. Strings,
vectors, maps, tags, closures and owned FFI pointers (`move ptr`) follow single-owner
move/borrow rules checked intra-procedurally by `borrow.cpp`:

- Bindings default to immutable (`let x = ...`). Reassignment (`=`, `+=`, `-=`,
  `*=`, `/=`, `%=`), field/index writes and mutating receiver methods require
  `let mut` or a `mut` parameter. `const NAME = expr` is permanently immutable.
- Assigning a non-Copy binding (`let r = s`) or passing an argument to a `ref`
  parameter (the default parameter mode) borrows the source place until the last
  read of the borrower.
- `move x` transfers ownership: subsequent reads of `x` fail with `E0382` (`use
  of moved`), and moving while a borrow is live fails with `E0505`.
- Passing a binding to a `mut` parameter requires `let mut` and exclusive access
  across all arguments of the call (`law of exclusivity`).
- Returning a local `ref` borrow or `ptr_of_str(local_s)` from a function, or
  capturing a `ref` borrow inside `spawn` or an escaping closure, is rejected.
- `for x in container` holds a read-only view of `container` across the loop
  body; mutating `container` inside the loop is rejected (`cannot mutate \`v\`
  while iterating it`).

## 4. Static type islands and numeric lowering

Type annotations are optional. Functions with an annotated return type (`-> i64`,
`-> f64`, `-> bool`, `-> str`, `-> vec[T]`, `-> map`, `-> StructName`,
`-> EnumName`, `-> Option[T]`, `-> Result[T, E]`, `-> none`) are statically
type-checked (`check.cpp`), including `if x.is_some()` / `if r.is_ok()` flow
narrowing for `unwrap()`; unannotated functions operate on dynamic `Val` values.
Generic type parameters (`fn first<T>(v: vec[T]) -> T`) are checked for
declaration consistency and erased before code generation.

Functions whose parameters are `i64`, `f64`, `bool`, `vec[i64]` or `vec[f64]`
and whose body uses arithmetic, comparisons, loops and calls to compatible
numeric functions compile to unboxed C scalar functions (`int64_t`, `double`,
`bool`, raw `Vec *` item access) paired with a tag-checking boxed wrapper for
dynamic callers. At `-O1` and `-O2`, non-escaping, unaliased local `struct`
bindings (including nested structs) are decomposed by Scalar Replacement of
Aggregates (SRA) into local scalars or `Val` slots without heap allocation.

## 5. Functions, closures, traits and errors

- Default parameter values (`fn connect(host: str, port: i64 = 443)`) are
  evaluated at the call site and must form a trailing suffix of the parameter
  list.
- Closures (`let f = fn(x) { return x + 1 }`) capture outer bindings by shallow
  copy. Named `fn` declarations are not first-class values; they may be called
  directly, passed as callbacks to built-in higher-order `vec` methods, or named
  by string literal in C FFI callback parameters.
- `trait Name { fn m(self) -> i64 }` declares method signatures; `impl Name for
  Type { ... }` must implement every method in the trait. Associated functions
  without `self` inside `impl Type` are called as `Type.func(...)`. Dynamic
  trait objects are not supported.
- `?` unwraps `Ok(v)` / `Some(v)` and passes non-tagged values through. Inside
  functions annotated `-> Result` / `-> Result[T, E]`, `Err(e)?` returns `Err(e)`
  early from the function; in non-`Result` functions, `?` on `Err(e)` or `None`
  unwinds to the enclosing `try/catch` (or terminates unhandled). `expr or
  fallback` substitutes `fallback` when `expr` evaluates to `None` (never for
  `Err`).
- `try { ... } catch e { ... }` catches `Err` unwinding from `?` / `raise(msg)`
  and runtime errors, binding `e` to the error payload while letting `return`,
  `break` and `continue` pass through untouched. `defer stmt` runs `stmt` in
  LIFO order on block exit along both normal and error paths.

## 6. Control flow

- `if cond { ... } else { ... }`, `if let Pat = expr { ... }`, `while cond { ... }`,
  `while let Pat = expr { ... }`, `for x in vec_expr { ... }`, and half-open
  integer range loops `for i in lo..hi { ... }`. Nested `let`, `match` arm and
  `catch` bindings are lexically scoped to their block without overwriting outer
  bindings of the same name, whereas `for` loop targets reuse an existing outer
  slot when one is in scope.
- `match expr { pat => { ... }, ... }` supports literals, variable bindings
  (lowercase), nullary enum tags (uppercase), struct destructuring
  (`Point { x, y }`, `Point { x, .. }`), nested destructuring (`Some(Ok(v))`),
  guards (`x if x > 0 => ...`) and `_` (must appear last). Duplicate
  literal/variant patterns are parse errors. Exhaustiveness without `_` is
  checked across local and imported `pub enum` types, `Option[T]`,
  `Result[T, E]`, closed `struct` patterns and `bool`.
- `unsafe { ... }` marks raw pointer or FFI sections for human readers without
  disabling any borrow, ownership or type check.

## 7. Concurrency and `par for`

- `spawn { ... }` runs on a real OS thread (`pthread` / Win32 thread pool).
  `join_all()` waits for tasks spawned in the current frame and re-raises the
  first uncaught child error; `scope { ... }` joins all transitive tasks spawned
  within the lexical block on normal exit, `return`, `break`, `continue` or
  error unwind.
- While a `spawn` is active (before `join_all()` or `scope` exit), captured
  places obey single-writer / multiple-reader exclusivity (B0522).
- `chan(cap)` (`1 <= cap <= 1048576`) creates a bounded thread-safe queue.
  `ch.send(v)` blocks when full and transfers ownership of non-Copy `v` (`use of
  sent \`v\`` on reuse); sending on a closed channel returns `Err`. `ch.recv()`
  blocks when empty and returns `None` once closed and drained. `ch.close()` is
  idempotent.
- `par for x in v reduce(op, unit) { ... }` evaluates pure, side-effect-free
  bodies in parallel across the worker pool and reduces results deterministically
  in index order (`0..len(v)`).

## 8. Modules, interfaces and packages

- `import a.b` loads `a/b.brat` from the entry directory, `vendor/`, or
  `lang/std/` (`$BRAT_STDLIB`). Only `pub fn`, `pub struct` and `pub enum`
  declarations (checked against an adjacent `b.bri` interface file when present)
  are visible to importers; `pub use a.b.fn_name` re-exports an imported public
  function without duplicating it.
- `brat-cxx pkg` resolves local archives, GitHub tarballs and `$BRAT_INDEX` /
  `brat-index.json` constraints (`=`, `>`, `>=`, `<`, `<=`), verifies SHA-256
  digests in `brat.lock`, compiles package `c_sources` and emits `.bratbind`
  DWARF/symbol manifests.

## 9. C FFI summary

- `extern ["lib"] fn name(params, ..VarTail) -> Ret [errno] [effects]`
- `extern [packed] struct Name { field: Type, ... }` and `extern union Name { ... }`
- Boundary widths: `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `usize`,
  `f32`, `f64`, `bool`, `str`, `ptr`, `ptr[T]`, `fn(...) -> T`, `none`.
- Out-parameters: `&mut_var` at an `extern` `ptr[T]` argument writes back exact
  `sizeof(T)` bytes on return.
- Pointer/buffer builtins: `ptr_null`, `ptr_is_null`, `ptr_offset`,
  `ptr_read_i64`/`f64`/`f32`/`ptr`/`str`/`struct`, `ptr_write_*`, `ptr_of_str`,
  `str_from_ptr(p, len)`, `slice_of(vec, lo, hi)`, `slice_of_ptr(p, len, kind)`,
  `slice_of_struct(p, len, "Name")`, `slice_len`, `slice_get`, `slice_set`.
- Effects and contracts: `pure`, `nothrow`, `reads(p)`, `writes(p)`, `borrows(p)`
  on `extern fn`; `thread(same|any)` and `reenter(yes|no)` on callback parameters.
- Build gate: any build or run invoking `extern` C functions requires `--header`,
  `--object`, a verified `.bratbind` manifest, or `--unsafe-ffi`.

## 10. Built-in regex, I/O and telemetry

- Regex (Pike VM in `crt.h`, linear time without catastrophic backtracking):
  `re_match`, `re_find`, `re_captures`, `re_replace` supporting literals, `.`,
  `*`, `+`, `?`, `{n,m}`, `[...]`, `[^...]`, `^`, `$`, `\d`, `\D`, `\w`, `\W`,
  `\s`, `\S`, `(...)` and `|`.
- System and I/O: `print`, `eprint`, `str`, `len`, `tagof`, `assert`, `raise`,
  `unwrap`, `is_err`, `is_none`, `is_bool`, `is_int`, `is_float`, `own`, `args`,
  `args_bytes`, `env_get`, `env_get_bytes`, `exit`, `time_ms`, `sleep_ms`,
  `read_file`, `read_bytes`, `write_file`, `append_file`, `write_bytes`,
  `fs_exists`, `fs_is_dir`, `fs_file_size`, `fs_mkdir_all`, `fs_list_dir`,
  `fs_remove_dir`, `fs_open`, `fs_read_line`, `fs_close`, `stdin_read_line`,
  `fs_rename`, `fs_remove`, `proc_run`, `sql_open`, `lib_open`,
  `lib_sym`, `lib_close`, `call`.
- Telemetry: `log.info(...)`, `log.warn(...)`, `log.error(...)` and
  `trace.span("name") { ... }` emit structured JSON lines (`ts`, `lvl`, `msg`,
  `span`).

## 11. Current guarantees and boundaries

1. `brat-cxx check` is an intra-function static checker, not a formal proof of
   memory safety or race freedom.
2. Heap objects are freed deterministically by reference counting (or bulk
   `arena` reset); container insertions check and refuse parent-pointer cycles at
   run time.
3. Values and call stacks are bounded at 2,000 levels by default
   (`BRAT_RECURSION_BUDGET`, plus a 256 KiB thread stack-floor check on Linux,
   macOS and Windows 8+).
4. FFI declarations, raw pointers and dynamic `lib_open` plugins are trusted
   past header/DWARF signature verification.
5. Relative file and SQLite operations default to module-root confinement unless
   `BRAT_FILES=1` is set, and subprocess execution requires `BRAT_EXEC=1` or
   `BRAT_EXEC_ALLOW`; run untrusted code inside an OS sandbox.
