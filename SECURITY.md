# Security Policy

BRAT is experimental software. Passing `brat-cxx check` does not prove memory
safety, data-race freedom or host isolation. Do not execute untrusted BRAT
source files, packages, compiler inputs or dynamic plugins with host privileges
or live credentials. Isolate untrusted workloads with an OS sandbox, container,
separate UID and kernel resource limits (`rlimit`, cgroups, seccomp).

## Trust boundaries

1. **Foreign code and raw pointers.** `extern fn`, `extern struct`, `lib_open`
   plugins, `ptr` / `ptr[T]` operations and declared FFI effects (`pure`,
   `nothrow`, `reads`, `writes`, `borrows`) are trusted contracts. Header and
   DWARF verification (`--header`, `--object`, `.bratbind`) checks ABI widths,
   offsets and exported symbols, not what the C implementation does with a
   pointer at run time. `--unsafe-ffi` bypasses header verification altogether.
2. **String pointer pinning (`ptr_of_str`).** Long heap strings (`>= 15` bytes)
   hand out a pointer tied to the owning `VStr` allocation, and the checker
   rejects returning `ptr_of_str(local)` from a function. Inline short strings
   (`<= 14` bytes) have no heap buffer of their own, so `ptr_of_str` interns them
   into a non-moving, demand-paged slab pool (`64 KiB` chunks) capped by
   `BRAT_PIN_BUDGET_BYTES` (default `67108864`, 64 MiB; `0` disables the cap,
   allowing unbounded pinning, which is unsafe for untrusted inputs).
   Reaching the budget raises a catchable runtime error (`string pin limit
   exceeded`) without invalidating already-issued pointers. Foreign code still
   must not retain borrowed string pointers after the owner is freed.
3. **Filesystem and SQLite confinement.** Unless `BRAT_FILES=1` (or `true`,
   `yes`, `on`) is set in the environment, built-in file operations (`read_file`,
   `write_file`, `fs_rename`, `fs_remove`) and `sql_open` confine relative paths
   to the program's module root using `openat`/`O_NOFOLLOW` leaf checks and
   reject NUL bytes (see [docs/guide/cli.md](docs/guide/cli.md)). `sql_open` additionally rejects `file:` URIs, checks
   `-wal`, `-shm` and `-journal` sidecars against symlink replacement, verifies
   `(st_dev, st_ino)` identity across `sqlite3_open_v2(SQLITE_OPEN_NOFOLLOW)`,
   enables `SQLITE_DBCONFIG_DEFENSIVE`, disables `TRUSTED_SCHEMA` and extension
   loading, and installs a SQLite authorizer that blocks `ATTACH DATABASE`
   outside the confined root. Treat these checks as defense-in-depth against
   accidental path traversal, not as a substitute for OS-level chroot/mount
   namespaces.
4. **Environment, subprocesses and tool servers.** Default `env_get` filtering
   uses a denylist of common secret variable names; pass a sanitized environment
   or set `BRAT_ENV_DENY_ALL=1` / `BRAT_ENV_ALLOW` explicitly. Subprocess
   execution (`proc_run`) is disabled unless `BRAT_EXEC=1` or `BRAT_EXEC_ALLOW`
   is set. `brat-cxx lsp` (16 MiB message cap) and `brat-cxx mcp` (2 MiB tool
   output cap) enforce protocol size limits, and `BRAT_CC_TIMEOUT_MS` (default
   120,000 ms) bounds C compiler invocations.

See [docs/project/LEDGER.md](docs/project/LEDGER.md) for current checker and
platform limitations.

## Reporting defects

Report defects via GitHub Issues (`https://github.com/Globalysimn/brat/issues`)
until a dedicated private advisory channel is set up. Include the commit hash,
operating system, C/C++ compiler versions, exact command line and a minimal
non-destructive reproducer. Avoid publishing weaponized exploit payloads in
public issues before maintainers have had a chance to reproduce and patch the
defect.
