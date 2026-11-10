# Contributing

## Build and test

Building requires `make`, a C++20 compiler (`g++` or `clang++`), and a C11
compiler (`cc`, `gcc`, or `clang`). Python 3 is used to run the test and
benchmark scripts:

```sh
make -j4
make -j4 asan
python3 lang/tests/run_all.py -j 4
```

`run_all.py` accepts individual test filenames and `--shard K/N` (1-based) for
CI sharding. Every test in `lang/tests/` compiles and executes against
`brat-cxx`.

Formatting across all `.brat` files is checked with:

```sh
./lang/cxx/bin/brat-cxx fmt --check $(git ls-files '*.brat')
```

## Submitting changes

- Bug fixes need a minimal regression test that fails before the change and
  passes after it.
- Runtime, FFI, and concurrency changes must pass under AddressSanitizer,
  UndefinedBehaviorSanitizer, and LeakSanitizer
  (`python3 lang/tests/test_asan_examples.py`), plus ThreadSanitizer when
  threads, locks, or channels are touched.
- Compiler and runtime edits must compile with zero warnings under `-Wall
  -Wextra -Wformat=2 -Werror` on both GCC and Clang, and `lang/engine/crt.h`
  must stay clean under `x86_64-w64-mingw32-gcc -Werror` (no bare `long` on
  Windows LLP64).
- Performance changes must include `make bench` (`python3 scripts/bench.py
  --runs 5`) numbers before and after, along with CPU model, OS, and C compiler
  version.
- Documentation changes must pass `python3 lang/tests/test_docs_examples.py` and
  `python3 lang/tests/test_newcomer.py`.
- Keep compiled binaries, `.brat-build-*` staging directories, and scratch files
  out of commits.
