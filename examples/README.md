# Examples

Runnable single-file programs covering core language workflows. Build the
compiler first with `make -j`.

| File | Demonstrates | Command |
|---|---|---|
| `hello.brat` | Minimal entry point and `print` | `./bin/brat run examples/hello.brat` |
| `json_arena.brat` | `std.json`, scoped `arena` scratch allocation, and `?` | `./bin/brat run examples/json_arena.brat` |
| `sqlite_kv.brat` | `std.sqlite`, parameterized queries, `defer`, and `Result` (`?`) | `./bin/brat run examples/sqlite_kv.brat` |
| `parallel_sum.brat` | Deterministic parallel map-reduce via `par for ... reduce` | `./bin/brat run examples/parallel_sum.brat` |
| `c_ffi_math.brat` | Header-verified `extern fn` calls (`hypot`, `fma`) | `./bin/brat run examples/c_ffi_math.brat --header examples/c_ffi_math.h` |
| `sqlite_ffi.brat` | Direct `libsqlite3` C FFI (`extern "libsqlite3.so.0"`, `&stmt` out-pointers, callbacks) | `./bin/brat run --unsafe-ffi examples/sqlite_ffi.brat` |

For ten annotated, self-contained walkthroughs (records/enums, `Result` + `defer`,
JSON + `arena`, maps, index-backed LRU lists, Pike-VM regexes, SQLite, HMAC,
CLI flags, and channels), see [BRAT by Example](../docs/guide/by-example.md).

To compile any example into a standalone native binary:

```sh
./bin/brat build -O2 examples/parallel_sum.brat -o parallel_sum
./parallel_sum
```

All files in this directory are verified in CI by
`lang/tests/test_examples_and_valgrind.py`.
