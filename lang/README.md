# Language implementation

- `engine/brat/`: lexer, parser, module loader and interpreter.
- `engine/borrow.py`, `typer.py`, `caps.py`: checker passes.
- `engine/crt.h`: runtime linked into generated C programs.
- `engine/cli.py`: Python command-line entry point and interpreter driver.
- `cxx/src/`: C++20 implementation of the compiler, sole C emitter and tools.
- `std/`: standard-library modules and interfaces.
- `examples/`: example programs.
- `tests/`: regression and differential tests; `run_all.py` runs the suite.
- `spec/SPEC.md`: implementation-oriented language reference.

See the [root README](../README.md) for build and test commands.
