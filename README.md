# BRAT

A systems language with ownership checking, compiled to C99.

One owner per value, `move` at the point of transfer, borrows that end at their
last use, and no lifetime annotations anywhere. Programs become portable C99
with no runtime to install; a second, independent front end written in C++20
emits the same C and prints the same diagnostics.

```
fn main() -> i64 {
  let name = "world"
  print("hello {name}")
  let port = unwrap("8080".parse_int())
  print(port)
  return 0
}
```

```
$ python3 lang/engine/cli.py run hello.brat
hello world
8080
$ python3 lang/engine/cli.py build hello.brat -o hello && ./hello
hello world
8080
```

Status: v0.1, experimental.

## Install

Nothing to install. Python 3.12+ (standard library only) and any C compiler.

```sh
git clone https://github.com/Nefor229883984/d.git brat && cd brat

# optional: the C++20 front end, same tree, same C
c++ -std=c++20 -O2 lang/cxx/src/*.cpp -o lang/cxx/bin/brat-cxx
```

The CLI is `python3 lang/engine/cli.py`; the documentation calls it `brat`.
With `brat-cxx` present, `brat run` and `brat build` use it and never start
Python. Without it, the Python engine is the reference implementation and the
same programs behave the same way.

## Language

| | |
|---|---|
| Ownership | one owner, `move`, `ref`/`mut` at call sites, checked per function with a line, a snippet and a hint; a `for` refuses a write into the container it walks. No `'a` |
| Errors | `Ok`/`Err`/`Some`/`None`, `?`, `or`, `try`/`catch`, `defer` on every exit path including error unwinding |
| Memory | refcounted strings, containers and tags, released at scope and loop exit; `arena { }` frees a block wholesale; `own(x)` copies a value out |
| Data | `i64`, `f64`, `bool`, `str`, `vec[T]`, `map`, structs, enums, `match` with guards, closures, generics by erasure |
| Threads | `spawn { }`, `scope { }`, `join_all()`, `chan(cap)`; real OS threads in both backends |
| Data races | one writer: while a `spawn` runs exactly one side may write a shared value, and both checkers refuse a program that breaks it before it runs |
| C interop | `extern fn` with every width the ABI distinguishes (`i8`..`u64`, `f32`, `f64`), `extern struct` by value in and out, out-parameters `&x` at `ptr[T]`, callbacks (`f32` and records included), variadic tails, `--link` for flags a library name cannot express, `brat bindgen`, `brat ffi --emit/--header`, `export fn`, `lib_open`/`call` at run time; `build`, `run`, `dist` and `emit-c` refuse a program that calls C until a `--header` proves the declarations, or `--unsafe-ffi` says to skip it |
| Modules | `import a.b`, typed `.bri` interfaces, `pub use`, `brat deps`, `brat api`, `brat pkg` with a sha256 lockfile |
| Tooling | `run`, `check --json`, `explain`, `test`, `fmt`, `emit-c`, `build`, `dist`, `deps`, `api`, `bindgen`, `ffi`, `lsp`, `mcp` |

`brat check --json` prints stable codes with a `helps` remedy — `E0382` moved
value, `E0505` move while borrowed, `E0499` double mutable borrow, `E0515`
borrow escapes, `E0596` write to an immutable place, `E0502` write into the
container a `for` is walking, `F0001` unsupported FFI type, `B0521` borrow captured by
`spawn`, `B0522` two threads on one value, `T0001` type error, `P0001` syntax
error — so a tool branches on the code, not on the English. `brat explain B0522`
turns one back into the rule and the fix, offline.

## Limits

What the language and the checker do not promise. Neither list is a to-do file;
`docs/project/ROADMAP.md` is.

| | |
|---|---|
| Checker soundness | a clean `brat check` is not a proof of memory safety or data-race freedom. Guarantees and their boundaries are in [`lang/spec/SPEC.md`](lang/spec/SPEC.md) §11 |
| Function values | a fn name is a callback (`v.map(f)`, `run("f")`), not a value; `let g = f` is a checker error |
| Ownership across calls | checked from the signature's `move`/`ref`/`mut`; what a callee does afterwards is an agreement, not a proof |
| Plugin prototypes | a `call` signature is checked against the `extern fn` the program declares; a name with no declaration is undefined behaviour |
| Data races through FFI | the sharing rule reads the program, not foreign memory: a `ptr` from a plugin and anything behind `unsafe` is the author's responsibility. Writes C makes through a pointer (`ptr_write_*`, `slice_set`, `fill`) and a `move` that ends a value's life do count as writes on this side |
| Deep values | `print`, `str()` and comparison stop at 2000 levels of nesting with `recursion too deep`; the C walkers are recursive |
| Packages | local index, `publish`, transitive resolution, sha256 lock; no hosted registry |
| Generics | erased, not monomorphized; no trait objects, no macros, no async |

Known holes with reproductions: [`docs/project/LEDGER.md`](docs/project/LEDGER.md)
and [`docs/guide/ownership.md`](docs/guide/ownership.md).

## Build from source

The build is the source tree; there is no configure step and no generated code
checked in. `lang/cxx/bin/brat-cxx` is the only binary, and it is optional:

```sh
mkdir -p lang/cxx/bin
c++ -std=c++20 -O2 lang/cxx/src/*.cpp -o lang/cxx/bin/brat-cxx
python3 lang/engine/cli.py check lang/examples/hello.brat
```

Tests:

```sh
python3 lang/tests/run_all.py -j 2     # every file, one line each
python3 lang/tests/test_lang.py        # or a single file
```

116 files, one per area, about 230 s on four cores (measured, `-j 4`). A test that cannot run exits
77 and is reported as `skipped`, never as `ok`; the C++ files skip themselves
when there is no `c++`, which is why CI builds `brat-cxx` before it runs the
suite. Three properties are checked by construction rather than by example: the
interpreter and the C backend print the same bytes and return the same exit
code, the two checkers produce the same diagnostics, and the emitted C compiles
as `-std=c99 -Wall -Wextra` without warnings.

## Repository

```
lang/engine/     parser, checkers, interpreter, C emitter, runtime (crt.h), CLI, LSP, MCP
lang/cxx/        C++20 front end: the same tree, the same diagnostics, the same C
lang/examples/   64 programs; the tests run most of them on both backends
lang/spec/       SPEC.md, the language contract
lang/tests/      116 files, one per area; run_all.py drives the whole suite
lang/servers/    example server programs
editors/         syntax highlighting and editor support
docs/            guide, design notes, ledger, roadmap
```

## Documentation

| | |
|---|---|
| [`docs/guide/tour.md`](docs/guide/tour.md) | the language, section by section, with runnable snippets |
| [`docs/guide/ownership.md`](docs/guide/ownership.md) | what the checker proves, and what it does not |
| [`docs/guide/ffi.md`](docs/guide/ffi.md) | C interop: declarations, generated bindings, the build gate |
| [`docs/guide/modules.md`](docs/guide/modules.md) | imports, interfaces, packages |
| [`lang/spec/SPEC.md`](lang/spec/SPEC.md) | the language contract, including §11 on limits |
| [`docs/design/ARCHITECTURE.md`](docs/design/ARCHITECTURE.md) | how the pieces fit |
| [`docs/project/LEDGER.md`](docs/project/LEDGER.md) | known holes, each with the file that proves it |
| [`docs/project/ROADMAP.md`](docs/project/ROADMAP.md) | where it stands, what comes next |

## Contributing

[`CONTRIBUTING.md`](CONTRIBUTING.md) — a change ships with a test that fails
before it and passes after, and safety-related changes need a case in the
interpreter and in the generated C. Security reports: [`SECURITY.md`](SECURITY.md).

## License

Apache-2.0, see [`LICENSE`](LICENSE).
