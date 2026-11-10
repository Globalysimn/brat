# Security Policy

BRAT is experimental single-author software. `brat-cxx check` catches local
ownership, type, and concurrency mistakes, not hostile code execution. Do not
run untrusted `.brat` sources, packages, compiler inputs, or dynamic plugins
with host privileges or live credentials; isolate untrusted workloads in an OS
container or sandbox (`rlimit`, cgroups, mount namespaces, seccomp).

## Trust boundaries

1. Foreign code and raw pointers. `extern fn`, `extern struct`, `lib_open`
   plugins, `ptr` / `ptr[T]` operations, and declared FFI effects (`pure`,
   `nothrow`, `reads`, `writes`, `borrows`) are trusted contracts. Header and
   DWARF verification (`--header`, `--object`, `.bratbind`) checks ABI widths,
   offsets, and exported symbols, not what the C implementation does with a
   pointer at run time. `--unsafe-ffi` skips header verification.
2. String pointer pinning (`ptr_of_str`). Long heap strings (`>= 15` bytes)
   return a pointer tied to the owning `VStr` buffer, and the checker rejects
   returning `ptr_of_str(local)` from a function. Inline short strings (`<= 14`
   bytes) have no heap buffer of their own, so `ptr_of_str` interns them into a
   non-moving slab pool (`64 KiB` chunks) capped by `BRAT_PIN_BUDGET_BYTES`
   (default `67108864`, 64 MiB; `0` disables the cap). Strings returned to C
   from `export fn` or callbacks and strings written via `ptr_write_str` /
   `ptr_write_struct` share the same deduplicated `BRAT_PIN_BUDGET_BYTES` pool.
   Reaching the cap raises a catchable runtime error (`string pin limit
   exceeded`) without invalidating pointers issued earlier. Foreign code still
   must not keep borrowed input string pointers past the end of the call.
3. Filesystem and SQLite confinement. Unless `BRAT_FILES=1` (`true`, `yes`,
   `on`) is set, built-in file operations (`read_file`, `write_file`,
   `fs_rename`, `fs_remove`) and `sql_open` confine relative paths to the
   program's module root using `openat`/`O_NOFOLLOW` leaf checks and reject NUL
   bytes (see [docs/guide/cli.md](docs/guide/cli.md)). `sql_open` also rejects
   `file:` URIs, checks `-wal`, `-shm`, and `-journal` sidecars against symlink
   swaps, verifies `(st_dev, st_ino)` identity across
   `sqlite3_open_v2(SQLITE_OPEN_NOFOLLOW)`, enables `SQLITE_DBCONFIG_DEFENSIVE`,
   disables `TRUSTED_SCHEMA` and extension loading, and installs an authorizer
   blocking `ATTACH DATABASE` outside the confined root. These checks prevent
   accidental path traversal in local tools and do not replace OS-level mount
   isolation.
4. Network, environment, subprocesses, and tool servers. `tcp_connect` and
   `tcp_listen` require canonical dotted-quad IPv4 literals, and `std.http`'s
   `get_follow` blocks redirects to loopback, private, link-local, ULA, and
   IPv4-embedded IPv6 addresses. Default `env_get` filtering uses a denylist of
   common secret variable names; set `BRAT_ENV_DENY_ALL=1` or `BRAT_ENV_ALLOW`
   explicitly for sensitive environments. Subprocess execution (`proc_run`) is
   disabled unless `BRAT_EXEC=1` or `BRAT_EXEC_ALLOW` is set. `brat-cxx lsp`
   (16 MiB message cap) and `brat-cxx mcp` (2 MiB tool output cap) bound
   protocol payloads, and `BRAT_CC_TIMEOUT_MS` (default 120,000 ms) bounds child
   C compiler calls.

See [docs/project/LEDGER.md](docs/project/LEDGER.md) for current checker and
platform limits.

## Reporting defects

Open an issue at `https://github.com/denionis23/brat/issues` with the commit
hash, OS, C/C++ compiler versions, command line, and a minimal reproducer. Avoid
posting exploit payloads against third-party systems.
