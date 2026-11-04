# Priorities

Product order: ecosystem, speed, security. Correctness remains a release gate,
not a feature to trade away. The [4 November audit](AUDIT-2026-11-04.md) records
the current evidence and concrete next steps; it does not claim a complete audit
or cross-platform validation.

The engineering work below supports that order:

1. Fix reproducible checker, runtime and backend-parity failures, with minimal
   regression tests. Treat FFI and raw pointers as trusted boundaries.
2. Review filesystem and SQLite confinement for races, and test Windows on a
   real Windows host. Do not rely on these checks as an OS sandbox.
3. Replace process-lifetime string pinning with an explicit, bounded lifetime
   and ownership API. The current table fails when full rather than invalidating
   live pointers; its limit and failure modes differ between backends.
4. Expand differential, fuzz and sanitizer coverage, including optimized and
   unoptimized code and 32-bit targets where available.
5. Reduce duplication between the two compiler implementations only when a
   change preserves their externally tested behavior.

These are priorities, not release commitments. See [known risks](LEDGER.md).
