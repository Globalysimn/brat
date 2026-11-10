# Modules, `.bri` Interfaces, and Packages

## 1. Module resolution and visibility

Every `.brat` file is a module. Top-level functions are private to their file
unless marked `pub fn`:

```brat
// math/vec2.brat
pub fn dot(ax: i64, ay: i64, bx: i64, by: i64) -> i64 {
  return ax * bx + ay * by
}
```

```brat
import math.vec2

fn main() -> i64 {
  print(vec2.dot(2, 3, 4, 5))
  print(dot(2, 3, 4, 5))        // unambiguous single export works unqualified too
  return 0
}
```

`import math.vec2` looks in three places, in order:

1. Relative to the importing file (`math/vec2.brat`).
2. `vendor/` next to the entry file or at the nearest parent directory
   containing `brat.lock`.
3. `lang/std/` (or `$BRAT_STDLIB`) when the path starts with `std.`.

Relative traversal outside those roots (`..`) is refused by the loader. Every
reachable module is parsed once, checked against its optional `.bri` interface,
and flattened into the program AST under mangled `module__name` symbols (unused
imported functions are pruned before code emission). If two imported modules
export the same function name, an unqualified call fails with `ambiguous
unqualified name` and asks you to spell `mod_a.fn_name(...)`.

### `.bri` interface files and `pub use`

Drop `circle.bri` next to `circle.brat` to lock down its public surface (`fn`,
`struct`, and `enum` declarations):

```brat
// circle.bri
struct Circle { r: f64 }
fn area(c: Circle) -> f64
```

When a `.bri` file exists, **only** functions and types listed in `.bri` are
visible to importers, even if the `.brat` file has other `pub` items. Missing
declarations or mismatched field/parameter/return types fail module loading
immediately.

`pub use other.helper` re-exports `helper` through the current module without
duplicating the underlying symbol, so diamond imports converging on the same
definition stay unambiguous.

### Inspecting module graphs and API drift

```sh
./bin/brat deps app.brat --json -o graph.json
./bin/brat deps app.brat --dot
./bin/brat api lib.brat
./bin/brat api --diff old.bri new.bri
```

`api --diff` classifies interface changes as `none`, `minor` (added exports
only), or `major` (removed exports or changed parameter/return types).

## 2. Standard library overview (`std.*`)

Twelve modules ship in `lang/std/`, each paired with a `.bri` contract. Full
signatures and runnable examples live in [Standard Library Reference](stdlib.md):

| Module | What it covers |
|---|---|
| `std.json` | C-backed RFC 8259 `parse`, `stringify`, `stringify_indent`, `get_path`, `get_str`, `get_int`, `get_bool` |
| `std.http` | HTTP/1.1 + HTTPS client (`get`, `post_json`, `request_ex`, keep-alive pools, custom CA) and server (`listen`, `serve_once`, `serve_keep_alive`) |
| `std.sqlite` | Confined SQLite3 (`open`, `open_memory`, `close`, `exec`, parameterized `query`, `query_one`, `scalar_int`) |
| `std.crypto` | `sha256`, `sha256_hex`, `hmac_sha256`, `hmac_sha256_hex`, `constant_time_eq`, OS CSPRNG `rand_bytes` / `rand_hex` |
| `std.fs` | `exists`, `read_or`, `read_lines`, `write_lines`, `append_text`, crash-safe `write_atomic`, `remove` |
| `std.path` | Pure path algebra: `is_abs`, `join`, `dir`, `base`, `ext`, `stem`, `with_ext`, `norm`, `rel` |
| `std.bytes` | Byte vectors (`vec[i64]` in `0..255`): `read`, `write`, `from_str`, `to_str`, `slice`, `index_of`, hex and Base64 codecs |
| `std.cli` | Zero-schema `argv` helpers (`has_flag`, `opt`, `opt_int`, `positional`, `subcommand`, `subcommand_args`) |
| `std.csv` | RFC 4180 quoted-field `parse` and `dump` |
| `std.time` | Epoch/monotonic clocks (`now_ms`, `now_mono_ns`, `unix_sec`, `sleep`), Hinnant civil calendar math, ISO-8601 UTC parse/format |
| `std.env` | Denylist-filtered `get`, `get_bytes`, `get_or`, `has`, boolean `flag` |
| `std.proc` | Shell-free `execve` subprocess execution (`run`, `run_timeout`, `status`, `output`; gated by `BRAT_EXEC`) |

## 3. Lockfile package manager (`brat pkg`)

BRAT does not depend on a central package registry. Packages
are `.tar.gz` archives (local files, HTTPS URLs, GitHub `owner/repo@ref`
tarballs, or entries in a static JSON index file) vendored straight into
`./vendor/` and pinned by SHA-256 in `brat.lock`:

```sh
./bin/brat pkg add https://example.com/mathx-1.2.tar.gz
./bin/brat pkg add github.com/acme/http-kit@v0.3.1
./bin/brat pkg add acme/mathx@">=1.0,<2.0"
./bin/brat pkg publish acme/mathx@1.2 mathx.brat
./bin/brat pkg update --dry-run
./bin/brat pkg install
./bin/brat pkg verify
./bin/brat pkg list
```

Operational rules:

- Version constraints compare dotted integers (`=`, `>`, `>=`, `<`, `<=` joined
  by `,`, e.g. `>=1.0,<2.0`) against `$BRAT_INDEX` or `./brat-index.json`.
  Dependency graphs above 64 packages, cycles, and case-insensitive path
  collisions are rejected.
- Archives are capped at 16 MiB, checked against lowercase 64-hex SHA-256
  digests before extraction, and swapped into `vendor/` + `brat.lock` atomically
  with rollback on failure.
- `pkg publish owner/name@ver file.brat` copies the artifact into
  `.brat-packages/<owner>/` beside `$BRAT_INDEX` (default `./brat-index.json`)
  and records its `file://` URI and SHA-256 digest in that index file.
- A package with `brat.pkg.json` can include `c_sources` and `c_headers`.
  During `pkg add` or `install`, `brat` compiles the C sources, checks all
  `.brat` `extern` declarations against the resulting DWARF debug info, and
  writes a `.bratbind` manifest so downstream users do not need `--unsafe-ffi`.
