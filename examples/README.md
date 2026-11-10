# Quickstart Examples

Run any file from the repository root after `make -j4`:

| File | Description | Run command |
|---|---|---|
| `json_arena.brat` | Parses JSON inside `arena { ... }`, escapes only the summary string with `own(...)`, and catches malformed JSON | `./bin/brat run examples/json_arena.brat` |
| `sqlite_query.brat` | Opens an in-memory SQLite database via `std.sqlite` with `defer` cleanup and runs parameterized queries | `./bin/brat run examples/sqlite_query.brat` |
| `parfor_sum.brat` | Computes a deterministic parallel fold with `par for ... reduce(...)` | `./bin/brat run examples/parfor_sum.brat` |
| `ffi_hypot.brat` + `ffi_hypot.h` | Calls C `hypot(double, double)` from `<math.h>` verified with `--header` | `./bin/brat build examples/ffi_hypot.brat --header examples/ffi_hypot.h -o /tmp/ffi_hypot && /tmp/ffi_hypot` |

64 smaller feature programs live under `lang/examples/` (notably
`lang/examples/newcomer.brat` and `lang/examples/logstat.brat`), and larger
worked programs are in [docs/guide/by-example.md](../docs/guide/by-example.md).
