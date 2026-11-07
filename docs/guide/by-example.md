# BRAT by Example

Ten self-contained programs covering the parts of BRAT you actually reach for in
a tool or service, plus the sharp questions people ask first when evaluating a
reference-counted language without lifetime annotations.

Every `brat run` and `brat check` block on this page is compiled and checked in
CI by `lang/tests/test_docs_examples.py`.

---

## 1. Records, `impl` methods, and exhaustive `enum` matching

Records need no boilerplate `struct` header: `Job { id: 7, ms: 18 }` creates a
tagged record with tag `"Job"`. `enum` registers variant constructors, and
`match` checks exhaustiveness at parse time when matching variants declared in
the same file:

```brat run
enum Outcome {
  Done(ms),
  Retry(attempt, reason),
  Cancelled
}

impl Job {
  fn make(id: i64, state) {
    return Job { id: id, state: state }
  }

  fn summary(self) -> str {
    match self.state {
      Done(ms) => {
        return "job #{self.id}: ok in {ms}ms"
      },
      Retry(attempt, reason) => {
        return "job #{self.id}: retry #{attempt} ({reason})"
      },
      Cancelled => {
        return "job #{self.id}: cancelled"
      }
    }
    return ""
  }
}

fn main() -> i64 {
  let batch = [
    Job.make(1, Done(14)),
    Job.make(2, Retry(3, "upstream 503")),
    Job.make(3, Cancelled)
  ]
  for j in batch {
    print(j.summary())
  }
  return 0
}
```

```output
job #1: ok in 14ms
job #2: retry #3 (upstream 503)
job #3: cancelled
```

---

## 2. Errors as values (`Result`, `?`, and LIFO `defer`)

Recoverable errors travel as `Ok(v)` and `Err(e)`. The `?` suffix unwraps `Ok`
and returns `Err` early from the enclosing function, running any registered
`defer` statements in reverse order on the way out:

```brat run
fn parse_port(line: str) {
  defer print("  leaving parse_port({line})")
  let parts = line.split_once("=")
  if is_none(parts) {
    return Err("expected key=value")
  }
  let port = parts[1].trim().parse_int()?
  if port < 1 || port > 65535 {
    return Err("port {port} out of range 1..65535")
  }
  return Ok(port)
}

fn main() -> i64 {
  for raw in ["port=8080", "port=99999", "broken"] {
    match parse_port(raw) {
      Ok(p) => { print("accepted {p}") },
      Err(e) => { print("rejected: {e}") }
    }
  }
  return 0
}
```

```output
  leaving parse_port(port=8080)
accepted 8080
  leaving parse_port(port=99999)
rejected: port 99999 out of range 1..65535
  leaving parse_port(broken)
rejected: expected key=value
```

---

## 3. Unknown-size JSON in an `arena`, keeping one result with `own()`

When parsing untrusted or nested JSON, intermediate `vec`, `map`, and `str`
allocations don't need individual `free()` traffic. Wrap the request in `arena {
... }`, walk the parsed tree dynamically, and promote only the surviving value
to the heap with `own(...)`:

```brat run
import std.json

fn extract_slow_endpoints(body: str, threshold_ms: i64) {
  arena {
    let root = json.parse(body)?
    let service = root["service"]
    let spans = root["trace"]["spans"]
    let mut slow = []
    for sp in spans {
      let ms = sp["ms"]
      if ms >= threshold_ms {
        let route = sp["route"]
        slow.push("{service}:{route}={ms}ms")
      }
    }
    return Ok(own(slow))
  }
}

fn main() -> i64 {
  let raw = "{\"service\":\"billing\",\"trace\":{\"spans\":[{\"route\":\"/charge\",\"ms\":120},{\"route\":\"/ping\",\"ms\":2},{\"route\":\"/refund\",\"ms\":85}]}}"
  let hits = unwrap(extract_slow_endpoints(raw, 50))
  print(hits.join(", "))
  return 0
}
```

```output
billing:/charge=120ms, billing:/refund=85ms
```

Forgetting `own(slow)` does not silently dangle at run time; `brat check`
rejects the program before C code is generated:

```brat check
fn bad() -> vec[str] {
  arena {
    let mut slow = ["a", "b"]
    return slow
  }
}
```

```diagnostics
example.brat: 0 error(s), 0 warning(s)
example.brat: 1 type error(s)
example.brat:4: error: arena: a value made inside the block is returned out of it  (hint: hand out a heap copy with `own(...)`, e.g. `return Err(own(msg))`)
  |     return slow
```

---

## 4. Hash table CRUD (`map`)

`map` is an open-addressed hash table with inline storage for small maps and
4-way inline caches on constant string keys. Indexing (`m[k]`) raises on a
missing key; `.get(k)` returns `None`; `.get_or(k, fallback)` supplies a
default:

```brat run
fn main() -> i64 {
  let mut hits = {"/health": 1, "/login": 3}

  // Insert and update
  hits["/charge"] = 2
  hits["/login"] = hits.get_or("/login", 0) + 1

  // Membership and safe lookup
  print("has /login:", hits.has("/login"))
  print("missing /nope:", is_none(hits.get("/nope")))

  // Delete returns true when the key existed
  assert(hits.del("/health"))

  // Deterministic reporting by sorting keys
  let mut keys = hits.keys()
  keys.sort()
  for k in keys {
    print("{k} -> {hits[k]}")
  }
  return 0
}
```

```output
has /login: true
missing /nope: true
/charge -> 2
/login -> 4
```

---

## 5. Doubly-linked lists and LRU queues without pointer cycles

BRAT uses deterministic reference counting without a background cycle collector.
Intra-function ownership loops (`v.push(v)` or `a["next"] = b; b["prev"] = a`)
fail statically at `brat check`, and indirect back-pointer cycles wired across
functions leak unless scoped inside an `arena`.

Just as in Rust or data-oriented C, long-lived doubly-linked lists and LRU
queues store nodes in flat vectors and link them with `i64` indices (`-1` for
null). You get $O(1)$ splice/detach, cache locality, and zero reference cycles:

```brat run
impl LruList {
  fn make() {
    return LruList { keys: [], prev: [], next: [], head: -1, tail: -1 }
  }

  fn push_front(mut self, key: str) -> i64 {
    let idx = len(self.keys)
    let old_head = self.head + 0
    self.keys.push(key)
    self.prev.push(-1)
    self.next.push(old_head)
    if old_head != -1 {
      self.prev[old_head] = idx
    } else {
      self.tail = idx
    }
    self.head = idx
    return idx
  }

  fn move_to_front(mut self, idx: i64) {
    let cur_head = self.head + 0
    let cur_tail = self.tail + 0
    if idx == cur_head {
      return
    }
    let p = self.prev[idx] + 0
    let n = self.next[idx] + 0
    if p != -1 { self.next[p] = n }
    if n != -1 { self.prev[n] = p }
    if idx == cur_tail { self.tail = p }
    self.prev[idx] = -1
    self.next[idx] = cur_head
    if cur_head != -1 { self.prev[cur_head] = idx }
    self.head = idx
  }

  fn order(self) -> str {
    let mut out = []
    let mut cur = self.head + 0
    while cur != -1 {
      out.push(self.keys[cur])
      cur = self.next[cur] + 0
    }
    return out.join(" -> ")
  }
}

fn main() -> i64 {
  let mut lru = LruList.make()
  let a = lru.push_front("page:a")
  let _b = lru.push_front("page:b")
  let _c = lru.push_front("page:c")
  print("initial:", lru.order())
  lru.move_to_front(a)
  print("touched a:", lru.order())
  return 0
}
```

```output
initial: page:c -> page:b -> page:a
touched a: page:a -> page:c -> page:b
```

---

## 6. Linear-time regular expressions (Pike VM)

Built-in `re_match`, `re_find`, `re_captures`, and `re_replace` run on a
non-backtracking Thompson/Pike VM in `crt/regex.h`, so pathological inputs do
not go exponential. Note `\{4\}`: unescaped `{4}` inside `"..."` would
interpolate the integer `4`:

```brat run
fn main() -> i64 {
  let line = "2026-11-07T14:22:09Z status=502 latency=184ms"

  assert(re_match(line, "status=5\d\d"))

  let status_tok = unwrap(re_find(line, "status=\d+"))
  print(status_tok)

  let caps = unwrap(re_captures(line, "^(\d\{4\}-\d\{2\}-\d\{2\})T.*latency=(\d+)ms$"))
  print("date={caps[1]} ms={caps[2]}")

  let redacted = re_replace("token=abc123 token=xyz987", "token=[a-z0-9]+", "token=[REDACTED]")
  print(redacted)
  return 0
}
```

```output
status=502
date=2026-11-07 ms=184
token=[REDACTED] token=[REDACTED]
```

---

## 7. Parameterized SQLite queries (`std.sqlite`)

`std.sqlite` binds directly to `libsqlite3` with defensive mode enabled,
extension loading off, and `?1`-style positional bindings so user strings never
touch SQL syntax:

```brat run
import std.sqlite

fn run_ledger() {
  let db = sqlite.open_memory()?
  defer sqlite.close(db)

  sqlite.exec(db, "CREATE TABLE events (service TEXT NOT NULL, ms INT NOT NULL);")?
  sqlite.query(db, "INSERT INTO events VALUES (?1, ?2);", ["auth", 19])?
  sqlite.query(db, "INSERT INTO events VALUES (?1, ?2);", ["billing", 64])?
  sqlite.query(db, "INSERT INTO events VALUES (?1, ?2);", ["auth", 23])?

  let total = sqlite.scalar_int(db, "SELECT SUM(ms) FROM events WHERE service = ?1;", ["auth"])?
  let top = unwrap(sqlite.query_one(db, "SELECT service, ms FROM events ORDER BY ms DESC LIMIT 1;", [])?)
  return Ok("auth_sum={total}ms slowest={top[\"service\"]}({top[\"ms\"]}ms)")
}

fn main() -> i64 {
  print(unwrap(run_ledger()))
  return 0
}
```

```output
auth_sum=42ms slowest=billing(64ms)
```

---

## 8. SHA-256, HMAC, and constant-time MAC verification (`std.crypto`)

`std.crypto` ships FIPS 180-4 SHA-256, RFC 2104 HMAC-SHA256, OS CSPRNG bytes,
and branchless byte-length-aware `constant_time_eq` for webhook or session
signatures:

```brat run
import std.crypto

fn verify_webhook(secret: str, body: str, header_hex: str) -> bool {
  let expected = unwrap(crypto.hmac_sha256_hex(secret, body))
  return crypto.constant_time_eq(expected, header_hex)
}

fn main() -> i64 {
  let secret = "whsec_test_key"
  let payload = "{\"event\":\"invoice.paid\",\"amount\":4200}"
  let sig = unwrap(crypto.hmac_sha256_hex(secret, payload))
  print("sha256:", unwrap(crypto.sha256_hex("abc")))
  print("valid sig:", verify_webhook(secret, payload, sig))
  print("tampered:", verify_webhook(secret, payload + " ", sig))
  return 0
}
```

```output
sha256: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
valid sig: true
tampered: false
```

---

## 9. CLI flags, paths, and ISO-8601 UTC timestamps

`std.cli`, `std.path`, and `std.time` handle command-line tools without external
packages or `strftime` locale surprises:

```brat run
import std.cli
import std.path
import std.time

fn main() -> i64 {
  let argv = ["--verbose", "--workers=4", "deploy", "configs/../configs/prod.json"]
  let value_flags = ["--workers"]

  let cmd = cli.subcommand(argv, value_flags)
  let rest = cli.subcommand_args(argv, value_flags)
  let workers = unwrap(cli.opt_int(argv, "--workers", 1))
  let target = path.norm(rest[0])

  let ts = unwrap(time.parse_iso8601_utc("2026-11-07T09:30:00Z"))
  let deadline = time.format_iso8601_utc(ts + 90)

  print("cmd={cmd} workers={workers} verbose={cli.has_flag(argv, \"--verbose\")}")
  print("target={target} stem={path.stem(target)} deadline={deadline}")
  return 0
}
```

```output
cmd=deploy workers=4 verbose=true
target=configs/prod.json stem=prod deadline=2026-11-07T09:31:30Z
```

---

## 10. Deterministic `par for` folds and bounded channels

`par for ... reduce(fn, init)` splits a vector across worker threads while
folding partial results in canonical index order, so multi-threaded floating or
string reductions produce identical output on 1 thread or 16. For pipeline
stages, `scope` + bounded `chan(cap)` enforces structured thread lifetimes:

```brat run
fn add_i64(a: i64, b: i64) -> i64 {
  return a + b
}

fn main() -> i64 {
  let mut xs = []
  for i in 1..101 {
    xs.push(i)
  }
  let sum_squares = par for x in xs reduce(add_i64, 0) {
    return x * x
  }
  print("sum_squares:", sum_squares)

  let ch = chan(4)
  scope {
    spawn {
      ch.send("chunk-1")
      ch.send("chunk-2")
      ch.close()
    }
  }
  let mut seen = []
  while true {
    let item = ch.recv()
    if is_none(item) {
      break
    }
    seen.push(item)
  }
  print("channel:", seen.join(", "))
  return 0
}
```

```output
sum_squares: 338350
channel: chunk-1, chunk-2
```
