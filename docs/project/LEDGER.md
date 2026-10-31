# LEDGER: Rust pain points against BRAT status

What BRAT does about each pain point, how it is proved, and what is honestly
missing. Each "proof" names a file in the tree.

## The C boundary

| What a driver header does | BRAT | Proof |
|---|---|---|
| `__attribute__((packed))` records | `extern packed struct Hdr { kind: u8, len: i32 }`, same typedef in all three backends | `test_ffi_ptrs.py` (7 / 1234 at offsets 0 and 1) |
| `unsigned char` / `short` fields | `u8` / `u16`, compared by width not spelling | `ffi.py` `same()`, `test_ffi_ptrs.py` neg |
| a callee that returns `T *` | `-> ptr[T]`, and `let p = f()` inherits it | `test_ffi_ptrs.py` |
| a table of records owned by C | `slice_of_struct(p, n, "Reg")`, read-only | `test_ffi_ptrs.py` refusal |
| a table built in the program | `slice_struct_of([...])`, writable | `test_ffi_ptrs.py` |
| a packedness or width mismatch | refused by `brat ffi` before anything links | `ffi.py` probe, `test_ffi_ptrs.py` |
| `ioctl(fd, req, &arg)` | `fill(&n, 21)`, width from the pointee | `test_out_params.py` |
| memory the program owns | `malloc` -> `ptr[u8]`, a `u8` window over it | `test_out_params.py` |
| a callback with `f32` / narrow parameters | `fn(f32)->f32`, widened once inside the wrapper | `test_ffi_widths.py` |
| a callback that returns a record | `fn(i64) -> Pt`, returned by value (native backend) | `test_ffi_widths.py` |
| a record in a variadic tail | `extern fn f(n: i64, ..Pt)`, passed by value | `test_ffi_widths.py` |
| a library flag a name cannot express | `--link -lcudart`, after the objects | `test_ffi_widths.py` |
| a struct-returning callback in the interpreter | refused with a sentence, not a `ctypes` `TypeError`; CPython cannot do it | `test_ffi_widths.py` neg |
| a C function that writes through a pointer, called across a `spawn` | `writes(p)` on the declaration makes the foreign call a write, so the race rule sees it | `test_ffi_effects.py` (5 cases, both checkers) |
| the compiler cannot know a C function is side-effect free | `pure` / `pure reads(p)` / `nothrow` on the declaration, emitted as `__attribute__((const))` / `((pure))` / `((nothrow))` | `test_ffi_effects.py` `check_emitted_attribute` |
| a declaration that promises two incompatible things | refused at parse time in both front ends, one message: `pure` against `writes`, `move` or `errno`; `reads`+`writes` of one parameter; an effect on a non-pointer | `test_ffi_effects.py` (9 cases, identical messages) |
| a C function that calls a callback from a thread the program never made | `thread(any)` on the callback parameter; then the callback's body must not reach a global, or a function that reaches one | `test_ffi_callback_contracts.py` (`any_thread_may_not_touch_state`) |
| a callback that calls its own library again (the `qsort` comparator shape) | refused unless the declaration says `reenter(no)`; the rule follows the call graph | `test_ffi_callback_contracts.py` (`reentry_refused`, `reentry_via_helper`) |
| a contract written where it does not belong | the parser refuses it in both front ends: on a non-callback parameter, at the end of the declaration, twice, or as an unknown word | `test_ffi_callback_contracts.py` (5 cases, identical messages) |

## Memory and ownership

| Rust pain | BRAT | Proof |
|---|---|---|
| Lifetime annotations | none needed; the program implies them | `lang/examples/*.brat` has no `'a` |
| Borrow checker rejections | NLL-lite: a borrow ends at the borrower's last use | `own_good.brat`, `test_ownership.py` b04/b05 |
| Use after move | `move x`, then `use of moved `x`` | `own_move.brat` |
| Move under a live borrow | `cannot move `x` while borrowed by `y`` | `own_sig.brat` |
| Two mutable borrows | `double mut borrow` / `law of exclusivity` | `test_ownership.py` |
| `String` against `&str` | one `str`, `ref` is a view | `lang/spec/SPEC.md` §2 |
| `unsafe` blocks | a marker for FFI and raw pointers; it disables no check | `borrow.py`, `borrow.cpp`, `test_hardening.py` |
| Dangling `-> ref` | not expressible: `-> ref` is a syntax error | `test_lang.py` |
| Function values | none: a fn name is a callback, not a value | `test_ownership.py` a34-a37 |
| script-level state | `let` at the top level is a file-scope binding a function reads and writes; a closure sees it live and shadows it with its own frame | `test_globals_in_fns.py` (7 cases, three backends) |
| Duplicate methods in an `impl` | `duplicate method T.m` in both checkers | `borrow.py`, `check.cpp` |

## Errors

| Rust pain | BRAT | Proof |
|---|---|---|
| `?` only in `Result` functions, `unwrap` panics | `?` anywhere, `or` for `None`, `try`/`catch`, `is_err`/`is_ok` | `result.brat`, `test_lang.py` |
| `anyhow`/`thiserror` and boxing | one `Ok`/`Err`, no libraries | `SPEC.md` §5 |
| Panic in debug, wrap in release | division by zero is an error in every build | `runtime.py`, `crt.h` |
| A parser that hangs on bad input | recursion limit plus a fuzzer | `test_fuzz.py`, 2000 inputs, no hangs |

## Types

| Rust pain | BRAT | Proof |
|---|---|---|
| Traits, orphan rule | `impl` with conformance checked on both sides; no crates, so no orphan rule | `trait.brat` |
| Generic monomorphization | erasure, like Java; a duplicate `T` is an error | `generic.brat` |
| Macros | none; methods, generics and builtins cover what the corpus needs | documented decision |
| Trait objects | none in v0.1 | `SPEC.md` §11 |
| The integer zoo | `i64`, `f64`, `bool`, `str`; wrapping overflow, floor division | `SPEC.md` §2 |

## Concurrency

| Rust pain | BRAT | Proof |
|---|---|---|
| `async`/`await`, `Send`/`Sync`/`Pin` | `spawn { }`, `scope { }`, `join_all()`, channels; OS threads in both backends | `spawn.brat`, `test_channels.py` |
| Data races | one writer: a value shared with a running `spawn` may be touched by exactly one side, until `join_all()`/`join(handle)` | `test_capability.py` (17 cases, both checkers); `test_scope_unwind.py` |
| Unbounded queues | `chan(cap)` is bounded and a sent value has one holder | `test_channels.py` |

## Tooling and ecosystem

| Rust pain | BRAT | Proof |
|---|---|---|
| Slow builds | interpreter instant, native build about a second for a small program | `test_native.py` timing |
| Cargo weight and supply chain | `brat pkg` with a URL or index entry, sha256 in `brat.lock`, one `vendor/` | `test_pkg.py`, `test_registry.py` |
| Cryptic errors | `file:line: error: message` plus snippet, note and hint; stable codes in `--json` | `diag.py`, `test_diag_json.py` |
| Edition churn | no editions; compatibility is the test suite | this file |
| No `mod` (crates) | `import a.b` with `.bri` interfaces and `pub use` | `test_modules*.py` |

## Speed

- Native backend: 54 programs byte-identical to the interpreter
  (`test_native.py`), and the agreement corpus in `test_agreement.py` at zero differences.
- The `i64` fast path compiles provably numeric functions to raw C integers, and
  `-O1` in `brat-cxx` removes division, modulo and index checks where an interval
  proof holds (`test_cxx_opt.py`).
- 100 million iterations of `s = (s + i * 3) % 1000003; i = i + 1`, `-O2`, same
  machine: C 430 ms, BRAT with `i64` annotations 434 ms, BRAT untyped 4013 ms.
  The untyped figure was 5035 ms until this pass: the temporaries the emitter
  declares were `volatile`, a leftover from when a raise used `longjmp`. A raise
  sets a thread-local flag and the frame leaves by `goto`, so nothing can cross a
  temporary, and volatile only pinned every `Val` to the stack -- 20% of the run.
  The rest of the gap is the 24-byte `Val`: it is past the 16-byte limit for
  register passing, so every operand and result round-trips through memory. Two
  integer operands also now skip `vnum0` and the numeric walk in `vadd`, `vsub`,
  `vmul`, `vmod` and `vcmpt`, which is what a loop condition and an accumulator
  are, but that is the smaller half.

## Known holes, with a repro where one is short

- Ownership is intra-function plus the modes a signature declares. A callee that
  stores a value where the checker cannot see it is not tracked.
- A simple `move x` is enforced statically and marks the source as moved in both
  runtimes by default. The marker is a backstop; projection moves and aliases do
  not have a complete runtime invalidation path.
- `spawn` copies the frame, not the object graph, so sharing is governed by a
  rule rather than by a type: while a `spawn` runs, exactly one side may write a
  shared place, and a reader in the child forbids a writer in the parent. FFI
  pointers and `unsafe` blocks remain outside that rule. `scope` joins on
  `break`/`continue` as well as ordinary exit and `return`; the regression is
  `lang/tests/test_scope_unwind.py`.
- A channel slot is reused once the channel closes and drains, so a handle held
  past that point names a closed channel.
- ~~`fn f() -> f64 { return 3 }` prints `3` in the interpreter and `3.0`
  natively~~ **closed 2026-10-30**. The annotation does not coerce either way:
  an `f64` function with an integer `return` anywhere -- on one branch only, or
  on all of them -- no longer takes the `double` fast path, so the integer
  survives the tagged-union path the interpreter takes as well. The parameter
  half was closed on 2026-10-29 (`runtime._coerce_params` converts an `i64`
  argument at every binding site; a closure literal is deliberately not coerced,
  because the C emitter types its parameters dynamically). Pinned by
  `test_f64_semantics.py` (`integer_return_stays_integer`,
  `integer_return_no_params`, `mixed_int_and_float_return`,
  `mixed_int_return_second`) and by the `f64_param_coercion` agreement case.
- Dynamic plugin signatures are checked against BRAT arguments, not an
  arbitrary C prototype. A wrong prototype remains undefined behavior; see
  `SECURITY.md` and `docs/guide/ffi.md`.
- An FFI proof is now carried by the artifact (2026-10-30). `brat build` writes the
  extern surface into the C as a marked `static const char brat_ffi_contract[]`:
  every declaration with its widths, every `extern struct` with size, alignment
  and offsets, the target's pointer and `long` widths, and the sha256 of the
  header it was proved against. `brat ffi --bin FILE` reports what a binary
  claims, and with the source in hand re-proves the record and names each drift
  (exit 1). Pinned by `lang/tests/test_ffi_contract.py`. What is still open: the
  record is written by the `brat` driver, so a binary from `brat-cxx build`
  directly carries none and says so; the proof was opt-in (`--header`) at the
  time, and is required as of the entry below; and a header can still disagree
  with the object file beside it, which only the linker could know.
- A pointer written inside `spawn` and touched outside was not a race (2026-10-30).
  The sharing pass read `ptr_write_i64(p, 1)` as a *read* of `p` — only `v.push`
  and friends were mutations — and a `move` argument as no effect at all, so
  `spawn { ptr_write_i64(p, 1) }` with a write in the parent, and
  `spawn { ptr_read_i64(p) }` followed by `free(move p)`, both checked clean.
  Both are now writes: a `ptr_write_*`/`slice_set`/`fill` call writes the place
  its first argument names, and a `move` ends that place's life here.
  `&x` now resolves to the place of `x` on the way. Fixed in `capability.py` and
  `capability.cpp`; four cases in `test_capability.py` pin it (two refused, two
  accepted once a `join_all()` is between the accesses), each through both
  checkers.
- The C cleanup idiom was refused as a double free (2026-10-30). `free` on the
  error branch, `return`, `free` on the normal path is one free per path, but the
  capability lattice joined every branch into the state after the `if`, including
  the branch that returns, so the second free looked like a second free of a moved
  pointer. Only `return` is an exit: `break`/`continue` leave the body, not the
  function, and a free on the way out of a loop is still the same pointer after
  it, so that shape stays refused. Fixed in `caps.py` and `caps.cpp`, and the same
  rule tightened in `borrow.py`/`borrow.cpp`, whose `move` merge had the matching
  hole: `if c { free(move p); break }` then `free(move p)` was accepted by the
  ownership pass and is a real double free. Pinned by three new cases in
  `test_caps.py` (`free_on_the_error_branch`, `free_twice_without_a_return`,
  `free_through_a_break`), each run through both checkers.
- A build that calls C without a header is refused (2026-10-30). Step 2 of the
  FFI plan: `build`, `run`, `dist` and `emit-c` run the same gate, and a program
  with an `extern` and no `--header` is not compiled. The refusal names both ways
  out, because a refusal with no way forward is a wall; `--unsafe-ffi` builds it,
  prints `calls C without a proof (--unsafe-ffi)` on stderr, and the contract in
  the binary still reads `against no header`. `brat run` refuses before
  `os.execv`, so the gate cannot be outrun by the exec; `--interp` is the
  reference implementation, not a binary, and is not gated, and a program with no
  `extern` has nothing to prove. Both flags are taken only up to `--`, so an
  argument a program is given is never eaten. The suite's own fixtures call C
  they write themselves and take the opt-out by name. Pinned by
  `lang/tests/test_ffi_mandate.py`.
- An `export fn` with an `f64` parameter lost it (2026-10-30). The C wrapper packs
  its arguments into a `Vec`; the Python emitter appended `double a1` to the
  prototype without the matching `vec_push`, so the callee read one element past
  what was written -- garbage, or a segfault, in a shared library built by that
  emitter. The C++ emitter emitted the push, and only it was exercised by default,
  so the two disagreed on a program no test compared. Both emit it now, and
  `test_ffi.py` builds the exported library with each emitter and calls it through
  ctypes in a subprocess.
- `slice_of_struct(p, n, "T")` refused a correct program (2026-10-30): an extern
  annotation is `("ptr", "T")` while an index into one answers the record itself,
  so the comparison read `want ptr[T], got ptr[T]`. `typer._resolve_ptr` resolves
  the name on both sides. Found because `brat build` now runs the Python type gate
  for programs with `extern` (it used to hand every build to `brat-cxx build`),
  and that gate refuses the same program as `brat check` -- same front end, same
  wording, including for a `call()` signature that disagrees with the declaration.
- An `extern struct` is laid out by the target's data model (2026-10-30): the
  emitted `_Static_assert`s used 8-byte pointers for every triple, so a 32-bit
  build of a program with an extern struct failed its own proof. `ffi.field_shape`
  and `ffi.wide_align` own the rule (i686 aligns 8-byte scalars to 4), the C++
  emitter takes the two numbers through `BRAT_FFI_PTR`/`BRAT_FFI_A8`, and
  `test_ffi_hardening.py` blesses the i686 numbers with a real `cc -m32`.
- `v.map(f)` over a closure no longer leaks: `lang/examples/closure.brat` is
  clean under LeakSanitizer, as are `map.brat`, `fastvec.brat`, `syslib.brat`,
  `kapp.brat` and the whole `test_containers_own` and `test_reclaim_calls`
  corpora. `lang/tests/test_asan_examples.py` compiles all 64 examples with
  `-fsanitize=address` on every run: a use-after-free, double free or overflow
  fails it outright, and a leak in an example not listed in `KNOWN_LEAKS` fails
  it too. 54 are clean, 5 are refused by the checker, and 5 still leak and are
  named there: `funclib` and `jsonlibtest` (values built inside a `for` body and
  never released), `newcomer` and `span` (a container handed out of a callee
  whose result nobody drops), and `syslib`, which ends with `exit(0)` and leaves
  before the script's exit drops. That list only shrinks. The append fix of 2026-10-30 (`s += f()` released the temp the
  callee handed back only on the error path) took `csv` from 181 leaked
  allocations to 176 and is pinned by `lang/tests/test_append_leak.py`, which
  compiles three append shapes against both emitters under LSan.
- Packages: local/HTTP(S) indexes, checksummed locks, publish and transitive
  resolution exist; there is no hosted registry.

- Five memory and semantics bugs a review found were fixed on 2026-10-31, in both
  emitters (`engine/codegen_c.py` and `cxx/src/emit.cpp`) at once, because a fix
  in one is a divergence. A `move` argument leaked the moved value on every call:
  the callee stored what it was handed and nothing released the argument slot the
  caller had filled -- 188,890 bytes in 3,000 allocations on the review's repro,
  now none. A `bool` returned by a numeric fast function printed `1` where the
  interpreter printed `True`, because the wrapper boxed the raw `long` without
  reading the annotation; it boxes through `fast_val()`. A `vec[i64]` parameter
  whose elements were not all `i64` raised `vec elem not int` out of the raw body
  where the interpreter summed `[1, "a"]` to `1a`; `vvec_all_int` now sends the
  whole call to the general path. Reading a struct field out of a vector element
  (`v[0].f`) leaked 360 bytes in 4 allocations per four reads, because the element
  handed back a temporary nobody owned; `Field`/`SetField` take an owned receiver
  temp. And a moved value printed as garbage: `V_MOVED` is spelled `moved` by
  `vprint_f_`, `vtypename`, `vsprint_q_` and `vsprint_len_q_`. Pinned by
  `lang/tests/test_review_regressions.py` (9 programs, each through both
  emitters, interpreter output against native, emitted C built with
  `-std=c99 -Wall -Wextra` and required to produce no diagnostics, then run under
  LeakSanitizer).
- The emitted C was not warning-free (2026-10-31). Its own `-Wall -Wextra` output
  ran 82-86 diagnostics per program: unused locals in fast functions, a `cap` and
  an `a` unused in the generated `vcall_any`, and `_brat_print_lock` unused in
  builds that never print. It is 0 over all 59 example programs now, and the
  warning check inside `test_review_regressions.py` keeps it there -- a claim in
  the README that nothing tested is how it drifted in the first place.
- A value whose runtime type is not its annotation leaks where it is returned
  (2026-10-31, open). `fn total(v) -> i64 { let mut s = 0; for x in v { s += x }
  return s }` called as `total([1, "a"])` answers `1a` in both backends -- correct
  -- and leaks 35 bytes per call: the callee transfers its string to the caller,
  and the caller does not release the result because the annotation says `i64`.
  The same program at the top level is clean. The class is the one the roadmap
  names: the ownership decision is made from the annotation, and the language is
  dynamic enough to defeat it. It is written down rather than fixed, because every
  candidate fix (release every call result at the call site) frees a value a
  callee returned as an alias of a global. A 2026-10-31 attempt to mark a rebinding
  local `owned` so the frame drop would release it changed nothing -- the value is
  returned, so it is already in the returned set -- and was reverted rather than
  shipped unproven.

- Comparison went through `double`, so integers past 2^53 were ordered and
  equated by the value they round to (2026-10-31). `a = 2^60`, `b = 2^60 + 1`:
  `a < b` answered `False` in both backends, because both converted the pair to
  `double` first -- `vcmp_body`/`veq_` in `crt.h` and `_ncmp`/`_eq_deep` in
  `runtime.py`, which is why the two "agreeing" backends agreed on a wrong
  answer. `a == b` answered `True` in C and `False` in the interpreter, an open
  disagreement, and `sort` ordered `[2^60+1, 2, 2^60]` as `[2, 2^60+1, 2^60]`.
  An integer is compared as an integer now, and against a float without rounding
  it (`vcmp_num` plus `vcmp_int_double`, which is exact for every `i64` and every
  finite `f64`; Python's own operators already do this, so `_ncmp` and `_eq_deep`
  use them). `veq` keeps IEEE for `nan`: it equals nothing, itself included.
  Pinned by `test_f64_semantics.py` (`int_compare_past_double`,
  `int_against_float_exact`), interpreter against native on the same bytes.
  Found while measuring the dynamic path for speed, which is the honest order of
  events: the conversion was there as a shortcut, and it cost correctness before
  it cost time.

- Three of the eight leaking examples were an ownership hole, not a leak
  (2026-10-31): `csv`, `path` and `rematch` are clean now, and three more fell
  from a bigger count (`jsonlibtest` 204 allocations to 58, `newcomer` 9 to 5).
  The shapes, in the order they were found:
  * `return x` for a binding was not counted as an owned return, so a function
    that answers with its accumulator never appeared in `heap_returns()` and the
    caller never dropped the result: `for seg in p.split("/")` plus
    `out = out + seg` leaked one string per call. `csv` alone was 176
    allocations.
  * The body inference that decides `heap_returns()` walked a loop or an `if`
    through a fresh `saw`, so a branch with no `return` in it made the whole
    function answer "hands back nothing owned" -- which is the common shape.
    The bodies share one walk now: the question is about the `return`s a
    statement contains, not about the statement.
  * `shared` (the set of names a `return` hands to the caller) was per frame, so
    a name one `return` transferred was skipped by the exit drops on every other
    path too. `path_norm` built `out` and answered `"."` on an early branch; the
    string it had built had no owner left. Every `return` now releases what that
    path owns except the value it hands over, and the drops set the binding to
    `vnil()`, so the exit point releasing it again is harmless.
  * A script-level binding returned from a function is counted on the way out
    (`vrc_inc`): the binding outlives the frame, so the caller needs a reference
    of its own before it may release the result.
  * The regex engine kept its compiled program and one buffer per `[...]` class
    for the life of the process -- `rematch`, 2 allocations per match. Both are
    released on every path out of `vre_match` now.
  * `sort`/`sort_by` sort in place and hand the receiver back, so treating them
    as "builds a value of its own" made a caller drop a vector that was still
    live: `assert(s.sort() == [1, 2, 5, 8])` was a use-after-free under ASan
    (`meth2.brat`), caught by the sanitizer run before it shipped. They are
    `SELF_RESULT` now: owned exactly when the receiver was, which is what fixes
    `for k in m.keys().sort()` without breaking `s.sort()`.
  The proof is `test_asan_examples.py`, which also runs `-fsanitize=undefined`
  as of today and builds at `-O1` -- at `-O0` LeakSanitizer scans the stack
  conservatively, keeps stale pointers in reach and hides leaks the shipped
  build has (that is why `syslib` looked clean and is not).

## Known divergences, named rather than hidden

| Divergence | Why it is still open | What would close it |
|---|---|---|
| ~~`brat check` runs the Python checker, `brat run` decides with the C++ one~~ **closed 2026-10-28**: the plain case dispatches to `brat-cxx check`, `--py` and `BRAT_INTERP=1` name the reference | — | — |
| `check --effects/--proto/--caps/--ownership` exist only in Python | they print a table, not a verdict, and the native checker has no such view. A port was attempted on 2026-10-29 and withdrawn before release: `--caps` agreed on 55 of 64 examples, `--ownership` on 4 of 64, and the disagreements were semantic (row order, read counts), not padding. A flag that is right four times in six on the surface it documents is worse than no flag | the port needs the ownership counters (`reads`, per-name `last_read`) carried in `caps.cpp` instead of recomputed, then `test_cxx_checker.py` extended to diff both tables over the whole corpus before the flag is exposed |
| ~~`return 3` from `-> f64` prints `3` in the interpreter and `3.0` natively~~ **closed 2026-10-30**: an `f64` function with any integer `return` stays off the `double` fast path in both emitters, so the annotation does not coerce; `test_f64_semantics.py` pins the constant and the mixed case | — | — |
| ~~Parser depth error has no line number in the Python front end~~ **closed 2026-10-29**: `_Depth` carries the cursor and appends `at line N`, the way every other parse error does; the C++ front end stopped repeating the line inside the message, so both print `file:line: error: ...` and `--json` has a line. `test_parse_diag.py` has the `too_deep_expr` and `too_deep_block` cases, compared byte for byte | — | — |
| `run --interp` executes a program the type pass rejects, the native build refuses it | by design: `test_ffi_safety.py` (`build_refuses_type_error`), `test_sock_linear.py` and `test_forge.py` run programs the checker refuses *in order to* reach the runtime guard underneath it -- FFI argument types, use after `tcp_close`, a forged `$type`. Gating the interpreter on the type pass turned those three files red | a `run --interp --no-check` spelling for the runtime-guard probes, so the default path can refuse cleanly instead of dying on `RuntimeError: no method len at line N` |

## Closed on 2026-10-30: three holes the differential tests could not see

A differential oracle compares two implementations. It therefore cannot see a
shape where **both** are wrong the same way (a crash, a hang), or where the two
agree on a *wrong* answer because neither is defined (a snapshot that was not a
snapshot). Each fix below is pinned by a test
that fails on the old tree.

- A flat chain crashed both front ends (2026-10-30). `1+1+...+1` is one nesting
  level and ten thousand nodes: `MAX_NEST` measured the text, not the tree, so
  the parser was happy, every later pass recursed once per term, and
  `check` on 9000 terms died of `SIGSEGV` -- in Python *and* in `brat-cxx`, at
  the same threshold, which is exactly the blind spot of `test_cxx_frontend.py`.
  `MAX_TREE_DEPTH` (1000) in `parser.py` and `kMaxTreeDepth` in `parser.cpp` now
  refuse the tree right after parsing, with one message both front ends print
  byte for byte; `_mark` and `check_nul`, the two walks that ran *during*
  parsing, are iterative. Pinned by `test_tree_depth.py` (9000/20000-term chains
  and a 9000-term string chain refused, a 900-chain accepted, built and run on
  both backends). The same file pins that a diagnostic on a 9000-character line
  shows 200 characters of it instead of all of it.
- A channel pipe that deadlocks was accepted (2026-10-30). The rule was
  `sent <= cap + recv`, which credits a receive that in source order has not
  happened yet, so `chan(1)` + two sends + one receive passed the checker and
  then hung forever, in both backends. The rule is now a prefix invariant --
  `sends_so_far - receives_so_far <= cap` at every send -- and it says nothing
  when a `spawn` is on the channel, because a spawn orders nothing. Fixed in
  `session.py` and `session.cpp`; three cases in `test_session.py`
  (`queue_full_at_the_second_send`, `three_into_two`, `drain_keeps_the_prefix_short`).
- `spawn` gave the child a *window* onto the parent, not a copy (2026-10-30).
  `docs/guide/ownership.md` says `spawn` copies the frame; the emitter only
  snapshotted the names the parent had **already assigned** at the launch, so a
  name the parent assigned *later* was read by the child straight out of the
  script-level global, with no synchronisation at all. `spawn { print(x) }` then
  `x = 2` was accepted by both checkers and the backends disagreed
  deterministically: the interpreter printed the old value, the native binary
  the new one, 20 runs out of 20. ThreadSanitizer on the emitted C reports the
  read of the 4-byte tag against the 8-byte write on a 24-byte `Val`, which is
  the shape that turns into a corrupted tag in production rather than a wrong
  number. Fixed in both emitters: the child snapshots every name its body
  mentions (`_refs` in `codegen_c.py`, `refs_in` in `emit.cpp`), and a captured
  name now resolves through the capture map instead of the global table in
  `define_fn`. Pinned by `test_capability.py`
  (`read_in_child_write_in_parent` accepted and green on both backends,
  `read_in_child_write_in_parent_joined` unchanged) and by the two programs that
  had been written around the old behaviour, `test_runtime_mem.py`
  (`spawn_capture_alias`) and `lang/tests/fixtures/scope_unwind.brat`. What is
  still open: a container captured by value is still the same container, so a
  child that reads a `vec` while the parent pushes to it is a real race and is
  what the `spawn` sharing rule in `capability.py`/`capability.cpp` refuses.
