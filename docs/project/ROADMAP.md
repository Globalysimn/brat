# Roadmap

What BRAT does today, and what is left before any stronger safety claim. The
state of each promise, with the test that proves it, is `LEDGER.md`; the language
contract is `../spec/SPEC.md`.

## Where it stands

- **Two backends, one semantics.** A Python interpreter and a C99 emitter whose
  output is compared byte for byte (`test_native.py`, `test_agreement.py`). A
  second front end in C++20 (`lang/cxx/`) produces the same tree, the same
  diagnostics and the same C; it also hosts `brat lsp`.
- **Ownership** is checked inside a function and at call sites whose callee
  declares `move`/`ref`/`mut`. Both runtimes mark simple moved bindings as a
  backstop. Intra-function plus declared modes is the whole of it — see
  `../guide/ownership.md`.
- **Memory** is refcounted strings, tags and containers, released at scope and
  loop exit; `arena { }` frees a block wholesale and `own(x)` copies a value out.
- **Errors** are `Ok`/`Err`/`Some`/`None` with `?`, `or`, `try`/`catch` and
  `defer` on every exit path. Division by zero is an error in every build.
- **Tooling**: `run`, `check` (`--json`, `--effects`, `--proto`, `--caps`,
  `--ownership`), `test`, `fmt`, `emit-c`, `build`, `dist`, `new`, `pkg`, `deps`,
  `api`, `bindgen`, `ffi`, `lsp`, `mcp`, `explain`.
- **C interop**: `extern fn`/`extern struct` in every ABI width, out-parameters,
  callbacks, variadics, `--link`, `brat bindgen`, `brat ffi` probes and runtime
  plugins. A declaration can also say what the callee does -- `pure`,
  `nothrow`, `reads(p)`, `writes(p)` -- and what it may do with a callback it is
  handed -- `thread(any)`, `reenter(no)`. Every promise is consumed by a checker
  or by the emitted C, never merely recorded; see the ladder below.
- **Denial of service**: recursive value walkers (`print`, `str`, comparison)
  stop at 2000 levels with `recursion too deep` in both backends instead of
  overflowing the stack (`test_depth_guard.py`).
- **Suite**: 115 files, one line per file, ~210 s on four cores
  (`python3 lang/tests/run_all.py -j 4`). A file that cannot run exits 77 and is
  reported as `skipped`, never as `ok`.

## The FFI ladder

Four obligations a boundary has to meet before a foreign call is as safe as a
BRAT call: **representation** (which bytes), **effects** (what it does to
memory), **time** (how long a value lives), **concurrency** (which thread it
runs on). Each rung closes one, and the proof of each travels with the artifact
rather than staying in a test file. Struck through means closed, with the date
and the file that proves it.

1. ~~**L2: effects.** `pure`, `nothrow`, `reads(p)`, `writes(p)` on an `extern
   fn`~~ **closed 2026-10-30.** A declared write is a write, so a foreign
   `free`/`memcpy` across a `spawn` is caught by the one-writer rule instead of
   being invisible; `pure`/`nothrow` reach C as `__attribute__` so the C
   compiler may reuse or hoist a call. Proof: `test_ffi_effects.py` (17 cases,
   both front ends, identical refusals). Speed: no win measured yet, because a
   `Val` is 24 bytes and therefore lives in memory, so the C compiler cannot
   keep an operand in a register across the call it would have to reuse. The
   `volatile` half of that sentence was closed on 2026-10-31 (see Speed below);
   the size half is still open.
2. ~~**L3: callback contracts.** `thread(any)`, `reenter(no)` on a callback
   parameter~~ **closed 2026-10-30.** `thread(any)` says the callee may call back
   from a thread the program never made, so the callback's body must not reach
   anything the caller's thread also owns; `reenter(no)` says the callee will
   not enter a callback while one of its own calls is on the stack, so a
   callback that calls its own library again is refused without it. Both are
   written where a reader looks (the callback parameter) and stored where the
   behaviour belongs (the declaration). Defaults are the dangerous pair -- the
   caller's thread, re-entry allowed -- because an unstated promise is a fact
   the checker cannot use. Scope proof: `test_ffi_callback_contracts.py`;
   ownership proof: `test_ffi_callback_gate.py`.
3. **L1: compare the declaration against the object, not the header.** A header
   is a promise; a symbol table and a `.o` are what actually shipped. Check
   presence, width and struct size against the object file, so the last LEDGER
   row about a header that disagrees with its library stops being a caveat.
   Cost: small -- `brat ffi` already owns the data model and the probe.
4. **L4: `.bratbind` proofs travel with the package.** Declarations, the
   `--header` comparison and the data model ride along with the artifact, and
   `brat pkg add` refuses a package whose proof does not hold for the target in
   front of it. This is the rung that pays for itself: today every consumer
   re-proves what was already proved, and in practice passes `--unsafe-ffi`
   instead. It is honest only once rungs 1-3 are closed -- a certificate that
   still contains three unproven obligations is a decoration.
5. **L5: ABI algebra.** `sizeof`/`offsetof` as a differential oracle against a
   C compiler on every supported triple. Not before there is a user on a
   non-LP64 target; the data models exist and are unit-tested, which is the
   right amount of work to have done for now.

**Speed, in the order it is measurable.** ~~(a) The `volatile` temporaries.~~
**closed 2026-10-31.** The keywords were a leftover from a `longjmp` that no
longer exists: a raise sets a thread-local flag and the frame leaves by `goto`,
so no temporary can be crossed by a non-local jump and the qualifier only pinned
every `Val` to the stack. Removing it from both emitters took a 100M-iteration
dynamic loop from 5035 ms to 4013 ms. (b) The `Val` itself: 24 bytes, so it is
past the 16-byte limit for register passing and every operand round-trips
through memory. That is the remaining 9x against C, and it is the item below. (b) `brat-cxx check` is 3.7 ms against 97.3 ms for `check --py` on
`lang/examples/jsonlib.brat`, so the Python front end is the slow one and is
already being demoted to oracle duty.

## Open work

In the order it blocks other things.

1. **A dynamic `call` prototype is a string the compiler never sees.** The static
   path no longer lets an unproven surface through: `build`, `run`, `dist` and
   `emit-c` refuse a program that declares `extern` and names no `--header`, and
   `--unsafe-ffi` is the opt-out that says so on stderr. The proof is recorded in
   the binary and re-checkable (`brat ffi --bin`, `test_ffi_contract.py`,
   `test_ffi_mandate.py`). The next step is the same rule for `lib_sym`/`call`,
   where the prototype is a string in the source; ownership of a pointer
   (`&T`/`own T`/`out T`) and callback retention are the two items after that.
2. **`spawn` shares the object graph.** The rule is "one writer" rather than a
   type, and `unsafe`/FFI pointers are outside it. A `Send`-shaped form or
   freezing an object at the spawn point would make the rule checkable per value.
3. **Leaks under LeakSanitizer.** Five examples still leak (`jsonlibtest`,
   `funclib`, `newcomer`, `span`, and `syslib`, which leaves through `exit(0)`)
   and are named in `test_asan_examples.py`; three more were closed on 2026-10-31
   by fixing the ownership holes behind them rather than the examples.
   That list only shrinks; a new leak in an unlisted example fails the suite.
   The `s += f()` shape -- one leaked allocation per append -- was fixed on
   2026-10-30 in both emitters; `test_append_leak.py` holds it. One shape behind
   the rest is a value a local owns when an early `return` leaves the function;
   releasing it needs an ownership decision per live local on every exit path,
   which is the cross-function item below rather than a one-line fix. A second
   shape, found on 2026-10-31: a function whose runtime answer is not its
   annotation (`-> i64` that returns a string) hands the caller a value it never
   releases, because the call site decides from the annotation; that is the same
   cross-function item, seen from the other end.
4. **Dynamic plugin signatures** are checked against the BRAT argument list, not
   an arbitrary C prototype. A wrong prototype is undefined behaviour and is
   documented as such in `SECURITY.md`.
5. **Cross-function ownership.** A callee that stores a value where the checker
   cannot see it is not tracked. Effect or escape annotations on the signature
   are the likely shape of the fix.
6. **Ecosystem.** `brat pkg` resolves through an index file with sha256 locks and
   vendor directories; there is no registry server, no `publish` to one, and no
   transitive resolution beyond the closure the index describes.
7. ~~**A function cannot use a script-level binding, in either emitter.**~~
   **closed 2026-10-30.** The interpreter ran it, both checkers accepted it, and
   neither emitter produced a working binary. Two defects, one per emitter, both
   in the *fast* path rather than in globals: a function naming a script-level
   `let` was classified as a raw-C fast function, where the name had no symbol
   (`KeyError: 'total'`, or `return ;` that C refuses), and the slow path
   declared a local of the same name, so a write inside a function landed in the
   local while the file-scope value stayed `vnil()`. Both emitters now keep
   globals off the fast path -- letting the fixpoint drop their callers -- and
   leave them out of the local declaration list. A third defect surfaced with the
   first two: a closure could not see the script frame at all (`NameError: no var
   base` in the interpreter, while both emitters printed the answer), so
   `run_closure` now walks the script frame below its own frame, which is what a
   named function already did. Proof: `test_globals_in_fns.py` (7 cases,
   interpreter and both emitters agree).
8. **Platforms.** Linux is the supported target. macOS is unverified here, and
   Windows is deferred: descriptor confinement is not implemented, so dynamic
   loading and sockets are unavailable.

Self-hosting is deferred by trigger: no BRAT-written compiler before the tree
passes 300k lines.
