# Security

BRAT is experimental. A successful check does not prove memory safety, data-race
freedom or isolation. Do not run untrusted BRAT programs, packages, compiler
inputs or plugins with sensitive credentials or privileges. Use an OS sandbox,
a restricted environment and resource limits appropriate to the host.

Foreign functions, dynamic libraries, raw pointers and their declared effects
are trusted. Checking a declaration cannot establish that the foreign code
honors it. `--unsafe-ffi` explicitly bypasses some verification. A bad pointer
or signature can corrupt memory or crash the process.

The runtime and tools contain resource and path checks, but they are not a
complete sandbox. In particular, path-based checks and SQLite sidecar files
need further adversarial review; platform behavior may differ. The default
`env_get` filtering is a denylist, not a reliable secret boundary. Start with a
sanitized environment; `BRAT_ENV_ALLOW` and `BRAT_ENV_DENY_ALL` are additional
controls. Process, network, package and LSP/MCP inputs also need OS-level
limits. See [known risks](docs/project/LEDGER.md).

Short strings passed to `ptr_of_str` on the native runtime are pinned in a
process-lifetime table. Entries are never evicted, so live pointers are not
silently overwritten. At 32768 unique short strings, another new pin terminates
the process. The interpreter pins up to 32768 distinct strings and 64 MiB of
string bytes and raises an error at the limit. Long native string pointers still
depend on the owner and correct checker behavior. Do not retain pointers beyond
their documented lifetime. These limits and failure modes differ by backend.

## Reporting

Report the affected version, platform, commands and a minimal non-destructive
reproducer. No dedicated private security contact is specified in this source
archive; avoid posting exploit details publicly until a private contact is
established with the project maintainer.
