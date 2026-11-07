# Standard Library Reference (`std.*`)

BRAT ships twelve standard-library modules under `lang/std/`. Each `.brat`
implementation is paired with a `.bri` interface contract; `test_docs_examples.py`
verifies that every signature listed below matches `lang/std/*.bri` verbatim and
compiles every `brat run` example on this page.

Import a module with `import std.<name>` and call functions qualified
(`json.parse(s)` or `std.json.parse(s)`). Unused runtime subsystems (HTTP, TLS,
SQLite, subprocesses) are stripped at compile time via `BRAT_CRT_MASK` when a
program does not reference them.

---

## `std.json` — RFC 8259 JSON parser and serializer

Backed directly by the C scanner/serializer in `crt/sql_proc.h`. JSON objects
become `map` values, arrays become `vec`, `null` becomes `None`, and numbers
parse as `i64` (when exact integer) or `f64`. `get_path` returns `Some(val)` or
`None`. `stringify` and `stringify_indent` return `str` directly and raise on
`NaN`/`Inf`.

```brat
fn parse(s: str)
fn stringify(v) -> str
fn stringify_indent(v, indent: i64) -> str
fn get_path(v, dotted: str)
fn get_str(m, key: str, fallback: str) -> str
fn get_int(m, key: str, fallback: i64) -> i64
fn get_bool(m, key: str, fallback: bool) -> bool
```

```brat run
import std.json

fn main() -> i64 {
  let doc = unwrap(json.parse("{\"app\":{\"port\":8080,\"tls\":true}}"))
  let port = unwrap(json.get_path(doc, "app.port"))
  print(port, json.stringify({"ok": true, "port": port}))
  return 0
}
```

```output
8080 {"ok":true,"port":8080}
```

---

## `std.bytes` — Byte vectors, hex, and Base64

Binary payloads are represented as `vec[i64]` with elements in `0..255`.
`to_hex`, `from_hex`, `to_base64`, `from_base64`, and `to_str` return
`Result[..., str]` (validating byte range `0..255`, padding, and UTF-8). File
operations (`read`, `write`) obey `BRAT_FILES` path confinement.

```brat
fn read(path: str)
fn write(path: str, data: vec[i64])
fn from_str(s: str) -> vec[i64]
fn to_str(data: vec[i64])
fn eq(a: vec[i64], b: vec[i64]) -> bool
fn starts_with(data: vec[i64], prefix: vec[i64]) -> bool
fn concat(a: vec[i64], b: vec[i64]) -> vec[i64]
fn slice(data: vec[i64], start: i64, stop: i64) -> vec[i64]
fn index_of(data: vec[i64], sub: vec[i64]) -> i64
fn to_hex(data: vec[i64])
fn from_hex(hex: str)
fn to_base64(data: vec[i64])
fn from_base64(text: str)
```

```brat run
import std.bytes

fn main() -> i64 {
  let raw = bytes.from_str("BRAT")
  let hex = unwrap(bytes.to_hex(raw))
  let b64 = unwrap(bytes.to_base64(raw))
  let back = unwrap(bytes.to_str(unwrap(bytes.from_base64(b64))))
  print(hex, b64, back)
  return 0
}
```

```output
42524154 QlJBVA== BRAT
```

---

## `std.cli` — Command-line flags and subcommands

Stateless helpers over `args()` (or any `vec[str]`). Both `--flag value` and
`--flag=value` are recognized; `--` stops option scanning. `opt_int` returns
`Ok(fallback)` when absent and `Err(...)` when malformed.

```brat
fn has_flag(argv: vec[str], flag: str) -> bool
fn opt(argv: vec[str], flag: str, fallback: str) -> str
fn opt_int(argv: vec[str], flag: str, fallback: i64)
fn positional(argv: vec[str], value_flags: vec[str]) -> vec[str]
fn subcommand(argv: vec[str], value_flags: vec[str]) -> str
fn subcommand_args(argv: vec[str], value_flags: vec[str]) -> vec[str]
```

```brat run
import std.cli

fn main() -> i64 {
  let argv = ["-n", "3", "--dry-run", "sync", "a.txt", "b.txt"]
  let vflags = ["-n"]
  print(cli.subcommand(argv, vflags), unwrap(cli.opt_int(argv, "-n", 1)), cli.has_flag(argv, "--dry-run"))
  print(cli.positional(argv, vflags).join(","))
  return 0
}
```

```output
sync 3 true
sync,a.txt,b.txt
```

---

## `std.crypto` — SHA-256, HMAC-SHA256, and CSPRNG

Hash and random functions return `Result[..., str]`. `sha256` / `hmac_sha256`
accept either `str` or `vec[i64]` (`0..255`). `constant_time_eq` compares strings
without early-exit timing leaks.

```brat
fn sha256(data)
fn sha256_hex(data)
fn hmac_sha256(key, msg)
fn hmac_sha256_hex(key, msg)
fn constant_time_eq(a: str, b: str) -> bool
fn rand_bytes(n: i64)
fn rand_hex(n_bytes: i64)
```

```brat run
import std.crypto

fn main() -> i64 {
  let digest = unwrap(crypto.sha256_hex(""))
  let rnd = unwrap(crypto.rand_hex(8))
  print(digest)
  print(len(rnd) == 16, crypto.constant_time_eq("same", "same"))
  return 0
}
```

```output
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
true true
```

---

## `std.csv` — RFC 4180 CSV parser and serializer

Handles quoted fields, embedded commas/newlines, and doubled `""` escapes.
`parse` returns `Result[vec[vec[str]], str]`; `dump` serializes rows with `\n`
line endings.

```brat
fn parse(text: str)
fn dump(rows: vec[vec[str]]) -> str
```

```brat run
import std.csv

fn main() -> i64 {
  let rows = unwrap(csv.parse("host,note\ndb-1,\"primary, eu\"\ndb-2,\"say \"\"hi\"\"\"\n"))
  print(rows[1][0], "->", rows[1][1])
  print(rows[2][0], "->", rows[2][1])
  return 0
}
```

```output
db-1 -> primary, eu
db-2 -> say "hi"
```

---

## `std.env` — Environment variables

Reads process environment variables subject to the runtime credential denylist
(`*_TOKEN`, `*_SECRET`, `*_KEY`, `*_PASSWORD`, cloud credentials) and
`BRAT_ENV_ALLOW` / `BRAT_ENV_DENY_ALL`. `flag` treats `"1"`, `"true"`, `"yes"`,
and `"on"` (case-insensitive) as `true`.

```brat
fn get(name: str)
fn get_bytes(name: str)
fn get_or(name: str, fallback: str) -> str
fn has(name: str) -> bool
fn flag(name: str) -> bool
```

---

## `std.fs` — Confined filesystem helpers

Wraps the runtime's `O_NOFOLLOW` confined file primitives. When `BRAT_FILES` is
unset, relative paths under the current working directory are allowed; set
`BRAT_FILES=/dir1:/dir2` to restrict access to explicit roots or `BRAT_FILES=""`
to deny all file I/O. `write_atomic` writes a sibling temp file and renames it
into place.

```brat
fn exists(path: str) -> bool
fn read_or(path: str, fallback: str) -> str
fn read_lines(path: str)
fn write_lines(path: str, lines: vec[str])
fn append_text(path: str, extra: str)
fn write_atomic(path: str, content: str)
fn remove(path: str)
```

---

## `std.path` — Pure path manipulation

Pure lexical operations (no disk touches), normalizing `.` and `..` segments
consistently across slash styles.

```brat
fn is_abs(p: str) -> bool
fn join(a: str, b: str) -> str
fn dir(p: str) -> str
fn base(p: str) -> str
fn ext(p: str) -> str
fn stem(p: str) -> str
fn with_ext(p: str, new_ext: str) -> str
fn norm(p: str) -> str
fn rel(from_dir: str, to_path: str) -> str
```

```brat run
import std.path

fn main() -> i64 {
  let p = path.join("var/log", "../cache/index.tar.gz")
  let clean = path.norm(p)
  print(clean, "| dir:", path.dir(clean), "| ext:", path.ext(clean))
  print(path.with_ext(clean, ".idx"), "| rel:", path.rel("var", clean))
  return 0
}
```

```output
var/cache/index.tar.gz | dir: var/cache | ext: .gz
var/cache/index.tar.idx | rel: cache/index.tar.gz
```

---

## `std.time` — Clocks, Hinnant calendar math, and ISO-8601 UTC

`now_ms` and `unix_sec` read wall-clock UTC; `now_mono_ns` reads `CLOCK_MONOTONIC`
for benchmarking and timeouts. Calendar conversions use Howard Hinnant's
proleptic Gregorian algorithms without calling `localtime`/`gmtime` (avoiding
global `TZ` mutexes and locale state).

```brat
fn now_ms() -> i64
fn now_mono_ns() -> i64
fn unix_sec()
fn sleep(ms: i64) -> i64
fn is_leap_year(year: i64)
fn days_in_month(year: i64, month: i64)
fn days_from_civil(y: i64, m: i64, d: i64)
fn civil_from_days(z_in: i64)
fn day_of_week(days: i64)
fn format_iso8601_utc(epoch_sec: i64) -> str
fn parse_iso8601_utc(s: str)
fn format_duration_ms(ms: i64) -> str
```

```brat run
import std.time

fn main() -> i64 {
  let days = time.days_from_civil(2026, 11, 7)
  let ts = unwrap(time.parse_iso8601_utc("2026-11-07T00:00:00Z"))
  print(days * 86400 == ts, time.is_leap_year(2024), time.format_duration_ms(1250))
  return 0
}
```

```output
true true 1.25s
```

---

## `std.sqlite` — Embedded SQLite3

Opens `:memory:` or confined file databases (`SQLITE_OPEN_NOFOLLOW`, defensive
mode, disabled `TRUSTED_SCHEMA`, authorizer blocking arbitrary `ATTACH`).
`query` binds positional parameters (`?1`, `?2`, ...) and returns `Ok(vec[map])`
keyed by column name; `query_one` returns `Ok(Some(row))` or `Ok(None)`.

```brat
fn open(path: str)
fn open_memory()
fn close(db: i64)
fn exec(db: i64, sql: str)
fn query(db: i64, sql: str, params)
fn query_one(db: i64, sql: str, params)
fn scalar_int(db: i64, sql: str, params)
```

---

## `std.http` — HTTP/1.1 & HTTPS client and server

Includes pure URL/query/wire parsers plus TCP/OpenSSL client requests,
keep-alive connection pools, redirect following (`get_follow`), and blocking or
keep-alive HTTP/1.1 servers (`listen`, `serve_once`, `serve_keep_alive`).

```brat
fn parse_url(url: str)
fn encode_query_component(s: str) -> str
fn decode_query_component(s: str)
fn encode_query(params) -> str
fn parse_query(qs: str)
fn request_tls_ca(method: str, url: str, headers, body: str, ca_file: str)
fn open_keep_alive(url: str)
fn request_keep_alive(conn: i64, method: str, url: str, headers, body: str)
fn get_keep_alive(conn: i64, url: str)
fn close_keep_alive(conn: i64)
fn request_headers(method: str, url: str, headers, body: str)
fn request_ex(method: str, url: str, headers, body: str)
fn request(method: str, url: str, content_type: str, body: str)
fn get(url: str)
fn get_ca(url: str, ca_file: str)
fn get_follow(url: str, max_redirects: i64)
fn post_json(url: str, json_body: str)
fn status_text(code: i64) -> str
fn parse_request(raw: str)
fn format_response(status: i64, headers, body: str)
fn read_request(conn: i64, max_bytes: i64)
fn respond(conn: i64, status: i64, headers, body: str)
fn respond_text(conn: i64, status: i64, body: str)
fn respond_json(conn: i64, status: i64, json_body: str)
fn listen(addr: str)
fn accept_request(srv_fd: i64, max_bytes: i64)
fn serve_once(srv_fd: i64, handler)
fn serve_keep_alive(srv_fd: i64, max_requests: i64, handler)
```

```brat run
import std.http

fn main() -> i64 {
  let u = unwrap(http.parse_url("https://api.example.com:8443/v1/items?limit=10&tag=a%20b"))
  let parts = u["path"].split_once("?")
  let q = unwrap(http.parse_query(parts[1]))
  print(u["host"], u["port"], parts[0], q["tag"], http.status_text(201))
  return 0
}
```

```output
api.example.com 8443 /v1/items a b Created
```

---

## `std.proc` — Subprocess execution (`execvp`, no shell)

Spawns child processes directly via `fork`/`execvp` (never `/bin/sh -c`), so
argument strings are not subject to shell injection. Disabled by default;
requires `BRAT_EXEC=1` (and honors `BRAT_EXEC_ALLOW=/usr/bin/git:...` and
`BRAT_PROC_TIMEOUT_MS`).

```brat
fn run(cmd: str, argv: vec[str])
fn run_timeout(cmd: str, argv: vec[str], timeout_ms: i64)
fn status(cmd: str, argv: vec[str])
fn output(cmd: str, argv: vec[str])
```
