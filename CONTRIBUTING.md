# Contributing

For a bug report, include a minimal program, the command, expected and actual
output, OS, Python version and compiler version. Do not include credentials or
sensitive data. See [SECURITY.md](SECURITY.md) for security reports.

## Build and test

```sh
mkdir -p lang/cxx/bin
c++ -std=c++20 -O2 lang/cxx/src/*.cpp -o lang/cxx/bin/brat-cxx
python3 lang/tests/run_all.py -j 2
```

Run the relevant tests during development. For a behavioral change, add a
regression that fails before the change. Compare the interpreter, native
runtime and both compiler front ends where applicable; mention skips and
platforms not tested. Memory and concurrency changes should include sanitizer
runs where supported, but a clean run is not a proof of safety.

Update the reference and guides when behavior changes. Keep generated binaries,
cache files and test scratch output out of commits. Performance claims need the
workload, baseline, commands, machine and raw results.
