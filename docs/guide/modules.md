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
`vendor/`, and refuses a path outside the root program directory. Every reachable
file is parsed once, imports are flattened into a single AST, and imported
functions are mangled `module__name`. The interpreter, both checkers and both
emitters consume that same flattened program, which is why all of them agree.
Callback function names supplied as string literals to `extern fn` are resolved
and mangled during flattening as well, so callbacks declared in imported
modules work in both backends.

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
brat deps graph.brat -o graph.json    # module -> import edges
brat api  lib.brat                    # exported signature of every module
```

## Packages

```sh
brat pkg add https://example.com/mathx.tar.gz    # vendor/ + brat.lock with sha256
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
and supported `https`, `http` or local `file` URLs.

`pkg update owner/name` re-resolves that package and its currently locked
dependency closure; unrelated locked package versions stay pinned. `pkg update`
re-resolves all indexed packages in the lock. Both commands validate all
download hashes before changing files and update `vendor/` and `brat.lock`
transactionally. Add `--dry-run` to resolve, validate and list the planned
version/hash changes without writing either. The lockfile stores resolved
versions, not the original version constraints.
