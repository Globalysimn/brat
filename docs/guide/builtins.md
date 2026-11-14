# Built-in Functions and Methods

Everything on this page works without an `import`. The `std.*` modules are in
[stdlib.md](stdlib.md); the pointer, slice and plugin builtins are in
[ffi.md](ffi.md).

Two conventions run through the tables:

- A lookup that can come back empty answers the value itself or `None`, not
  `Some(...)`: `m.get(k)`, `v.first()`, `v.find(f)`, `s.index_of(t)`. Test the
  result with `is_none(x)`, or give a fallback with `x or fallback`. A `match`
  on it takes `None` and `_`.
- `Option` and `Result` come from the functions that say so: the two parsers
  below, `re_find`, `re_captures`, the file functions and the `std.*` modules.

## Functions

| Call | Answer |
|---|---|
| `print(a, b, ...)` | writes the values to stdout with a space between them and a newline after |
| `eprint(a, b, ...)` | the same to stderr |
| `len(x)` | code points of a string, items of a vector, keys of a map |
| `str(x)` | the text `print` writes for `x` |
| `i64(x)`, `f64(x)` | a number as the other type; `i64` drops the fraction toward zero |
| `chr(n)`, `ord(s)` | the one-character string of code point `n`; the code point of a one-character string |
| `assert(cond)`, `assert(cond, msg)` | raises `assert failed`, or `msg`, when `cond` is false |
| `raise(x)` | leaves the fn with `Err(x)`: a `try` around it in the same fn catches `x`, a caller gets the `Err` as a value and unwraps it with `?`, and `main` ends the program with status 1 |
| `exit(code)` | ends the program with that status |
| `args()` | the command-line arguments as a `vec[str]`, without the program name |
| `unwrap(x)` | the payload of `Ok` or `Some`; raises on `Err` and `None` |
| `unwrap_or(x, fallback)` | the payload, or `fallback` on `Err` and `None` |
| `is_ok(x)`, `is_err(x)`, `is_some(x)`, `is_none(x)` | `bool` |
| `is_int(x)`, `is_float(x)`, `is_str(x)`, `is_bool(x)`, `is_vec(x)`, `is_map(x)` | what the value is at run time |
| `own(x)` | a heap copy of a value built inside `arena { ... }` |
| `chan(cap)`, `join_all()`, `sleep_ms(ms)` | channels and tasks, see the [tour](tour.md#threads) |
| `time_ms()`, `mono_ns()` | milliseconds since the epoch; a monotonic clock in nanoseconds |
| `read_file(path)` | `Ok(text)` or `Err(message)` |
| `write_file(path, text)`, `append_file(path, text)` | `Ok(bytes written)` or `Err(message)` |
| `re_match(text, pattern)` | whether the pattern matches anywhere in the text |
| `re_find(text, pattern)` | `Some(first match)` or `None` |
| `re_captures(text, pattern)` | `Some([match, group 1, ...])` or `None` |
| `re_replace(text, pattern, with)` | the text with every match replaced |

## Strings

Indices and lengths count code points. A string is never changed in place:
every method answers a new value.

| Method | Answer |
|---|---|
| `s.split(sep)` | the pieces between occurrences of `sep`, empty ones included; `s.split()` splits on runs of whitespace |
| `s.split_once(sep)` | `[left, right]` around the first `sep`, or `None` |
| `s.lines()` | the lines, without `\n` or `\r\n` |
| `s.chars()` | one string per code point |
| `s.trim()` | without the whitespace at both ends |
| `s.lower()`, `s.upper()` | Unicode case mapping |
| `s.starts_with(t)`, `s.ends_with(t)`, `s.contains(t)` | `bool` |
| `s.index_of(t)` | where `t` first starts, or `None` |
| `s.replace(old, new)` | every `old` replaced |
| `s.repeat(n)` | `n` copies in a row |
| `s.slice(a, b)` | from index `a` up to, not including, `b`; a negative index counts from the end and one past the end is clipped |
| `s.parse_int()`, `s.parse_float()` | `Ok(number)` or `Err(message)` |
| `s.copy()` | the same string |

`s[i]`, `a + b`, `s * n`, `t in s`, the comparisons and `for ch in s` work on
strings as well.

## Vectors

| Method | Answer |
|---|---|
| `v.push(x)` | appends `x`; the new length |
| `v.pop()` | removes the last item and answers it, `None` when there is none |
| `v.ins(i, x)` | inserts `x` before index `i`; the new length |
| `v.del(i)` | removes the item at `i` and answers it |
| `v.first()`, `v.last()` | the item, or `None` when the vector is empty |
| `v.slice(a, b)` | a new vector, indices as for `s.slice` |
| `v.rev()`, `v.copy()` | a reversed copy; a shallow copy |
| `v.sort()` | sorts in place: numbers by value, strings by code point, vectors item by item |
| `v.sort_by(key)` | sorts in place by what `key(item)` returns; items with equal keys keep their order |
| `v.join(sep)` | the items as one string with `sep` between them |
| `v.sum()` | the sum, `0` for an empty vector |
| `v.min()`, `v.max()` | the smallest and the largest item, `None` for an empty vector |
| `v.pos(x)` | the index of the first item equal to `x`, or `None` |
| `v.enumerate()` | `[[0, a], [1, b], ...]` |
| `v.zip(w)` | `[[a, x], [b, y], ...]`, as long as the shorter of the two |
| `v.chunks(n)` | pieces of `n` items; the last one can be shorter |
| `v.map(f)`, `v.filter(f)` | a new vector of `f(item)`; of the items with `f(item)` true |
| `v.any(f)`, `v.all(f)` | `bool` |
| `v.find(f)` | the first item with `f(item)` true, or `None` |
| `v.fold(init, f)` | `f(acc, item)` over the items, starting from `init` |

`v[i]` raises `index out of range` outside `0..len(v)`. `x in v`, `v + w`, `==`
and `for x in v` work on vectors.

## Maps

Keys keep the order they were inserted in, and `for k in m` walks the keys.

| Method | Answer |
|---|---|
| `m.get(k)` | the value, or `None` |
| `m.get_or(k, fallback)` | the value, or `fallback` |
| `m.has(k)` | `bool` |
| `m.set(k, x)` | the same as `m[k] = x` |
| `m.del(k)` | removes the key; `true` when it was there |
| `m.keys()`, `m.vals()` | vectors, in insertion order |
| `m.items()` | `[[key, value], ...]` |
| `m.copy()` | a shallow copy |

`m[k]` raises `no key` for a key that is not there. `k in m` and `len(m)` work
on maps.

## Option, Result and channels

| Method | Answer |
|---|---|
| `x.unwrap()`, `x.unwrap_or(fallback)` | as the functions of the same name |
| `x.is_ok()`, `x.is_err()`, `x.is_some()`, `x.is_none()` | `bool` |
| `ch.send(x)` | puts `x` on the channel, waiting while it is full |
| `ch.recv()` | the next value, or `None` once the channel is closed and empty |
| `ch.close()` | no more values will be sent |

The file functions stay inside the directory of the program unless
`BRAT_FILES=1` is set, see [cli.md](cli.md).

## A program that uses them

```brat run
fn main() -> i64 {
  let text = "the quick brown fox jumps over the lazy dog the end"
  let mut count = {}
  for word in text.split(" ") {
    count[word] = count.get_or(word, 0) + 1
  }
  let mut words = count.keys()
  words.sort_by(fn(w) { return 0 - count[w] })
  print(words.slice(0, 3), count["the"])

  let lengths = words.map(fn(w) { return len(w) })
  print(lengths.max(), lengths.sum(), words.find(fn(w) { return len(w) > 4 }))

  let first = count.get("cat")
  print(is_none(first), first or 0, "fox".upper(), i64(7 / 2.0))
  match "42".parse_int() {
    Ok(n) => { print(n + 1) },
    Err(e) => { print(e) }
  }
  return 0
}
```

```output
['the', 'quick', 'brown'] 3
5 35 quick
true 0 FOX 3
43
```
