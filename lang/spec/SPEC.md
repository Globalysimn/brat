# BRAT language reference

This reference describes intended behavior; implementation differences and checker gaps may exist. See [security](../../SECURITY.md) and [known risks](../../docs/project/LEDGER.md).

Coverage: the Python interpreter, the Python C99 emitter, and the C++20 front end are expected to agree on the language contract. The C++ front end emits the same C; it is not a third runtime. This specification follows the current implementation, and unsupported or incomplete guarantees are listed in §11.

## 1. Goals

Explore systems programming with explicit ownership operations and no lifetime-annotation syntax. The target is C99 output; the ergonomics borrow ideas from Python and Go. BRAT does **not** currently claim Rust-equivalent memory- or data-race safety. The checker enforces a useful but incomplete subset of ownership rules; see §11. Avoiding `'a` is a design constraint, not proof that every lifetime can be inferred soundly.

## 2. Types

```
i32, i64, u64, f64, bool, str, bytes
vec[T], map[K]V
Option[T] = Some(T) | None
Result[T,E] = Ok(T) | Err(E)
ref T — borrow (view), mut ref — mut-borrow
```

Literal: `let codes = [200, 404, 500]`, length `len(codes)`, index inside `for`.
String interpolation: `"{expr}"` — any expression parsed by the same parser (`"{len(v)}"`, `"{p.x}"`, `"{n + 1}"`, `"{m.get(k)}"`). Braces that do not contain exactly one expression remain text — therefore `"{\"a\": 1}"` works as a JSON template without escaping.
Strings are UTF-8: `len(s)` — code points (not bytes), `s[i]` — i-th code point, `chars()` — by code points, `slice(a, b)` — code point boundaries, identical in both backends. Escapes in literals: `\n \t \r \\ \" \' \0`.
map: `let m = {"a": 1}` — read `m["a"]`, write `m["a"] = 2` and `v[0] = x`, methods `m.get(k)` (value or None), `m.has(k)`, `m.keys()`, `m.del(k)`, `m.set(k, v)`. `len(m)` — number of keys, `for k in m` — traversal of keys in insertion order. Miss on key — error, not null. Keys str/int/bool/f64, typed in both backends (in C the key is stored as `Val`, comparison via `veq` — `m[1]` and `m["1"]` are different keys).
No `String vs &str`. There is `str` (ownership) + `ref str` (view). Conversion is automatic.
Copying: small types (`i32..f64,bool`) — Copy. Everything else — move by default.

## 3. Ownership

Model — Vale linear-aliasing (vale.dev/linear-aliasing-model): single ownership + moves, without borrow checker syntax. Checks — subset of rustc-dev-guide borrow-check: initialization, no double move, no move while borrowed, no access under mut-borrow.

- `let y = x` — borrow (as before). Calls also borrow, do not move.
- `move x` — ownership-transfer annotation, checked statically. Reuse is a compile error (`E0382`); moving while borrowed is `E0505`. For a simple variable move, both the interpreter and C runtime also mark the source as moved as a backstop. Projection moves such as `move p.field` are checked statically but do not have an equally complete runtime invalidation path. The static checker remains incomplete; see §11.
- Signatures carry ownership: `fn eat(s: move str)` takes ownership, `fn peek(s: ref str)` borrows for the duration of the call (NLL: the call borrow does not outlive the call). Without annotation a parameter is `ref`. Checks at the call site: passing to a `move`-parameter moves and is forbidden under a live borrow.
- Immutability by default (as in Rust): assignment (`=`, `+=`, `v[i] =`, `m[k] =`, `p.x =`) only in `let mut` or `mut`-parameter — otherwise `assign to immutable` with a line. Writing via a builtin mutator (`push`, `pop`, `sort`, `sort_by`, `ins`, `del`, `set`) is the same kind of write: `let v = []; v.push(1)` — error `cannot mutate immutable`. `const` — same prohibition always. `let mut x = y` — mut-borrow; live mut-borrow + any second — error `double mut borrow`. Passing to a `mut`-parameter requires a mut-place without live borrows and without duplicates in a single call (Law of Exclusivity). Borrows are isolated by if/while/try/spawn branches (NLL + scopes).
- `let mut` one writer OR N readers; duplicate `mut` — error.
- NLL-lite: a borrow dies at the last use of the borrower, not at the end of scope (liveness over direct blocks; through branches — conservative).
- `ref` from `return`/`spawn` — escape, error with name.
- A `for` holds a view of the container it walks for the whole of its body: writing into that container inside the loop (`v.push(x)`, `v[0] = x`, `m.set(k, v)`, passing `v` to a `mut` parameter) is `cannot mutate `v` while iterating it`. The rule reads a place, so `for i in 0..n` (a range) and `for x in v.copy()` (a fresh container) are unaffected. Both checkers apply it with the same words.

## 3.5 Static islands (typer)

`brat check` = borrow + typer. Functions with an annotated return (`-> i64/f64/bool/str/vec[..]/map`) are checked STATICALLY: expression types are inferred (literals, parameter annotations and `let x: i64`, call signatures), mismatch — error with snippet: `return: want i64, got str`, `arg 1 of `g`: want i64, got str`, `let `y`: annotated i64, got f64`, `assign to `a`: want i64, got str`. Top-level is also checked (calls to typed functions with explicit literals).
Unannotated — dynamic island, `dyn` compatible with everything; `Ok/Err/None` — tags, always compatible (the error channel is untyped in this implementation).
Rules: `int+int→int`, `float?_→float`, `str+T→str` (concatenation as in runtime), `str*int→str`, comparisons and `in` → `bool`, `/` over int → int (floor).
Ordering (`<`, `<=`, `>`, `>=`) is defined inside one class of value: numbers (`i64`, `f64`, `bool`) with numbers, and a number is compared by value, not by the `f64` it rounds to: two `i64` compare as integers (`1152921504606846976 < 1152921504606846977` is true), an `i64` against an `f64` compares exactly (`9007199254740993 > 9007199254740992.0`), `nan` equals nothing (itself included) and is ordered against nothing, `str` with `str`, a vec with a vec, a map with a map, a tag with a tag, and `None` below everything else. The rule holds at every level of a nested value: a cross-class comparison inside a vector, a tag argument or a map slot is a runtime error named after the operator the program wrote (`type error in <`), never an answer read off an internal tag enum and never a host-language exception. A vec orders element-wise with a length tie-break, a map compares its struct type, then keys and values pairwise, then its length, and a tag compares its name, then its arguments, then how many there are. `sort`, `sort_by`, `min` and `max` use that same order, so a container whose elements are not all of one class is refused rather than ordered arbitrarily.
Annotations are checked and affect types, rather than being decorative.

## 4. Functions

```
fn route(req: Request) -> Response {
  let up = pick_upstream()
  return forward(up, req)?
}

fn add(a: i64, b: i64) -> i64 {
  return a + b
}
```

- `mut` only where needed: `fn inc(x: mut i64) -> i64`
- default parameters: `fn greet(name: str = "world") -> str` — suffix of parameters, skipping from the tail substitutes the default (computed at call); `fn f(a, b = 1)` after a default all following must be default. In native — for direct calls (dynamic — arity as before).
- generics support: `fn first(v: vec[i64]) -> i64` (full generic typing is not in this implementation; no later release is promised)

## 5. Errors — real values

`Ok(v)` / `Err(e)` / `Some(v)` / `None` — runtime tags. `?` unwraps `Ok`/`Some`, propagates `Err` from the function, bare values pass through. `or` — default only for `None` (not for `Err`). `unwrap(x)` — value or propagate `Err` or panic on `None`. `is_err(x)`, `is_none(x)` — predicates. Uncaught `Err` at top — `unhandled` + exit 1. Integer division — integer, `/0` — error, not zero.

`try/catch` catches ONLY errors: `?`-unwinding of `Err` and runtime errors (div by zero, index out of range, no key — in BOTH backends these are channel values, not exit). `catch e` binds `e` to the error payload (message string for runtime errors). `return`/`break`/`continue` through `try` pass freely — catch does NOT swallow them (return-unwind is a separate channel). Uncaught at top: `unhandled: ('err', msg)` in native, `RuntimeError: msg at line N` in interpreter, exit 1 in both.

```
let resp = probe(up)?
let port = parse_port(s) or 8080
try {
  forward(up, req)?
} catch e {
  log.info("fail {e}")
  return serve_default()
}
```

## 6. Control

```
if is_up(up) { route(up) } else { mark_down(up) }
while running { serve_once() }        // serve_once, check: ordinary fn
match code { 200 => ok(), _ => retry() }
for x in backends { check(x) }
for i in 0..n { print(i) }
```

`match` — desugars into an `if`-chain on equality, `_` last, duplicates — error. Guards: `Ok(v) if v > 100 => ...` — guard failure leads to the next arm. Bare name in pattern: lowercase — binding scrutin (`x => ...` — same as `_`, but named), Uppercase — nullary variant. A guarded arm is not counted as total in exhaustiveness (guard may fail — need `_` or a full arm). Exhaustiveness as in Rust: if all variant-arms belong to one enum declared in the same file, each variant must have a total arm (sub-patterns — bind or `_`) or a trailing `_` is needed — otherwise parse error `non-exhaustive match: missing blue in enum Color` with line number. bool — closed domain: `match b { true => .., false => .. }` is exhaustive without `_`; a single literal — `missing false`. Arm after catch-all — error `_ must be last`. Literal arms without `_` allowed (dynamic), mixed enums — outside check.
`for x in v` — native traversal of vec, `for i in a..b` — counter `[a, b)`. `break`/`continue` work in both; `continue` in range does not break the step. Return `-> ref` forbidden by parser (dangling); `-> move` and plain — ownership outward.
`unsafe { ... }` — a marker for raw pointers and FFI, and a promise the author keeps. It switches off no check: the ownership, borrow and type rules apply inside it exactly as outside, and a borrowed parameter still cannot be moved.

## 7. Concurrency

```
spawn { check(up) }
join_all()
let ch = chan(4)
ch.send(x); let y = ch.recv(); ch.close()
scope { spawn { work() } }
```

Channel: `chan(cap)` with `1 <= cap <= 1048576` (`BRAT_MAX_CHAN_CAP`) — a bounded queue with a mutex and a
condition variable per channel. `send` blocks while the queue is full, `recv`
blocks while it is empty, `close` wakes every waiter; `recv` on a closed and
drained channel gives `None`, `send` on a closed one returns an `err` rather
than raising (a checker cannot see the ordering), a second `close` is fine.
A value has one holder at a time: `send` hands it to the channel, `recv` hands
it to the receiver. The checker follows that — using a binding after sending it
is `use of sent `x``, with a note on the send — and refuses to send a
parameter, because a parameter is a view of the caller's frame and the receiver
would read storage that is gone; `move` is the parameter that takes ownership.
`i32/i64/u64/f64/bool` are copies, so sending one leaves the binding usable.

Interpreter: `spawn` executes the block in a real thread (OS pool), environment — snapshot of frames at spawn time; shared vec/map objects remain shared. Concurrent mutation and aliasing are not covered by a complete race checker; see §11. `join_all()` waits for all tasks of the generation and propagates the first error. End of program does an implicit `join_all` — no lost threads. A task's channel
handle stays valid after the task ends, because a slot is only reused once the
channel it named has closed and drained. C-backend: real OS threads (CreateThread/pthread). Spawn block is lifted into a closure (same capture machinery). Error unwinding — as values (TLS `verr_pending`/`vuwind`, no `setjmp`), so it is thread-local: an error in a thread reaches `join_all` and is propagated to the main thread. `?`/unwind inside a thread does not cross foreign stacks.

## 8. Telemetry

```
log.info("up {host} ok")
log.warn("slow {ms}ms")
trace.span("forward") { forward(up, req)? }
```

Export: stdout JSON + OTel OTLP (runtime v0.3). Format: `{ts, lvl, msg, trace_id, attrs}`.

## 9. Modules, FFI

`import shapes.a` loads `a.brat` from `shapes/` next to the importing file, then from `vendor/`; paths must stay inside the root program directory. Each reachable file is parsed once, top level executes once (cycle protection). Functions are mangled `a__name` and the whole program becomes one flat AST, so all backends see the same names.

```
import shapes.a
```

Privacy is per file: `fn name` is private to its file, `pub fn name` is visible to importers. The check is static — a private cross-file call and a calling-private-qualified call are both errors in `brat run` and `brat build`, and the runtime never sees the call.

A call may be qualified or bare. `import shapes.a` accepts both `shapes.a.name()` and `a.name()`. A bare `name()` resolves when exactly one loaded module exports it; two exports of the same name make it ambiguous and the programmer must qualify (`ambiguous unqualified name`).

A module may ship an interface, `name.bri` next to `name.brat`, one `fn name(params) -> type` signature per line. When the file exists it is the export list: a listed function is visible even without `pub`, and a `pub` function that is not listed is private. A listed function that is missing from the implementation, that takes a different number of parameters, or that differs in a parameter or return type, is a load error: the interface is a promise, not a comment.

C values are `int64_t` end to end: `long` is 32 bits under Windows LLP64, so it is not a value type anywhere in the runtime or the emitted code. `pub use a.name` re-exports a function through the current module: importers of this module can call `this.name()` and a bare `name()` even though the definition lives in `a`. The target must be `pub` in its own module; a private target is an error, as is a name the target module does not define. A re-export is an alias, not a second definition, so it never makes a bare name ambiguous.

`mod` is not supported.

FFI declarations: `extern ["lib"] fn name(params, ..T) -> type;`, `extern ["lib"] { extern ... }` blocks, and `extern struct Name { field: type; ... }` are supported for C functions/structs, and every such `name`, `Name` and `field` is a plain identifier that is not a C keyword: it is the library's own symbol, written into the generated C as is, so a string or a keyword there is a parser error (`extern fn name wants a plain name`) rather than C text; FFI structs are emitted as `bf_<Name>` and their members as `bf_<field>`, so a name the runtime already declares (`Val`) still compiles; types: `i32`, `i64`, `f32`, `f64`, `str`, `ptr`, `none`, and extern struct names. `f32` is a boundary type: an argument narrows once into C and a result widens once back, so a kernel that takes and returns `float` needs no cast in BRAT and the arithmetic around it stays i64/f64. `extern packed struct` adds `__attribute__((packed))` to the emitted typedef, so a record the header declares packed is read at the header's offsets; packedness is part of what `brat ffi` proves. Extern widths include `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `usize`, `f32`, `f64`, `str`, `ptr`, `ptr[T]`, and struct names; the header is compared by width, so `unsigned char` matches `u8`. A `let` bound to a call whose extern returns `ptr[T]` inherits the element, so `p[0]` is a `T`. `&x` is an out-parameter: at an `extern`'s `ptr[T]` argument it passes the address of the binding's storage, and the binding holds what the callee wrote as soon as the call returns; the storage is the width of the pointee (`u8` one byte, `i16` two, `i32` four, `i64` eight, `f32`/`f64` four and eight). `&` is refused outside such an argument, refused on anything but a name (`&(a + 1)` is `` `&` takes a binding ``), and `let p = &x` needs `mut`. Two out-parameters in one call are supported. `ptr[T]` types a raw pointer by what it points at (`i64`, `f64`, `str`, `ptr`); a bare `ptr` converts to and from any `ptr[T]`, two different `ptr[T]` do not mix. `slice[T]` is a checked window: `slice_of(v, a, b)` over a vec, `slice_of_ptr(p, n, "i64"|"i32"|"f64"|"f32"|"u8")` over C memory, elements of that width (one byte for `u8`, four for `i32`/`f32`, eight for `i64`/`f64`), so a block of 32-bit registers or a `float` kernel buffer is read at its own stride; any other kind is a checker error
for `u8` — a read gives 0..255 and a write outside that range is refused (`slice_set: byte 300 out of range 0..255`); an `i32` window refuses a value outside -2147483648..2147483647 (`slice_set: i32 value 2147483648 out of range`)
Both refuse a bad range (`slice range 1..9 out of range 0..3`); `slice_get`/`slice_set` refuse an index outside the window (`slice index 5 out of range 0..2`), `slice_len` gives the length. `ptr_read_struct(p, "Name")` copies the bytes behind a pointer into a struct value whose layout is the `extern struct Name` declared in the same program, and `ptr_write_struct(p, value, "Name")` writes them back; both refuse a null or negative pointer and name the field they could not read. `str_from_ptr(p, n)` copies `n` bytes from C into a `str` and refuses a NUL (`str_from_ptr: NUL byte at 2`) or invalid UTF-8, so a C buffer is never truncated silently. A string literal containing NUL is a parse error, because native strings are NUL-terminated and would silently disagree with the interpreter on `len`. `..T` marks a typed variadic tail. Without `lib`, libc/main handle is used. Index: `v[0]` with bounds check. `len(v)` for vec and str. Methods via dot:
str: `split([sep])`, `lines()`, `trim()`, `lower()`, `upper()`,
  `starts_with(p)`, `ends_with(s)`, `contains(sub)`, `parse_int()`,
  `replace(a, b)`, `index_of(sub)` (index or None), `repeat(n)`,
  `split_once(sep)` (`[head, tail]` or None), `copy()`, `chars()`,
  `parse_float()` (`Ok`/`Err`).
  Whitespace, for `trim`, `split()` without a separator and `parse_int`/`parse_float`, is
  exactly the six ASCII characters space, `\t`, `\n`, `\v`, `\f`, `\r`; U+00A0, U+2003 and
  the other Unicode spaces are ordinary characters. `lines()` breaks on `\r\n`, `\n` and a
  lone `\r`, and nothing else. `parse_int` takes an optional sign and decimal digits; a value
  outside `i64` is `Err("number out of range")`, never a wrapped number. `parse_float`
  takes an optional sign and a decimal literal with an optional fraction and exponent, or
  `inf`, `infinity` and `nan` in any case; hex floats, `nan(...)` payloads and non-ASCII
  digits are `Err`. A NaN always prints `nan`. `replace("", x)` inserts `x` at every
  code point boundary and `split("")` and `split_once("")` raise `empty separator`.
vec: `join(sep)`, `push(x)` (length), `pop()`, `first()`/`last()` (None on
  empty), `sort()` (in-place, homogeneous), `rev()` (new), `ins(i, x)`,
  `del(i)` (value or error), `slice(a, b)`, `sum()` (0 on empty),
  `min()`/`max()` (None on empty), `enumerate()` (`[[i, x]...]`),
  `copy()` (shallow copy), `zip(v)`, `chunks(n)`.
map: `get(k)` (or None), `has(k)`, `keys()`, `del(k)`, `set(k, v)`,
  `items()` (`[[k, v]...]`), `vals()`, `copy()`.
Method precedence: user `impl`-methods ALWAYS above builtins (like inherent impls in Rust), identical in both backends. Operator `in`: `x in v`, `"k" in m` (key), `"sub" in s`. Duplicate literals in `match` — parse error. `defer expr` — LIFO; expr captures binding values at registration (like a closure), cleared on block exit. Assignments: `x += e`, `-=`, `*=`, `/=`, `%=` (incl. `v[i] += e`, `m[k] *= e`, `p.x += e`); multi-level write `a.b.c = v`. Destructuring: `let [a, b] = pair` (length checked at runtime, `_` skips slot). Structs: `Point { x: 1, y: 2 }` (type — Uppercase), access `p.x`, nesting `a.b.c`. Moving a struct `move p` invalidates fields too. Methods: `impl Point { fn sum(self) -> i64 { ... } }`, call `p.sum()`. `self` — first parameter, ordinary ownership. Traits: `trait Shout { fn loud(self) -> i64 }` — signatures only; `impl Shout for Point { ... }` must provide all methods, otherwise error `impl Shout for Point missing loud` (checked by both runtime and borrow). Trait objects (`let s: Shout`) are not in this implementation; a later release is not promised. Enums: `enum Res { Ok(v), Err(e) }` registers constructors; `Ok`/`Err`/`Some` from prelude — same tags. Empty variant — bare name (`Red`). Matching with binding: `Ok(v) => ...`, `None => ...`, duplicates — error. Nesting: `Has(Ok(v))`, `Has(_)`; bare name in sub-position — bind. `tagof(x)` returns the tag. Enum methods: `impl Color { fn code(self) -> i64 { match self { ... } } }`, call `g.code()` and `Red.code()` (nullary constructor + method). Duplicate methods in one `impl` — error on both sides. Generics by erasure (like Java): `fn first<T>(v) { return v[0] }` works for vec and str without monomorphization; duplicate `T` — error. Body checking is unified, turbofish not needed. Monomorphization is not in this implementation; no later release is promised. Closures by copy-capture: `let f = fn(x) { return x + 1 }`, named for recursion `fn fib(n) {...}`, `v.map(f)` / `v.filter(f)` with global fns and inline closures. A fn name is a callback, not a value: a bare `f` is accepted only as the callback argument of `map`, `filter`, `fold`, `any`, `all`, `find` and `sort_by` (and as the string literal an `extern` callback parameter takes, `run("f")`); anywhere else reading it is an error, `use of `f` as a value: a fn name is a callback, not a value`, in both checkers. There are no first-class function values, so `let g = f` never reaches a backend. Capture — copy of bindings, heap shared. Inline closure bodies are checked at their declaration/call site; returning a borrow of a local from a closure is rejected. A named function is still only a callback, not a general function value. Files and strings: `read_file(path)` gives `Ok(text)`/`Err`, `write_file(path, text)` gives `Ok(n)/Err`, `str(x)` converts int/float/bool to string. stdout/stderr of interpreter — always UTF-8 (like Go/Rust: bytes, no encoding failures). Exit codes: `main` returns i64 in exit code. `const X = v` — immutable: assignment to `const` — checker error. `brat build f.brat -o f.exe` — single binary via C; the compiler is `$BRAT_CC`, else `cc`. `zig cc` is used only for `--target` and an explicit `--static`. `brat pkg add <url>` — vendor/ + `brat.lock` with sha256; `brat pkg install` verifies hashes. `brat pkg add owner/name[@version]` resolves through an index — `$BRAT_INDEX`, else `./brat-index.json`, `{"packages": {"owner/name": {"1.2": {"url": ..., "sha256": ...}}}}` — takes the highest version when none is named, lists the known ones when the named version is missing (`acme/mathx has no version 9.9 (index has 1.0, 1.2)`), and stops before writing on a hash mismatch (`HASH MISMATCH acme/mathx@1.2`). `brat.lock` records `name` and `version` next to `file`, `url`, `sha256`, so `brat pkg list` prints `acme/mathx@1.2  <sha12>  <url>` and `brat pkg install` re-verifies. There is no hosted registry; local or HTTP(S) indexes and transitive dependency resolution are supported. `import` finds `vendor/` itself. `brat deps f.brat [-o graph.json]` prints `module -> import` edges (JSON with `root`, `modules`, `edges`); `brat api f.brat` prints the exported signature of every module, private functions excluded. Standard library in BRAT itself: `strlib`, `veclib` (21 asserts), `json` (esc/jstr/jarr/jobj, 7 asserts). Library code — `pub` only. Parser errors — always with a line (`stuck at 'x' at line 7`); end of file inside a construct — also an error, never hangs. `assert(cond)` / `assert(cond, msg)` — panic with message; `brat test` runs files and counts ok/FAIL by exit code. Parser recursion — limit 10000, fuzzer on 2000 inputs: 0 hangs. System builtins: `args()` (vec[str] of arguments), `exit(code)`, `eprint(...)`, `raise(x)` (Err-unwinding with payload x), `time_ms()`, `sleep_ms(n)`, `env_get(name)` (value or None), `re_match(s, pat)`, `re_find(s, pat)`, `re_captures(s, pat)`, `re_replace(s, pat, repl)` — regex subset (literals, `.`, `*`, `+`, `?`, `{n}`, `{n,}`, `{n,m}`, `[..]`, `[^..]`, ranges, `^`, `$`, `\d \D \w \W \s \S \n \t \r`, groups `( )` and alternation `|`), engine identical in both backends. Interpreter — backtracking over tree, native — set of states simulation (Pike VM), so patterns like `(a+)+b` do not explode exponentially. Not supported: POSIX classes `[[:alpha:]]`. Bad pattern — error `bad regex pattern`, not a silent literal. Libraries known only at run time: `lib_open(path)`, `lib_sym(h, name)`, `lib_close(h)`, and `call(fn, "i64(i64,i64)", a, b)`; the signature is a literal over `i64`/`f64`/`str`/`ptr`/`none` and is checked against BRAT argument types and arity before the call (`call: arg 1 wants i64, got str`). BRAT cannot verify the literal against the loaded C symbol's real prototype without `--header`; without a proof, `--unsafe-ffi` is required (see §11). `lib_open`/`lib_sym` raise `lib_open: <dlerror>` and `lib_sym: no symbol <name>`. HoF over vec: `fold(init, f(acc,x))`, `any(f)`, `all(f)`, `find(f)` (or None), `sort_by(f)` (by key), `pos(x)` (index or None). map: `get_or(k, d)`. `&&`/`||` — short-circuit semantics (right side not evaluated) in both backends. JSON parser in BRAT itself: `import jsonlib; json_parse(s)?` gives value (None/bool/i64/f64/str/vec/map) or Err. Parser errors — always with a line (`stuck at 'x' at line 7`) and source snippet (`--> file:line` + line). Module errors marked `(module name)`. Runtime errors — the interpreter prints `RuntimeError: div by zero at line 3` (line of the operator where the error occurred, including inside functions); the native runtime reports `unhandled: ('err', 'div by zero')` by default and appends `at line N` when built or run with `--trace-lines` (`BRAT_TRACE=1`). The message a `catch` binds is the same string in both backends. Checker prints Rust-style diagnostics: `file:line: error: msg` + snippet of the line below. `if let Ok(v) = e { } else { }` and `while let Some(x) = e { }` — desugar through the same mechanism as `match` (tags, bindings, `_` allowed).

## 10. Numeric fast path

A function whose ALL parameters are annotated `i64` (or ALL `f64`) and whose body is provably numeric (arithmetic, comparisons, `&&`/`||`, `if`/`while`/`forrange`, calls to such fast-functions of the same kind) compiles to raw C `long`/`double` without `Val` boxing — Rust/C speed on number crunching. Third kind — `i64v`: parameters `i64`/`vec[i64]` in any mix. vec arrives as `Vec *` directly: `v[i]` — element read without boxing (bounds + tag check), `v[i] = x` — raw write, `len(v)` — field `->len`, `for x in v` — raw loop over `items`. Every element of a `vec[i64]` parameter is checked (not just the vector): a non-`i64` element sends the call to the general path, so `[1, "a"]` still sums to `1a`. Wrapper checks tags: unsuitable arguments go to the general path, interpreter semantics preserved 1:1. Division `/` — floor as in Python (not C truncation), `%` — sign of divisor; in f64-functions `/` and `%` over purely-integer subexpressions are forbidden (interpreter would treat them as integer — divergence unacceptable). `forrange` bounds — integers in both kinds. `i64` overflow — wraps like Rust release, in BOTH backends (interpreter holds no bignums: sum and unary minus also wrap; `parse_int` is the exception, a literal outside `i64` is an `Err`, not a wrapped number). Returning `bool` from a numeric fast-function gives a real `bool`: `print` shows `True`/`False`, as the interpreter does. Mixing: fast-functions are called from dynamic for free, dynamic from fast — via the general path (then the function is not fast). `print` works inside fast: arguments i64/f64/str-literals with interpolation `{x}`; bool-producing expressions (comparisons, `&&`, `!`) forbidden — otherwise 1/0 vs True/False.

Copy-types (`i32/i64/u64/f64/bool` by parameter annotation) are not borrowed by the checker: `let x = n; return x` on an annotated scalar — copy, not ref escape. Without annotation the type is not fixed (dynamic code) — conservative borrow.


## 11. Current guarantees and limits

BRAT is experimental. A clean `brat check` means the program passes the implemented checker; it is not currently a general proof of memory safety or race freedom.

- Iteration is checked as a view, not as a borrow: a write into the iterated
  container inside the loop body is refused (see §3), but a write that reaches
  the same memory through foreign code -- a `ptr` handed to C, a plugin, an
  `unsafe` block -- is outside what the checker reads, as everywhere else.
  Only a `for` loop over a container opens that view. A `while` loop that
  writes through the index it tests -- `while i < len(v) { v.push(i); i += 1 }`
  -- is legal: nothing is being iterated, the loop re-reads `len(v)` each turn,
  and it terminates exactly when its condition says so. A `for` loop whose body
  grows the vec is the case the rule refuses, because the iteration holds the
  vec it is walking.
- Ownership is checked inside functions and at calls using the declared `move`/`ref`/`mut` modes. The module loader exposes signatures across modules, but the checker does not infer arbitrary effects or prove what foreign code does with an argument.
- A simple `move x` invalidates the source in both the interpreter and the C runtime. A move through a projection (`move p.field`, `move v[i]`) cannot empty the slot its parent still owns, so it is a counted copy: the binding is given a reference of its own, the value stays readable through the parent, and both releases happen. What the checker tracks is the place, not the holders: a value that is also held by a tag, a closure or a `spawn` frame is not counted as an alias of the moved binding, so `move` there reads as a transfer while the other holder keeps its own reference. That is safe for memory (refcounting), and it is not linearity: `move` is a transfer of ownership, not a proof of uniqueness. Refcounting releases managed strings, tags and containers, but reference cycles are not collected.
- `spawn` copies the frame, not the whole object graph. A value shared with a running `spawn` is governed by one rule: exactly one side may write it. Until `join_all()` or `join(handle)`, a write in the child plus any touch in the parent is an error, and a read in the child plus a write in the parent is an error; two readers are fine. Exchange between threads goes through a channel. This is the data-race-free part of the usual bargain, and it is what the rest of the language leans on: a program the checker accepts has no two conflicting accesses without a join between them, so it may be reasoned about as a sequentially consistent one (the DRF-SC theorem of Adve and Hill, POPL 1990). The join is the barrier that makes it true — `join_all()`, `join(handle)`, or the end of a `scope`. Runtime state that is shared no matter what the program says — refcounts, the literal and tag interning tables, the print buffer, `vset_at` — is synchronized in `lang/engine/crt.h` and checked under ThreadSanitizer. Pointers and foreign state carry no such rule and are the caller's responsibility (`unsafe`). `scope` joins child tasks on normal exit and on `return`, `break` and `continue`; the interpreter and native C path share a regression fixture in `lang/tests/fixtures/scope_unwind.brat`. The order in which tasks *run* is not specified, so the order of their output is not either: a program whose result depends on which task reached `print` first has no answer this specification promises, in either backend.
- FFI is a trusted boundary. `call` checks a signature literal against BRAT argument types and arity, but cannot verify that the loaded C symbol really has that prototype. A mismatch is undefined behavior. `lib_close` invalidates symbol IDs, and an active native call keeps the library loaded until it returns; however, a `ptr` returned by plugin code is not owned or pinned and must not be used after closing its library. Follow `docs/guide/ffi.md`.
- Values nest to `BRAT_MAX_VALUE_DEPTH` (2000) for `print`, `str()`,
  comparison and `own(x)`; past it both backends stop cleanly with `recursion too
  deep`. All runtime walkers (`vrelease`, `vprint_f`/`vsprint_q`, `veq`, `vcmp_`,
  `vown`, and their Python counterparts) use explicit bounds rather than
  unbounded machine call-stack recursion.
- `sort_by` evaluates its key function once per element (Schwartzian
  decorate-sort-undecorate) and caches the keys in both backends.
- Function names are callbacks in supported higher-order operations, not general function values. Closures are supported in the documented forms; arbitrary named-function values are not.
- Generics are erased rather than monomorphized. Trait objects, macros and async are not part of v0.7.
- Package resolution supports local or HTTP(S) indexes, `.tar.gz` and `github.com/owner/repo@ref` archives, `c_sources` + `.bratbind` native C verification, checksums, lockfiles and transitive dependency resolution; there is no centralized hosted BRAT registry.
- Runtime/library edge cases and known ownership gaps are maintained in `docs/project/LEDGER.md` and `SECURITY.md`. Those files, rather than an old migration checklist, are the source of truth for known issues.

## 12. Versioning and design policy

The current language is v0.7. A future feature is not promised until its syntax, semantics, diagnostics, interpreter behaviour and native C emission have regression coverage. Safety-related changes require tests in both the interpreter and generated C paths, and native emission must remain deterministic. The project priorities are tracked in `docs/project/ROADMAP.md`.
