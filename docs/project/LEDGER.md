# Known Risks and Technical Ledger

_Updated: 2026-11 (`brat-cxx` 0.7.x)._

Current checker boundaries and operational limits in `brat-cxx` and `crt.h`:

- Intra-procedural ownership checking: `borrow.cpp` tracks places (`x`, `x.f`,
  `x[i]`) within one function at a time and checks call sites against declared
  `move`, `ref`, and `mut` modes. Projection moves (`move p.f`) are enforced
  statically while retaining a counted reference in the runtime container slot;
  reference cycles built across separate functions are not collected; foreign
  pointer mutations are invisible unless declared with `writes(p)`. Details:
  [ownership](../guide/ownership.md).
- Raw pointers and pinned strings: static escape checks reject returning
  `ptr_of_str(local)` when `local` dies on return. Short SSO strings (`<= 14`
  bytes) passed to C and strings returned to C across `export fn`, callbacks,
  `ptr_write_str`, or `ptr_write_struct` are interned in non-moving pools capped
  by `BRAT_PIN_BUDGET_BYTES` (default 64 MiB, raising catchable `string pin
  limit exceeded` when full). Borrowed `str` and `ptr` arguments passed into
  `extern fn` calls are valid only for the duration of the call; nothing stops a
  C library from stashing a borrowed address in a global and reading it after
  the BRAT owner is freed.
- User-space confinement vs OS isolation: `crt.h` uses `openat`/`O_NOFOLLOW`
  leaf checks, rejects symlinks on confined `fs_rename`/`fs_remove`, opens
  SQLite with `SQLITE_OPEN_NOFOLLOW` plus `(st_dev, st_ino)` and
  `-wal`/`-shm`/`-journal` checks, enables `SQLITE_DBCONFIG_DEFENSIVE`, disables
  `TRUSTED_SCHEMA` and extension loading, and restricts `ATTACH DATABASE` via an
  authorizer. These guard against accidental path traversal in ordinary tools;
  untrusted inputs still require OS-level isolation (containers, namespaces,
  seccomp).
- Environment denylist: default `env_get` filtering blocks common credential
  names (`*_TOKEN`, `*_SECRET`, `*_KEY`, `*_PASSWORD`, cloud keys), which avoids
  accidental secret dumps in logs but cannot guess custom variable names. Set
  `BRAT_ENV_DENY_ALL=1` or `BRAT_ENV_ALLOW` when running with sensitive
  environments.
- Platform test matrix: Linux x86-64 runs all 174 test suites plus
  ASan/UBSan/LSan on every push. Native macOS arm64 (`macos-26`) and Windows
  x86-64 (`windows-2022`, MinGW-w64 LLP64) CI jobs build `brat-cxx` and run
  frontend, checker, borrow, module, `bindgen`/`ffi --preprocess`, and `bundle`
  checks; POSIX-specific socket/process suites and 32-bit (`ILP32`) runtime
  tests are not run end-to-end on macOS or Windows.
