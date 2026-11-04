# Modules and packages

## Imports

A function is private to its file unless it is declared `pub fn`.

```brat
// shapes/a.brat
pub fn name() -> str { return "A" }
```

```brat
import shapes.a
fn main() -> i64 {
  print(a.name())
  print(name())      // bare call: the name is exported once, so it resolves
  return 0
}
```

`import shapes.a` accepts `shapes.a.name()` and `a.name()`. Qualification is
required when two loaded modules export the same name (`ambiguous unqualified
name`); a call to a private function from another file is always an error.

The loader resolves each import relative to the importing file, then from
`vendor/` (including an ancestor `<project_root>/vendor/` when anchored by
`brat.lock`), and for `std.<name>` from the toolchain standard library
(`lang/std/`, overrideable via `$BRAT_STDLIB`). It refuses any path outside
those roots. Every reachable
file is parsed once, imports are flattened into a single AST, and imported
functions are mangled `module__name`. The interpreter, both checkers and the
native emitter consume that same flattened program, which is why they agree.
Callback function names supplied as string literals to `extern fn` are resolved
and mangled during flattening as well, so callbacks declared in imported
modules work in both backends.

## Standard library (`std.*`)

`lang/std/` ships twelve modules with `.bri` interfaces, available in every
program without copying files or running `brat pkg`:

- `std.path` — `is_abs`, `join`, `dir`, `base`, `ext`, `stem`, `norm`
- `std.cli` — `has_flag`, `opt`, `opt_int`, `positional` (respects `--`)
- `std.fs` — `exists`, `read_or`, `read_lines`, `write_lines`, `append_text`
- `std.csv` — RFC 4180 `parse(s)`, `parse_records(s)`, and `dump(rows)`
- `std.json` — RFC 8259 `parse(s)` and `stringify(v)`
- `std.http` — HTTP/1.1 + HTTPS `parse_url`, `request`, `get`, `post`, `post_json`, `Session` keep-alive pooling, `get_stream`, and loopback `serve_once`/`serve_n`
- `std.env` — `get`, `get_or`, `has`, `flag`
- `std.proc` — `run`, `status`, `output` (capability-gated via `BRAT_EXEC=1` or `BRAT_EXEC_ALLOW`, `execvp` without a shell)
- `std.bytes` — confined `read`, `write`, `eq`, `starts_with`, `concat`, `to_hex`, `from_hex`, `to_base64`, `from_base64`
- `std.time` — `now_ms`, `monotonic_ns`, `sleep_ms`, UTC ISO-8601 `format_utc` and `parse_utc`
- `std.crypto` — constant-time `ct_eq`, `sha256_hex`, `hmac_sha256_hex`, `hkdf_sha256_hex`, CSPRNG `random_hex`
- `std.sqlite` — `open`, `close`, `exec`, parameterized `query` (`?1..` bindings) and `with_tx` transactions

## Interfaces

A `.bri` file next to the module lists its surface, one signature per line:

```
// circle.bri
fn area(r: i64) -> i64
```

When the interface exists it decides what is exported: a listed function is
visible even without `pub`, and a `pub` function that is not listed stays
private. A listed function that is missing is an error (`declared in the
interface but not defined`), and so is a parameter count that disagrees.

## Re-exports

`pub use a.helper` puts `helper` on the surface of the module that writes it:

```brat
// api/b.brat
import a
pub use a.helper
```

Re-exporting is an alias, not a definition: `helper` keeps one mangled name. The
target must be `pub` in its own module and must exist. A bare `use` without `pub`
is a syntax error. Re-exports are followed in dependency order (`facade -> mid ->
base`); the same name re-exported from two different functions is an error, while
a diamond that ends at one definition is not.

## Tools

```sh
brat deps graph.brat -o graph.json    # module -> import edges (JSON)
brat deps graph.brat --dot            # Graphviz dependency graph
brat api  lib.brat                    # exported signature of every module
brat api --diff old.bri new.bri       # recommended semantic-version bump
```

The diff treats added functions as a `minor` bump, removed functions or changed
signatures as `major`, and identical interfaces as `none`. It compares arity,
parameter types, and return types; parameter-name-only changes do not alter the
`.bri` interface contract.

## Packages

```sh
brat pkg add https://example.com/mathx.tar.gz    # multi-file .tar.gz/.tgz (+ brat.pkg.json deps) -> vendor/<pkg>/ + brat.lock
brat pkg add github.com/acme/http-kit@v0.3.1     # GitHub ref tarball (+ brat.pkg.json deps) -> vendor/http_kit/ + brat.lock
brat pkg add acme/mathx@1.2                      # exact version
brat pkg add acme/mathx@">=1.0,<2.0"             # highest version in range
brat pkg publish acme/mathx@1.2 mathx.brat       # local index entry + sha256
brat pkg publish acme/app@1.0 app.brat --depends "acme/mathx@>=1.0,<2.0"
brat pkg update acme/mathx --dry-run           # preview package + dependency changes
brat pkg update acme/mathx                     # update it and its dependency closure
brat pkg update --dry-run                      # preview all package updates
brat pkg update                                # update all locked index packages
brat pkg install                                 # verifies, then installs the lock
brat pkg verify                                  # check local vendor files offline
brat pkg list
```

`acme/mathx@1.2` resolves through `$BRAT_INDEX`, or `./brat-index.json`:

```json
{"packages": {"acme/app": {"1.0": {
  "url": "...", "sha256": "...", "dependencies": {"acme/mathx": "1.2"}
}}}}
```

Without a version it takes the highest one. A constraint can use numeric
versions with `=`, `>`, `>=`, `<` and `<=`, joined with commas for AND; for
example, `>=1.0,<2.0` selects the highest matching version. Versions compare
numerically by dotted component, so `1.10` is newer than `1.2`. The syntax is
not SemVer: prerelease identifiers, `^`, `~` and OR constraints are unsupported.
An exact version that is missing lists the versions the index has
(`acme/mathx has no version 9.9 (index has 1.0, 1.2)`). A hash mismatch stops
before writing (`HASH MISMATCH acme/mathx@1.2`).
Each indexed version needs a lowercase 64-character SHA-256; missing or malformed
hashes are rejected before that package is fetched.
`brat.lock` records name, version, file, url and sha256, so `brat pkg list`
prints `acme/mathx@1.2  <sha12>  <url>`.

`publish` validates BRAT syntax and writes a content-addressed source file next
to the local index. It refuses a remote index; it does not upload to a server.
Dependencies can use exact versions or numeric version constraints. `pkg add`
resolves the full dependency closure with one selected version per package name,
backtracks across candidates to satisfy all transitive constraints, and
preserves versions already present in the lock when compatible. It rejects
unsatisfiable constraints, cycles and case-insensitive vendor filename
collisions, verifies every hash before changing `vendor/` or `brat.lock`, then
installs and locks the complete closure with exact versions. `pkg install`
validates the lock and all package hashes before staging vendor changes;
filesystem errors roll back replaced files. `pkg verify` checks local vendor
files against the lock without network access. Package files are limited to
16 MiB and lock entries require safe `.brat` filenames, valid SHA-256 hashes
and supported `https`, `http` or local `file` URLs. Archive packages with
`brat.pkg.json` may also declare `c_sources` and `c_headers`: `brat pkg` compiles
the C sources into `vendor/<pkg>/lib<pkg>.so` (plus `lib<pkg>.a` for multi-file C
packages), verifies the `.brat` extern declarations against the compiled DWARF
type/symbol tables, and records a signed `.bratbind` manifest so downstream
`brat build` / `brat run` require `--unsafe-ffi` nowhere.

`pkg update owner/name` re-resolves that package and its currently locked
dependency closure; unrelated locked package versions stay pinned. `pkg update`
re-resolves all indexed packages in the lock. Both commands validate all
download hashes before changing files and update `vendor/` and `brat.lock`
transactionally. Add `--dry-run` to resolve, validate and list the planned
version/hash changes without writing either. The lockfile stores resolved
versions, not the original version constraints.
