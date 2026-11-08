# Limits and Sharp Edges

This page documents concrete operational limits, representation costs and edge
cases in `brat-cxx` and the C11 runtime. Every `brat run` and `brat check`
example on this page is verified by `lang/tests/test_docs_examples.py`.

## Speed

Reproduce on your machine with `make bench` (`python3 scripts/bench.py`).
Median of 5 runs on Linux x86-64 (AMD EPYC 7763, GCC 13.3.0 `-O2`, Go 1.27.3, CPython 3.14.2):

| Workload | C (`gcc -O2`) | Go 1.27 (`go build`) | BRAT (`brat-cxx build -O2`) | CPython 3.14 |
|---|---:|---:|---:|---:|
| `int_loop(100M)` (wrapping `i64` arithmetic loop) | 0.126 s | 0.129 s | 0.126 s | 16.795 s |
| `vec3_sra(50M)` (loop-carried nested `struct` step) | 0.134 s | 0.155 s | 0.141 s | 90.569 s |
| `fib(35)` (29.9M recursive `i64` calls) | 0.024 s | 0.055 s | 0.050 s | 1.167 s |
| `str_append(200k)` (chained `+` vs `Builder`) | 0.014 s | 0.015 s | 0.031 s | 0.057 s |
| `dyn_records(100k)` (`map` + `str` concat + RC drops) | n/a | 0.017 s | 0.020 s | 0.047 s |
| `json_roundtrip(5k)` (`std.json` parse + stringify) | n/a | 0.039 s | 0.010 s | 0.067 s |

Performance follows value representation:

- **Unboxed `i64`, `f64`, `bool`, and `vec[i64]`/`vec[f64]` kernels** compile to
  raw C scalar functions (`int64_t` / `double`). Tight arithmetic loops match
  `gcc -O2` (`0.126 s` vs `0.126 s`). Self-recursive scalar kernels (`fib(35)`)
  keep the remaining recursion budget in a register across internal calls and
  amortize stack-floor probes to 64-frame intervals (`0.050 s` vs unchecked C's
  `0.024 s` and Go's `0.055 s`).
- **Nominal `struct` values** use a canonical declaration-order `BShape` layout
  regardless of field order at the literal site, lowering statically typed field
  loads and stores directly to constant-slot accesses (`vstruct_slot_ptr` /
  `vstruct_slot_set`) with a gradual-typing fallback when an unshaped `map`
  arrives via `Dyn`. At `-O1` and `-O2`, multi-level Escape Analysis + Scalar
  Replacement of Aggregates (SRA) decomposes non-escaping, unaliased local
  `struct` bindings (including `.copy()` value copies) directly into unboxed C
  scalars or local `Val` slots (`0.141 s` vs `0.134 s` in `gcc -O2` across 50M
  nested `Body { pos: Vec3, vel: Vec3 }` steps; inspect with
  `brat-cxx cc -O2 --why-opt`).
- **Dynamic records, maps and strings** use 16-byte tagged `Val` values with
  14-byte inline SSO strings, SWAR ASCII scanning, in-place `rc == 1` buffer
  growth across chained `s = s + ...` concatenations (`0.031 s` on 200k chained
  appends), constant-key shaped maps (`vmap_shaped`) backed by small-container
  freelists, and 4-way inline caches (`BFieldIC`) on constant-key indexing
  (`0.020 s` on 100,000 map allocations + string concatenations + drops).
- **`std.json`** delegates RFC 8259 parsing and serialization to a direct C
  scanner/emitter in `crt/sql_proc.h`, finishing 5,000 nested object roundtrips
  in `0.010 s` (`0.039 s` in Go `encoding/json`, `0.067 s` in CPython's C `json`
  module).

## Recursion budget (2000 frames)

The runtime caps BRAT call depth at 2000 frames (`main` included) and raises a
catchable `recursion too deep` error:

```brat run
fn depth(n: i64) -> i64 {
  if n == 0 {
    return 0
  }
  return 1 + depth(n - 1)
}

fn main() -> i64 {
  print(depth(1998))
  try {
    print(depth(1999))
  } catch e {
    print(e)
  }
  return 0
}
```

```output
1998
recursion too deep
```

Set `BRAT_RECURSION_BUDGET=N` (`1..1000000`) to adjust the frame cap. On Linux
(`pthread_getattr_np`), macOS (`pthread_get_stackaddr_np`), and Windows 8+
(`GetCurrentThreadStackLimits`), compiled binaries also query the thread stack
bounds and raise `recursion too deep` when less than 256 KiB of stack remains,
turning stack exhaustion under large budgets into a catchable error instead of a
crash. Other targets rely on the frame-count budget alone. Recursive value
walkers (`print`, `==`, `<`, `own`) enforce the same 2000-level nesting cap.

## Signed 64-bit wrapping integers

All BRAT integers are `i64`. `+`, `-`, `*` and unary `-` wrap in two's
complement; `/` rounds toward negative infinity and `%` takes the divisor's
sign. `parse_int()` rejects overflow instead of wrapping:

```brat run
fn main() -> i64 {
  let top = 9223372036854775807
  print(top + 1)
  print(-7 / 2)
  print(-7 % 2)
  print(7 / 2.0)
  print("9223372036854775808".parse_int())
  print("+42".parse_int())
  print("1e3".parse_int())
  return 0
}
```

```output
-9223372036854775808
-4
1
3.5
Err('number out of range')
Ok(42)
Err('not a number')
```

## Integer operators

Bitwise operators work on `i64`: `&`, `|`, `^`, `~`, `<<` and `>>`.
Shift counts use their low six bits; right shift sign-extends. Hexadecimal,
binary and octal literals use `0x`, `0b` and `0o` prefixes. Unary `&x` is an
extern C out-parameter. There is no `**` operator.

```brat run
fn main() -> i64 {
  let flags = 6 | 1
  print(flags)
  return 0
}
```

```output
7
```

## Named functions are callbacks, not first-class values

A top-level `fn` name can be passed directly to `map`, `filter`, `fold`, `any`,
`all`, `find` and `sort_by` (or as `"fn_name"` to a C callback parameter), but
cannot be stored in a variable. Use a `fn(x) { ... }` closure when you need a
first-class function value:

```brat check
fn double(x: i64) -> i64 {
  return x * 2
}

fn main() -> i64 {
  let f = double
  print(f(2))
  return 0
}
```

```diagnostics
example.brat: 1 error(s), 0 warning(s)
example.brat:6: error: use of `double` as a value: a fn name is a callback, not a value (hint: call it: `double()`)
  |   let f = double
example.brat: note: help: a bare fn name is a callback argument to `map`, `filter`, `fold`, `any`, `all`, `find` and `sort_by` only
example.brat: 0 type error(s)
```

```brat run
fn double(x: i64) -> i64 {
  return x * 2
}

fn main() -> i64 {
  print([1, 2, 3].map(double))
  let triple = fn(x) { return x * 3 }
  print(triple(4))
  return 0
}
```

```output
[2, 4, 6]
12
```

## Indexing, variants and map keys

Vector and string indices accept `i64` or integral `f64` values (`2.0`), while
fractional floats, `bool`, `nan` and `inf` raise `index must be int`. Enum and
`Result`/`Option` variants (`Ok(42)`) are not indexable by `[]`; extract their
payloads with `match`, `if let`, `?` or `unwrap`:

```brat run
fn main() -> i64 {
  let v = [10, 20, 30]
  print(v[2.0])
  for i in [1.5, true] {
    try {
      print(v[i])
    } catch e {
      print(e)
    }
  }
  try {
    let r = Ok(42)
    print(r[1])
  } catch e {
    print(e)
  }
  return 0
}
```

```output
30
index must be int
index must be int
index of non-indexable
```

Map keys distinguish tag and value: `1`, `1.0` and `true` are three distinct
keys in the same map. Vectors and maps cannot be used as map keys:

```brat run
fn main() -> i64 {
  let m = {1: "int", 1.0: "float", true: "bool"}
  print(len(m))
  print(m[1])
  print(m[1.0])
  print(m[true])
  try {
    print(m[[1]])
  } catch e {
    print(e)
  }
  return 0
}
```

```output
3
int
float
bool
no key [1]
```

## UTF-8 code-point strings and raw OS bytes

`len(s)`, `s[i]`, `s.slice(a, b)` and `s.chars()` count Unicode scalar values
(code points), not UTF-8 bytes and not extended grapheme clusters. NUL bytes are
rejected in string literals and across FFI string boundaries. For non-UTF-8 OS
arguments, environment variables or paths, use `args_bytes()`, `env_get_bytes()`
and byte-vector paths in `read_file`/`write_file`/`sql_open`/`proc_run`:

```brat run
fn main() -> i64 {
  let s = "héllo"
  print(len(s))
  print(s[1])
  print(s.upper())
  print("ß".upper())
  return 0
}
```

```output
5
é
HÉLLO
SS
```

## Top-level error reporting and dynamic method checks

- Uncaught top-level errors print `unhandled: Err('...')` to `stderr` (with `at
  line N` when built or run with `--trace-lines`) and exit with status `1`.
  `try { ... } catch e { ... }` receives the payload string directly.
- Built-in method arity errors are caught statically by `brat-cxx check` (for
  example, `"abc".chars(1)` reports `method \`chars\` wants 0 args`), while user
  `impl` methods with the same name override built-in arity freely.
- On untyped dynamic receivers passed wrong argument types at run time, compiled
  binaries reject the call cleanly without leaking internal C helper names
  (`test_method_arguments_text.py` pins all 1,571 cases).

## Compile time and caching

Pass-2 subsystem trimming (`BRAT_CRT_MASK`) strips unused runtime sections from
`crt.h` before invoking the C compiler, so a minimal `fn main() -> i64 { return
0 }` cold `brat-cxx build -O2` finishes in `0.74 s` (GCC 14.2 on Xeon 2.60 GHz).
Functions with 24 or more boxed `Val` locals pack them into a single `Val
__fv[N]` array so `-O2` compile times scale linearly with local count (`make
bench`, `BRAT_NATIVE_CACHE_DISABLE=1`):

| Heap string locals (`N`) | Emitted C | Cold `brat-cxx build -O2` |
|---:|---:|---:|
| 50 | 27.3 KB | 1.10 s |
| 100 | 47.4 KB | 1.40 s |
| 200 | 88.2 KB | 2.20 s |
| 400 | 169.6 KB | 3.98 s |

`brat-cxx run` caches compiled binaries keyed by compiler binary digest,
effective environment, emitted C, `crt.h` digest, optimization flags and
resolved foreign library contents (`test_native_cache_inputs.py`). Set
`BRAT_NATIVE_CACHE_DISABLE=1` to force a cold compile, `BRAT_CC` to select an
alternate C compiler executable, and `BRAT_CC_TIMEOUT_MS` (default `120000`, max
`3600000`) to bound C compiler run time.

## Platform status

Linux x86-64 runs the full 174-file test suite (GCC and Clang, 8-way sharded) and
ASan/UBSan/LSan checks in CI on every push. Native macOS arm64 (`macos-15`, Apple
Clang) and Windows x86-64 (`windows-2022`, MinGW-w64 LLP64) CI jobs build
`brat-cxx`, verify the frontend/checker/borrow/module suites, exercise `bindgen
--preprocess` and `ffi --preprocess` header/object verification, and test
portable `bundle` archives; POSIX-specific process/socket tests remain Linux-only.
