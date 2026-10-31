# Contributing

Bug reports, failing programs and documentation fixes are all welcome. The bar
is low but real: a change ships with a run.

## Setup

```sh
python3 --version                     # 3.12 or newer, stdlib only
c++ -std=c++20 -O2 lang/cxx/src/*.cpp -o lang/cxx/bin/brat-cxx   # C++ front end
```

`brat-cxx` is optional. Without it the six `test_cxx_*` files exit 77 and are
reported as `skipped`, never as `ok`.

## Tests

Run the file that covers what you touched while you work, then the whole suite
once before you open the pull request:

```sh
python3 lang/tests/test_lang.py            # the interpreter
python3 lang/tests/test_agreement.py       # one corpus, three backends, byte for byte
python3 lang/tests/test_ownership.py       # the checker cases
python3 lang/tests/test_cxx_emit.py        # both emitters, identical C
python3 lang/tests/run_all.py              # everything, about 5 minutes
```

`run_all.py` fails on a difference in any file and does not count a skipped file
as a pass.

## Rules

1. A change ships with a run. Paste the command and its real output; a summary
   is not evidence.
2. The two backends must agree. Anything that works in the interpreter works in
   the native build and the other way round: same stdout, same exit code. If
   they cannot agree, the divergence is a bug and belongs in the changelog.
3. Emitter changes land in both `lang/engine/codegen_c.py` and
   `lang/cxx/src/emit.cpp`, or `test_cxx_emit.py` fails on byte identity.
4. No claim without a number. "Faster" needs a before and an after measured on
   the same machine; unreproducible numbers are deleted, not carried forward.
5. Do not weaken a test to make it pass. A test that swallows an error and
   exits 0 is a broken test.
6. `lang/spec/SPEC.md` and `docs/project/LEDGER.md` are the record of what the
   language promises. A behaviour change needs a spec proposal in the pull
   request text, not a quiet edit.
7. Generated files (`*.c`, `*.o`, `*.exe`, `lang/cxx/bin/`) and scratch
   directories stay out of the tree.

## Style

Match the file you are editing. Comments say why, not what. No dead code, no
commented-out blocks, no dependency that the standard library can replace.

## Where to start

- `docs/guide/tour.md` — the language, section by section.
- `docs/guide/ownership.md` — the checker, and its known holes.
- `docs/project/ROADMAP.md` — what is being worked on.
- `AGENTS.md` — architecture and the red lines.
