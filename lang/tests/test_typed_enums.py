"""Conformance, borrow, arena, .bri, and LSP tests for Option[T], Result[T, E], and typed enums."""
import json
import os
import subprocess
import sys

import _cxx
import _scratch

NATIVE = _cxx.build()
TMP = _scratch.mkdtemp(prefix="brat-typed-enums-")
ENV = dict(os.environ, BRAT_FILES="1", BRAT_NO_NATIVE_CLI="1")

n = 0


def check(cond, msg):
    global n
    if not cond:
        raise AssertionError(msg)
    n += 1


def write_file(rel, content):
    p = os.path.join(TMP, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    return p


def run_brat(*args):
    return subprocess.run([NATIVE, *args], capture_output=True, text=True, timeout=120, env=ENV)


# 1. End-to-end execution: positional/named/untyped enum variants, Option[T], Result[T, E],
#    nested ADTs, lazy unwrap_or, method forms, and gradual untyped coexistence.
prog_ok = write_file("run/main.brat", """enum Expr {
  Lit(i64),
  Add(left: i64, right: i64),
  Note(tag: str),
  Empty
}

// Gradual untyped enum still works without mandatory annotations.
enum Loose {
  Bag(item),
  Blank
}

impl Expr {
  fn eval(self) -> i64 {
    match self {
      Lit(v) => return v,
      Add(a, b) => return a + b,
      Note(s) => return len(s),
      Empty => return 0
    }
  }
}

fn parse_pair(a: str, b: str) -> Result[i64, str] {
  let x: i64 = a.parse_int()?
  let y: i64 = b.parse_int()?
  return Ok(x + y)
}

fn forward_err(s: str) -> Result[vec[i64], str] {
  let r = s.parse_int()
  if is_err(r) {
    return r
  }
  return Ok([r.unwrap()])
}

fn find_even(xs: vec[i64]) -> Option[i64] {
  let mut found = None
  for x in xs {
    if x % 2 == 0 {
      found = Some(x)
    }
  }
  return found
}

fn nested_lookup(hit: bool) -> Result[Option[str], str] {
  if !hit {
    return Ok(None)
  }
  return Ok(Some("ready"))
}

fn bump(mut counter: vec[i64], v: i64) -> i64 {
  counter.push(v)
  return v
}

fn main() -> i64 {
  let e1: Expr = Lit(19)
  let e2: Expr = Add(10, 13)
  let e3: Expr = Note("brat")
  assert(e1.eval() + e2.eval() + e3.eval() + Empty.eval() == 46)

  let loose = Bag([1, 2, 3])
  match loose {
    Bag(xs) => assert(len(xs) == 3),
    Blank => assert(false)
  }

  let ok_r: Result[i64, str] = parse_pair("20", "22")
  let err_r: Result[i64, str] = parse_pair("20", "nope")
  assert(ok_r.is_ok() && !ok_r.is_err() && is_ok(ok_r))
  assert(err_r.is_err() && !err_r.is_ok() && is_err(err_r))
  assert(ok_r.unwrap() == 42)
  assert(err_r.unwrap_or(7) == 7)

  // Lazy unwrap_or: fallback must NOT run on Ok / Some.
  let mut side: vec[i64] = []
  assert(unwrap_or(ok_r, bump(side, 99)) == 42)
  assert(ok_r.unwrap_or(bump(side, 88)) == 42)
  assert(len(side) == 0)
  assert(unwrap_or(err_r, bump(side, 11)) == 11)
  assert(err_r.unwrap_or(bump(side, 12)) == 12)
  assert(side == [11, 12])

  // RC safety on heap strings in temporary + variable fallback unwrap_or.
  let fb_str = "fallback-" + "value"
  let s1: str = unwrap_or(Ok("hello-" + "world"), fb_str)
  let s2: str = unwrap_or(Err("bad-" + "input"), fb_str)
  let s3: str = Some("opt-" + "hit").unwrap_or(fb_str)
  let mv: Option[str] = None
  let s4: str = mv.unwrap_or(fb_str)
  assert(s1 == "hello-world" && s2 == "fallback-value")
  assert(s3 == "opt-hit" && s4 == "fallback-value" && fb_str == "fallback-value")

  let fw = forward_err("bad")
  assert(fw.is_err())
  assert(forward_err("9").unwrap() == [9])

  let opt_hit = find_even([1, 3, 8, 5])
  let opt_miss = find_even([1, 3, 5])
  assert(opt_hit.is_some() && !opt_hit.is_none() && is_some(opt_hit))
  assert(opt_miss.is_none() && !opt_miss.is_some() && is_none(opt_miss))
  if let Some(v) = opt_hit {
    let doubled: i64 = v * 2
    assert(doubled == 16)
  } else {
    assert(false)
  }

  let nl = nested_lookup(true).unwrap()
  assert(nl.unwrap_or("none") == "ready")
  assert(nested_lookup(false).unwrap().is_none())

  let items: vec[Option[str]] = [None, Some("alpha")]
  assert(items[0].is_none() && items[1].unwrap_or("") == "alpha")
  let res_list: vec[Result[i64, str]] = [Ok(1), Err("x")]
  assert(res_list[0].unwrap() == 1 && res_list[1].is_err())

  print("typed-enums-run-ok")
  return 0
}
""")

r = run_brat("run", prog_ok)
check(r.returncode == 0 and "typed-enums-run-ok" in r.stdout,
      f"run failed: rc={r.returncode} out={r.stdout} err={r.stderr}")

# Verify zero memory leaks / UB in emitted C code under ASan+UBSan(+LSan on Linux).
if sys.platform.startswith("linux"):
    c_out = write_file("run/main_out.c", "")
    bin_out = os.path.join(TMP, "run/main_asan_bin")
    rc_cc = run_brat("emit-c", prog_ok)
    check(rc_cc.returncode == 0, f"brat emit-c failed: {rc_cc.stderr}")
    with open(c_out, "w", encoding="utf-8") as cf:
        cf.write(rc_cc.stdout)
    cc_bin = os.environ.get("CC", "cc")
    eng_inc = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "engine"))
    comp = subprocess.run(
        [cc_bin, "-std=c11", "-O1", "-g1", "-fsanitize=address,undefined",
         "-I", eng_inc, c_out, "-lm", "-lpthread", "-o", bin_out],
        capture_output=True, text=True, timeout=120,
    )
    if comp.returncode == 0:
        asan_env = dict(ENV, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                        UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
        ar = subprocess.run([bin_out], capture_output=True, text=True, timeout=60, env=asan_env)
        check(ar.returncode == 0 and "typed-enums-run-ok" in ar.stdout,
              f"emitted C ASan/LSan failed: rc={ar.returncode} out={ar.stdout} err={ar.stderr}")


# 2. Static type errors on constructors, payloads, return types, and pattern arities.
def expect_check_err(name, src, needle):
    p = write_file(f"neg/{name}.brat", src)
    res = run_brat("check", p)
    combined = res.stdout + res.stderr
    check(res.returncode != 0 and needle in combined,
          f"{name}: expected failure containing {needle!r}, got rc={res.returncode}: {combined}")


expect_check_err(
    "ctor_field_type",
    """enum Msg { Code(i64), Quit }
fn main() -> i64 {
  let _m = Code("wrong")
  return 0
}
""",
    "arg 1 of `Code`: want i64, got str",
)

expect_check_err(
    "ctor_arity",
    """enum Msg { Pair(a: i64, b: i64) }
fn main() -> i64 {
  let _m = Pair(1)
  return 0
}
""",
    "`Pair` wants 2 args, got 1",
)

expect_check_err(
    "match_payload_type",
    """enum Msg { Text(str), Ping }
fn bad(m: Msg) -> i64 {
  match m {
    Text(n) => return n,
    Ping => return 0
  }
}
fn main() -> i64 { return 0 }
""",
    "return: want i64, got str",
)

expect_check_err(
    "quest_payload_type",
    """fn f(r: Result[str, str]) -> Result[i64, str] {
  let x: i64 = r?
  return Ok(x)
}
fn main() -> i64 { return 0 }
""",
    "annotated i64, got str",
)

expect_check_err(
    "err_narrow_mismatch",
    """fn f(r: Result[i64, i64]) -> Result[str, str] {
  if is_err(r) {
    return r
  }
  return Ok("ok")
}
fn main() -> i64 { return 0 }
""",
    "return: want Result[str, str]",
)

expect_check_err(
    "option_non_exhaustive",
    """fn f(o: Option[i64]) -> i64 {
  match o {
    Some(v) => return v
  }
  return 0
}
fn main() -> i64 { return 0 }
""",
    "non-exhaustive match",
)

expect_check_err(
    "result_guarded_not_exhaustive",
    """fn f(r: Result[i64, str]) -> i64 {
  match r {
    Ok(v) if v > 0 => return v,
    Err(_) => return 0
  }
  return 0
}
fn main() -> i64 { return 0 }
""",
    "non-exhaustive match",
)

expect_check_err(
    "variant_pat_arity",
    """enum Shape { Circle(r: i64), Dot }
fn f(s: Shape) -> i64 {
  match s {
    Circle(a, b) => return a + b,
    Dot => return 0
  }
}
fn main() -> i64 { return 0 }
""",
    "variant `Circle` of `Shape` wants 1 field(s), got 2",
)

expect_check_err(
    "method_arity",
    """fn f(r: Result[i64, str]) -> bool {
  return r.is_ok(1)
}
fn main() -> i64 { return 0 }
""",
    "is_ok",
)


# 3. Arena escape: returning enum variant wrapping arena string fails unless `own(...)` is used.
expect_check_err(
    "arena_enum_escape",
    """enum Boxed { Hold(str) }
fn leak() -> Boxed {
  arena {
    let s = "hello" + " arena"
    return Hold(s)
  }
}
fn main() -> i64 { return 0 }
""",
    "arena:",
)

arena_ok = write_file("arena/ok.brat", """enum Boxed { Hold(str) }
fn safe_make() -> Boxed {
  arena {
    let s = "hello" + " owned"
    return Hold(own(s))
  }
}
fn main() -> i64 {
  match safe_make() {
    Hold(s) => assert(s == "hello owned")
  }
  return 0
}
""")
r = run_brat("run", arena_ok)
check(r.returncode == 0, f"arena own(s) inside enum ctor failed: {r.stdout} {r.stderr}")


# 4. Borrow, Copy-slot unwrapping (`r?`), acyclic enum sharing, and `par for` `.unwrap()` effect.
borrow_copy_ok = write_file("borrow/copy_ok.brat", """fn check_copy(mut r: Result[i64, str]) -> Result[i64, str] {
  let mut x = r?
  r = Ok(99)
  x = x + 1
  return Ok(x + r.unwrap())
}
fn main() -> i64 {
  assert(check_copy(Ok(10)).unwrap() == 110)
  return 0
}
""")
r = run_brat("run", borrow_copy_ok)
check(r.returncode == 0, f"Copy payload r? borrow check failed: {r.stdout} {r.stderr}")

expect_check_err(
    "borrow_vec_payload",
    """fn bad_alias(mut r: Result[vec[i64], str]) -> Result[i64, str] {
  let mut xs = r?
  r = Ok([])
  xs.push(1)
  return Ok(len(xs))
}
fn main() -> i64 { return 0 }
""",
    "borrow",
)

expect_check_err(
    "parfor_method_unwrap",
    """fn main() -> i64 {
  let items: vec[Result[i64, str]] = [Ok(1), Ok(2)]
  let out = par for r in items {
    r.unwrap()
  }
  return len(out)
}
""",
    "par for",
)


# 5. Cross-module `pub enum`, private `enum` hiding, `.bri` contract verification, `api` & `doc`.
write_file("mod/shapes.brat", """/// 2D shape sum type.
pub enum Shape {
  Circle(r: i64),
  Rect(w: i64, h: i64),
  Dot
}

enum InternalOnly {
  Secret(i64)
}

pub fn area2(s: Shape) -> i64 {
  match s {
    Circle(r) => return 3 * r * r,
    Rect(w, h) => return w * h,
    Dot => return 1
  }
}
""")

write_file("mod/shapes.bri", """pub enum Shape {
  Circle(r: i64),
  Rect(w: i64, h: i64),
  Dot
}

fn area2(s: Shape) -> i64
""")

mod_app = write_file("mod/app.brat", """import shapes

fn describe(s: Shape) -> str {
  match s {
    Circle(r) => return "c:{r}",
    Rect(w, h) => return "r:{w}x{h}",
    Dot => return "dot"
  }
}

fn main() -> i64 {
  let a = shapes.Circle(4)
  let b = Rect(3, 5)
  let d = shapes.Dot
  assert(shapes.area2(a) == 48)
  assert(shapes.area2(b) == 15)
  assert(shapes.area2(d) == 1)
  assert(describe(b) == "r:3x5")
  assert(describe(d) == "dot")
  return 0
}
""")
r = run_brat("run", mod_app)
check(r.returncode == 0, f"cross-module pub enum run failed: {r.stdout} {r.stderr}")

# Private enum variants are rejected when constructed via module qualifier.
mod_priv = write_file("mod/priv_use.brat", """import shapes
fn main() -> i64 {
  let _x = shapes.Secret(1)
  return 0
}
""")
r = run_brat("check", mod_priv)
check(r.returncode != 0 and "is private in module" in (r.stdout + r.stderr),
      f"private enum variant not rejected: {r.stdout} {r.stderr}")

# Non-exhaustive match on imported `pub enum` is rejected!
mod_nonex = write_file("mod/nonex.brat", """import shapes
fn bad(s: Shape) -> i64 {
  match s {
    Circle(r) => return r
  }
  return 0
}
fn main() -> i64 { return 0 }
""")
r = run_brat("check", mod_nonex)
check(r.returncode != 0 and "non-exhaustive match: missing Rect" in (r.stdout + r.stderr),
      f"imported pub enum exhaustiveness failed: {r.stdout} {r.stderr}")

# Private enum constructor does not leak into importer.
mod_priv = write_file("mod/priv.brat", """import shapes
fn main() -> i64 {
  let _x = Secret(1)
  return 0
}
""")
r = run_brat("check", mod_priv)
check(r.returncode != 0, f"private enum leaked across module: {r.stdout} {r.stderr}")

# Mismatched variant type in .bri is rejected!
write_file("badbri/m.brat", """pub enum Token { Num(i64), Word(str) }
""")
write_file("badbri/m.bri", """pub enum Token { Num(str), Word(str) }
""")
badbri_app = write_file("badbri/app.brat", """import m
fn main() -> i64 { return 0 }
""")
r = run_brat("check", badbri_app)
check(r.returncode != 0 and "does not match its interface" in (r.stdout + r.stderr),
      f".bri enum mismatch not caught: {r.stdout} {r.stderr}")

# `brat doc` formats typed enum variants cleanly.
doc_out = write_file("mod/shapes.md", "")
r = run_brat("doc", os.path.join(TMP, "mod/shapes.brat"), "-o", doc_out)
check(r.returncode == 0, f"brat doc failed: {r.stdout} {r.stderr}")
doc_txt = open(doc_out, encoding="utf-8").read()
check("Circle(r: i64)" in doc_txt and "Rect(w: i64, h: i64)" in doc_txt,
      f"brat doc missing typed enum variants: {doc_txt}")


# 6. LSP verification: `.bri` enum parsing, variant hover, and Result/Option dot-completions.
def lsp_session(messages):
    payload = b""
    for m in messages:
        body = json.dumps(m).encode("utf-8")
        payload += f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body
    proc = subprocess.run([NATIVE, "lsp"], input=payload, capture_output=True, timeout=30, env=ENV)
    out = proc.stdout
    idx = 0
    frames = []
    while idx < len(out):
        hdr_end = out.find(b"\r\n\r\n", idx)
        if hdr_end < 0:
            break
        hdr = out[idx:hdr_end].decode("ascii", errors="replace")
        clen = 0
        for ln in hdr.split("\r\n"):
            if ln.lower().startswith("content-length:"):
                clen = int(ln.split(":", 1)[1].strip())
        body = out[hdr_end + 4:hdr_end + 4 + clen]
        frames.append(json.loads(body.decode("utf-8")))
        idx = hdr_end + 4 + clen
    return frames


bri_uri = "file://" + os.path.join(TMP, "mod/shapes.bri")
bri_text = open(os.path.join(TMP, "mod/shapes.bri"), encoding="utf-8").read()
app_uri = "file://" + os.path.join(TMP, "lsp_app.brat")
app_text = """enum Event {
  Click(x: i64, y: i64),
  Quit
}

fn demo(r: Result[i64, str]) -> i64 {
  let ev = Click(1, 2)
  r.
  return 0
}
"""

frames = lsp_session([
    {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": "file://" + TMP, "capabilities": {}}},
    {"jsonrpc": "2.0", "method": "initialized", "params": {}},
    {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": bri_uri, "languageId": "brat", "version": 1, "text": bri_text}
    }},
    {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": app_uri, "languageId": "brat", "version": 1, "text": app_text}
    }},
    {"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
        "textDocument": {"uri": app_uri}, "position": {"line": 6, "character": 13}
    }},
    {"jsonrpc": "2.0", "id": 3, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": app_uri}, "position": {"line": 7, "character": 4}
    }},
    {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None},
    {"jsonrpc": "2.0", "method": "exit", "params": None},
])

bri_diags = [f for f in frames if f.get("method") == "textDocument/publishDiagnostics"
             and f["params"]["uri"] == bri_uri]
check(bri_diags and bri_diags[-1]["params"]["diagnostics"] == [],
      f".bri with pub enum produced diagnostics: {bri_diags}")

hover_resp = next((f for f in frames if f.get("id") == 2), None)
hover_md = hover_resp["result"]["contents"]["value"] if hover_resp and hover_resp.get("result") else ""
check("Click(x: i64, y: i64) -> Event" in hover_md, f"variant hover mismatch: {hover_md!r}")

comp_resp = next((f for f in frames if f.get("id") == 3), None)
comp_labels = {it["label"] for it in (comp_resp.get("result") or [])}
check({"unwrap", "unwrap_or", "is_ok", "is_err"}.issubset(comp_labels),
      f"Result dot-completions missing methods: {comp_labels}")

print(f"ok: {n} typed enum / Option / Result checks passed")
