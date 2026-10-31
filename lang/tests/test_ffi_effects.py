"""FFI effects: what an `extern fn` promises about the C function behind it.

An effect is the only thing standing between a foreign call and the three
guarantees the language makes everywhere else -- that memory is not written by
two threads at once, that the optimizer may reuse a result, and that a failure
has one channel. Without effects the checkers treated every foreign call as
"touches nothing", so a `free` or a `memcpy` racing across a `spawn` was
invisible (docs/guide/ffi.md, LEDGER 2026-10-30). These cases pin down the
vocabulary, the refusals, and the fact that the two front ends agree on both.

Run: python3 test_ffi_effects.py
"""
import _cxx
import _scratch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(os.path.join(HERE, "..", "engine", "cli.py"))
CXX = None
TMP = _scratch.mkdtemp(prefix="brat-eff-")

MALLOC = "extern fn malloc(n: i64) -> move ptr\n"
FREE = "extern fn free(move p: ptr) -> none\n"


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)


def fail(name, msg):
    sys.stdout.write("FAIL %s: %s\n" % (name, msg))
    sys.stdout.flush()
    raise SystemExit(1)


def write(name, src):
    path = os.path.join(TMP, name + ".brat")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    return path


def errors(cmd):
    """The diagnostics that carry a message. Parse errors go to stderr, semantic
    ones to stdout -- `check` prints both, so both streams are read here; a
    helper that read only stdout would see a refusal and report no message."""
    p = sh(cmd)
    text = (p.stdout or "") + (p.stderr or "")
    return p.returncode, "\n".join(l for l in text.splitlines() if ": error: " in l)


def both_checkers(path):
    """(rc, messages) from each front end -- they must be identical, not similar."""
    return {"py": errors([sys.executable, CLI, "check", "--py", path]),
            "cxx": errors([CXX, "check", path])}


# A declaration that cannot mean what it says. Each is refused by the parser, so
# the message is the same in both front ends by construction: the tree the two
# parsers agree on is the tree that carries the effect list.
REFUSALS = {
    "pure_with_writes": "extern fn f(p: ptr) -> i64 pure writes(p)\n",
    "pure_with_move": "extern fn f(p: ptr) -> move ptr pure\n",
    "pure_with_errno": "extern fn f(n: i64) -> i64 errno pure\n",
    "reads_unknown_param": "extern fn f(p: ptr) -> i64 reads(q)\n",
    "writes_without_param": "extern fn f(p: ptr) -> i64 writes p\n",
    "reads_and_writes": "extern fn f(p: ptr) -> i64 reads(p) writes(p)\n",
    "twice": "extern fn f(n: i64) -> i64 pure pure\n",
    "effect_on_param": "extern fn f(p: ptr pure) -> i64\n",
    "writes_non_pointer": "extern fn f(n: i64) -> i64 writes(n)\n",
}

# A foreign write is a write. The pair differs only in whether the declaration
# says so, and the verdicts must differ with it, in both checkers.
RACES = {
    # `writes(p)` on the callee makes the parent's second call a second writer.
    "extern_write_race": ('''%s
extern fn my_poke(p: ptr, v: i64) -> none writes(p)

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    my_poke(p, 1)
  }
  my_poke(p, 2)
  join_all()
  free(move p)
  return 0
}
''' % (MALLOC + FREE), "`p` is written inside `spawn`"),
    # Same program, promise withheld: nothing in the tree says the call writes.
    "extern_no_effect_no_race": ('''%s
extern fn my_poke(p: ptr, v: i64) -> none

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    my_poke(p, 1)
  }
  my_poke(p, 2)
  join_all()
  free(move p)
  return 0
}
''' % (MALLOC + FREE), None),
    # A `pure` callee that only reads is a read on both sides: two readers, no race.
    "extern_pure_read_ok": ('''%s
extern fn my_peek(p: ptr) -> i64 pure reads(p)

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    print(my_peek(p))
  }
  print(my_peek(p))
  join_all()
  free(move p)
  return 0
}
''' % (MALLOC + FREE), None),
    # A declared write after `join_all()` is ordered, not racing.
    "extern_write_joined_ok": ('''%s
extern fn my_poke(p: ptr, v: i64) -> none writes(p)

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    my_poke(p, 1)
  }
  join_all()
  my_poke(p, 2)
  free(move p)
  return 0
}
''' % (MALLOC + FREE), None),
    # The write effect composes through a helper: the helper's own body does the
    # foreign write, and the caller inherits it.
    "extern_write_via_helper": ('''%s
extern fn my_poke(p: ptr, v: i64) -> none writes(p)

fn poke_one(mut p: ptr) -> i64 {
  my_poke(p, 1)
  return 0
}

fn main() -> i64 {
  let p = malloc(8)
  spawn {
    poke_one(p)
  }
  poke_one(p)
  join_all()
  free(move p)
  return 0
}
''' % (MALLOC + FREE), "`p` is written inside `spawn`"),
}


def check_str_effect_accepted():
    """`str` is a pointer to bytes, so `reads(s)` is a promise that can hold."""
    src = 'extern fn my_strlen(s: str) -> i64 pure reads(s)\nfn main() -> i64 { return 0 }\n'
    path = write("str_effect", src)
    seen = both_checkers(path)
    if len(set(seen.values())) != 1:
        fail("str_effect", "checkers disagree: %r" % (seen,))
    if seen["py"][0] != 0:
        fail("str_effect", "must be accepted, got:\n%s" % seen["py"][1])
    print("EFFECTS str ok (`reads(s)` on a `str` parameter)")


def check_dumps_agree():
    """The effect list is part of the tree, so both parsers must print it alike."""
    src = ('extern fn strlen(s: str) -> i64 pure nothrow\n'
           'extern fn read(fd: i64, buf: ptr, n: i64) -> i64 writes(buf)\n'
           'extern fn peek(p: ptr) -> i64 pure reads(p)\n'
           'extern fn plain(n: i64) -> i64\n')
    path = write("dumps", src)
    sys.path.insert(0, os.path.join(HERE, "..", "engine"))
    import _cxx as ref
    from brat.lexer import Cur, lex
    from brat.parser import parse_prog
    mine = ref.sx(parse_prog(Cur(lex(src))))
    theirs = sh([CXX, "ast", path]).stdout.strip()
    if mine != theirs:
        fail("dumps", "front ends disagree:\n  py : %s\n  cxx: %s" % (mine, theirs))
    # Source order is kept, not sorted: the tree is a transcript of the
    # declaration, and a checker that sorts would make two spellings of the same
    # promise two different trees.
    for want in ('("pure" "nothrow")', '("writes:buf")', '("pure" "reads:p")', '()'):
        if want not in mine:
            fail("dumps", "want %r in the tree, got %s" % (want, mine))
    print("EFFECTS dumps ok (both front ends print the list alike)")


def check_refusals():
    for name, src in sorted(REFUSALS.items()):
        path = write(name, src)
        seen = both_checkers(path)
        if len(set(seen.values())) != 1:
            fail(name, "checkers disagree: %r" % (seen,))
        rc, text = seen["py"]
        if rc == 0:
            fail(name, "must be refused, got 0 error(s)")
        if not text:
            fail(name, "refused with no message")
        if seen["cxx"][1] != text:
            fail(name, "messages differ:\n  py : %s\n  cxx: %s" % (text, seen["cxx"][1]))
    print("EFFECTS refusals ok (%d cases, identical messages)" % len(REFUSALS))


def check_races():
    for name, (src, want) in sorted(RACES.items()):
        path = write(name, src)
        seen = both_checkers(path)
        if len(set(seen.values())) != 1:
            fail(name, "checkers disagree: %r" % (seen,))
        rc, text = seen["py"]
        if want is None:
            if rc != 0:
                fail(name, "must be accepted, got:\n%s" % text)
        elif rc == 0 or want not in text:
            fail(name, "want %r in the diagnostic, got:\n%s" % (want, text))
    print("EFFECTS races ok (%d cases, both checkers)" % len(RACES))


def check_emitted_attribute():
    """The promise must reach C, or it is a comment the compiler cannot read."""
    sys.path.insert(0, os.path.join(HERE, "..", "engine"))
    import codegen_c
    cases = (("pure", "pure", "BRAT_ATTR_CONST"),
             ("pure_reads", "pure reads(p)", "BRAT_ATTR_PURE"),
             ("nothrow", "nothrow", "BRAT_ATTR_NOTHROW"),
             ("none", "", ""))
    for name, tail, want in cases:
        src = 'extern "libprobe.so" {\n  extern fn probe(p: ptr) -> i64 %s\n}\nfn main() { }\n' % tail
        text = codegen_c.emit(src, TMP)
        line = [l for l in text.splitlines() if " probe(" in l and "extern" in l]
        if not line:
            fail("attribute_" + name, "no prototype emitted for probe")
        got = line[0]
        if want and want not in got:
            fail("attribute_" + name, "want %s on the prototype, got: %s" % (want, got))
        if not want and "BRAT_ATTR" in got:
            fail("attribute_" + name, "no effect, no attribute, got: %s" % got)
    print("EFFECTS attributes ok (pure->const, pure reads->pure, nothrow)")


def check_lib_block_with_typed_return():
    """A documented example that did not parse: the return type ate the `}`.

    `extern "libm.so" { extern fn sin(x: f64) -> f64 }` is what docs/guide/ffi.md
    tells people to write, and the return-type scan ran past the closing brace
    into the next declaration, so the block form was unusable for any function
    that returns something. Both front ends had it; both stop at `}` now.
    """
    src = ('extern "libm.so" {\n  extern fn cos(x: f64) -> f64\n}\n'
           'fn main() -> i64 { return 0 }\n')
    path = write("libblock", src)
    seen = both_checkers(path)
    if len(set(seen.values())) != 1:
        fail("libblock", "checkers disagree: %r" % (seen,))
    if seen["py"][0] != 0:
        fail("libblock", "a lib block with a typed return must parse:\n%s" % seen["py"][1])
    print("EFFECTS lib block ok (typed return inside `extern \"lib\" { }`)")


def main():
    global CXX
    CXX = _cxx.build()
    check_dumps_agree()
    check_refusals()
    check_str_effect_accepted()
    check_races()
    check_emitted_attribute()
    check_lib_block_with_typed_return()
    print("FFI EFFECTS ALL GREEN")


if __name__ == "__main__":
    main()
