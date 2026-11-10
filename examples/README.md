# Quickstart Examples

Run any file from the repository root after `make -j4`:

| File | Description | Run command |
|---|---|---|
| `hello.brat` | Minimal entry point printing a single line | `./bin/brat run examples/hello.brat` |
| `json_arena.brat` | Parses JSON inside `arena { ... }` and returns a compact summary string | `./bin/brat run examples/json_arena.brat` |
| `sqlite_kv.brat` | Opens an in-memory SQLite database via `std.sqlite` with `defer` cleanup and runs parameterized queries | `./bin/brat run examples/sqlite_kv.brat` |
| `parallel_sum.brat` | Computes a deterministic parallel fold with `par for ... reduce(...)` | `./bin/brat run examples/parallel_sum.brat` |
| `c_ffi_math.brat` + `c_ffi_math.h` | Calls C `hypot` and `fma` from `<math.h>` verified against `c_ffi_math.h` with `--header` | `./bin/brat run examples/c_ffi_math.brat --header examples/c_ffi_math.h` |
| `sqlite_ffi.brat` | Direct C FFI binding to `libsqlite3` using typed pointers and prepared statements | `./bin/brat run examples/sqlite_ffi.brat` |
| `code_stats.brat` | CLI source-line counter (`--json`, `--ext`) with string/comment lexing, `arena` promotion, and SHA-256 manifest | `./bin/brat run examples/code_stats.brat -- .` |

64 smaller feature programs live under `lang/examples/` (notably
`lang/examples/newcomer.brat` and `lang/examples/logstat.brat`), and larger
worked programs are in [docs/guide/by-example.md](../docs/guide/by-example.md).
