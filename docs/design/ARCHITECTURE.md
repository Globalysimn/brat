# Architecture

The Python implementation parses and loads modules, checks ownership, types and
effects, and interprets the program. It does not emit C.

`lang/cxx/src/` contains the C++20 frontend, checker, sole C emitter, optimizer
and command-line tools. Generated C uses `lang/engine/crt.h` as its runtime,
and a host C compiler produces the native executable. Tests compare selected
programs and diagnostics between the interpreter and native implementation;
parity is a goal, not an established property for every input.

The runtime represents dynamic values using tagged `Val` values. Heap objects use
reference counting; `arena` blocks use region allocation. The checker tracks
some ownership, escaping references and concurrent access. Its analysis has
limits described in [ownership](../guide/ownership.md) and
[security](../../SECURITY.md).

The Python CLI delegates native-capable commands to a fresh
`lang/cxx/bin/brat-cxx`. Set `BRAT_NO_NATIVE_CLI=1` to exercise Python CLI
paths: interpreted/checking paths stay in Python, while `emit-c`, `build` and
`dist` still require the native compiler and fail explicitly if it is missing,
stale or broken. `run --interp` selects the interpreter. The native compiler
supports bundling its runtime and standard library, but a host C compiler
remains necessary to build native programs.

Relevant tests include `test_agreement.py`, `test_cxx_frontend.py`,
`test_cxx_checker.py`, `test_cxx_emit.py` and `test_pin_lifetime.py`. Run
`python3 lang/tests/run_all.py -j 2` for the suite and inspect skips as well as
failures.
