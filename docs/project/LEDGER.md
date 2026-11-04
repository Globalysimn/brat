# Known risks and work items

This is not an exhaustive audit. The interpreter and two compiler front ends
have substantial independent implementations; tests cover a finite set of
inputs and platform configurations.

- A passing ownership or concurrency check does not prove memory safety or
  race freedom. Projection moves and effects of foreign code have limited
  visibility; see [ownership](../guide/ownership.md).
- `ptr_of_str` and other raw pointers rely on source lifetime checks. Native
  long-string pointers are tied to their owner. Short strings use a fixed,
  process-lifetime pin table; reaching its limit aborts. The interpreter's pin
  limits and failure mode differ. Neither implementation can prevent foreign
  code from retaining or misusing pointers.
- Filesystem confinement and SQLite path/sidecar handling need further review
  for races. Do not use them as an OS security boundary.
- Environment filtering is not a complete secret filter. Run untrusted work
  only with a sanitized environment and external isolation.
- The repository contains platform-specific code; a Linux test run does not
  verify Windows, macOS or 32-bit behavior.
- Benchmark and sanitizer claims in older project notes were not independently
  established by this document. Reproduce them with the relevant tests and
  report the exact environment before relying on them.

For a reproducible defect, record the program, command, actual and expected
result, affected backend and platform. Add a regression when fixing it.
