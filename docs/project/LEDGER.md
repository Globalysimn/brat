# Known Risks and Technical Ledger

_Updated: 2026-11; applies to `brat-cxx` 0.7.x._

This ledger records current architectural limits and residual risks in the
implementation. Passing `brat-cxx check` and the test suite does not prove
absence of memory errors, data races or sandbox escapes.

- **Intra-procedural ownership checking.** `borrow.cpp` tracks places (`x`,
  `x.f`, `x[i]`) within a single function and checks call boundaries against
  declared `move`, `ref` and `mut` modes. Projection moves (`move p.f`) copy a
  counted reference rather than emptying the parent slot, reference cycles built
  across opaque function boundaries are not garbage-collected, and
  mutations performed through opaque foreign pointers are invisible unless
  annotated with `writes(p)`. See [ownership](../guide/ownership.md).
- **Raw pointer and `ptr_of_str` lifetimes.** Static escape analysis rejects
  returning `ptr_of_str(local)` when `local` dies at function exit, and short
  SSO strings (`<= 14` bytes) are interned in a non-moving slab pool capped by
  `BRAT_PIN_BUDGET_BYTES` (default 64 MiB, raising a catchable `string pin limit
  exceeded` error when full). Even so, neither the checker nor the runtime can
  stop a C library from storing a borrowed `const char *` or `ptr` in a global
  and dereferencing it after the BRAT owner is released.
- **User-space path and SQLite confinement is not an OS sandbox.** `crt.h`
  enforces `openat`/`O_NOFOLLOW` leaf checks, rejects symlinks on confined
  `fs_rename`/`fs_remove`, opens SQLite databases with `SQLITE_OPEN_NOFOLLOW`
  plus `(st_dev, st_ino)` and `-wal`/`-shm`/`-journal` sidecar verification,
  enables `SQLITE_DBCONFIG_DEFENSIVE`, disables `TRUSTED_SCHEMA` and extension
  loading, and restricts `ATTACH DATABASE` via an authorizer. Kernel-level
  isolation (containers, mount namespaces, seccomp) is still required for
  hostile inputs.
- **Environment variable denylist.** Default `env_get` filtering blocks common
  credential variable names (`*_TOKEN`, `*_SECRET`, `*_KEY`, `*_PASSWORD`, cloud
  credentials), which catches accidental leaks in logs but cannot know custom
  secret names. Use `BRAT_ENV_DENY_ALL=1` or `BRAT_ENV_ALLOW` in sensitive
  deployments.
- **Platform verification scope.** Linux x86-64 runs all 174 test suites and
  ASan/UBSan/LSan on every push. Native macOS arm64 (`macos-15`) and Windows
  x86-64 (`windows-2022`, MinGW-w64 LLP64) CI jobs verify `brat-cxx` compilation,
  frontend/checker/borrow/module suites, `bindgen`/`ffi --preprocess` ABI probes
  and portable bundles, while POSIX-specific runtime tests and 32-bit (`ILP32`)
  checks are not run end-to-end on Windows/macOS.
