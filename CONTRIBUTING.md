# Contributing

## Reporting bugs

Include a minimal `.brat` program, the exact `brat-cxx` command, expected output,
actual output, operating system and C/C++ compiler versions (`g++ --version`,
`cc --version`). Include the Python version only when reporting an issue in
`lang/tests/run_all.py` or `scripts/bench.py`. Never include tokens or private
credentials. See [SECURITY.md](SECURITY.md) for security-sensitive bugs.

## Building and running tests

Build the native compiler and run the test suite from the repository root:

```sh
make -j2
python3 lang/tests/run_all.py -j 2
```

Run targeted native suites while iterating:

```sh
python3 lang/tests/test_agreement.py --native-only
python3 lang/tests/test_cxx_frontend.py
python3 lang/tests/test_cxx_checker.py
python3 lang/tests/test_cxx_borrow.py
python3 lang/tests/test_cxx_modules.py
python3 lang/tests/test_modules_deep.py
python3 lang/tests/test_sqlite_fs_confinement.py
python3 lang/tests/test_docs_examples.py
```

`run_all.py` accepts explicit test filenames and `--shard K/N` (1-based) for CI
sharding. Every test in `lang/tests/` compiles and executes directly against
`brat-cxx`.

## Submitting changes

- **Every bug fix needs a regression test** that fails before your change and
  passes after it.
- **Runtime, FFI and concurrency changes** must stay clean under AddressSanitizer,
  UndefinedBehaviorSanitizer and LeakSanitizer (`python3 lang/tests/test_asan_examples.py`),
  and ThreadSanitizer where threads or channels are touched.
- **Performance changes** must include `make bench` (`python3 scripts/bench.py --runs 5`)
  before/after numbers, CPU model, OS and C compiler version.
- **Documentation changes** must run `python3 lang/tests/test_docs_examples.py`
  and `python3 lang/tests/test_newcomer.py` so code fences, test counts and
  internal links stay verified.
- Keep compiled binaries, `.brat-build-*` staging directories and scratch files
  out of git commits.
