# Known Engineering Limits Ledger

Current compiler, runtime, and platform limits for `brat-cxx` (175 test suites
in `lang/tests/`):

| Area | Current behavior and limit |
|---|---|
| Ownership scope | `borrow.cpp` is intra-procedural. Cross-function lifetimes rely on reference counting rather than static region variables; raw `ptr` writes (`ptr_write_*`) inside `unsafe` or C code are not tracked across arbitrary pointer graphs. |
| Reference cycles | No cycle collector. Intra-function container self-cycles are rejected by `brat-cxx check`; cycles constructed across separate helper functions leak unless allocated inside `arena { ... }`. |
| Generics & numbers | Generic functions and structs erase type arguments to 16-byte `Val` slots (`vec[i64]` and `vec[f64]` specialize). All BRAT integers are signed `i64` with two's-complement wraparound; unsigned widths exist only at C FFI boundaries. |
| Concurrency | Fixed worker pool (at most 32 OS threads, an unbounded run queue) with cooperative `join_all()` barriers, bounded `chan(cap)` queues (`cap <= 1,048,576`), and pure `par for` folds. No green-thread scheduler or `epoll`/`kqueue` async event loop. |
| String FFI pins | Short strings (`<= 14` bytes) passed via `ptr_of_str`, returned across `export fn`/callbacks, or stored via `ptr_write_str` intern into a deduplicated 64 MiB slab (`BRAT_PIN_BUDGET_BYTES`); exceeding the cap raises a catchable error. |
| Platform matrix | Tested in CI on Linux x86-64 (GCC/Clang + ASan/UBSan/LSan), macOS arm64, and Windows x86-64 (MinGW-w64 LLP64). Big-endian targets and 32-bit Windows are unsupported. |
| Host isolation | Default path confinement (`O_NOFOLLOW` under module root), secret env denylist, and disabled `proc_run` prevent accidental mistakes in local scripts; they are not a sandbox against untrusted `.brat` code. |
