import importlib.util as _ilu
import os as _o
import re as _re
import subprocess as _sub
import unicodedata as _ud

_BRAT_PATH = _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), "brat.py")
_spec = _ilu.spec_from_file_location("brat_parser_full", _BRAT_PATH)
_brat = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_brat)

_EFF_PATH = _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), "effects.py")
_eff_spec = _ilu.spec_from_file_location("brat_effects", _EFF_PATH)
_eff = _ilu.module_from_spec(_eff_spec)
_eff_spec.loader.exec_module(_eff)


_FFI_MOD = None


def _ffi_mod():
    """`ffi.py`: the one owner of the target's data model.

    An `extern struct` is laid out by the target's rules, not the host's, so the
    emitter asks the module that already answers what a pointer or an `i64`
    weighs on a given triple.
    """
    global _FFI_MOD
    if _FFI_MOD is None:
        path = _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), "ffi.py")
        spec = _ilu.spec_from_file_location("brat_ffi_model", path)
        _FFI_MOD = _ilu.module_from_spec(spec)
        spec.loader.exec_module(_FFI_MOD)
    return _FFI_MOD


def set_ffi_target(triple):
    """Lay `extern struct`s out by `triple`; None means the host."""
    return _ffi_mod().set_target(triple)


def _ffi_model_env():
    """The numbers the C++ emitter needs, so it never parses a triple itself."""
    ffi = _ffi_mod()
    return {"BRAT_FFI_PTR": str(ffi.ptr_width()), "BRAT_FFI_A8": str(ffi.wide_align())}


def _typer_abi():
    """The typer's ABI helpers, so emission and `check` quote one rule.

    Loaded the way everything else here is: by path, from this file's own
    directory, because the engine is run as a script tree, not a package.
    """
    path = _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), "typer.py")
    spec = _ilu.spec_from_file_location("brat_typer_abi", path)
    mod = _ilu.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod._abi_type, mod._cb_signature

_C_KW = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "int", "long", "register", "return", "short", "signed", "sizeof",
    "static", "struct", "switch", "typedef", "union", "unsigned", "void",
    "volatile", "while",
}

_STRVEC_METH = ("split", "lines", "trim", "lower", "upper", "starts_with",
                "ends_with", "contains", "parse_int", "replace",
                "index_of", "repeat", "split_once", "chars", "parse_float",
                "join", "push", "pop", "first", "last", "sort", "rev",
                "ins", "del", "slice", "sum", "min", "max", "enumerate",
                "copy", "zip", "chunks")

_SV_SIMPLE = {
    "index_of": ("vindex_of", 1), "repeat": ("vrepeat", 1),
    "split_once": ("vsplit_once", 1), "ins": ("vins", 2),
    "sum": ("vsum", 0), "min": ("vmin", 0),
    "max": ("vmax", 0), "enumerate": ("venum", 0), "chars": ("vchars", 0),
    "parse_float": ("vparse_float", 0), "copy": ("vcopy", 0),
    "zip": ("vzip", 1), "chunks": ("vchunks", 1),
}

_VBUILTIN_DIRECT = ("del", "copy", "slice")

_MAP_METH = ("get", "has", "keys", "del", "set", "items", "vals", "copy", "get_or")


def _refs(node, acc=None):
    """Every name a statement list mentions as a value.

    `spawn` copies the frame: the child gets the value each name had at the
    launch, not a window onto the parent's binding. The set of names to copy is
    therefore "what the body mentions", not "what the parent has already
    assigned" -- the old rule missed a name the parent assigned *after* the
    launch, so the child read the script-level global directly, and the two
    backends then disagreed about a plain `spawn { print(x) }` (see LEDGER,
    2026-10-30).
    """
    if acc is None:
        acc = set()
    stack = [node]
    while stack:
        cur = stack.pop()
        if isinstance(cur, tuple) and cur:
            if cur[0] == "var" and len(cur) > 1 and isinstance(cur[1], str):
                acc.add(cur[1].split(".", 1)[0])
                continue
            if cur[0] == "set" and len(cur) > 1 and isinstance(cur[1], str):
                acc.add(cur[1].split(".", 1)[0])
                stack.extend(x for x in cur[2:] if isinstance(x, (tuple, list)))
                continue
            stack.extend(x for x in cur[1:] if isinstance(x, (tuple, list)))
        elif isinstance(cur, list):
            stack.extend(cur)
    return acc


def _bound_in(body):
    """Names a body binds itself: params are separate, these shadow a global."""
    out = set()
    stack = [body]
    while stack:
        node = stack.pop()
        if isinstance(node, list):
            stack.extend(node)
            continue
        if not isinstance(node, tuple) or not node:
            continue
        if node[0] in ("let", "forrange", "foriter") and len(node) > 1 and isinstance(node[1], str):
            out.add(node[1])
        for part in node[1:]:
            if isinstance(part, (list, tuple)):
                stack.append(part)
    return out


def _names_in(node, out=None):
    """Every name a body reads or binds, iteratively -- the tree can be deep."""
    out = set() if out is None else out
    stack = [node]
    while stack:
        n = stack.pop()
        if isinstance(n, tuple):
            if n == () :
                continue
            if n[0] == "var" and len(n) > 1 and isinstance(n[1], str):
                out.add(n[1])
            for part in n[1:]:
                stack.append(part)
        elif isinstance(n, list):
            stack.extend(n)
    return out


def _collect_vars(body, acc):
    for s in body:
        if not isinstance(s, tuple) or not s:
            continue
        k = s[0]
        if k == "let":
            acc.add(s[1].split(".", 1)[0])
        elif k == "set":
            acc.add(s[1].split(".", 1)[0])
        elif k == "foriter":
            acc.add(s[1])
            _collect_vars(s[3], acc)
        elif k == "forrange":
            acc.add(s[1])
            _collect_vars(s[4], acc)
        elif k == "if":
            _collect_vars(s[2], acc)
            els = s[3]
            if isinstance(els, tuple):
                _collect_vars([els], acc)
            elif els:
                _collect_vars(els, acc)
        elif k == "while":
            _collect_vars(s[2], acc)
        elif k == "try":
            _collect_vars(s[1], acc)
            _collect_vars(s[2], acc)
            if len(s) > 3 and s[3]:
                acc.add(s[3])
        elif k in ("spawn", "scope"):
            _collect_vars(s[1], acc)
        elif k in ("fn", "impl", "trait", "enum", "pub", "import", "unsafe"):
            if k == "unsafe":
                _collect_vars(s[1], acc)
            pass
        elif k in ("expr", "return", "defer", "break", "continue", "setidx", "setfield"):
            pass
        else:
            raise RuntimeError("stmt " + k)


def _block_let_names(body):
    """Names bound by a top-level `let` in `body` (block-scoped bindings)."""
    out = []
    for s in body:
        if isinstance(s, tuple) and s and s[0] == "let":
            out.append(s[1].split(".", 1)[0])
    return out


def _expr_set_vars(e, acc):
    """Names an expression assigns to, so a defer can write back a local."""
    if not isinstance(e, tuple) or not e:
        return
    if e[0] == "set" and len(e) > 1 and isinstance(e[1], str):
        acc.add(e[1].split(".", 1)[0])
    for x in e[1:]:
        if isinstance(x, tuple):
            _expr_set_vars(x, acc)
        elif isinstance(x, list):
            for y in x:
                _expr_set_vars(y, acc)


def _expr_vars(e, acc):
    """Every name an expression reads, so a defer body can be captured whole.

    A defer runs after the statement that follows it in the block may already
    have declared locals, and its closure is emitted against the scope of the
    defer site, so the names it reads have to be in the capture set.
    """
    if not isinstance(e, tuple) or not e:
        return
    if e[0] == "var":
        acc.add(e[1])
    for x in e[1:]:
        if isinstance(x, tuple):
            _expr_vars(x, acc)
        elif isinstance(x, list):
            for y in x:
                _expr_vars(y, acc)


# Methods whose result is a heap value the caller owns. `parse_int` and
# `parse_float` answer with an `Ok`/`Err` tag, which is as much a heap value as a
# string: left out, `print("8080".parse_int()?)` dropped the payload and kept the
# tag, 152 bytes a run.
# Methods that answer with a value of their own. `sort` and `sort_by` sort in
# place and hand the container back, which makes the call the only holder of a
# container somebody built for it: `for k in m.keys().sort()` leaked the key
# vector, because the expression looked like an alias of a thing that had no
# other name.
_HEAP_METHODS = {"upper", "lower", "trim", "replace", "repeat", "copy", "join",
                 "split", "split_once", "lines", "chars", "slice", "map", "filter",
                 "keys", "vals", "items", "entries", "values", "recv",
                 "parse_int", "parse_float", "rev"}

# Methods that mutate the receiver and hand it back. The result is owned exactly
# when the receiver was: `m.keys().sort()` is a container with no other name,
# `s.sort()` is `s`, and dropping that freed the binding.
_SELF_RESULT = frozenset({"sort", "sort_by"})


def _parse_sig(node):
    """`"i64(i64,f64)"` -> ("i64", ["i64", "f64"]); raises when the literal is malformed."""
    if not (isinstance(node, tuple) and node and node[0] == "str"):
        raise SyntaxError("call: signature must be a string literal")
    text = "".join(node[1].replace(" ", ""))
    if "(" not in text or not text.endswith(")"):
        raise SyntaxError("call: bad signature `%s`" % text)
    ret_t, rest = text.split("(", 1)
    params = [p for p in rest[:-1].split(",") if p]
    return ret_t, params


# Builtins that hand back a private heap value.
_HEAP_CALLS = ("str", "own", "str_from_ptr", "args", "read_file", "write_file",
               "tcp_listen", "tcp_connect", "tcp_accept", "tcp_recv", "sbuf",
               "sbuf_str", "chan")


def _returns_param(body, params):
    """True when some `return` in this body hands back a caller's storage.

    `fn me(b: Box) -> Box { return b }` looks like a fresh `Box` by its type, but
    the value belongs to the caller. Treating it as fresh is how a caller came to
    hold a value nothing owned.
    """
    out = [False]

    def walk(stmts):
        for st in stmts:
            if not isinstance(st, tuple) or not st or out[0]:
                continue
            k = st[0]
            if k == "return" and len(st) > 1:
                e = st[1]
                while isinstance(e, tuple) and e and e[0] in ("field", "idx", "move"):
                    e = e[1]
                if isinstance(e, tuple) and e and e[0] == "var" and e[1] in params:
                    out[0] = True
                    return
            for sub in st[1:]:
                if isinstance(sub, list) and sub and isinstance(sub[0], tuple):
                    walk(sub)
                elif isinstance(sub, tuple) and sub and sub[0] not in (
                        "var", "num", "str", "bool", "bin", "call", "mcall", "none"):
                    walk([sub])

    walk(body)
    return out[0]


def _container_tmp(val):
    """The C temp holding a container literal, from `vvecv(_v1)` / `vstruct(_m2)`."""
    for prefix in ("vvecv(", "vstruct("):
        if val.startswith(prefix) and val.endswith(")"):
            return val[len(prefix):-1]
    return None


def _owned_temp(node, val):
    """The C temp that owns a value this statement built, from its operand.

    Only a call: a container literal's temp is a `Vec *`, released by the code
    that built it, and `vdrop_temp` takes a `Val`.
    """
    if not isinstance(node, tuple) or not node:
        return None
    if node[0] in ("call", "mcall", "closure"):
        return val if val.startswith("_") else None
    # `"a" + "b" == "ab"`: the `+` binds its result to a temp, and the comparison
    # that consumes it is the end of its life -- nobody else names it. Only `+`
    # here: `-`, `*`, `/` and `%` answer with numbers, and a number is no heap
    # value to release (the drop is a no-op, but saying so in the table would
    # hide that the operand was a scalar).
    if node[0] == "bin" and len(node) > 1 and node[1] == "+":
        return val if isinstance(val, str) and val.startswith("_") else None
    return None


# Methods that build a new value instead of answering with a part of the receiver.
# The emitter releases a temporary receiver only for these.
# Methods that answer with a part of the receiver (an element). Such a result is
# kept alive by a reference before the receiver is released.
_PART_RESULT = frozenset({"min", "max", "first", "last", "pop", "get"})

_FRESH_RESULT = frozenset({
    "split", "split_once", "join", "upper", "lower", "trim", "replace", "repeat",
    "chars", "lines", "zip", "chunks", "enumerate", "copy", "keys", "items",
    "vals", "sum", "index_of", "starts_with", "ends_with", "contains", "has",
    "parse_int", "parse_float", "rev",
})


# Operands of a binary operator that never need a temp of their own: an
# immediate, a string literal (immortal), a plain binding (borrowed), and a
# container literal, whose `Vec *` temp the emitter releases through `vrc_dec`.
_NOT_OWNED_HERE = frozenset({"num", "bool", "none", "str", "rstr", "var",
                             "vec", "maplit", "struct", "move"})


def _try_let_names(body, acc=None):
    """Every `let` name declared inside a try body, at any depth.

    Those bindings are released at the end of their block, and the catch path
    jumps over that code: one of them still holds the error value, which the
    error channel frees, so the exit cleanup would read freed memory. Clearing
    them at the catch label is enough -- a block-scoped `let` cannot be live in
    the handler.
    """
    if acc is None:
        acc = set()
    for st in body:
        if not isinstance(st, tuple) or not st:
            continue
        k = st[0]
        if k == "let":
            acc.add(st[1].split(".", 1)[0])
        elif k == "if":
            _try_let_names(st[2], acc)
            els = st[3]
            if isinstance(els, list):
                _try_let_names(els, acc)
            elif isinstance(els, tuple):
                _try_let_names([els], acc)
        elif k in ("while",):
            _try_let_names(st[2], acc)
        elif k in ("foriter", "forrange"):
            acc.add(st[1])
            _try_let_names(st[-1], acc)
        elif k == "try":
            _try_let_names(st[1], acc)
            _try_let_names(st[2], acc)
    return acc


def _may_raise(e):
    """True when evaluating `e` can raise: it calls something, or it divides."""
    if isinstance(e, list):
        return any(_may_raise(sub) for sub in e)
    if not isinstance(e, tuple) or not e:
        return False
    if e[0] in ("call", "mcall"):
        return True
    if e[0] == "bin" and e[1] in ("/", "%"):
        return True
    return any(_may_raise(sub) for sub in e[1:])


def _stmt_returns_owned(st, heap, saw):
    """Does every `return` this statement contains hand back an owned value?

    `saw` records that the function returns at all. A branch or a loop with no
    `return` inside is not a counter-example: the walk answers for the `return`s
    it finds, not for the shape of the branch, which is why the body walks share
    one `saw` instead of each starting a fresh one.
    """"""
    """
    if not isinstance(st, tuple) or not st:
        return True
    if st[0] == "return":
        if len(st) < 2:
            return True
        # A function that answers with a number hands back nothing anybody has to
        # release; counting it as a heap return only made the caller bind a temp
        # for a value `vdrop_temp` ignores.
        val = st[1]
        if isinstance(val, tuple) and val and val[0] in ("num", "bool"):
            return True
        if isinstance(val, tuple) and val and val[0] == "var":
            # `return x` for a binding is a transfer: the exit drops skip it, and
            # a parameter or a script-level name is counted on the way out, so the
            # caller holds a reference it may release. Without this a function
            # that answers with its accumulator was not in `heap_returns`, so the
            # caller never dropped the result: `for seg in p.split("/")` plus
            # `out = out + seg` leaked one string per call.
            saw[0] = True
            return True
        saw[0] = True
        return _dec_able(val, heap)
    if st[0] == "block":
        return _block_returns_owned(st[1], heap, saw)
    if st[0] == "if":
        ok = _block_returns_owned(st[2], heap, saw)
        if len(st) > 3 and st[3]:
            els = st[3]
            if isinstance(els, list):
                ok = ok and _block_returns_owned(els, heap, saw)
            else:
                ok = ok and _stmt_returns_owned(els, heap, saw)
        return ok
    if st[0] == "try":
        # What the handler returns says nothing about the value the function
        # hands back on its normal path.
        return _block_returns_owned(st[1], heap, saw)
    if st[0] in ("while", "forrange", "foriter", "spawn", "scope", "unsafe",
                 "defer"):
        return _block_returns_owned(st[-1], heap, saw)
    return True


def _block_returns_owned(body, heap, saw):
    for st in body or []:
        if not _stmt_returns_owned(st, heap, saw):
            return False
    return True


def _body_returns_owned(body, heap):
    saw = [False]
    return _block_returns_owned(body, heap, saw) and saw[0]


def _is_index_read(e):
    """`a[i]` and its desugared `__at(a, i)` form: both yield an owned reference."""
    if not isinstance(e, tuple) or not e:
        return False
    if e[0] == "idx":
        return True
    return e[0] == "call" and e[1] == "__at" and len(e) == 3


def _dec_able(e, heap=()):
    """True when a value from `e` survives vrc_dec: a fresh heap value or an
    immediate.  A string literal counts because the emitter stores it heap-backed.

    A call counts when the callee returns a heap type (`heap`), because the
    emitter shares every argument before the call, so the result carries its own
    reference and releasing it here cannot free a caller's value.
    """
    if not isinstance(e, tuple) or not e:
        return False
    if e[0] in ("str", "rstr", "num", "bool", "none", "bin", "vec", "maplit",
                "struct", "closure", "or"):
        return True
    # `x?` yields a reference the caller owns: the runtime takes one out of the
    # tag and frees the tag's argument vector when it is the only owner.
    if e[0] == "quest":
        return True
    if e[0] == "call":
        # `unwrap` hands back a reference the caller owns (see vtag_payload),
        # and `Ok(x)`/`Err(x)`/`Some(x)` build a fresh tag on the spot.
        # `__at` is the desugared `a[i]`, which hands back an owned reference;
        # `Ok`/`Err`/`Some` build a fresh tag on the spot.
        if e[1] in ("unwrap", "Ok", "Err", "Some", "__at"):
            return True
        return isinstance(e[1], str) and (e[1] in heap or e[1] in _HEAP_CALLS)
    if e[0] == "dotted" and len(e) >= 2 and isinstance(e[1], str):
        return e[1].rsplit(".", 1)[-1] in _HEAP_METHODS or e[1] in heap
    if e[0] == "idx":
        # `a[i]` hands back a reference the holder owns (see vat_owned).
        return True
    if e[0] == "mcall":
        meth = e[2] if len(e) > 2 else None
        if isinstance(meth, str) and meth in _SELF_RESULT:
            return _dec_able(e[1], heap)
        if isinstance(meth, str) and meth in _PART_RESULT:
            # A part lifted out of a temporary receiver is handed over by
            # vdrop_recv_part, so the holder already owns that reference.
            return len(e) > 3 and _dec_able(e[1], heap)
        # `_FRESH_RESULT` and `_HEAP_METHODS` answer the same question from two
        # directions -- which method builds a value of its own -- and a method
        # missing from both was treated as an alias and never released (`rev`
        # was). Asked of the union, so a name only has to be in one of them.
        return isinstance(meth, str) and (meth in _HEAP_METHODS or meth in _FRESH_RESULT
                                          or meth in heap)
    return e[0] == "move" and _dec_able(e[1], heap)


def _reclaim_names(body, params, heap=()):
    """Return (reclaim, relet) for a statement list.
    reclaim: locals rebound by `set`, whose every stored value survives
    vrc_dec -- a rebind releases the overwritten value.
    relet: locals declared under a loop, so the `let` itself re-runs each
    pass and must release the previous pass value before overwriting.
    Parameters never qualify: their storage belongs to the caller."""
    ok = {}
    has_set = set()
    loop_let = set()

    def walk(stmts, loop):
        for s in stmts:
            if not isinstance(s, tuple) or not s:
                continue
            k = s[0]
            if k == "let":
                n = s[1].split(".", 1)[0]
                ok[n] = ok.get(n, True) and _dec_able(s[2], heap)
                if loop:
                    loop_let.add(n)
            elif k == "set":
                n = s[1].split(".", 1)[0]
                ok[n] = ok.get(n, True) and _dec_able(s[2], heap)
                has_set.add(n)
            elif k == "foriter":
                ok[s[1]] = False  # elements come from the container
                walk(s[3], True)
            elif k == "forrange":
                ok[s[1]] = False
                walk(s[4], True)
            elif k == "if":
                walk(s[2], loop)
                els = s[3]
                if isinstance(els, tuple):
                    walk([els], loop)
                elif els:
                    walk(els, loop)
            elif k == "while":
                walk(s[2], True)
            elif k == "try":
                walk(s[1], loop)
                walk(s[2], loop)
                if len(s) > 3 and s[3]:
                    ok[s[3]] = False
            # a spawn body is its own frame, like a closure

    walk(body, False)
    reclaim = set(n for n, v in ok.items() if v and n in has_set and n not in params)
    relet = set(n for n in loop_let if ok.get(n) and n not in params)
    return reclaim, relet


def _block_defers(body):
    """True when this block level itself holds a `defer`.

    Deliberately does not recurse: a `defer` in a nested block is drained by
    that inner block, so the outer one needs no frame of its own. A `defer` in
    a nested `fn` belongs to that function's frame, not to this block's.
    """
    for s in body:
        if isinstance(s, tuple) and s and s[0] == "defer":
            return True
    return False


def _name_path(e):
    """Dotted name of a var/field chain, or None when `e` computes a value."""
    if isinstance(e, tuple) and e:
        if e[0] == "var":
            return e[1]
        if e[0] == "field":
            b = _name_path(e[1])
            return b + "." + e[2] if b else None
    return None


def _cname(n):
    n = n.replace(".", "_")
    n = "".join(c if (c.isalnum() or c == "_") else "_" for c in n)
    if not n or n[0].isdigit():
        n = "_" + n
    if n in _C_KW:
        n += "_"
    return "b_" + n


def _ffi_c(n):
    """C name of an FFI struct or member.

    The source name is used as written because the struct layout must match the
    library's, but a C keyword or a name the runtime already declares (`Val`)
    cannot be spelled in generated C, so every such name gets the same prefix.
    `b_` names are all taken by _cname, so the prefix cannot meet one.
    """
    return "bf_" + n


# Argument types the native backend accepts (structs and fn* are checked separately).
def _out_ctype(width):
    """The C type an out-parameter is written through."""
    return {1: "unsigned char", 2: "unsigned short", 4: "int"}.get(width, "int64_t")


# One table for the whole FFI boundary: BRAT width -> C spelling, and the range
# the value must sit in before a narrower C type may see it. Two ladders that
# disagree (a declaration saying `int64_t`, a call site saying `short`) is how an
# ABI bug is born: it compiles, links and corrupts the next stack slot. Widths
# the emitter does not know are refused at emit time instead of falling back to
# a machine word -- a silent `int64_t` for `i16` is the exact bug this table
# exists to make impossible.
_FFI_C = {
    "bool": ("_Bool", None, None),
    "i8": ("signed char", "-128LL", "127LL"),
    "u8": ("unsigned char", "0LL", "255LL"),
    "i16": ("short", "-32768LL", "32767LL"),
    "u16": ("unsigned short", "0LL", "65535LL"),
    "i32": ("int", "-2147483648LL", "2147483647LL"),
    "u32": ("unsigned", "0LL", "4294967295LL"),
    "i64": ("int64_t", None, None),
    "u64": ("uint64_t", "0LL", "9223372036854775807LL"),
    "usize": ("size_t", "0LL", "9223372036854775807LL"),
    "f32": ("float", None, None),
    "f64": ("double", None, None),
    "str": ("const char *", None, None),
    "ptr": ("void *", None, None),
}

_FFI_TYPES = set(_FFI_C)

# Integer FFI types checked at the boundary; BRAT ints are i64.
_FFI_RANGE = {k: (lo, hi) for k, (_c, lo, hi) in _FFI_C.items() if lo is not None}

# Integer widths whose C result widens to `int64_t` on the way back.
_FFI_INT_RESULT = {"i8", "u8", "i16", "u16", "i32", "u32", "i64"}


def _ffi_ctype(ann):
    """C spelling of a scalar FFI type, or None when it is not one."""
    return _FFI_C[ann][0] if ann in _FFI_C else None


def ann_is_ptr(ann):
    """A raw pointer in any spelling: `ptr`, `ptr[T]`, or a callback slot."""
    return isinstance(ann, str) and (ann == "ptr" or ann.startswith("ptr[") or ann.startswith("fn"))


class Sym:
    """One BRAT binding as seen by the C emitter.

    `cn` is the C identifier holding the value. `own` classifies the binding:
    * "copy"   -- trivially copyable / immortal (scalars, string literals),
                  never dropped.
    * "borrow" -- aliases another binding's storage; never dropped, the
                  owner drops instead.
    * "owned"  -- the binding owns a heap value; dropped once at its death
                  point (or block end).
    `moved` is set when `move` reads the source, so a moved binding is never
    dropped (ownership has been transferred).
    """
    __slots__ = ("cn", "own", "moved", "ptr", "elem")

    def __init__(self, cn, own):
        self.cn = cn
        # The element type of a `ptr[T]` binding: `None` for a plain `ptr`, the
        # struct or scalar name otherwise. Dereferencing reads it through this,
        # so `p[0].field` needs no second argument and no string type name.
        self.ptr = None
        # The element of a `slice[T]` binding: a window over C records has to
        # know its layout to hand back a struct; a window over built elements
        # already holds values.
        self.elem = None
        self.own = own
        self.moved = False


# Ownership is the default: a program releases what it built. BRAT_MOVE_RT=0
# turns the drops off, for bisecting an emitter change against older output.
_MOVE_RT = _o.environ.get("BRAT_MOVE_RT", "1") != "0"


def _cstr_path(p):
    """C string literal for a filesystem path.

    Only the two characters that can end the literal get escaped: the backslash
    first, so a path holding `\n` does not reach C as a newline escape, then the
    quote. Bytes above 0x7F are left alone -- a UTF-8 path is valid inside a C
    literal as long as the compiler reads the file as UTF-8, which `_clit`'s
    hex escapes cannot be used for here without changing the bytes.
    """
    return p.replace("\\", "\\\\").replace('"', '\\"')


def _clit(s):
    o = []
    for ch in s:
        if ch == "\\":
            o.append("\\\\")
        elif ch == "\"":
            o.append("\\\"")
        elif ch == "\n":
            o.append("\\n")
        elif ch == "\r":
            o.append("\\r")
        elif ch == "\t":
            o.append("\\t")
        elif ch == "?":
            # A trigraph: under strict ISO C `??/` is a backslash, so a string
            # holding `??/` would not mean what it says. Adjacent literals are
            # one literal, so splitting the character keeps its bytes and stops
            # the pair from forming.
            o.append('?""')
        elif 32 <= ord(ch) < 127:
            o.append(ch)
        else:
            for b in ch.encode("utf-8"):
                o.append("\\x%02x" % b)
            o.append("\"\"")
    return "".join(o)


_INTERP_RE = _re.compile(r'\{([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\}')

BINT_FMT = "%lld"
_I64 = 1 << 63
_INF = float("inf")


def _cf64(v):
    """A C99 double literal for `v`, including the values Python prints as
    `inf`/`-inf`/`nan`: `vfloat(inf)` is not C, and the C compiler then fails
    on a valid BRAT program. `INFINITY`/`NAN` come from <math.h>, which `crt.h`
    already includes."""
    v = float(v)
    if v != v:
        return "NAN"
    if v == _INF:
        return "INFINITY"
    if v == -_INF:
        return "-INFINITY"
    return repr(v)


def _wrap64(n):
    """i64 wrap, matching brat/runtime.wrap64 (SPEC section 10, Rust release)."""
    if -_I64 <= n < _I64:
        return n
    n &= (1 << 64) - 1
    return n - (1 << 64) if n >= _I64 else n


def _clit64(n):
    """A C literal for an i64 value.

    `-9223372036854775808` is not a valid C expression: the constant is parsed
    as an unsigned long long and the unary minus is applied to it, so gcc warns
    ("integer constant is so large that it is unsigned") and the value is
    implementation-defined under `-Werror`. INT64_MIN is spelled the way the C
    standard does: `(-9223372036854775807LL - 1LL)`.
    """
    if n == -(1 << 63):
        return "(-9223372036854775807LL - 1LL)"
    return "(%dLL)" % n


def _split_interp(s):
    parts = []
    i = 0
    buf = ""
    while i < len(s):
        if s[i] == "{":
            m = _INTERP_RE.match(s[i:])
            if m:
                if buf:
                    parts.append(("lit", buf))
                    buf = ""
                parts.append(("var", m.group(1)))
                i += len(m.group(0))
                continue
        buf += s[i]
        i += 1
    if buf:
        parts.append(("lit", buf))
    return parts


class Emitter:
    def __init__(self):
        # Names declared `extern packed struct`: emitted with the attribute, so
        # a record read out of a buffer has the layout the header promised.
        self.packed_structs = set()
        self.tmp = 0
        self.fns = {}
        self.order = []
        self.mtab = {}
        self.vtab = {}
        self.traits = {}
        self.externs = {}
        self.errno_externs = set()   # externs declared `-> ... errno`
        self.structs = {}
        self.enums = set()
        self.exported = set()
        self.fn_ret = {}
        self.codes = {}
        self.user_meths = set()
        self.clos = []
        self.late = []
        self.top_exe = []
        self.global_lets = set()
        self.out = []
        self.sym = {}
        self.cap = None
        self.dfb = []
        self.dfb_locals = set()
        self.try_n = 0
        self.alias_ret = set()  # functions that return a parameter
        self.cur_params = set()  # parameters of the function being emitted
        self.scopes = []        # scope handles to join on every exit path
        # For each loop, remember the lexical scope/defer depths at its body
        # boundary. `break`/`continue` must unwind scopes and defers entered
        # inside that loop body before transferring control.
        self.loop_exit_depths = []
        self.fin = "__fin_top"
        self.err_goto = None  # catch label while inside try; else None → use fin
        self.err_base = None  # defer frame to drain before that catch label
        self.fast = {}
        self.fastc = {}
        self.fastkinds = {}
        self.fkind = "int64_t"
        self.fsym = {}
        self.need_idiv = False
        self.need_imod = False
        self.need_fdiv = False
        self.need_fmod = False
        self.need_viat = False
        self.need_vset = False
        self.fastvec = {}
        self.fvecvars = frozenset()
        self.nesting = 0
        self.nested_seq = 0
        self.fn_alias = {}
        self.assigned = set()
        self.reclaim = set()    # bindings safe to release on rebind
        self.relet = set()      # loop-declared bindings a re-run `let` must release
        self.move_rt = _MOVE_RT
        # Ownership tracking (only active under BRAT_MOVE_RT).
        self.moved = set()      # bindings whose ownership was moved away
        self.captured = set()   # bindings captured by a closure/spawn/defer
        self.shared = set()     # bindings borrowed by another binding
        self.dropped = set()    # bindings already vdrop'ped (avoid double drop)
        self.alias_temps = set()  # `@`-temps that took a counted alias of a binding

    def new_tmp(self, p="t"):
        # Temps are plain `Val`, never `volatile`: a raise sets a thread-local
        # flag and the frame leaves by `goto`, so no longjmp can cross them, and
        # volatile cost a fifth of the time in a dynamic loop by pinning every
        # temporary to the stack.
        self.tmp += 1
        return "_%s%d" % (p, self.tmp)

    def err_bail(self):
        """Leave the current frame after a raise (no longjmp)."""
        if self.err_goto is not None:
            pre = ("vdfreset(%s); " % self.err_base) if self.err_base else ""
            return "{ %sgoto %s; }" % (pre, self.err_goto)
        if self.fin == "__fin_top":
            return (
                '{ fprintf(stderr, "unhandled: "); vprint_err(verr_take()); '
                'fprintf(stderr, "\\n"); return 1; }'
            )
        return "{ __ret = verr_leave(); goto %s; }" % self.fin

    def err_check_line(self, drops=()):
        """One C statement: if a raise is pending, leave this frame.

        `drops` are temps this statement built and did not hand to anything: on the
        way to the catch their owner is gone, so they are released here.
        """
        rel = "".join("vdrop_temp(%s); " % d for d in drops)
        if self.err_goto is not None:
            if self.err_base:
                return "if (verr_pending()) { %svdfreset(%s); goto %s; }" % (
                    rel, self.err_base, self.err_goto)
            return "if (verr_pending()) { %sgoto %s; }" % (rel, self.err_goto)
        if self.fin == "__fin_top":
            return (
                'if (verr_pending()) { %sfprintf(stderr, "unhandled: "); '
                'vprint_err(verr_take()); fprintf(stderr, "\\n"); return 1; }' % rel
            )
        return (
            "if (verr_pending()) { %s__ret = verr_leave(); goto %s; }" % (rel, self.fin)
        )

    def emit_err_check(self, ind, drops=()):
        """Propagate a pending runtime error without longjmp."""
        self.out.append(ind + self.err_check_line(drops))

    def int_expr(self, e, cand, kind, intvars, vecvars=frozenset()):
        if not isinstance(e, tuple) or not e:
            return False
        k = e[0]
        if k == "num":
            if isinstance(e[1], bool):
                return True
            if isinstance(e[1], int):
                return True
            return kind == "f64"
        if k == "bool":
            return True
        if k == "var":
            return e[1] not in vecvars
        if k == "un":
            return e[1] in ("!", "-") and self.int_expr(e[2], cand, kind, intvars, vecvars)
        if k == "move":
            return self.int_expr(e[1], cand, kind, intvars, vecvars)
        if k == "idx":
            return (kind == "i64v" and isinstance(e[1], tuple) and e[1][0] == "var"
                    and e[1][1] in vecvars and self.intish(e[2], cand, kind, intvars))
        if k == "bin":
            if e[1] not in ("+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||"):
                return False
            if not self.int_expr(e[2], cand, kind, intvars, vecvars):
                return False
            if not self.int_expr(e[3], cand, kind, intvars, vecvars):
                return False
            if (e[1] in ("/", "%") and kind == "f64"
                    and self.intish(e[2], cand, kind, intvars)
                    and self.intish(e[3], cand, kind, intvars)):
                return False
            if e[1] in ("&&", "||") and (_may_raise(e[2]) or _may_raise(e[3])):
                return False
            return True
        if k == "call":
            if e[1] == "len" and kind == "i64v" and len(e[2]) == 1:
                a = e[2][0]
                if isinstance(a, tuple) and a[0] == "var" and a[1] in vecvars:
                    return True
            return (e[1] in cand and self.fastkinds.get(e[1]) == kind
                    and all(self.int_expr(a, cand, kind, intvars, vecvars) for a in e[2]))
        return False

    def intish(self, e, cand, kind, intvars):
        if not isinstance(e, tuple) or not e:
            return False
        k = e[0]
        if k == "num":
            return isinstance(e[1], int) and not isinstance(e[1], bool)
        if k == "bool":
            return True
        if k == "var":
            return e[1] in intvars
        if k == "un":
            return self.intish(e[2], cand, kind, intvars)
        if k == "move":
            return self.intish(e[1], cand, kind, intvars)
        if k == "idx":
            return kind == "i64v"
        if k == "bin":
            if e[1] in ("==", "!=", "<", ">", "<=", ">=", "&&", "||"):
                return True
            return (self.intish(e[2], cand, kind, intvars)
                    and self.intish(e[3], cand, kind, intvars))
        if k == "call":
            return kind == "i64" and e[1] in cand and self.fastkinds.get(e[1]) == "i64"
        return False

    def _booly(self, e):
        return (isinstance(e, tuple) and e and
                (e[0] == "bool" or (e[0] == "un" and e[1] == "!")
                 or (e[0] == "bin" and e[1] in ("==", "!=", "<", ">", "<=", ">=", "&&", "||"))))

    def print_ok(self, e, cand, kind, intvars):
        if not (isinstance(e, tuple) and e and e[0] == "call" and e[1] == "print"):
            return False
        for a in e[2]:
            if not isinstance(a, tuple) or not a:
                return False
            if a[0] == "str":
                for kd, val in _split_interp(a[1]):
                    if kd == "var" and "." in val:
                        return False
                continue
            if self._booly(a):
                return False
            if not self.int_expr(a, cand, kind, intvars):
                return False
        return True

    def int_body(self, body, cand, kind, intvars, vecvars=frozenset(), returned=None):
        for s in body:
            if not isinstance(s, tuple) or not s:
                return False
            k = s[0]
            if k == "let":
                if not self.int_expr(s[2], cand, kind, intvars, vecvars):
                    return False
                if self.intish(s[2], cand, kind, intvars):
                    intvars.add(s[1])
                else:
                    intvars.discard(s[1])
            elif k == "set":
                if "." in s[1] or not self.int_expr(s[2], cand, kind, intvars, vecvars):
                    return False
                if self.intish(s[2], cand, kind, intvars):
                    intvars.add(s[1])
                else:
                    intvars.discard(s[1])
            elif k == "setidx":
                if kind != "i64v":
                    return False
                if not (isinstance(s[1], tuple) and s[1][0] == "var" and s[1][1] in vecvars):
                    return False
                if not self.intish(s[2], cand, kind, intvars) or not self.intish(s[3], cand, kind, intvars):
                    return False
            elif k in ("return", "expr"):
                if k == "expr" and self.print_ok(s[1], cand, kind, intvars):
                    continue
                if not self.int_expr(s[1], cand, kind, intvars, vecvars):
                    return False
                if k == "return" and returned is not None:
                    returned.append(bool(self.intish(s[1], cand, kind, intvars)))
            elif k == "if":
                if self.fraise(s[1]) or not self.int_expr(s[1], cand, kind, intvars, vecvars):
                    return False
                if not self.int_body(s[2], cand, kind, set(intvars), vecvars, returned):
                    return False
                els = s[3]
                if els and not self.int_body(els if isinstance(els, list) else [els], cand, kind, set(intvars), vecvars, returned):
                    return False
            elif k == "while":
                if self.fraise(s[1]) or not self.int_expr(s[1], cand, kind, intvars, vecvars):
                    return False
                if not self.int_body(s[2], cand, kind, set(intvars), vecvars):
                    return False
            elif k == "forrange":
                if self.fraise(s[2]) or self.fraise(s[3]):
                    return False
                if not self.intish(s[2], cand, kind, intvars) or not self.intish(s[3], cand, kind, intvars):
                    return False
                sub = set(intvars)
                sub.add(s[1])
                if not self.int_body(s[4], cand, kind, sub, vecvars):
                    return False
            elif k == "foriter":
                if kind != "i64v":
                    return False
                ve = s[2]
                if not (isinstance(ve, tuple) and ve[0] == "var" and ve[1] in vecvars):
                    return False
                sub = set(intvars)
                sub.add(s[1])
                if not self.int_body(s[3], cand, kind, sub, vecvars):
                    return False
            elif k in ("break", "continue"):
                pass
            else:
                return False
        return True

    def compute_fast(self):
        kinds = {}
        self.fastvec = {}
        for name, entry in self.fns.items():
            if not (isinstance(entry, tuple) and len(entry) == 2):
                continue
            params, body = entry
            anns = []
            ok = True
            for p in params:
                if isinstance(p, tuple) and len(p) > 2 and p[2] in ("i64", "f64", "vec[i64]"):
                    anns.append(p[2])
                else:
                    ok = False
            # A fast function is raw C over `int64_t`/`double`; a script-level
            # `let` is a `Val` in the file scope, so a body that names one cannot
            # be raw C. It used to be classified fast anyway, and the symbol
            # table had no entry for it: `KeyError: 'total'` out of `fx`, or an
            # empty call in the emitted C. Dropping it here rather than after the
            # fixpoint matters -- a caller has to fall back with it, and the
            # fixpoint is what makes callers fall back.
            if (_names_in(body) & self.global_lets) - _bound_in(body):
                continue
            if ok and anns and "vec[i64]" in anns and all(a in ("i64", "vec[i64]") for a in anns):
                kinds[name] = "i64v"
                self.fastvec[name] = set(p[0] if isinstance(p, tuple) else p
                                         for p in params if isinstance(p, tuple) and len(p) > 2 and p[2] == "vec[i64]")
            elif ok and (not anns or len(set(anns)) == 1):
                kinds[name] = anns[0] if anns else "i64"
        self.fastkinds = dict(kinds)
        self.fails = _eff.may_fail([(n, e[0], e[1]) for n, e in self.fns.items()
                                    if isinstance(e, tuple) and len(e) == 2])
        cand = dict(kinds)
        changed = True
        while changed:
            changed = False
            for name in list(cand):
                params, body = self.fns[name]
                kind = kinds[name]
                intvars = set()
                if kind == "i64":
                    intvars = set(p[0] if isinstance(p, tuple) else p for p in params)
                # A `f64` fast path with any integer `return` prints `3.0`
                # natively and `3` from the interpreter: the C signature is
                # `double`, so the value is a float before `print` sees it. One
                # such `return` is enough to keep the whole body on the
                # tagged-union path, where the integer is preserved.
                returned = []
                if not self.int_body(body, set(cand), kind, intvars,
                                     self.fastvec.get(name, frozenset()), returned):
                    del cand[name]
                    changed = True
                elif kind == "f64" and any(returned):
                    del cand[name]
                    changed = True
        cand = self._drop_recursive_fast(cand)
        self.fast = {n: self.fns[n] for n in cand}
        self.fastkinds = {n: kinds[n] for n in cand}
        for i, name in enumerate(sorted(self.fast)):
            self.fastc[name] = "bf_f%d" % i
        for name in self.fast:
            kind = self.fastkinds[name]
            for op in self.ops_in(self.fast[name][1]):
                if op == "/":
                    if kind in ("i64", "i64v"):
                        self.need_idiv = True
                    else:
                        self.need_fdiv = True
                if op == "%":
                    if kind in ("i64", "i64v"):
                        self.need_imod = True
                    else:
                        self.need_fmod = True
            if kind == "i64v":
                self.need_viat = True
                if self.has_setidx(self.fast[name][1]):
                    self.need_vset = True

    def has_setidx(self, body):
        for s in body:
            if not isinstance(s, tuple) or not s:
                continue
            if s[0] == "setidx":
                return True
            for sub in s[1:]:
                if isinstance(sub, list) and self.has_setidx(sub):
                    return True
        return False

    def ops_in(self, body):
        ops = set()

        def walk(e):
            if not isinstance(e, tuple) or not e:
                return
            if e[0] == "bin":
                ops.add(e[1])
            for sub in e[1:]:
                if isinstance(sub, tuple):
                    walk(sub)
                elif isinstance(sub, list):
                    for x in sub:
                        if isinstance(x, tuple):
                            walk(x)

        for s in body:
            walk(s)
        return ops

    def fx(self, e):
        k = e[0]
        dbl = self.fkind == "f64"
        if k == "num":
            if not dbl:
                return _clit64(_wrap64(e[1]))
            f = float(e[1])
            if f != f or f in (_INF, -_INF):
                return "(%s)" % _cf64(f)
            lit = "%.17g" % f
            # `%.17g` of 1e10 is "10000000000", which C reads as an int: in an
            # f64 fast function `1e10 * 1e10` then multiplies two int64s and
            # overflows, while the interpreter multiplies two doubles. A float
            # literal keeps its point.
            if isinstance(e[1], float) and \
                    "." not in lit and "e" not in lit and "E" not in lit:
                lit += ".0"
            return "(%s)" % lit
        if k == "bool":
            return "(%.17g)" % (1.0 if e[1] else 0.0) if dbl else "(%dLL)" % (1 if e[1] else 0)
        if k == "var":
            return self.fsym[e[1]]
        if k == "un":
            v = self.fx(e[2])
            if e[1] == "!":
                return "(!(%s))" % v
            return "(-(%s))" % v if dbl else "((int64_t)(0ULL - (uint64_t)(%s)))" % v
        if k == "move":
            return self.fx(e[1])
        if k == "idx":
            return "bf_viat(%s, %s)" % (self.fsym[e[1][1]], self.fx(e[2]))
        if k == "bin":
            op = e[1]
            a = self.fx(e[2])
            b = self.fx(e[3])
            if op == "/":
                return "%s(%s, %s)" % ("bf_fdiv" if dbl else "bf_idiv", a, b)
            if op == "%":
                return "%s(%s, %s)" % ("bf_fmod" if dbl else "bf_imod", a, b)
            if op == "&&":
                return "(%s ? %s : %s)" % (a, b, a)
            if op == "||":
                return "(%s ? %s : %s)" % (a, a, b)
            if not dbl and op in ("+", "-", "*"):
                return "((int64_t)((uint64_t)(%s) %s (uint64_t)(%s)))" % (a, op, b)
            return "(%s %s %s)" % (a, op, b)
        if k == "call":
            if e[1] == "len":
                return "((%s)->len)" % self.fsym[e[2][0][1]]
            args = ", ".join(self.fx(a) for a in e[2])
            return "%s(%s)" % (self.fastc[e[1]], args)
        raise RuntimeError("fast node " + k)

    def fraise(self, e):
        """A fast-path expression that can panic and must not feed a value on."""
        if not isinstance(e, tuple) or not e:
            return False
        if e[0] == "bin" and e[1] in ("/", "%"):
            return True
        if e[0] == "idx":
            return True
        if e[0] == "call":
            name = e[1]
            if name == "len":
                return False          # a length is a field read, it cannot fail
            return name not in self.fastc or name in self.fails
        return any(self.fraise(sub) for sub in e[1:] if isinstance(sub, (tuple, list)))

    def fchk(self, e, ind):
        """Evaluate a fast-path expression and bail before its value is used."""
        if not self.fraise(e):
            return self.fx(e)
        t = self.new_tmp("t")
        dbl = self.fkind == "f64"
        self.out.append(ind + "%s %s = %s;" % ("double" if dbl else "int64_t", t, self.fx(e)))
        self.out.append(ind + "if (verr_pending()) return %s;" % ("0.0" if dbl else "0"))
        return t

    def fprint(self, args, ind):
        dbl = self.fkind == "f64"
        fmt = []
        vals = []
        for ai, a in enumerate(args):
            if ai:
                fmt.append(" ")
            if a[0] == "str":
                for kind, val in _split_interp(a[1]):
                    if kind == "lit":
                        fmt.append(val.replace("%", "%%"))
                    else:
                        fmt.append("%s" if dbl else BINT_FMT)
                        vals.append("vfmt_double(%s)" % self.fsym[val] if dbl
                                    else "(long long)(%s)" % self.fsym[val])
            elif a[0] == "num" and isinstance(a[1], float):
                fmt.append("%s")
                vals.append("vfmt_double(%r)" % a[1])
            elif dbl:
                fmt.append("%s")
                vals.append("vfmt_double(%s)" % self.fchk(a, ind))
            else:
                fmt.append(BINT_FMT)
                vals.append("(long long)(%s)" % self.fchk(a, ind))
        esc = _clit("".join(fmt))
        self.out.append(ind + 'printf("%s\\n"%s);' % (esc, "".join(", " + v for v in vals)))

    def fst(self, s, ind):
        k = s[0]
        if k in ("let", "set"):
            self.out.append(ind + "%s = %s;" % (self.fsym[s[1]], self.fchk(s[2], ind)))
        elif k == "return":
            self.out.append(ind + "return %s;" % self.fchk(s[1], ind))
        elif k == "expr":
            if isinstance(s[1], tuple) and s[1] and s[1][0] == "call" and s[1][1] == "print":
                self.fprint(s[1][2], ind)
            else:
                self.out.append(ind + "(void)(%s);" % self.fchk(s[1], ind))
        elif k == "if":
            self.out.append(ind + "if (%s) {" % self.fx(s[1]))
            for t in s[2]:
                self.fst(t, ind + "  ")
            els = s[3]
            if els:
                self.out.append(ind + "} else {")
                for t in (els if isinstance(els, list) else [els]):
                    self.fst(t, ind + "  ")
            self.out.append(ind + "}")
        elif k == "while":
            self.out.append(ind + "while (%s) {" % self.fx(s[1]))
            for t in s[2]:
                self.fst(t, ind + "  ")
            self.out.append(ind + "}")
        elif k == "forrange":
            _, vn, se, ee, body = s
            kk = self.new_tmp("k")
            lo = self.new_tmp("lo")
            hi = self.new_tmp("hi")
            self.out.append(ind + "int64_t %s = %s, %s = %s;" % (lo, self.fx(se), hi, self.fx(ee)))
            self.out.append(ind + "for (int64_t %s = %s; %s < %s; ++%s) {" % (kk, lo, kk, hi, kk))
            self.out.append(ind + "  %s = %s;" % (self.fsym[vn], kk))
            for t in body:
                self.fst(t, ind + "  ")
            self.out.append(ind + "}")
        elif k == "foriter":
            _, vn, ve, body = s
            vv = self.fsym[ve[1]]
            kk = self.new_tmp("k")
            self.out.append(ind + "for (int64_t %s = 0; %s < %s->len; ++%s) {" % (kk, kk, vv, kk))
            self.out.append(ind + "  %s = bf_viat(%s, %s);" % (self.fsym[vn], vv, kk))
            self.out.append(ind + "  if (verr_pending()) return 0;")
            for t in body:
                self.fst(t, ind + "  ")
            self.out.append(ind + "}")
        elif k == "setidx":
            self.out.append(ind + "bf_vset(%s, %s, %s);" % (self.fsym[s[1][1]], self.fchk(s[2], ind), self.fchk(s[3], ind)))
        elif k == "break":
            self.out.append(ind + "break;")
        elif k == "continue":
            self.out.append(ind + "continue;")
        else:
            raise RuntimeError("fast stmt " + k)

    def _drop_recursive_fast(self, cand):
        """Route recursive fast fns through their counted Val frame.

        The raw C fast path calls itself directly, so it would skip the depth
        counter and tail-recurse into an endless loop. Anything that calls a
        dropped fn must drop too, or its raw call site loses its target.
        """
        fast = {n: self.fns[n][1] for n in cand}

        def callees(body):
            out = set()

            def walk(x):
                if isinstance(x, tuple):
                    if x and x[0] == "call" and isinstance(x[1], str):
                        out.add(x[1])
                    for y in x:
                        walk(y)
                elif isinstance(x, list):
                    for y in x:
                        walk(y)

            walk(body)
            return out & set(fast)

        edges = {n: callees(b) for n, b in fast.items()}

        def reaches(src, dst, seen):
            if src == dst:
                return True
            if src in seen:
                return False
            seen.add(src)
            return any(reaches(c, dst, seen) for c in edges.get(src, ()))

        drop = {n for n in fast if any(reaches(c, n, set()) for c in edges[n])}
        changed = True
        while changed:
            changed = False
            for n in fast:
                if n not in drop and edges[n] & drop:
                    drop.add(n)
                    changed = True
        return [n for n in cand if n not in drop]

    def emit_fast_fn(self, name):
        params, body = self.fast[name]
        self.fkind = self.fastkinds[name]
        ct = "double" if self.fkind == "f64" else "int64_t"
        self.fvecvars = self.fastvec.get(name, frozenset())
        names = [p[0] if isinstance(p, tuple) else p for p in params]
        self.fsym = {}
        args = []
        for n in names:
            cn = _cname(n)
            self.fsym[n] = cn
            if n in self.fvecvars:
                args.append("Vec *%s" % cn)
            else:
                args.append("%s %s" % (ct, cn))
        self.out.append("static %s %s(%s) {" % (ct, self.fastc[name], ", ".join(args) if args else "void"))
        vars_ = set(names)
        _collect_vars(body, vars_)
        for n in sorted(vars_ - set(names)):
            self.fsym[n] = _cname(n)
            self.out.append("  %s BRAT_UNUSED %s;" % (ct, _cname(n)))
        for t in body:
            self.fst(t, "  ")
        self.out.append("  return %s;" % ("0.0" if self.fkind == "f64" else "0"))
        self.out.append("}")
        self.out.append("")
        self.fsym = {}
        self.fkind = "int64_t"
        self.fvecvars = frozenset()

    def fn_code(self, key):
        if key not in self.codes:
            self.codes[key] = len(self.codes)
        return self.codes[key]

    def collect_file(self, path):
        loaded = _brat.load_file(path)
        self.collect_prog(loaded.prog)

    def collect_src(self, src, base):
        loaded = _brat.load_source(src, base)
        self.collect_prog(loaded.prog)

    def collect_prog(self, prog):
        for s in prog[1]:
            if s[0] in ("fn", "impl", "trait", "enum", "pub", "extern", "externstruct", "exportfn"):
                self.collect_top(s, self.mod_bases[0])
        for s in prog[1]:
            if s[0] not in ("fn", "impl", "trait", "enum", "pub", "extern", "externstruct", "exportfn"):
                self.top_exe.append(("stmt", s))
                if s[0] == "let":
                    self.global_lets.add(s[1])

    def collect_top(self, s, base):
        k = s[0]
        if k == "pub":
            return self.collect_top(s[1], base)
        if k == "externstruct":
            self.structs[s[1]] = s[2]
            # `extern packed struct`: the fifth element says the record has no
            # padding, which decides every offset after the first field.
            if len(s) > 3 and s[3]:
                self.packed_structs.add(s[1])
            return
        if k == "exportfn":
            if s[1] not in self.fns:
                self.fns[s[1]] = (s[2], s[3])
                self.fn_ret[s[1]] = s[5]
                self.order.append(("fn", s[1]))
            self.collect_nested(s[3])
            self.exported.add(s[1])
            return
        if k == "extern":
            lib = s[1]
            name = s[2]
            params = s[3]
            ret = s[4]
            varargs = s[5] if len(s) > 5 else None
            effects = s[8] if len(s) > 8 else ()
            self.externs[name] = (params, ret, lib, varargs, effects)
            if len(s) > 7 and s[7]:
                self.errno_externs.add(name)
            return
        if k == "fn":
            if s[1] not in self.fns:
                self.fns[s[1]] = (s[2], s[3])
                self.fn_ret[s[1]] = s[5]
                names = [p[0] if isinstance(p, tuple) else p for p in s[2]]
                if _returns_param(s[3], names):
                    self.alias_ret.add(s[1])
                self.order.append(("fn", s[1]))
            self.collect_nested(s[3])
        elif k == "impl":
            _, trait, tname, fns = s
            if trait is not None:
                if trait not in self.traits:
                    raise RuntimeError("unknown trait " + trait)
                missing = [m for m in self.traits[trait] if m not in [f[1] for f in fns]]
                if missing:
                    raise RuntimeError("impl %s for %s missing %s" % (trait, tname, ",".join(missing)))
            for f in fns:
                key = tname + "." + f[1]
                if key not in self.fns:
                    self.fns[key] = (f[2], f[3])
                    self.order.append(("meth", tname, f[1]))
        elif k == "enum":
            self.enums.add(s[1])
            for vname, fields in s[2]:
                tag = "none" if vname == "None" and not fields else vname.lower()
                self.vtab[tag] = s[1]
                if vname not in self.fns:
                    self.fns[vname] = ("ctor", tag, len(fields))
        elif k == "trait":
            if s[1] not in self.traits:
                self.traits[s[1]] = [g[1] for g in s[2]]
        else:
            self.top_exe.append(("stmt", s))

    def collect_nested(self, node):
        """Register every `fn` declared inside a body, with a stable mangled name.

        The mangled name is assigned in discovery order, and collect_src runs
        once per program, so both emission passes agree on it. Without this the
        emitter only learns about a nested fn while writing the enclosing body,
        which is after the point where a call to it has to be numbered.
        """
        if isinstance(node, tuple) and node and node[0] == "fn":
            self.nested_seq += 1
            mangled = "%s@%d" % (node[1], self.nested_seq)
            self.fns[mangled] = (node[2], node[3])
            self.fn_alias[node[1]] = mangled
            if mangled not in [e[1] for e in self.late]:
                self.late.append(("fn", mangled, node[2], node[3]))
        if isinstance(node, (tuple, list)):
            for sub in node[1:] if isinstance(node, tuple) else node:
                if isinstance(sub, (tuple, list)):
                    self.collect_nested(sub)

    def cfn(self, key):
        return "bf_c%d" % self.fn_code(key)

    def fn_key(self, name):
        """Emission key for a callable name: the mangled key when `name` is a
        nested fn, so the call and the definition agree on one C name."""
        return self.fn_alias.get(name, name)

    def _check_cb_arg(self, ann, a, name, pos):
        """Refuse a callback slot this call cannot honour.

        Emission used to write `brat_cb_<name>` for any string literal, so a
        name with no BRAT function behind it came out as a call to a function
        that does not exist: C that does not compile, handed to the user by a
        compiler. A closure or a bare fn name came out as `_fa.as.ival`, which
        compiles and then means nothing. Both are refused here, in the words
        `check` uses, so `cc` and `check` say one thing.
        """
        _abi_type, _cb_signature = _typer_abi()
        if not (isinstance(a, tuple) and a[0] == "str"):
            raise SyntaxError("arg %d of `%s`: callback must be a function name" % (pos, name))
        fname = a[1]
        key = self.fn_key(fname)
        info = self.fns.get(key)
        if not (isinstance(info, tuple) and len(info) == 2):
            raise SyntaxError("unknown callback `%s`" % fname)
        params = info[0]
        want = _abi_type(ann)
        sig = _cb_signature(want)
        if sig is None:
            raise SyntaxError("callback `%s` does not have a verifiable C ABI signature"
                              % fname)
        if len(params) != len(sig[0]):
            raise SyntaxError("callback `%s` takes %d parameter(s), %s wants %d"
                              % (fname, len(params), want, len(sig[0])))
        anns = [_abi_type(p[2] if isinstance(p, tuple) and len(p) > 2 else None)
                for p in params]
        ret = _abi_type(self.fn_ret.get(key))
        if tuple(anns) != tuple(_abi_type(t) for t in sig[0]) or ret != _abi_type(sig[1]):
            raise SyntaxError("callback `%s` has signature fn(%s)->%s, expected %s"
                              % (fname, ",".join(anns), ret, want))

    def _ffi_decl_type(self, ann):
        """C spelling of one declaration slot: a scalar width, a struct or a pointer.

        Anything else is refused here rather than spelled as a machine word: an
        extern the emitter cannot spell is an ABI the program must not call.
        """
        if ann is None or ann == "none":
            return "void"
        if isinstance(ann, str) and ann in self.structs:
            return _ffi_c(ann)
        if isinstance(ann, str) and ann == "ptr":
            return "void *"
        if isinstance(ann, str) and ann.startswith("fn"):
            _abi_type, callback_signature = _typer_abi()
            signature = callback_signature(ann)
            if signature is None:
                raise SyntaxError("unsupported FFI callback type `%s`" % ann)
            params, result = signature
            cparams = [self._ffi_decl_type(param) for param in params]
            return "%s (*) (%s)" % (self._ffi_decl_type(result),
                                    ", ".join(cparams) if cparams else "void")
        if isinstance(ann, str) and ann.startswith("ptr[") and ann.endswith("]"):
            elem = ann[4:-1].strip()
            if elem.startswith("fn"):
                raise SyntaxError("pointer to callback is not a supported FFI type")
            return self._ffi_decl_type(elem) + " *"
        ctype = _ffi_ctype(ann)
        if ctype is None:
            raise SyntaxError("unsupported FFI type `%s` in an extern declaration" % (ann,))
        return ctype

    def _out_addr(self, e, ann=None):
        """`&x` -- the address of a binding, as a C pointer.

        Only a name can be written back through, so `&` takes a name, and the
        width comes from the extern's `ptr[T]`: an `i32 *` out-parameter is four
        bytes. The binding's value goes into a scratch of that width, the callee
        writes the scratch, and `_out_flush_stmts` puts it back.
        """
        if not (isinstance(e, tuple) and e[0] == "var"):
            raise SyntaxError("`&` takes a binding: `let mut p = &x`")
        elem = ann or ""
        width = {"bool": 1, "u8": 1, "i8": 1, "u16": 2, "i16": 2,
                 "i32": 4, "u32": 4, "f32": 4, "usize": 8,
                 "u64": 8, "i64": 8, "f64": 8}.get(elem, 8)
        _s = self.sym.get(e[1])
        cn = getattr(_s, "cn", None) or e[1]
        scratch = self.new_tmp("out")
        ctype = {"bool": "_Bool", "i8": "int8_t", "u8": "uint8_t",
                 "i16": "int16_t", "u16": "uint16_t", "i32": "int32_t",
                 "u32": "uint32_t", "i64": "int64_t", "u64": "uint64_t",
                 "usize": "size_t", "f32": "float", "f64": "double"}.get(
                     elem, _out_ctype(width))
        initial = "vis_true(%s)" % cn if elem == "bool" else \
            "vnum(%s)" % cn if elem in ("f32", "f64") else "(%s).as.ival" % cn
        self._out_binds.append((e[1], cn, scratch, width, elem))
        return (["%s %s = (%s)(%s);" % (ctype, scratch, ctype, initial)],
                "(void *)&%s" % scratch)

    def _out_flush_stmts(self):
        """The callee has run: every `&` in this call takes its value back.

        Statements, not a prelude: they run after the call, so a raise the callee
        left behind leaves the binding as it was -- never a value nobody wrote.
        """
        out = []
        for _name, cn, scratch, width, elem in self._out_binds:
            out.append("vrc_dec(%s);" % cn)
            if elem in ("f32", "f64"):
                out.append("%s = vfloat((double)%s);" % (cn, scratch))
            elif elem == "bool":
                out.append("%s = vbool(%s);" % (cn, scratch))
            else:
                cast = "(int64_t)(uint64_t)%s" % scratch if elem in (
                    "u8", "u16", "u32", "u64", "usize") else "(int64_t)%s" % scratch
                out.append("%s = vint(%s);" % (cn, cast))
        return out

    def ex(self, e):
        k = e[0]
        if k == "num":
            v = e[1]
            if isinstance(v, bool):
                return ([], "vbool(%d)" % (1 if v else 0))
            if isinstance(v, int):
                return ([], "vint(%s)" % _clit64(_wrap64(v)))
            return ([], "vfloat(%s)" % _cf64(v))
        if k == "bool":
            return ([], "vbool(%d)" % (1 if e[1] else 0))
        if k == "str":
            return self.ex_str(e[1])
        if k == "rstr":
            return ([], 'vlit("%s")' % _clit(e[1]))
        if k == "none":
            return ([], "vnil()")
        if k == "var":
            return ([], self.var_ref(e[1]))
        if k == "parfor":
            return self.ex_parfor(e)
        if k == "un":
            if e[1] == "&":
                return self._out_addr(e[2])
            pre, v = self.ex(e[2])
            if e[1] == "!":
                return (pre, "vbool(!vis_true(%s))" % v)
            return (pre, "vneg(%s)" % v)
        if k == "bin":
            return self.ex_bin(e)
        if k == "or":
            pa, a = self.ex(e[1])
            pb, b = self.ex(e[2])
            t = self.new_tmp()
            pre = pa + ["Val %s = (%s);" % (t, a)] + pb
            # `pick_first([]) or 99` answers with the tag the call built or with
            # the fallback. Whichever it is, the binding that receives it owns one
            # reference: the left side is released when the fallback wins, and the
            # fallback is counted up front so both branches hand over one.
            fb = self._keep(e[2], b)
            held = _dec_able(e[1], self.heap_returns()) or (
                isinstance(e[1], tuple) and e[1] and e[1][0] == "call" and _may_raise(e[1]))
            if held:
                pre.append("if (vor_fb(%s)) { vdrop_temp(%s); %s = (%s); }" % (t, t, t, fb))
            else:
                pre.append("if (vor_fb(%s)) { %s = (%s); }" % (t, t, fb))
            return (pre, t)
        if k == "quest":
            pa, a = self.ex(e[1])
            return self.part_of_temp(pa, e[1], a, "vquest_ok(vquest(", "))")
        if k == "vec":
            return self.ex_vec(e[1])
        if k == "maplit":
            return self.ex_maplit(e[1])
        if k == "struct":
            return self.ex_struct(e[1], e[2])
        if k == "field":
            pa, a = self.ex(e[1])
            t = self.new_tmp("f")
            fname = _clit(e[2])
            bail = self.err_bail()
            # `v[0].x` and `f().x` read a field out of a value nobody owns: the
            # receiver arrived as an owned reference and was used twice in the
            # two lines below, so it was never released. Held in a temp, the
            # field is lifted with its own reference and the receiver goes, the
            # way `f()[0]` already did.
            owned = _dec_able(e[1], self.heap_returns())
            recv = a
            if owned:
                recv = self.new_tmp("r")
                pa.append("Val %s = (%s);" % (recv, a))
            pa.append("if ((%s).tag != V_STRUCT) { vraise(\"no field %s\"); %s }" % (recv, fname, bail))
            pa.append("Val *%s = map_get((%s).as.map, \"%s\");" % (t, recv, fname))
            pa.append("if (!%s) { vraise(\"no field %s\"); %s }" % (t, fname, bail))
            if owned:
                v = self.new_tmp("v")
                pa.append("Val %s = vrc_inc(*%s);" % (v, t))
                pa.append("vdrop_temp(%s);" % recv)
                return (pa, v)
            return (pa, "(*%s)" % t)
        if k == "idx":
            _sp = self._ptr_elem(e[1])
            if _sp is not None:
                return self._ptr_idx(_sp, e[1], e[2])
            pa, a = self.ex(e[1])
            pb, b = self.ex(e[2])
            if not _dec_able(e[1], self.heap_returns()):
                return (pa + pb, "vat_owned(%s, %s)" % (a, b))
            # Indexing a temporary: the element is lifted out and the container is
            # then released, or every `f()[0]` leaves a container behind.
            recv = self.new_tmp("r")
            t = self.new_tmp("c")
            return (pa + ["Val %s = (%s);" % (recv, a)] + pb +
                    ["Val %s = vat_owned(%s, %s);" % (t, recv, b),
                     "vdrop_temp(%s);" % recv], t)
        if k == "mcall":
            return self.ex_mcall(e)
        if k == "call":
            return self.ex_call(e)
        if k == "closure":
            return self.ex_closure(e)
        if k == "move":
            pre, v = self.ex(e[1])
            src = self._place_root(e)
            if (
                src is not None
                and src in self.sym
                and isinstance(e[1], tuple)
                and e[1][0] == "var"
            ):
                self.moved.add(src)
                v = "vmove(&%s)" % self.sym[src].cn
            return (pre, v)
        raise RuntimeError("node " + k)

    def ex_str(self, s):
        parts = _split_interp(s)
        if not parts:
            return ([], 'vlit("")')
        if len(parts) == 1 and parts[0][0] == "lit":
            return ([], 'vlit("%s")' % _clit(parts[0][1]))
        t = self.new_tmp("s")
        pre = ['Val %s = vlit("");' % t]
        for kind, val in parts:
            if kind == "lit":
                pre.append('%s = vadd(%s, vlit("%s"));' % (t, t, _clit(val)))
            else:
                pre.append('%s = vadd(%s, vstr_of(%s));' % (t, t, self.path_ref(val)))
        return (pre, t)

    def path_ref(self, path):
        """C expression for an interpolation placeholder, dotted paths included."""
        parts = path.split(".")
        e = self.var_ref(parts[0])
        for f in parts[1:]:
            e = 'vfield(%s, "%s")' % (e, _clit(f))
        return e

    def ex_bin(self, e):
        op = e[1]
        if op in ("&&", "||"):
            pa, a = self.ex(e[2])
            pb, b = self.ex(e[3])
            t = self.new_tmp()
            pre = pa + ["Val %s = (%s);" % (t, a)]
            guard = "if (vis_true(%s)) {" % t if op == "&&" else "if (!vis_true(%s)) {" % t
            pre += [guard] + ["  " + p for p in pb] + ["  %s = (%s);" % (t, b), "}"]
            return (pre, t)
        pa, a = self.ex(e[2])
        pb, b = self.ex(e[3])
        # `n + f()` must not add the placeholder value `f` left behind when it
        # failed: the pending error travels to the enclosing try, not into `+`.
        pre = list(pa)
        # An operand that builds a fresh heap value -- `f().left() == "a"`,
        # `vrev(v) == [3, 2, 1]`, `"a" + "b" == "ab"` -- belongs to this operator
        # only while it reads it. Bound to a temp here and released once the
        # result exists: nothing else names it, and leaving it leaked one value
        # per evaluation. A container literal is excluded because its `Vec *`
        # temp is released by the code below, through `vrc_dec`.
        drops = []

        def _hold(node, val):
            # A call that fails leaves its error tag in the temp even when its
            # declared result is a number, and the tag has nobody once the frame
            # that raised is gone: `n = n - boom()` leaked one message per raise.
            # Only a plain call: `s.sort()` answers with its receiver, and the
            # receiver may be a local that must stay alive.
            raising_call = (isinstance(node, tuple) and node and node[0] == "call"
                            and _may_raise(node))
            if not (_dec_able(node, self.heap_returns()) or raising_call):
                return val
            if isinstance(node, tuple) and node and node[0] in _NOT_OWNED_HERE:
                return val
            if not (isinstance(val, str) and val.startswith("_")):
                t = self.new_tmp("v")
                pre.append("Val %s = (%s);" % (t, val))
                val = t
            drops.append(val)
            return val

        a = _hold(e[2], a)
        if pa and _may_raise(e[2]):
            pre.append(self.err_check_line(drops))
        pre += pb
        b = _hold(e[3], b)
        if pb and _may_raise(e[3]):
            pre.append(self.err_check_line(drops))
        if op == "+":
            t = self.new_tmp("v")
            pre = pre + ["Val %s = vadd(%s, %s);" % (t, a, b)]
            for node, val in ((e[2], a), (e[3], b)):
                # A container built for this addition only is released here:
                # the result took its own references to the elements.
                tmp = _container_tmp(val)
                if tmp is not None and isinstance(node, tuple) and node and node[0] in ("vec", "maplit"):
                    pre.append("vrc_dec(vvecv(%s));" % tmp)
            for d in drops:
                pre.append("vdrop_temp(%s);" % d)
            if drops:
                pre.append(self.err_check_line())
            return (pre, t)
        if op in ("-", "*", "/", "%"):
            res = "v%s(%s, %s)" % ({"-": "sub", "*": "mul", "/": "div", "%": "mod"}[op], a, b)
        elif op == "==":
            res = "vbool(veq(%s, %s))" % (a, b)
        elif op == "!=":
            res = "vbool(!veq(%s, %s))" % (a, b)
        elif op in ("<", ">", "<=", ">="):
            res = 'vbool(vcmpt(%s, %s, "%s") %s 0)' % (a, b, op, op)
        elif op == "in":
            res = "vbool(vcontains(%s, %s))" % (b, a)
        else:
            raise RuntimeError("operator " + op)
        # A container built for a comparison is read, not consumed: `v == [1, 2, 3]`
        # left the literal's vector behind on every evaluation.
        cdrops = []
        if op in ("==", "!=", "<", ">", "<=", ">=", "in"):
            nodes_vals = ((e[2], a), (e[3], b)) if op != "in" else ((e[3], b),)
            for node, val in nodes_vals:
                tmp = _container_tmp(val)
                if tmp is None or not (isinstance(node, tuple) and node):
                    continue
                if node[0] in ("vec", "maplit", "struct"):
                    cdrops.append("vrc_dec(vvecv(%s));" % tmp)
        if not drops and not cdrops:
            return (pre, res)
        t = self.new_tmp("v")
        out = pre + ["Val %s = (%s);" % (t, res)]
        # A temp the comparison finished with is nobody's on the success path
        # too. Releasing it only when a raise was pending is what leaked one
        # string per `assert("a" + "b" == "ab")`: the error path was covered and
        # the path a correct program takes was not.
        out.extend("vdrop_temp(%s);" % d for d in drops)
        out.append(self.err_check_line())
        return (out + cdrops, t)

    _SIG_TYPES = ("i64", "f64", "str", "ptr", "none")

    def ex_plugin(self, name, args):
        """`lib_*` and `call(fn, "ret(args)", ...)`: a literal signature, so the C cast is static."""
        if name == "lib_open":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "vlib_open(%s)" % a)
        if name == "lib_sym":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "vlib_sym(%s, %s)" % (a, b))
        if name == "lib_close":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "vlib_close(%s)" % a)
        ret_t, params = _parse_sig(args[1])
        if ret_t not in self._SIG_TYPES:
            raise SyntaxError("call: unsupported return type `%s`" % ret_t)
        for t in params:
            if t not in self._SIG_TYPES or t == "none":
                raise SyntaxError("call: unsupported argument type `%s`" % t)
        if len(args) - 2 != len(params):
            raise SyntaxError("call: signature wants %d argument(s), got %d"
                              % (len(params), len(args) - 2))
        pf, f = self.ex(args[0])
        pre = list(pf)
        cvals = []
        for t, a in zip(params, args[2:]):
            pa, v = self.ex(a)
            pre.extend(pa)
            if t == "f64":
                cvals.append("(%s).as.fval" % v)
            elif t == "str":
                cvals.append("(%s).as.str" % v)
            elif t == "ptr":
                cvals.append("(void *)(intptr_t)(%s).as.ival" % v)
            else:
                cvals.append("(int64_t)(%s).as.ival" % v)
        plugin_owner = self.new_tmp("plugin_owner")
        plugin_fn = self.new_tmp("plugin_fn")
        pre.append(self.err_check_line())
        pre.append("bint %s = 0;" % plugin_owner)
        pre.append("void *%s = vplugin_acquire(%s, &%s);" % (plugin_fn, f, plugin_owner))
        pre.append(self.err_check_line())
        release = ["vplugin_release(%s);" % plugin_owner, self.err_check_line()]
        ctypes_ = {"i64": "int64_t", "f64": "double", "str": "const char *", "ptr": "void *"}
        c_ret = {"i64": "int64_t", "f64": "double", "str": "const char *",
                 "ptr": "void *", "none": "void"}[ret_t]
        cparam = ", ".join(ctypes_[t] for t in params)
        if params:
            call_expr = "(*(%s (*)(%s))(intptr_t)%s)(%s)" % (
                c_ret, cparam, plugin_fn, ", ".join(cvals))
        else:
            call_expr = "(*(%s (*)(void))(intptr_t)%s)()" % (c_ret, plugin_fn)
        t_tmp = self.new_tmp("p")
        if ret_t == "none":
            return (pre + ["%s;" % call_expr] + release, "vint(0)")
        if ret_t == "f64":
            return (pre + ["double %s = %s;" % (t_tmp, call_expr)] + release,
                    "vfloat(%s)" % t_tmp)
        if ret_t == "str":
            raw = self.new_tmp("plugin_str")
            result = self.new_tmp("plugin_result")
            return (pre + ["const char *%s = %s;" % (raw, call_expr),
                           "Val %s = vcall_str(%s);" % (result, raw)] + release,
                    result)
        if ret_t == "ptr":
            return (pre + ["void *%s = %s;" % (t_tmp, call_expr)] + release,
                    "vint((int64_t)(intptr_t)%s)" % t_tmp)
        return (pre + ["int64_t %s = %s;" % (t_tmp, call_expr)] + release,
                "vint(%s)" % t_tmp)

    _last_slice_elem = None
    # `&x` inside the call being built: (name, c name, scratch, width)
    _out_binds = []

    def ex_slice(self, name, args):
        """Slices are runtime values: [base, start, len, kind], every access checked."""
        if name == "slice_of":
            self.need_arity(args, 3, name)
            pb, b = self.ex(args[0])
            pa, a = self.ex(args[1])
            pc, c = self.ex(args[2])
            return (pb + pa + pc, "vslice_of(%s, %s, %s)" % (b, a, c))
        if name == "slice_of_ptr":
            self.need_arity(args, 3, name)
            kind = args[2]
            if not (isinstance(kind, tuple) and kind[0] == "str"
                    and kind[1] in ("i64", "f64", "u8", "i32", "f32")):
                raise SyntaxError('slice_of_ptr: kind must be the literal '
                                  '"i64", "i32", "f64", "f32" or "u8"')
            pb, b = self.ex(args[0])
            pn, n = self.ex(args[1])
            return (pb + pn, "vslice_of_ptr(%s, %s, vint(%d))"
                    % (b, n, {"i64": 1, "f64": 2, "u8": 3, "i32": 4, "f32": 5}[kind[1]]))
        if name == "slice_of_struct":
            # slice_of_struct(p, n, "Reg"): a window over a table of C records.
            # The stride is the struct's size and the layout stays the
            # compiler's, so reading an element is a pointer read and writing
            # one is refused -- the table belongs to whoever allocated it.
            self.need_arity(args, 3, name)
            sname = args[2]
            if not (isinstance(sname, tuple) and sname[0] == "str"):
                raise SyntaxError("slice_of_struct wants a struct name as a string literal")
            if sname[1] not in self.structs:
                raise SyntaxError("slice_of_struct: %s is not an extern struct" % sname[1])
            pa, a = self.ex(args[0])
            pn, n = self.ex(args[1])
            self._last_slice_elem = sname[1]
            return (pa + pn, "vslice_of_struct(%s, %s, vint(sizeof(%s)))"
                    % (a, n, _ffi_c(sname[1])))
        if name == "slice_struct_of":
            # A window over values holds values: no layout is being declared,
            # so it must not inherit the record table's element.
            self._last_slice_elem = None
            # slice_struct_of([a, b, c]): a window over elements built in the
            # program. The table of C records beside it is read-only, so this is
            # how a window of structs is written: `sl[1].count = ...` reaches the
            # element the literal made, and the elements live exactly as long as
            # the window does.
            self.need_arity(args, 1, name)
            items = args[0]
            if not (isinstance(items, tuple) and items[0] == "vec"):
                raise SyntaxError("slice_struct_of wants a vector literal: a window that is written cannot start empty")
            pres, vals = [], []
            for kid in items[1]:
                pk, vk = self.ex(kid)
                pres.extend(pk)
                vals.append(vk)
            arr = self.new_tmp("el")
            pres.append("Val %s[] = { %s };" % (arr, ", ".join(vals) if vals else "vnil()"))
            res = self.new_tmp("sl")
            pres.append("Val %s = vslice_elems((_Elems){ %s, %d });" % (res, arr, len(vals)))
            return (pres, res)
        if name == "slice_len":
            self.need_arity(args, 1, name)
            ps, sv = self.ex(args[0])
            return (ps, "vslice_len(%s)" % sv)
        if name == "slice_get":
            self.need_arity(args, 2, name)
            ps, sv = self.ex(args[0])
            pi, iv = self.ex(args[1])
            # A window over C records hands back the record, not its address:
            # the element the window declared is the layout to read it with.
            elem = self._slice_elem_of(args[0])
            if elem in self.structs:
                t = self.new_tmp("sr")
                return (ps + pi + ["Val %s = vunwind;" % t,
                                   "if (vptr_live(vslice_at(%s, %s))) { %s = %s; }"
                                   % (sv, iv, t, self._c_struct_to_val(
                                       elem, "(*(%s *)(vslice_at(%s, %s)).as.ival)"
                                       % (_ffi_c(elem), sv, iv)))],
                        t)
            return (ps + pi, "vslice_at(%s, %s)" % (sv, iv))
        if name == "slice_set":
            self.need_arity(args, 3, name)
            ps, sv = self.ex(args[0])
            pi, iv = self.ex(args[1])
            pv, vv = self.ex(args[2])
            return (ps + pi + pv, "vslice_set(%s, %s, %s)" % (sv, iv, vv))
        if name == "str_from_ptr":
            self.need_arity(args, 2, name)
            pp, pv = self.ex(args[0])
            pn, nv = self.ex(args[1])
            return (pp + pn, "vstr_from_ptr(%s, %s)" % (pv, nv))
        raise SyntaxError("unknown slice builtin " + name)

    def _slice_elem_of(self, node):
        """The element a window declares, for a `slice[T]` binding or a call."""
        if isinstance(node, tuple) and node and node[0] == "var":
            sym = self.sym.get(node[1])
            if sym is not None and sym.elem:
                return sym.elem
        if isinstance(node, tuple) and node and node[0] == "call" \
                and node[1] == "slice_of_struct" and len(node[2]) == 3:
            lit = node[2][2]
            if isinstance(lit, tuple) and lit and lit[0] == "str":
                return lit[1]
        return None

    def ex_vec(self, items):
        t = self.new_tmp("v")
        pre = ["Vec *%s = vvec();" % t]
        for x in items:
            px, sx = self.ex(x)
            pre.extend(px)
            pre.append("vec_push(%s, %s);" % (t, self._keep(x, sx)))
        return (pre, "vvecv(%s)" % t)

    def ex_maplit(self, pairs):
        t = self.new_tmp("m")
        pre = ['Map *%s = vmap("");' % t]
        for ke, ve in pairs:
            pk, sk = self.ex(ke)
            pv, sv = self.ex(ve)
            pre.extend(pk)
            pre.extend(pv)
            pre.append("map_set_v(%s, %s, %s);" % (t, self._keep(ke, sk), self._keep(ve, sv)))
        r = self.new_tmp("ml")
        pre.append("Val %s = vmap_has_reserved(%s) ? vmapkey_err() :"
                   " (vmap_bad_seen(%s) ? vmap_bad_key_err() : vstruct(%s));"
                   % (r, t, t, t))
        return (pre, r)

    def ex_struct(self, tname, fields):
        t = self.new_tmp("m")
        pre = ['Map *%s = vmap("%s");' % (t, _clit(tname))]
        for fname, fval in fields:
            pf, sf = self.ex(fval)
            pre.extend(pf)
            pre.append('map_set(%s, "%s", %s);' % (t, _clit(fname), self._keep(fval, sf)))
        return (pre, "vstruct(%s)" % t)

    def args_vec(self, args):
        t = self.new_tmp("a")
        pre = ["Vec *%s = vvec();" % t]
        for a in args:
            pa, sa = self.ex(a)
            pre.extend(pa)
            pre.append("vec_push(%s, %s);" % (t, self._keep(a, sa)))
            pre.append(self.err_check_line())
        return (pre, t)

    def emit_loop_drops(self, body, ind):
        """Release what the loop's last pass left behind: values declared in the
        body are out of scope here, so nothing can read them after this point."""
        seen = set()

        def walk(stmts):
            for st in stmts:
                if not isinstance(st, tuple) or not st:
                    continue
                if st[0] == "let":
                    name = st[1].split(".", 1)[0]
                    if name in self.relet and name not in seen:
                        seen.add(name)
                        self.out.append(ind + "vrelease(%s);" % _cname(name))
                elif st[0] in ("if",):
                    walk(st[2])
                    els = st[3]
                    if isinstance(els, tuple):
                        walk([els])
                    elif els:
                        walk(els)
                elif st[0] == "try":
                    walk(st[1])
                    walk(st[2])

        walk(body)

    def heap_returns(self):
        """Functions that build a new heap value: their result can be released by the holder."""
        out = set()
        for name, ret in self.fn_ret.items():
            if ret in ("vec", "map", "str", "tag") or (ret or "").startswith("vec["):
                out.add(name)
            elif isinstance(ret, str) and (ret in self.structs or ret in self.enums):
                out.add(name)
        # Bodies that prove it, whatever the annotation claims: `fn first<T>(v)
        # -> i64 { return v[0] }` says i64 and hands back a string, and a caller
        # that believes the annotation treats the result as an alias and never
        # releases it. Asked through `_dec_able`, to a fixpoint, because one such
        # function may call another.
        for _ in range(3):
            grown = set(out)
            for _name, _info in self.fns.items():
                if not (isinstance(_info, tuple) and len(_info) == 2):
                    continue
                if _body_returns_owned(_info[1], grown):
                    grown.add(_name)
                    grown.add(_name.rsplit(":", 1)[-1])
            if len(grown) == len(out):
                break
            out = grown
        # `Has(Ok(9))` builds a fresh tag in place of the call: counted as a
        # borrow, the tag and everything inside it stayed allocated.
        for _name, _info in self.fns.items():
            if isinstance(_info, tuple) and _info and _info[0] == "ctor":
                out.add(_name)
        return out

    def part_of_temp(self, pre, src, val, open_, close):
        """A part read out of a tag (`x?`, `x.unwrap()`).

        The tag owns the payload, so the result is counted once for the caller
        (see vtag_payload). When the tag was built for this expression -- ...
        `Ok(42)?`, `f().unwrap()` -- nothing else holds it and the tag is
        released here. Decided on the expression, because at runtime a binding
        and a temporary are the same value.
        """
        expr = "%s%s%s" % (open_, val, close)
        tmp = _owned_temp(src, val)
        if not _dec_able(src, self.heap_returns()) or tmp is None:
            return (pre, expr)
        r = self.new_tmp("a")
        return (list(pre) + ["Val %s = %s;" % (r, expr),
                             "vdrop_temp(%s);" % tmp], r)

    def read_only(self, arg, fn):
        """A call that only reads its argument (`len`, `str`, `tagof`, `is_err`)."""
        return self.read_only_call(arg, lambda v: "%s(%s)" % (fn, v))

    def read_only_call(self, arg, mk):
        """Evaluate `mk` over an argument that is read, never kept.

        A value the argument built has nobody else: `len(v.join("/"))` leaked the
        joined string, `is_err("xx".parse_float())` leaked the tag. Read through a
        temp so the value can be released as soon as the answer exists.
        """
        pa, a = self.ex(arg)
        if not _dec_able(arg, self.heap_returns()):
            return (pa, mk(a))
        t = self.new_tmp("a")
        return (pa + ["Val %s = (%s);" % (t, a),
                      "Val %s_r = %s;" % (t, mk(t)),
                      "vdrop_temp(%s);" % t], t + "_r")

    def call_result(self, args, pre, vec, call):
        """A call can hand back one of its arguments, so those bindings are
        counted before the call and the argument vector is freed after it."""
        head = list(pre)
        t = self.new_tmp("c")
        return (head + ["Val %s = (%s);" % (t, call),
                        "vrc_dec(vvecv(%s));" % vec], t)

    def need_arity(self, args, n, name):
        if len(args) != n:
            raise RuntimeError(name + " arity")

    def ex_call(self, e):
        name, args = e[1], e[2]
        if name == "print":
            if not args:
                return ([], '(BRAT_PRINT_LOCK(), printf("\\n"), BRAT_PRINT_UNLOCK(), vint(0))')
            pre_all = []
            parts = []
            for a in args:
                pa, sa = self.ex(a)
                pre_all.extend(pa)
                t = self.new_tmp("p")
                pre_all.append("Val %s = (%s);" % (t, sa))
                pre_all.append(self.err_check_line())
                parts.append(t)
            out = ["BRAT_PRINT_LOCK();"]
            for i, t in enumerate(parts):
                if i:
                    out.append('printf(" ");')
                out.append("vprint(%s);" % t)
            out.append("vprint_nl();")
            out.append("BRAT_PRINT_UNLOCK();")
            # vprint reads its argument; a value this statement built is nobody's
            # afterwards, and leaving it was a leak of one container per print.
            for a, t in zip(args, parts):
                if _dec_able(a, self.heap_returns()):
                    out.append("vdrop_temp(%s);" % t)
            return (pre_all + out, "vint(0)")
        if name in ("log.info", "log.warn", "log.error"):
            if not args:
                raise RuntimeError("log arity")
            pa, vec = self.args_vec(args)
            lvl = name.split(".")[1].upper()
            return (pa + ['vlogv("%s", %s);' % (lvl, vec),
                          "vrc_dec(vvecv(%s));" % vec], "vint(0)")
        if name == "trace.span":
            if not args:
                raise RuntimeError("span arity")
            rest = list(args)
            body = None
            if rest and isinstance(rest[-1], tuple) and rest[-1] and rest[-1][0] == "closure":
                body = rest.pop()
            pa, vec = self.args_vec(rest)
            out = list(pa) + ["vspan(vvecv(%s));" % vec,
                              "vrc_dec(vvecv(%s));" % vec]
            if body is not None:
                pb, cb = self.ex(body)
                out.extend(pb)
                t = self.new_tmp("sp")
                sv = self.new_tmp("sv")
                out.append("Vec *%s = vvec();" % sv)
                out.append("Val %s = vcall_any(%s, %s);" % (t, cb, sv))
                out.append("vrc_dec(vvecv(%s));" % sv)
                return (out, t)
            return (out, "vint(0)")
        if name == "len":
            self.need_arity(args, 1, name)
            return self.read_only(args[0], "vlen")
        if name == "str":
            self.need_arity(args, 1, name)
            return self.read_only(args[0], "vstr_of")
        if name == "__at":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "vat_owned(%s, %s)" % (a, b))
        if name in ("Ok", "Err", "Some"):
            self.need_arity(args, 1, name)
            return self.ex_tag(name.lower(), args)
        if name == "unwrap":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return self.part_of_temp(pa, args[0], a, "vunwrap(", ")")
        if name == "is_err":
            self.need_arity(args, 1, name)
            return self.read_only_call(args[0], lambda v: "vbool(vis_err(%s))" % v)
        if name == "is_none":
            self.need_arity(args, 1, name)
            return self.read_only_call(args[0], lambda v: "vbool(vis_none(%s))" % v)
        if name == "is_vec":
            self.need_arity(args, 1, name)
            return self.read_only_call(args[0], lambda v: "vbool((%s).tag == V_VEC)" % v)
        if name == "is_str":
            self.need_arity(args, 1, name)
            return self.read_only_call(args[0], lambda v: "vbool((%s).tag == V_STR)" % v)
        if name == "is_map":
            self.need_arity(args, 1, name)
            return self.read_only_call(args[0], lambda v: "vbool((%s).tag == V_STRUCT)" % v)
        if name == "tagof":
            self.need_arity(args, 1, name)
            # `vtagof` names the tag, it does not keep it: a tag built for this
            # expression had nobody to release it.
            return self.read_only(args[0], "vtagof")
        if name == "assert":
            if not (1 <= len(args) <= 2):
                raise RuntimeError("assert arity")
            pa, a = self.ex(args[0])
            bail = self.err_bail()
            if len(args) == 2:
                pb, b = self.ex(args[1])
                return (pa + pb + [
                    'if (!vis_true(%s)) { Vec *_ae = vvec(); vec_push(_ae, %s); '
                    'vunwind = vtagv("err", _ae); vunwind_do(); %s }' % (a, b, bail)
                ], "vbool(1)")
            return (pa + ['if (!vis_true(%s)) { vraise("assert failed"); %s }' % (a, bail)], "vbool(1)")
        if name == "read_file":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "vreadfile(%s)" % a)
        if name == "write_file":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "vwritefile(%s, %s)" % (a, b))
        if name in ("tcp_listen", "tcp_connect", "tcp_accept", "tcp_close"):
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "v%s(%s)" % (name, a))
        if name == "sbuf":
            self.need_arity(args, 0, name)
            return ([], "vsbuf()")
        if name == "sbuf_add":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "vsbuf_add(%s, %s)" % (a, b))
        if name in ("sbuf_len", "sbuf_str"):
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "v%s(%s)" % (name, a))
        if name in ("tcp_recv", "tcp_send"):
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "v%s(%s, %s)" % (name, a, b))
        if name == "chan":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "vchan_new(%s)" % a)
        if name == "join_all":
            self.need_arity(args, 0, name)
            return ([], "(vjoin_all(), vint(0))")
        if name == "args":
            self.need_arity(args, 0, name)
            return ([], "vargs()")
        if name == "exit":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "(vexit(%s), vint(0))" % a)
        if name == "eprint":
            pa, vec = self.args_vec(args)
            return (pa + ["veprint(%s);" % vec,
                          "vrc_dec(vvecv(%s));" % vec], "vint(0)")
        if name == "raise":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa + ["vraisev(%s);" % self._keep(args[0], a), self.err_bail()], "vnil()")
        if name == "own":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "vown(%s)" % a)
        if name in ("__arena_enter", "__arena_leave"):
            self.need_arity(args, 0, name)
            fn = "varena_enter" if name == "__arena_enter" else "varena_leave"
            return (["%s();" % fn], "vint(0)")
        if name == "time_ms":
            self.need_arity(args, 0, name)
            return ([], "vtime_ms()")
        if name == "sleep_ms":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "(vsleep_ms(%s), vint(0))" % a)
        if name in ("slice_of", "slice_of_ptr", "slice_len", "slice_get", "slice_set",
                    "str_from_ptr",
                        "slice_of_struct", "slice_struct_of"):
            return self.ex_slice(name, args)
        if name in ("lib_open", "lib_sym", "lib_close", "call"):
            return self.ex_plugin(name, args)
        if name == "ptr_null":
            self.need_arity(args, 0, name)
            return ([], "vint(0)")
        if name == "ptr_is_null":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "vbool((%s).as.ival == 0)" % a)
        if name == "ptr_offset":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "vint((int64_t)((char *)(%s).as.ival + (%s).as.ival))" % (a, b))
        if name == "ptr_read_f32":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "((vptr_live(%s)) ? vfloat((double)*(float *)(%s).as.ival) : vunwind)" % (a, a))
        if name == "ptr_write_f32":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "((vptr_live(%s)) ? ((*(float *)(%s).as.ival = (float)(%s).as.fval), vint(0)) : vunwind)"
                    % (a, a, b))
        if name == "ptr_read_i64":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "((vptr_live(%s)) ? vint(*(int64_t *)(%s).as.ival) : vunwind)" % (a, a))
        if name == "ptr_write_i64":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "((vptr_live(%s)) ? ((*(int64_t *)(%s).as.ival = (%s).as.ival), vint(0)) : vunwind)" % (a, a, b))
        if name == "ptr_read_f64":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "((vptr_live(%s)) ? vfloat(*(double *)(%s).as.ival) : vunwind)" % (a, a))
        if name == "ptr_write_f64":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "((vptr_live(%s)) ? ((*(double *)(%s).as.ival = (%s).as.fval), vint(0)) : vunwind)" % (a, a, b))
        if name == "ptr_read_ptr":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "((vptr_live(%s)) ? vint((int64_t)*(void **)(%s).as.ival) : vunwind)" % (a, a))
        if name in ("ptr_read_struct", "ptr_write_struct"):
            # A struct behind a pointer is the normal shape of a C API: the
            # handle is opaque and the library owns the memory. The type is the
            # last argument, a string literal, so both directions name the same
            # layout the extern struct declared.
            writing = name == "ptr_write_struct"
            self.need_arity(args, 3 if writing else 2, name)
            pa, a = self.ex(args[0])
            tname = args[-1]
            if not (isinstance(tname, tuple) and tname[0] == "str"):
                raise SyntaxError("%s wants a struct name as a string literal" % name)
            sname = tname[1]
            if sname not in self.structs:
                raise SyntaxError("%s: %s is not an extern struct" % (name, sname))
            if writing:
                pb, b = self.ex(args[1])
                return (pa + pb, "((vptr_live(%s)) ? ((*(%s *)(%s).as.ival = %s), vint(0)) : vunwind)"
                        % (a, _ffi_c(sname), a, self._val_to_c_struct(sname, b)))
            pre = list(pa)
            r = self.new_tmp("pr")
            pre.append("Val %s = vunwind;" % r)
            pre.append("if (vptr_live(%s)) { %s = %s; }"
                       % (a, r, self._c_struct_to_val(sname, "(*(%s *)(%s).as.ival)"
                                                      % (_ffi_c(sname), a))))
            return (pre, r)
        if name == "ptr_read_str":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "((vptr_live(%s)) ? vstr((const char *)(%s).as.ival) : vunwind)" % (a, a))
        if name == "ptr_write_ptr":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "((vptr_live(%s)) ? ((*(void **)(%s).as.ival = (void *)(%s).as.ival), vint(0)) : vunwind)" % (a, a, b))
        if name == "ptr_of_str":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "vint((int64_t)(%s).as.str)" % a)
        if name == "ptr_write_str":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "((vptr_live(%s)) ? ((*(const char **)(%s).as.ival = (const char *)(%s).as.str), vint(0)) : vunwind)" % (a, a, b))
        if name == "errno":
            self.need_arity(args, 0, name)
            return ([], "vint(errno)")
        if name == "strerror":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "vstr(strerror((int)(%s).as.ival))" % a)
        if name == "env_get":
            self.need_arity(args, 1, name)
            pa, a = self.ex(args[0])
            return (pa, "venv_get(%s)" % a)
        if name == "re_match":
            self.need_arity(args, 2, name)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (pa + pb, "vbool(vre_match(%s, %s))" % (a, b))
        if self.fn_key(name) in self.fns:
            return self.ex_direct(self.fn_key(name), args)
        if "." in name:
            return self.ex_dotted(name, args)
        if (not args and "." not in name and name[:1].isupper()
                and (name in self.sym or (self.cap is not None and name in self.cap[1]))):
            # A bare uppercase name parses as a zero-arg call; when it names a
            # local binding it reads the binding, as the interpreter does.
            return self.ex(("var", name))
        if name in self.externs:
            params, ret, _lib, varargs, _effects = self.externs[name]
            pa_all = []
            cargs = []
            for i, a in enumerate(args):
                if i < len(params):
                    ann = params[i][2] if isinstance(params[i], tuple) and len(params[i]) > 2 else None
                else:
                    ann = varargs
                if isinstance(ann, str) and ann.startswith("fn"):
                    self._check_cb_arg(ann, a, name, i + 1)
                    cargs.append("brat_cb_%s" % a[1])
                    continue
                if ann is not None and not (ann in _FFI_TYPES or ann in self.structs
                                            or ann.startswith("fn")
                                            or ann.startswith("ptr[")):
                    raise SyntaxError("unsupported FFI argument type `%s` in extern fn %s" % (ann, name))
                if isinstance(ann, str) and ann.startswith("ptr") \
                        and isinstance(a, tuple) and a[0] == "un" and a[1] == "&":
                    # `&x` is the out-parameter: the callee gets the address of
                    # the binding's storage, and the binding takes the value back
                    # as soon as the call returns.
                    self._in_out_arg = True
                    _p, _addr = self._out_addr(
                        a[2], ann[4:-1] if ann.startswith("ptr[") else None)
                    self._in_out_arg = False
                    pa_all.extend(_p)
                    cargs.append(_addr)
                    continue
                pa, sa = self.ex(a)
                pa_all.extend(pa)
                if ann in self.structs:
                    cargs.append(self._val_to_c_struct(ann, sa))
                    continue
                # FFI misuse is a BRAT error, never a segfault: check the tag
                # once into a temp, then read the payload.
                t = self.new_tmp("fa")
                pa_all.append("Val %s = %s;" % (t, sa))
                var_slot = i >= len(params) and ann == "ptr"
                if ann == "str":
                    ok = "%s.tag == V_STR" % t
                elif ann == "bool":
                    ok = "%s.tag == V_BOOL" % t
                elif var_slot:
                    # A `str` fills a variadic `..ptr` slot as `const char *`,
                    # which is what `%s` reads.
                    ok = "((%s.tag == V_INT && %s.as.ival >= 0) || %s.tag == V_STR)" % (t, t, t)
                elif ann in ("f64", "f32"):
                    ok = "(%s.tag == V_FLOAT || %s.tag == V_INT)" % (t, t)
                else:
                    ok = "%s.tag == V_INT" % t
                    if ann in _FFI_RANGE:
                        lo, hi = _FFI_RANGE[ann]
                        ok += " && %s.as.ival >= %s && %s.as.ival <= %s" % (t, lo, t, hi)
                    elif ann in ("ptr",) or (isinstance(ann, str) and ann.startswith("ptr[")):
                        ok += " && %s.as.ival >= 0" % t   # negative pointers never reach C
                pa_all.append('if (!(%s)) { vraise("ffi: arg %d of %s wants %s"); %s }'
                              % (ok, i + 1, name, ann or "value", self.err_bail()))
                if ann == "str":
                    cargs.append("%s.as.str" % t)
                elif ann == "bool":
                    cargs.append("%s.as.b" % t)
                elif var_slot:
                    cargs.append("(%s.tag == V_STR ? (void *)%s.as.str : (void *)%s.as.ival)"
                                 % (t, t, t))
                elif ann == "ptr" or (isinstance(ann, str) and ann.startswith("ptr[")):
                    cargs.append("(void *)%s.as.ival" % t)
                elif ann == "f64":
                    cargs.append("(%s.tag == V_INT ? (double)%s.as.ival : %s.as.fval)" % (t, t, t))
                elif ann == "f32":
                    # A CUDA/OpenGL kernel takes floats: the value narrows here,
                    # once, instead of every caller writing a cast in C terms.
                    cargs.append("(float)(%s.tag == V_INT ? (double)%s.as.ival : %s.as.fval)" % (t, t, t))
                else:
                    cargs.append("%s.as.ival" % t)
            _fl = self._out_flush_stmts() if self._out_binds else []
            self._out_binds = []
            call = "%s(%s)" % (name, ", ".join(cargs))
            if name in self.errno_externs:
                # Failure is NULL (ptr) or -1 (integers); errno is read right after.
                r = self.new_tmp("er")
                sentinel = "0" if ret == "ptr" else "-1"
                pa_all.append("errno = 0;")
                pa_all.append("int64_t %s = (int64_t)%s;" % (r, call))
                pa_all.extend(_fl)
                pa_all.append('if (%s == %s) { vraise_errno("%s"); %s }'
                              % (r, sentinel, name, self.err_bail()))
                return (pa_all + _fl, "vint(%s)" % r)
            if ret == "bool":
                return (pa_all + _fl, "vbool(%s)" % call)
            if ret in _FFI_INT_RESULT:
                # A scalar result goes into a temp before the flush, so a raise
                # from the flush still leaves a value the call produced. The cast
                # sign-extends the width the header declared: a `short` return
                # that holds -1 arrives as -1, not as 65535.
                r = self.new_tmp("ir")
                pa_all.append("int64_t %s = (int64_t)%s;" % (r, call))
                pa_all.extend(_fl)
                return (pa_all, "vint(%s)" % r)
            if ret in ("u64", "usize"):
                r = self.new_tmp("ur")
                pa_all.append("uint64_t %s = %s;" % (r, call))
                pa_all.extend(_fl)
                pa_all.append('if (%s > 9223372036854775807ULL) { vraise("ffi: result of %s does not fit i64"); %s }'
                              % (r, name, self.err_bail()))
                return (pa_all + _fl, "vint((int64_t)%s)" % r)
            if ret == "str":
                t = self.new_tmp("fr")
                pa_all.append("Val %s = vstr_ffi(%s);" % (t, call))
                pa_all.extend(_fl)
                pa_all.append("if (verr_pending()) %s" % self.err_bail())
                return (pa_all + _fl, t)
            if ret == "ptr" or (isinstance(ret, str) and ret.startswith("ptr[")):
                return (pa_all + _fl, "vint((int64_t)(%s))" % call)
            if ret == "f64":
                return (pa_all + _fl, "vfloat(%s)" % call)
            if ret == "f32":
                return (pa_all + _fl, "vfloat((double)%s)" % call)
            if ret in self.structs:
                # The record is read field by field, so the call goes into a temp
                # first: inlined into every field access it would be called once
                # per field -- three calls, three side effects, and a different
                # record in every field of the one value.
                rt = self.new_tmp("cs")
                pa_all.append("%s %s = %s;" % (_ffi_c(ret), rt, call))
                return (pa_all + _fl, self._c_struct_to_val(ret, rt))
            # `-> none` and an omitted arrow are the same declaration; the
            # checker reads them as one, so the emitter has to as well.
            if ret is None or ret == "none":
                return (pa_all + ["%s;" % call] + _fl, "vnil()")
            raise SyntaxError("unsupported FFI return type `%s` in extern fn %s" % (ret, name))
        if name in self.sym:
            return self.ex_vcall(name, args)
        if self.cap is not None and name in self.cap[1]:
            pa, vec = self.args_vec(args)
            return self.call_result(
                args, pa, vec,
                "vcall_any((*map_get(%s, \"%s\")), %s)" % (self.cap[0], name, vec))
        raise RuntimeError("unresolved call " + name)

    def ex_tag(self, tag, args):
        t = self.new_tmp("g")
        pre = ["Vec *%s = vvec();" % t]
        for a in args:
            pa, sa = self.ex(a)
            pre.extend(pa)
            pre.append("vec_push(%s, %s);" % (t, self._keep(a, sa)))
        return (pre, 'vtagv("%s", %s)' % (tag, t))

    def ex_dotted(self, name, args):
        parts = name.split(".")
        head, meth = parts[0], parts[-1]
        if not (head[:1].isupper() and len(parts) == 2 and head not in self.sym
                and not (self.cap is not None and head in self.cap[1])):
            node = ("var", head)
            for f in parts[1:-1]:
                node = ("field", node, f)
            po, so = self.ex(node)
            if ((meth in _VBUILTIN_DIRECT or (meth in _STRVEC_METH and meth in _MAP_METH))
                    and meth not in self.user_meths):
                pa, vec = self.args_vec(args)
                # No builtin hands its receiver back, so the receiver is not
                # shared: counting it here left a reference nobody released.
                return self.call_result(
                    args, po + pa, vec,
                    'vbuiltin(%s, "%s", %s)' % (so, _clit(meth), vec))
            if meth in _STRVEC_METH and meth not in self.user_meths:
                return self.ex_strvec_meth(so, po, meth, args)
            if meth in _MAP_METH and meth not in self.user_meths:
                return self.ex_map_meth(so, po, meth, args)
            pa, vec = self.args_vec(args)
            return self.call_result(
                args, po + pa, vec,
                'vcall_method(%s, "%s", %s)' % (so, _clit(meth), vec))
        if head[:1].isupper() and len(parts) == 2:
            entry = self.fns.get(head)
            if isinstance(entry, tuple) and entry and entry[0] == "ctor" and entry[2] == 0:
                tag = entry[1]
                base = "vnil()" if tag == "none" else 'vtagv("%s", vvec())' % _clit(tag)
                pa, vec = self.args_vec(args)
                return self.call_result(
                    args, pa, vec,
                    'vcall_method(%s, "%s", %s)' % (base, _clit(meth), vec))
            key = head + "." + meth
            if key in self.fns and isinstance(self.fns[key], tuple) and len(self.fns[key]) == 2:
                params = self.fns[key][0]
                names = [p[0] if isinstance(p, tuple) else p for p in params]
                if names and names[0] == "self":
                    raise RuntimeError(key + " needs instance, use value.method()")
                args = self.pad_defaults(params, args, key)
                pa, vec = self.args_vec(args)
                return self.call_result(
                    args, pa, vec, "%s(NULL, %s)" % (self.cfn("meth:" + key), vec))
        node = ("var", head)
        for f in parts[1:-1]:
            node = ("field", node, f)
        po, so = self.ex(node)
        pa, vec = self.args_vec(args)
        return self.call_result(
            args, po + pa, vec,
            'vcall_method(%s, "%s", %s)' % (so, _clit(meth), vec))

    def pad_defaults(self, params, args, name):
        if len(args) == len(params):
            return args
        if len(args) < len(params):
            tail = params[len(args):]
            if all(isinstance(p, tuple) and len(p) > 3 and p[3] is not None for p in tail):
                return list(args) + [p[3] for p in tail]
        raise RuntimeError(name + " arity")

    def ex_direct(self, name, args):
        entry = self.fns[name]
        if isinstance(entry, tuple) and entry and entry[0] == "ctor":
            _, tag, arity = entry
            if len(args) != arity:
                raise RuntimeError(name + " arity")
            if tag == "none":
                return ([], "vnil()")
            return self.ex_tag(tag, args)
        params = entry[0]
        args = self.pad_defaults(params, args, name)
        pa, vec = self.args_vec(args)
        return self.call_result(
            args, pa, vec,
            "%s(NULL, %s)" % (self.cfn("fn:" + self.fn_key(name)), vec))

    def ex_vcall(self, name, args):
        pa, vec = self.args_vec(args)
        return self.call_result(
            args, pa, vec, "vcall_any(%s, %s)" % (self.var_ref(name), vec))

    def ex_mcall(self, e):
        obj_e, meth, args = e[1], e[2], e[3]
        if (isinstance(obj_e, tuple) and len(obj_e) == 3 and obj_e[0] == "call"
                and not obj_e[2] and isinstance(obj_e[1], str) and "." not in obj_e[1]
                and obj_e[1][:1].isupper()
                and (obj_e[1] in self.sym or (self.cap is not None and obj_e[1] in self.cap[1]))):
            # A bare uppercase local parses as a zero-arg call; `C.len()` reads
            # the local itself, the same way a dotted spelling did.
            obj_e = ("var", obj_e[1])
        if meth in ("map", "filter") and len(args) == 1:
            return self.ex_mapfilter(obj_e, meth, args[0])
        if meth in ("any", "all", "find") and len(args) == 1:
            return self.ex_hof(obj_e, meth, args[0])
        if meth == "fold" and len(args) == 2:
            return self.ex_hof(obj_e, meth, args[1], args[0])
        if meth == "sort_by" and len(args) == 1:
            return self.ex_hof(obj_e, meth, args[0])
        path = _name_path(obj_e)
        if path is not None and path.split(".")[0] not in self.sym and (
                self.cap is None or path.split(".")[0] not in self.cap[1]):
            # Static path: `log.info`, `Type.method`, `Type.Variant` -- the callee
            # is resolved by name, so no receiver storage is read.
            return self.ex_call(("call", path + "." + meth, args))
        po, so = self.ex(obj_e)
        # `"a,b".split(",")` builds a container nothing else names: the receiver is
        # released once the call returns, or every chain leaks its first link.
        # Only methods whose result is a new object qualify: `[[3],[1]].min()`
        # answers with an element *inside* the receiver, so dropping it would
        # free what the result points at.
        recv = None
        if ((meth in _FRESH_RESULT or meth in _PART_RESULT or meth in self.user_meths)
                and _dec_able(obj_e, self.heap_returns())):
            recv = self.new_tmp("r")
            po = po + ["Val %s = (%s);" % (recv, so)]
            so = recv

        def done(res):
            pre, val = res
            if recv is None:
                return res
            t = self.new_tmp("c")
            lines = ["Val %s = (%s);" % (t, val)]
            drop = "vdrop_recv_part" if meth in _PART_RESULT else "vdrop_recv"
            return (pre + lines + ["%s(%s, %s);" % (drop, recv, t)], t)

        if ((meth in _VBUILTIN_DIRECT or (meth in _STRVEC_METH and meth in _MAP_METH))
                and meth not in self.user_meths):
            pa, vec = self.args_vec(args)
            return done(self.call_result(
                args, po + pa, vec,
                'vbuiltin(%s, "%s", %s)' % (so, _clit(meth), vec)))
        if meth in _STRVEC_METH and meth not in self.user_meths:
            return done(self.ex_strvec_meth(so, po, meth, args))
        if meth in _MAP_METH and meth not in self.user_meths:
            return done(self.ex_map_meth(so, po, meth, args))
        pa, vec = self.args_vec(args)
        return done(self.call_result(
            args, po + pa, vec,
            'vcall_method(%s, "%s", %s)' % (so, _clit(meth), vec)))

    def ex_map_meth(self, so, po, meth, args):
        if meth == "keys":
            self.need_arity(args, 0, meth)
            return (po, "vmap_keys(%s)" % so)
        if meth == "items":
            self.need_arity(args, 0, meth)
            return (po, "vmap_items(%s)" % so)
        if meth == "vals":
            self.need_arity(args, 0, meth)
            return (po, "vmap_vals(%s)" % so)
        if meth == "copy":
            self.need_arity(args, 0, meth)
            return (po, "vcopy(%s)" % so)
        if meth == "get_or":
            self.need_arity(args, 2, meth)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (po + pa + pb, "vmap_get_or(%s, %s, %s)" % (so, a, b))
        if meth in ("get", "has", "del"):
            self.need_arity(args, 1, meth)
            pa, a = self.ex(args[0])
            fn = {"get": "vmap_getv", "has": "vmap_has", "del": "vmap_del"}[meth]
            return (po + pa, "%s(%s, %s)" % (fn, so, a))
        self.need_arity(args, 2, meth)
        pa, a = self.ex(args[0])
        pb, b = self.ex(args[1])
        return (po + pa + pb, "vmap_setv(%s, %s, %s)"
                % (so, self._keep(args[0], a), self._keep(args[1], b)))

    def release_lit_args(self, pre, nodes, vals, call):
        """`vzip(recv, ["a", "b"])`: a container literal built for an argument has
        nobody to release it once the call has read it. Bound to a temp that holds
        the answer, so the literal goes right after the call, never before it.
        """
        lits = []
        for node, val in zip(nodes, vals):
            if isinstance(node, tuple) and node and node[0] in ("vec", "maplit", "struct"):
                tmp = _container_tmp(val)
                if tmp is not None:
                    lits.append(tmp)
        if not lits:
            return (pre, call)
        t = self.new_tmp("v")
        return (pre + ["Val %s = (%s);" % (t, call)]
                + ["vrc_dec(vvecv(%s));" % l for l in lits], t)

    def ex_strvec_meth(self, so, po, meth, args):
        simple = _SV_SIMPLE.get(meth)
        if simple is not None:
            fn, arity = simple
            self.need_arity(args, arity, meth)
            pre = po
            cargs = []
            for a in args:
                pa, sa = self.ex(a)
                pre = pre + pa
                cargs.append(sa)
            if meth == "ins":
                cargs[1] = self._keep(args[1], cargs[1])
            call = "%s(%s%s)" % (fn, so, "".join(", " + c for c in cargs))
            return self.release_lit_args(pre, list(args), cargs, call)
        if meth == "split":
            if len(args) > 1:
                raise RuntimeError("split arity")
            if not args:
                return (po + ["Vec *_sp = vvec();"], "vsplit(%s, vvecv(_sp))" % so)
            pa, a = self.ex(args[0])
            return (po + pa + [
                "if ((%s).tag == V_STR && !(%s).as.str[0]) { vraise(\"empty separator\"); %s }"
                % (a, a, self.err_bail())],
                "vsplit(%s, %s)" % (so, a))
        if meth == "lines":
            self.need_arity(args, 0, meth)
            return (po, "vlines(%s)" % so)
        if meth in ("trim", "lower", "upper"):
            self.need_arity(args, 0, meth)
            if meth == "trim":
                return (po, "vtrim(%s)" % so)
            return (po, "vcase(%s, %d)" % (so, 1 if meth == "upper" else 0))
        if meth in ("starts_with", "ends_with", "contains"):
            self.need_arity(args, 1, meth)
            pa, a = self.ex(args[0])
            flags = {"starts_with": "1, 0", "ends_with": "0, 1"}.get(meth, "0, 0")
            return self.release_lit_args(po + pa, [args[0]], [a],
                                         "vbool(vaffix(%s, %s, %s))" % (so, a, flags))
        if meth == "parse_int":
            self.need_arity(args, 0, meth)
            return (po, "vparse_int(%s)" % so)
        if meth == "replace":
            self.need_arity(args, 2, meth)
            pa, a = self.ex(args[0])
            pb, b = self.ex(args[1])
            return (po + pa + pb, "vreplace(%s, %s, %s)" % (so, a, b))
        if meth in ("first", "last", "sort", "rev"):
            self.need_arity(args, 0, meth)
            fn = {"first": "vfirst", "last": "vlast", "sort": "vsort", "rev": "vrev"}[meth]
            return (po, "%s(%s)" % (fn, so))
        if meth == "join":
            self.need_arity(args, 1, meth)
            pa, a = self.ex(args[0])
            return (po + pa, "vjoin(%s, %s)" % (so, a))
        if meth == "push":
            self.need_arity(args, 1, meth)
            pa, a = self.ex(args[0])
            return (po + pa, "vpush(%s, %s)" % (so, self._keep(args[0], a)))
        if meth == "pop":
            self.need_arity(args, 0, meth)
            return (po, "vpop(%s)" % so)
        raise RuntimeError("method " + meth)

    def ex_hof(self, obj_e, meth, arg, first=None):
        po, so = self.ex(obj_e)
        entry = self.fns.get(arg[1]) if (isinstance(arg, tuple) and arg and arg[0] == "var") else None
        if isinstance(entry, tuple) and len(entry) == 2:
            code = self.fn_code("fn:" + arg[1])
            t = self.new_tmp("c")
            pre = po + [
                "Vec *_cb%d = vvec();" % self.tmp,
                "vec_push(_cb%d, vint(%d));" % (self.tmp, code),
                "vec_push(_cb%d, vstruct(vmap(\"\")));" % self.tmp,
                "Val %s = vtagv(\"closure\", _cb%d);" % (t, self.tmp),
            ]
            fnv = t
        else:
            pa, sa = self.ex(arg)
            pre = po + pa
            fnv = sa
        if first is not None:
            pf, sf = self.ex(first)
            pre = pre + pf
            return (pre, 'vfold(%s, %s, %s)' % (so, sf, fnv))
        crt = {"any": 'vpred(%s, %s, "any")', "all": 'vpred(%s, %s, "all")',
               "find": 'vpred(%s, %s, "find")', "sort_by": "vsort_by(%s, %s)"}[meth]
        return (pre, crt % (so, fnv))

    def ex_mapfilter(self, obj_e, meth, arg):
        po, so = self.ex(obj_e)
        entry = self.fns.get(arg[1]) if (isinstance(arg, tuple) and arg and arg[0] == "var") else None
        if isinstance(entry, tuple) and len(entry) == 2:
            code = self.fn_code("fn:" + arg[1])
            t = self.new_tmp("c")
            pre = po + [
                "Vec *_cb%d = vvec();" % self.tmp,
                "vec_push(_cb%d, vint(%d));" % (self.tmp, code),
                "vec_push(_cb%d, vstruct(vmap(\"\")));" % self.tmp,
                "Val %s = vtagv(\"closure\", _cb%d);" % (t, self.tmp),
            ]
            return (pre, 'vmap_with(%s, %s, "%s")' % (so, t, _clit(meth)))
        pa, sa = self.ex(arg)
        # vmap_with releases the closure it is given, so a binding the caller
        # still owns gets one more reference and a temporary is handed over as it is.
        return (po + pa, 'vmap_with(%s, %s, "%s")' % (so, self._keep(arg, sa), _clit(meth)))

    def ex_parfor(self, e):
        """`par for x in v reduce(op, unit) { body }` -> one vparfor call.

        The body becomes a closure of one parameter, the operation becomes a
        second closure, and the runtime fans out and folds in index order. The
        checker has already proved both are pure, so nothing here describes an
        order the program could observe.
        """
        _, var, it_e, op, unit_e, body = e
        pi, si = self.ex(it_e)
        pu, su = self.ex(unit_e)
        code = self.fn_code("clo:%d" % len(self.clos))
        capnames = sorted(((set(self.sym) & self.assigned)
                           | (set(self.cap[1]) if self.cap is not None else set()))
                          - {var})
        self.clos.append(([(var, "ref", None, None)], body, capnames))
        m = self.new_tmp("m")
        body_cb = self.tmp
        self.new_tmp("c")
        op_cb = self.tmp
        opc = self.new_tmp("c")
        pre = pi + pu + ["Map *_cm%d = vmap(\"\");" % body_cb]
        for name in capnames:
            pre.extend(self._capture("_cm%d" % body_cb, name))
        pre.append("Vec *_cb%d = vvec();" % body_cb)
        pre.append("vec_push(_cb%d, vint(%d));" % (body_cb, code))
        pre.append("{ Val _cv = vstruct(_cm%d); vec_push(_cb%d, _cv); }"
                   % (body_cb, body_cb))
        pre.append("Val %s = vtagv(\"closure\", _cb%d);" % (m, body_cb))
        pre.append("Vec *_cb%d = vvec();" % op_cb)
        pre.append("vec_push(_cb%d, vint(%d));" % (op_cb, self.fn_code("fn:" + op)))
        pre.append("vec_push(_cb%d, vstruct(vmap(\"\")));" % op_cb)
        pre.append("Val %s = vtagv(\"closure\", _cb%d);" % (opc, op_cb))
        t = self.new_tmp("pf")
        pre.append("Val %s = vparfor(%s, %s, %s, %s);" % (t, si, m, su, opc))
        return (pre, t)

    def ex_closure(self, e):
        _, name, params, body = e
        code = self.fn_code("clo:%d" % len(self.clos))
        outer = set(self.cap[1]) if self.cap is not None else set()
        base = set(self.sym) & self.assigned
        capnames = sorted(base | outer | ({name} if name else set()))
        self.clos.append((params, body, capnames))
        t = self.new_tmp("m")
        pre = ['Map *_cm%d = vmap("");' % self.tmp]
        for var in sorted((base | outer) - ({name} if name else set())):
            pre.extend(self._capture("_cm%d" % self.tmp, var))
        pre.append("Vec *_cb%d = vvec();" % self.tmp)
        pre.append("vec_push(_cb%d, vint(%d));" % (self.tmp, code))
        pre.append("Val %s = vtagv(\"closure\", _cb%d);" % (t, self.tmp))
        pre.append("{ Val _cv = vstruct(_cm%d); vec_push(_cb%d, _cv); }" % (self.tmp, self.tmp))
        if name:
            pre.append('map_set(_cm%d, "%s", %s);' % (self.tmp, name, t))
        return (pre, t)

    def st(self, s, ind):
        k = s[0]
        if k == "unsafe":
            self.emit_block(s[1], ind)
            return
        if k == "let":
            _, name, e = s[0], s[1], s[2]
            if name in self.global_lets and self.fin == "__fin_top":
                pre, v = self.ex(e)
                for p in pre:
                    self.out.append(ind + p)
                self.out.append(ind + "%s = (%s);" % (_cname(name), self._keep(e, v)))
                self.emit_err_check(ind)
                self.assigned.add(name)
                return
            pre, v = self.ex(e)
            for p in pre:
                self.out.append(ind + p)
            if "." in name:
                head, rest = name.split(".", 1)
                if head in self.sym:
                    self.out.append(ind + 'vfieldset(%s, "%s", %s);'
                                     % (self.sym[head].cn, rest, self._keep(e, v)))
                    self.emit_err_check(ind)
                    return
            own = self._own_of(e)
            fresh = _dec_able(e, self.heap_returns())
            root = self._place_root(e)
            # A call hands its result over: the reference the callee built is the
            # one the binding stores, and a failed call hands over its error the
            # same way. A method call may hand back its receiver, so its alias
            # gets a reference of its own.
            if not fresh and not (isinstance(e, tuple) and e and e[0] == "call"):
                v = "vrc_inc(%s)" % v
            if (name in self.reclaim or name in self.relet) and v.startswith("vlit("):
                v = "vstr(" + v[5:]  # a release needs a real rc header
                own = "owned"
            if isinstance(e, tuple) and e and e[0] == "move":
                # Only `move x` empties `x`. A projection move leaves the parent
                # owning everything else, and marking the root moved here is what
                # kept the parent from ever being released.
                if isinstance(e[1], tuple) and e[1] and e[1][0] == "var":
                    src = self._place_root(e)
                    if src is not None:
                        self.moved.add(src)
            d = self.sym.get(name)
            relet = (name in self.relet and d is not None
                     and name not in self.moved and name not in self.dropped
                     and self._place_root(e) != name)
            # What the binding promises about what it holds: a `ptr[T]` names
            # the element `p[i]` steps by, a `slice[T]` (or a window built right
            # here) names the layout `slice_get` reads a record with.
            _ann = s[5] if len(s) > 5 else None
            _prel, _selem = None, None
            if isinstance(_ann, str) and _ann.startswith("ptr[") and _ann.endswith("]"):
                _prel = _ann[4:-1].strip()
            elif isinstance(_ann, str) and _ann.startswith("slice[") and _ann.endswith("]"):
                _selem = _ann[6:-1].strip()
            elif isinstance(e, tuple) and e and e[0] == "call":
                if e[1] == "slice_of_struct" and len(e[2]) == 3 \
                        and isinstance(e[2][2], tuple) and e[2][2][0] == "str":
                    _selem = e[2][2][1]
                else:
                    # A callee that returns `ptr[T]` has declared the element:
                    # `let p = node_new(...)` is the whole shape of a C API.
                    _ext = self.externs.get(e[1])
                    _ret = _ext[1] if _ext else None
                    if isinstance(_ret, str) and _ret.startswith("ptr[") and _ret.endswith("]"):
                        _prel = _ret[4:-1].strip()
            if _selem is None and getattr(self, "_last_slice_elem", None) is not None:
                _selem = self._last_slice_elem
            self._last_slice_elem = None
            if isinstance(e, tuple) and e and e[0] == "un" and e[1] == "&" \
                    and not (len(s) > 3 and s[3]):
                raise SyntaxError("`let p = &x` needs `mut`: the callee writes through it")
            cn = self.var_def(name, own, _prel, _selem)[1]
            self.assigned.add(name)
            if relet:
                # after the RHS: it may still read the previous pass value
                tmp = self.new_tmp("v")
                self.out.append(ind + "Val %s = (%s);" % (tmp, v))
                self.out.append(ind + "vrc_dec(%s);" % d.cn)
                self.out.append(ind + "%s = %s;" % (cn, tmp))
            else:
                self.out.append(ind + "%s = (%s);" % (cn, v))
            self.emit_err_check(ind)
            if name.startswith("@") and not fresh:
                # A match scrutinee temp holds a counted alias of the value it
                # matched; the block that matched gives the reference back.
                self.alias_temps.add(name)
        elif k == "set":
            tgt = s[1]
            d = self.sym.get(tgt)
            self_concat = (isinstance(s[2], tuple) and s[2] and s[2][0] == "bin"
                           and s[2][1] == "+" and isinstance(s[2][2], tuple)
                           and s[2][2][0] == "var" and s[2][2][1] == tgt)
            drop = []
            if self_concat:
                pre, v = self.ex(s[2][3])
                for p in pre:
                    self.out.append(ind + p)
                drop = [d for d in (_owned_temp(s[2][3], v),) if d]
                # `s = s + f()`: an append must not read the placeholder value a
                # failed `f` left behind; the error goes to the enclosing try.
                if pre and _may_raise(s[2][3]):
                    self.emit_err_check(ind)
                own = "owned"
                cn_tmp = self.var_def(tgt, own)[1]
                v = "vadd_self(%s, %s)" % (cn_tmp, v)
            else:
                pre, v = self.ex(s[2])
                for p in pre:
                    self.out.append(ind + p)
                drop = [d for d in (_owned_temp(s[2], v),) if d]
                # `n = n + f()`: see the append case above.
                if pre and _may_raise(s[2]):
                    self.emit_err_check(ind)
                own = self._own_of(s[2])
                if tgt in self.reclaim and v.startswith("vlit("):
                    v = "vstr(" + v[5:]  # a release needs a real rc header
                    own = "owned"
            if tgt in self.reclaim and v.startswith("vlit(") and not self_concat:
                v = "vstr(" + v[5:]
                own = "owned"
            release = (tgt in self.reclaim and d is not None
                       and tgt not in self.moved and tgt not in self.dropped
                       and (self._place_root(s[2]) != tgt if not self_concat else True))
            if not _dec_able(s[2], self.heap_returns()):
                # Storing an alias: the binding takes its own reference, the
                # same rule as `let`.
                v = "vrc_inc(%s)" % v
            cn = self.var_def(tgt, own)[1]
            self.assigned.add(tgt)
            # A store must not land when the RHS raised: the target keeps its old
            # value and the enclosing try sees the error, as in the interpreter.
            if self_concat or release or _may_raise(s[2]):
                tmp = self.new_tmp("v")
                self.out.append(ind + "Val %s = (%s);" % (tmp, v))
                self.emit_err_check(ind, drop)
                # after the RHS so a hot store loop pays one dec per store, not per
                # element; never for an append, `vadd_self` hands back the receiver
                if release and not self_concat:
                    self.out.append(ind + "vrc_dec(%s);" % d.cn)
                self.out.append(ind + "%s = %s;" % (cn, tmp))
                if self_concat:
                    # `vadd_self` copies the right side in and keeps its own
                    # reference, so the temp this statement built dies here.
                    for t in drop:
                        self.out.append(ind + "vdrop_temp(%s);" % t)
            else:
                self.out.append(ind + "%s = (%s);" % (cn, v))
                self.emit_err_check(ind)
        elif k == "setidx":
            _sp = self._ptr_elem(s[1])
            if _sp is not None:
                self._ptr_setidx(_sp, s[1], s[2], s[3], ind)
                return
            pb, sb = self.ex(s[1])
            pi, si = self.ex(s[2])
            pv, sv = self.ex(s[3])
            for p in pb + pi + pv:
                self.out.append(ind + p)
            # `m["a"]["b"] = 1` indexes into a temporary row; that reference is
            # nobody's once the store is done (`vat_owned` made every index read
            # an owned value), so it is held in a temp and released.
            if _dec_able(s[1], self.heap_returns()):
                r = self.new_tmp("r")
                self.out.append(ind + "Val %s = (%s);" % (r, sb))
                self.out.append(ind + "vset_at(%s, %s, %s);"
                                % (r, self._keep(s[2], si), self._keep(s[3], sv)))
                self.out.append(ind + "vdrop_temp(%s);" % r)
            else:
                self.out.append(ind + "vset_at(%s, %s, %s);"
                                % (sb, self._keep(s[2], si), self._keep(s[3], sv)))
            self.emit_err_check(ind)
            base = s[1]
            if isinstance(base, tuple) and base and base[0] == "var":
                self.assigned.add(base[1])
        elif k == "setfield":
            pb, sb = self.ex(s[1])
            pv, sv = self.ex(s[3])
            for p in pb + pv:
                self.out.append(ind + p)
            # Same receiver rule as a field read: `v[0].x = 5` stores through a
            # reference nobody owns, so it is held and released like `m[k][j] = v`.
            if _dec_able(s[1], self.heap_returns()):
                r = self.new_tmp("r")
                self.out.append(ind + "Val %s = (%s);" % (r, sb))
                self.out.append(ind + 'vfieldset(%s, "%s", %s);'
                                % (r, s[2], self._keep(s[3], sv)))
                self.out.append(ind + "vdrop_temp(%s);" % r)
            else:
                self.out.append(ind + 'vfieldset(%s, "%s", %s);'
                                % (sb, s[2], self._keep(s[3], sv)))
            self.emit_err_check(ind)
        elif k == "return":
            pre, v = self.ex(s[1])
            for p in pre:
                self.out.append(ind + p)
            self.emit_err_check(ind)
            for sc in reversed(self.scopes):
                self.out.append(ind + "vscope_end(vint(%s));" % sc)
            # The value leaves with the caller; the exit drops must not free it.
            # `a[i]` already hands back a reference the holder owns (vat_owned),
            # so the container is dropped at exit like any other local; lifting
            # the part out and stealing it was the pre-vat_owned shape.
            owned_part = _is_index_read(s[1])
            root = self._place_root(s[1])
            if root is not None and not owned_part and root in self.sym:
                if self._is_part_of_local(s[1], root):
                    # `return v[0]` hands out a part of a local container: the part
                    # is lifted into a temp, then the container goes -- the exit
                    # drops are about to skip it as the value's root.
                    t = self.new_tmp("c")
                    self.out.append(ind + "Val %s = (%s);" % (t, v))
                    self.out.append(ind + "vdrop_recv_part(%s, %s);"
                                    % (self.sym[root].cn, t))
                    self.dropped.add(root)
                    v = t
                else:
                    if root in getattr(self, "script_lets", ()):
                        # A script-level binding outlives the frame that returns
                        # it, so the caller gets a reference of its own; without
                        # the inc nobody may release the result.
                        v = "vrc_inc(%s)" % v
                    self.shared.add(root)
            if root in self.cur_params and not owned_part:
                # `return x` hands back storage owned by the
                # argument vector, which the caller releases: the value needs a
                # reference of its own or the caller holds freed memory.
                v = "vrc_inc(%s)" % v
            handed = self._place_root(s[1])
            if handed is not None and (_is_index_read(s[1])
                                       or self._is_part_of_local(s[1], handed)):
                handed = None
            self._emit_fn_drops(ind, handed, at_return=True, keep=_refs(s[1]))
            if self.fin == "__fin_top":
                self.out.append(ind + '{ fprintf(stderr, "unhandled: "); vprint_err(%s); fprintf(stderr, "\\n"); return 1; }' % v)
            else:
                self.out.append(ind + "{ __ret = (%s); goto %s; }" % (v, self.fin))
        elif k == "expr":
            pre, v = self.ex(s[1])
            for p in pre:
                self.out.append(ind + p)
            if _dec_able(s[1], self.heap_returns()):
                # A statement whose value nobody keeps owns it. `vadd(vlit("a"),
                # vlit("b"))` in an `assert` builds a string, and leaving it as
                # `(void)(...)` leaked it once per evaluation; the temp is
                # released before the error check, so a raise cannot skip it.
                t = self.new_tmp("ex")
                self.out.append(ind + "Val %s = (%s);" % (t, v))
                self.out.append(ind + "vdrop_temp(%s);" % t)
            else:
                self.out.append(ind + "(void)(%s);" % v)
            self.emit_err_check(ind)
        elif k == "if":
            _, cond, then, els = s
            pc, c = self.ex(cond)
            for p in pc:
                self.out.append(ind + p)
            self.emit_err_check(ind)
            self.out.append(ind + "if (vis_true(%s)) {" % c)
            self.emit_block(then, ind + "  ")
            if not els:
                self.out.append(ind + "}")
                return
            if isinstance(els, tuple):
                # Braced even for a single statement: the statement may have
                # pre-lines of its own, and a declaration cannot follow a bare
                # `else` ("expected expression before `volatile`").
                self.out.append(ind + "} else {")
                self.st(els, ind + "  ")
                self.out.append(ind + "}")
                return
            self.out.append(ind + "} else {")
            self.emit_block(els, ind + "  ")
            self.out.append(ind + "}")
        elif k == "while":
            pc, c = self.ex(s[1])
            self.out.append(ind + "while (1) {")
            for p in pc:
                self.out.append(ind + "  " + p)
            self.emit_err_check(ind + "  ")
            self.out.append(ind + "  if (!vis_true(%s)) break;" % c)
            self.loop_exit_depths.append((len(self.scopes), len(self.dfb)))
            self.emit_block(s[2], ind + "  ")
            self.loop_exit_depths.pop()
            self.out.append(ind + "}")
            self.emit_loop_drops(s[2], ind)
        elif k == "forrange":
            _, vn, se, ee, body = s
            pa, a = self.ex(se)
            pb, b = self.ex(ee)
            for p in pa + pb:
                self.out.append(ind + p)
            self.emit_err_check(ind)
            lo = self.new_tmp("lo")
            hi = self.new_tmp("hi")
            kk = self.new_tmp("k")
            self.out.append(ind + "int64_t %s = vlong(%s);" % (lo, a))
            self.out.append(ind + "int64_t %s = vlong(%s);" % (hi, b))
            self.emit_err_check(ind)
            self.out.append(ind + "for (int64_t %s = %s; %s < %s; ++%s) {" % (kk, lo, kk, hi, kk))
            cn = self.var_def(vn)[1]
            self.out.append(ind + "  %s = vint(%s);" % (cn, kk))
            self.assigned.add(vn)
            self.loop_exit_depths.append((len(self.scopes), len(self.dfb)))
            self.emit_block(body, ind + "  ")
            self.loop_exit_depths.pop()
            self.out.append(ind + "}")
        elif k == "foriter":
            _, vn, ve, body = s
            pv, v = self.ex(ve)
            for p in pv:
                self.out.append(ind + p)
            self.emit_err_check(ind)
            vv = self.new_tmp("v")
            kk = self.new_tmp("k")
            src = self.new_tmp("v")
            self.out.append(ind + "Val %s = (%s);" % (src, v))
            self.out.append(ind + "Vec *%s = viter_vec(%s);" % (vv, src))
            self.emit_err_check(ind)
            self.out.append(ind + "for (int64_t %s = 0; %s < %s->len; ++%s) {" % (kk, kk, vv, kk))
            cn = self.var_def(vn)[1]
            self.out.append(ind + "  %s = %s->items[%s];" % (cn, vv, kk))
            self.assigned.add(vn)
            self.loop_exit_depths.append((len(self.scopes), len(self.dfb)))
            self.emit_block(body, ind + "  ")
            self.loop_exit_depths.pop()
            self.out.append(ind + "}")
            self.out.append(ind + "viter_done(%s, %s);" % (src, vv))
            # `for x in [10, 20, 30]` builds a vector nobody else holds; the
            # loop borrowed it, so the reference that built it goes here.
            if _dec_able(ve, self.heap_returns()):
                self.out.append(ind + "vdrop_temp(%s);" % src)
        elif k == "break":
            self.emit_loop_exit("break", ind)
        elif k == "continue":
            self.emit_loop_exit("continue", ind)
        elif k == "try":
            self.try_n += 1
            n = self.try_n
            ename = s[3] if len(s) > 3 else None
            catch_lbl = "__catch%d" % n
            after_lbl = "__after_try%d" % n
            saved_goto = self.err_goto
            saved_base = self.err_base
            base = self.new_tmp("dfb")
            self.out.append(ind + "{")
            self.out.append(ind + "  int64_t %s = vdfsp;" % base)
            self.err_goto = catch_lbl
            self.err_base = base
            self.emit_block(s[1], ind + "  ")
            self.err_goto = saved_goto
            self.err_base = saved_base
            self.out.append(ind + "  goto %s;" % after_lbl)
            self.out.append(ind + "%s:;" % catch_lbl)
            for _nm in sorted(_try_let_names(s[1])):
                # The try body's own bindings cannot be live here and their drop
                # at the end of the block is unreachable from this path. Released
                # through `vrelease`: it leaves the error value alone, which is
                # what the channel is about to free.
                self.out.append(ind + "  vrelease(%s);" % _cname(_nm))
                self.out.append(ind + "  %s = vnil();" % _cname(_nm))
            if ename:
                cn = self.var_def(ename)[1]
                self.out.append(ind + "  %s = verr_catch_part();" % cn)
                self.assigned.add(ename)
            else:
                self.out.append(ind + "  (void)verr_take();")
            self.emit_block(s[2], ind + "  ")
            self.out.append(ind + "  verr_catch_end();")
            if ename:
                # The error belongs to the error channel, which frees it here. The
                # binding must not be released again: `catch e { }` followed by a
                # later `let e` reuses the same C local, and the exit cleanup would
                # read a tag that `verr_catch_end` already freed.
                self.out.append(ind + "  %s = vnil();" % _cname(ename))
            if ename and ename in self.sym:
                del self.sym[ename]
                self.assigned.discard(ename)
            self.out.append(ind + "%s:;" % after_lbl)
            self.out.append(ind + "}")
        elif k == "scope":
            sc = self.new_tmp("sc")
            self.out.append(ind + "int64_t %s = vscope_begin().as.ival;" % sc)
            self.scopes.append(sc)
            self.emit_block(s[1], ind)
            self.scopes.pop()
            self.out.append(ind + "vscope_end(vint(%s));" % sc)
        elif k == "spawn":
            code = self.fn_code("clo:%d" % len(self.clos))
            capnames = sorted((_refs(s[1]) & set(self.sym))
                              | (set(self.cap[1]) if self.cap is not None else set()))
            self.clos.append(([], s[1], capnames))
            self.new_tmp("m")
            self.out.append(ind + 'Map *_cm%d = vmap("");' % self.tmp)
            for var in capnames:
                for ln in self._capture("_cm%d" % self.tmp, var):
                    self.out.append(ind + ln)
            self.out.append(ind + "Vec *_cb%d = vvec();" % self.tmp)
            self.out.append(ind + "vec_push(_cb%d, vint(%d));" % (self.tmp, code))
            self.out.append(ind + "{ Val _cv = vstruct(_cm%d); vec_push(_cb%d, _cv); }" % (self.tmp, self.tmp))
            self.out.append(ind + 'vspawn(vtagv("closure", _cb%d));' % self.tmp)
        elif k == "defer":
            self.emit_defer(s[1], ind)
        elif k == "fn":
            name = s[1]
            if not self.nesting:
                self.fns[name] = (s[2], s[3])
                if ("fn", name) not in self.order:
                    self.order.append(("fn", name))
                self.late.append(s)
            else:
                mangled = self.fn_alias.get(name)
                if mangled is None:
                    self.nested_seq += 1
                    mangled = "%s@%d" % (name, self.nested_seq)
                    self.fns[mangled] = (s[2], s[3])
                    self.fn_alias[name] = mangled
                    self.late.append(("fn", mangled, s[2], s[3]))
                if mangled not in [e[1] for e in self.late]:
                    self.late.append(("fn", mangled, s[2], s[3]))
        elif k in ("trait", "enum"):
            pass
        elif k == "impl":
            pass
        elif k == "pub":
            self.st(s[1], ind)
        elif k == "unsafe":
            self.collect_nested(s[1]) if isinstance(s[1], list) else None
        else:
            raise RuntimeError("stmt " + k)

    def _ptr_elem(self, base):
        """The element type when `base` is a `ptr[T]` binding, else None.

        Only a bare binding carries the type: `p[i]` reads through `p`, and the
        annotation on `let p: ptr[T]` is where T comes from. A plain `ptr` is
        untyped and keeps the old behaviour (an indexable vec/map value).
        """
        if not (isinstance(base, tuple) and base and base[0] == "var"):
            return None
        sym = self.sym.get(base[1])
        return sym.ptr if sym is not None else None

    def _ptr_setidx(self, elem, base, index, value, ind):
        """Write `p[i] = v` through a `ptr[T]`: the element, not the address."""
        pa, a = self.ex(base)
        pv, v = self.ex(value)
        store = self.new_tmp("ps")
        pre = list(pa) + list(pv)
        slot = "(char *)(%s).as.ival + (%s).as.ival * %%s)" % (a, self.ex(index)[1])
        word = {"i64": ("(*(int64_t *)", ").as.ival", 8),
                "f64": ("(*(double *)", ").as.fval", 8),
                "f32": ("(*(float *)", ").as.fval", 4),
                "i32": ("(*(int32_t *)", ").as.ival", 4),
                "u8": ("(*(unsigned char *)", ").as.ival", 1)}
        if elem in self.structs:
            store_expr = "*(%s *)%s = %s" % (_ffi_c(elem), slot % ("sizeof(%s)" % _ffi_c(elem)),
                                             self._val_to_c_struct(elem, v))
            pre.append("if (%s.tag != V_STRUCT || !verr_pending()) { }" % store)
        elif elem in word:
            (open_, payload, width) = word[elem]
            store_expr = "%s%s%s" % (open_, slot % width, payload)
        else:
            raise SyntaxError("ptr[%s]: %s is neither an extern struct nor a machine word"
                              % (elem, elem))
        pre.append("Val %s = vunwind;" % store)
        pre.append("if (vptr_live(%s)) { %s, vint(0)); }" % (a, "%s = ((%s)" % (store, store_expr)))
        for line in pre:
            self.out.append(ind + line)
        self.emit_err_check(ind)
        if isinstance(base, tuple) and base and base[0] == "var":
            self.assigned.add(base[1])

    def _ptr_idx(self, elem, base, index):
        """Read `p[i]` where `p: ptr[T]` -- the element, not the address.

        A C struct is read with the layout the extern struct declares, the same
        way `ptr_read_struct` does; a machine word is read through the matching
        runtime accessor, so `p: ptr[i32]` reads four bytes and not eight.
        Nothing here can be a segfault: the pointer is checked first.
        """
        pa, a = self.ex(base)
        pb, b = self.ex(index)
        r = self.new_tmp("pi")
        pre = list(pa) + list(pb)
        word = {"i64": "vint(*(int64_t *)", "f64": "vfloat(*(double *)",
                "f32": "vfloat((double)*(float *)", "i32": "vint((int64_t)*(int32_t *)",
                "u8": "vint((int64_t)*(unsigned char *)"}
        if elem in self.structs:
            size = "sizeof(%s)" % _ffi_c(elem)
            read = self._c_struct_to_val(elem, "(*(%s *)((char *)(%s).as.ival + (%s).as.ival * %s))"
                                         % (_ffi_c(elem), a, b, size))
        elif elem in word:
            read = "%s((char *)(%s).as.ival + (%s).as.ival * %d))" % (
                word[elem], a, b, {"i64": 8, "f64": 8, "f32": 4, "i32": 4, "u8": 1}[elem])
        else:
            raise SyntaxError("ptr[%s]: %s is neither an extern struct nor a machine word"
                              % (elem, elem))
        pre.append("Val %s = vunwind;" % r)
        pre.append("if (vptr_live(%s)) { %s = %s; }" % (a, r, read))
        return (pre, r)

    def var_def(self, name, own="copy", ptr=None, elem=None):
        cn = _cname(name)
        self.sym[name] = Sym(cn, own)
        self.sym[name].ptr = ptr
        self.sym[name].elem = elem
        return (None, cn)

    def emit_block(self, body, ind):
        """Emit a statement list as a defer block.

        A block that holds a defer of its own gets a defer frame per pass:
        pushed on entry, drained LIFO on the way out. Blocks without one emit
        nothing extra, so the common case is byte-identical to before.
        """
        if not _block_defers(body):
            for t in body:
                self.st(t, ind)
            if self.move_rt:
                for name in _block_let_names(body):
                    self.emit_drop(name, ind)
            self._drop_alias_temps(body, ind)
            return
        b = self.new_tmp("dfb")
        self.out.append(ind + "volatile int64_t %s = vdfsp;" % b)
        self.out.append(ind + "vdfpush();")
        self.dfb.append(b)
        for t in body:
            self.st(t, ind)
        self.dfb.pop()
        self.out.append(ind + "vdfreset(%s);" % b)
        if self.move_rt:
            for name in _block_let_names(body):
                self.emit_drop(name, ind)
        self._drop_alias_temps(body, ind)

    def _drop_alias_temps(self, body, ind):
        for name in _block_let_names(body):
            if name in self.alias_temps:
                self.alias_temps.discard(name)
                self.out.append(ind + "vrc_dec(%s);" % _cname(name))

    def emit_defer(self, e, ind):
        """Register a defer at RUNTIME, like the interpreter does.

        The body becomes a zero-arg closure over the locals it reads, pushed on
        the current defer frame; the frame belongs to the innermost lexical
        block being executed, or to the function. A defer in the top-level
        statement list has neither, and the interpreter never runs one there
        either, so it stays a no-op instead of raising.
        """
        if self.fin == "__fin_top" and not self.dfb:
            return
        code = self.fn_code("clo:%d" % len(self.clos))
        outer = set(self.cap[1]) if self.cap is not None else set()
        reads = set()
        _expr_vars(e, reads)
        writes = set()
        _expr_set_vars(e, writes)
        # Only what the body reads or writes is captured. Capturing every assigned
        # local counted a reference the closure never used and never released it.
        capnames = sorted(((reads | writes) & (set(self.sym) | self.dfb_locals | outer))
                          | outer)
        self.clos.append(([], [("expr", e)], capnames))
        cm = self.new_tmp("m")
        cb = self.new_tmp("cb")
        pre = ['Map *%s = vmap("");' % cm]
        for var in capnames:
            pre.extend(self._capture(cm, var))
        pre.append("Vec *%s = vvec();" % cb)
        pre.append("vec_push(%s, vint(%d));" % (cb, code))
        pre.append("{ Val _cv = vstruct(%s); vec_push(%s, _cv); }" % (cm, cb))
        pre.append('vdfadd(vtagv("closure", %s));' % cb)
        for p in pre:
            self.out.append(ind + p)

    def emit_loop_exit(self, k, ind):
        """Unwind block defers and structured task scopes before loop control.

        A `break`/`continue` can be nested under several statement blocks. A
        single `vdfreset` leaks the outer frames, while skipping `vscope_end`
        lets children outlive a scope that was exited by loop control. The
        recorded loop boundary tells us exactly which lexical frames leave.
        """
        scope_depth, defer_depth = (self.loop_exit_depths[-1]
                                    if self.loop_exit_depths else (len(self.scopes),
                                                                   max(0, len(self.dfb) - 1)))
        for frame in reversed(self.dfb[defer_depth:]):
            self.out.append(ind + "vdfreset(%s);" % frame)
        for scope in reversed(self.scopes[scope_depth:]):
            self.out.append(ind + "vscope_end(vint(%s));" % scope)
        self.out.append(ind + "%s;" % k)

    def var_ref(self, name):
        if name in self.sym:
            return self.sym[name].cn
        if self.cap is not None and name in self.cap[1]:
            return "(*map_get(%s, \"%s\"))" % (self.cap[0], name)
        return _cname(name)

    def _place_root(self, e):
        """Root var name of a place expression, or None when `e` computes a
        fresh value."""
        if not isinstance(e, tuple) or not e:
            return None
        k = e[0]
        if k == "var":
            return e[1]
        if k == "move":
            return self._place_root(e[1])
        if k == "field":
            return self._place_root(e[1])
        if k == "idx":
            return self._place_root(e[1])
        if k == "call" and len(e) == 3 and e[1] == "__at":
            # Reading an element: a place expression rooted at the container.
            return self._place_root(e[2][0]) if e[2] else None
        return None

    def _is_part_of_local(self, e, root):
        """True when `e` reads a part (element, field) of the binding `root` rather
        than the binding itself."""
        d = self.sym.get(root)
        if d is None or d.own != "owned":
            return False
        cur = e
        while isinstance(cur, tuple) and cur and cur[0] == "move":
            cur = cur[1]
        if isinstance(cur, tuple) and cur and cur[0] == "var":
            return False
        return True

    def _keep(self, e, val):
        """The C expression for a value a container is about to store.

        A container owns what it holds and gives the reference back when it is
        released, so a value built for the store arrives with its one reference
        and anything borrowed needs its own. Shared bookkeeping (`self.shared`)
        is not needed: both sides hold counted references.
        """
        if _dec_able(e, self.heap_returns()):
            return val
        # `move x` of a live local transferred the binding's reference already:
        # `vmove` empties `x`, so the scope drop will not release it and the
        # container that receives `val` is its only owner from here. Retaining
        # here left one reference with no owner, one per call with a moved
        # argument, unbounded inside a loop. `_dec_able` cannot answer this: it
        # describes the value, and a move is a statement about the binding.
        if e[0] == "move" and isinstance(e[1], tuple) and e[1] and e[1][0] == "var" \
                and self._place_root(e) in self.sym:
            return val
        return "vrc_inc(%s)" % val

    def _capture(self, cm, var):
        """Store `var` in a closure's capture map, which will release it.

        Capturing always takes its own reference: the closure may be dropped
        before the frame (a defer in a loop) or outlive it (a returned closure),
        so neither side can assume it owns what the other holds.
        """
        ref = self.var_ref(var)
        # Always, even for a scalar or a value that came out of another capture
        # map: the store takes the caller's reference and the map hands it back
        # on death, so the frame must hold one of its own too. `vrc_inc` on a
        # value that is not on the heap is a no-op.
        lines = ["vrc_inc(%s);" % ref]
        self.captured.add(var)
        lines.append('map_set(%s, "%s", %s);' % (cm, var, ref))
        return lines

    def _own_of(self, e):
        """Ownership class of a value freshly computed by `e`.

        `copy` -- trivially copyable / immortal (scalars, string literals),
                  never dropped.
        `borrow` -- aliases another binding's storage; never dropped itself.
        `owned` -- the binding owns a fresh heap value; dropped once.
        """
        if not isinstance(e, tuple) or not e:
            return "copy"
        k = e[0]
        if k in ("num", "bool", "none", "str", "rstr"):
            return "copy"  # scalars and string literals are immortal
        if k == "var":
            d = self.sym.get(e[1])
            if d is None:
                return "copy"  # param / capture storage belongs to the caller
            return "copy" if d.own == "copy" else "borrow"
        if k == "move":
            # A move takes the reference: a whole-value `move x` takes the one the
            # source held (and the source is invalidated), a projection move
            # `move p.x` is given a reference of its own by `let`. Either way the
            # binding owns it -- classified as a borrow, its reference was never
            # released and every projection move leaked one value.
            return "owned"
        if k == "closure":
            return "owned"
        if k in ("vec", "maplit", "struct"):
            return "owned"  # fresh heap container
        if k == "bin":
            return "owned"  # fresh result (vdrop is a no-op for scalars/strings)
        if k == "un":
            return "copy"
        if k == "or":
            # `a or b` hands over one counted reference, whichever side wins.
            return "owned"
        if k == "quest":
            # `x?` takes a reference out of the tag (`vtag_payload` counts it), so
            # the binding that receives it owns that reference. Calling it a copy
            # left the payload -- a struct, a vector, a string -- allocated for the
            # whole run.
            return "owned"
        if k == "field":
            return "borrow"  # read out of a struct: aliases it
        if k == "idx":
            return "owned"   # `a[i]` hands back its own reference (vat_owned)
        if k in ("call", "mcall"):
            # `Ok(x)`/`Err(x)`/`Some(x)` build a fresh tag on the spot, so its
            # argument vector belongs to whoever holds the tag. Treating it as a
            # copy left that vector, and every container inside it, allocated:
            # `let r = Ok(Ok(vec))` leaked the inner vector on every pass.
            if k == "call" and e[1] in ("Ok", "Err", "Some"):
                return "owned"
            # A callee that hands back a container or a string transfers a
            # reference: arguments were shared before the call, so the result is
            # safe to release here.  Scalars and borrowed returns stay `copy`.
            return "owned" if _dec_able(e, self.heap_returns()) else "copy"
        return "owned"

    def _drop_allowed(self, name):
        d = self.sym.get(name)
        return (
            self.move_rt
            and d is not None
            and d.own == "owned"
            and name not in self.moved
            and name not in self.shared
            and name not in self.dropped
        )

    def emit_drop(self, name, ind):
        if self._drop_allowed(name):
            self.dropped.add(name)
            self.out.append(ind + "vrelease(%s);" % self.sym[name].cn)
            self.out.append(ind + "%s = vnil();" % self.sym[name].cn)

    def _emit_scope_drops(self, ind):
        """Drop every owned binding still alive at the end of this scope."""
        for name in sorted(self.sym):
            if self._drop_allowed(name):
                self.out.append(ind + "vrelease(%s);" % self.sym[name].cn)
                self.out.append(ind + "%s = vnil();" % self.sym[name].cn)
                self.dropped.add(name)

    def _emit_fn_drops(self, ind, skip=None, at_return=False, keep=()):
        """Release every owned local at the single exit point.

        A `return` inside a `match` arm jumps straight to the exit label, over
        the drop the end of the arm emits; that drop is then dead code, and the
        value it was written for -- a payload alias, a temporary tag -- leaked.
        Releasing here covers every path, and releasing twice is harmless: a
        binding dropped on the way out was set to `vnil()` first.

        `at_return` is that path, `skip` is the binding it hands to the caller,
        and `keep` is what the returned expression reads: the drop lines come
        before the value is evaluated, so a name the value mentions has to
        survive. A name one `return` transfers is shared for the whole frame, so
        the exit drops skipped it on every other path too and whatever it held
        leaked -- `path_norm` built `out`, answered `"."` on an early branch, and
        lost it.
        """
        for name in sorted(self.sym):
            d = self.sym[name]
            if not (self.move_rt and d is not None and d.own == "owned"
                    and name not in self.moved):
                continue
            if name == skip or name in keep:
                continue
            if not at_return and name in self.shared:
                continue
            self.out.append(ind + "vrelease(%s);" % d.cn)
            self.out.append(ind + "%s = vnil();" % d.cn)

    def fast_val(self, name, kind, expr):
        """The Val a fast wrapper hands back for the raw C result `expr`.

        The body of a fast function is raw C, so a `-> bool` annotation was lost
        on the way back: the wrapper built `vint(1)` and the program printed `1`
        where the interpreter printed `True`. The annotation is the only place
        that knows, and it is in `fn_ret`.
        """
        if self.fn_ret.get(name) == "bool":
            return "vbool((%s) != 0)" % expr
        return ("vfloat(%s)" if kind == "f64" else "vint(%s)") % expr

    def define_fn(self, cname, params, body, cap=None, fast=None):
        names = [p[0] if isinstance(p, tuple) else p for p in params]
        saved_sym, saved_cap, saved_dfb, saved_fin = self.sym, self.cap, self.dfb, self.fin
        # A top-level `let` or `const` is a file-scope Val; a function that reads
        # it resolves the name through the same table as its own locals. A name
        # this closure captured is the exception: it resolves through the capture
        # map, because the child holds the value the parent had at the launch, not
        # a window onto the global the parent may be writing.
        captured = set(cap) if cap is not None else set()
        self.sym = {n: Sym(_cname(n), "copy") for n in self.global_lets if n not in captured}
        saved_assigned = self.assigned
        self.assigned = set()
        self.cap = ("cap", set(cap)) if cap is not None else None
        dfb = "__vdf_%s" % cname
        self.dfb = []
        self.fin = "__fin_%s" % cname
        self.out.append("static Val %s(Map *cap, Vec *args) {" % cname)
        self.out.append("  (void)cap;")
        self.out.append("  if (args->len != %d) { vraise(\"arity\"); return vunwind; }" % len(names))
        if fast is not None:
            raw, kind = fast[0], fast[1]
            vret = self.fast_val(fast[2], kind, "__r")
            if kind == "f64":
                checks = " && ".join("(args->items[%d].tag == V_INT || args->items[%d].tag == V_FLOAT)" % (i, i) for i in range(len(names)))
                rawargs = ", ".join("(args->items[%d].tag == V_INT ? (double)args->items[%d].as.ival : args->items[%d].as.fval)" % (i, i, i) for i in range(len(names)))
                self.out.append("  if (%s) { double __r = %s(%s); if (verr_pending()) return verr_leave(); return %s; }"
                                % (checks if checks else "1", raw, rawargs, vret))
            elif kind == "i64v":
                vv = self.fastvec.get(fast[2], frozenset())
                checks = []
                rawargs = []
                for i, n in enumerate(names):
                    if n in vv:
                        # The tag says vec, not vec[i64]: the raw body reads
                        # elements without a per-element check, so the wrapper
                        # scans once and falls back if one is not an int.
                        checks.append("args->items[%d].tag == V_VEC && vvec_all_int(args->items[%d].as.vec)" % (i, i))
                        rawargs.append("args->items[%d].as.vec" % i)
                    else:
                        checks.append("args->items[%d].tag == V_INT" % i)
                        rawargs.append("args->items[%d].as.ival" % i)
                self.out.append("  if (%s) { int64_t __r = %s(%s); if (verr_pending()) return verr_leave(); return %s; }"
                                % (" && ".join(checks) if checks else "1", raw, ", ".join(rawargs), vret))
            else:
                checks = " && ".join("args->items[%d].tag == V_INT" % i for i in range(len(names)))
                rawargs = ", ".join("args->items[%d].as.ival" % i for i in range(len(names)))
                self.out.append("  if (%s) { int64_t __r = %s(%s); if (verr_pending()) return verr_leave(); return %s; }"
                                % (checks if checks else "1", raw, rawargs, vret))
        self.out.append("  if (bdepth_push()) { vraise(\"recursion too deep\"); return vunwind; }")
        self.out.append("  Val __ret = vnil();")
        self.out.append("  volatile int64_t %s = vdfsp;" % dfb)
        self.out.append("  vdfpush();")
        vars_ = set(names)
        _collect_vars(body, vars_)
        caps = set(cap) if cap is not None else set()
        # A script-level `let` is a file-scope Val that `self.sym` already points
        # at (see the top of this function). Declaring a local of the same name
        # would shadow it: `b_total` was declared inside the function and
        # assigned to, so the file-scope global the top level had written stayed
        # `vnil()` and the next read raised `type error in +`. A captured name is
        # not a global here -- the child holds its own copy by design.
        # Only globals the body does not bind itself: `fn bump(mut m)` next to a
        # script-level `let mut m` is a parameter that shadows the global, and
        # erasing it from `vars_` left the parameter undeclared -- the callee
        # then wrote through a stale local and the top-level release of `m`
        # fired on a value already freed (ASan, map.brat).
        vars_ -= (self.global_lets - caps - set(names) - _bound_in(body))
        saved_reclaim = self.reclaim
        saved_relet = self.relet
        # A captured binding has no owner here: releasing it would drop the map's reference.
        self.reclaim, self.relet = _reclaim_names(body, set(names), self.heap_returns())
        self.reclaim -= caps
        self.relet -= caps
        saved_shared = self.shared
        saved_captured = self.captured
        saved_dropped = self.dropped
        self.shared = set()
        # Per function: a name dropped in one function must still be released in
        # the next one that happens to use the same identifier.
        self.dropped = set()
        self.captured = set()
        saved_locals = self.dfb_locals
        self.dfb_locals = vars_
        saved_params = self.cur_params
        self.cur_params = set(names)
        param_names = set(names)
        for n in sorted(vars_):
            own = "borrow" if n in param_names else "copy"
            self.sym[n] = Sym(_cname(n), own)
            self.out.append("  Val BRAT_UNUSED %s = vnil();" % _cname(n))
        for n in sorted(self.relet & vars_):
            # First pass of the loop has nothing to release yet.
            self.out.append("  %s = vnil();" % _cname(n))
        for n in sorted((vars_ & caps) - param_names):
            # An assigned capture must start from the snapshot, not a fresh frame local.
            self.out.append('  %s = (*map_get(cap, "%s"));' % (_cname(n), n))
        for i, pn in enumerate(names):
            self.out.append("  %s = args->items[%d];" % (_cname(pn), i))
            self.assigned.add(pn)
        self.nesting += 1
        for t in body:
            self.st(t, "  ")
        self.nesting -= 1
        if self.move_rt:
            # The label is the single exit point, so locals are released here
            # and not at the end of the body, which every `return` jumps over.
            self.out.append("  __fin_%s:" % cname)
            self._emit_fn_drops("  ")
            # The same goes for the counted aliases a `match` takes of its
            # scrutinee: a `return` from inside an arm jumps over the drop at
            # the end of the block, so the alias is given back here.
            for _al in sorted(self.alias_temps):
                _d = self.sym.get(_al)
                if _d is None or _al in self.dropped:
                    continue
                self.out.append("  vrc_dec(%s);" % _d.cn)
                self.alias_temps.discard(_al)
            self.out.append("  bdepth_pop();")
        else:
            self.out.append("  __fin_%s:" % cname)
            self.out.append("  bdepth_pop();")
        self.out.append("  vdfreset(%s);" % dfb)
        self.out.append("  return __ret;")
        self.out.append("}")
        self.out.append("")
        self.sym, self.cap, self.dfb, self.fin, self.dfb_locals = saved_sym, saved_cap, saved_dfb, saved_fin, saved_locals
        self.cur_params = saved_params
        self.assigned = saved_assigned
        self.reclaim = saved_reclaim
        self.relet = saved_relet
        self.shared = saved_shared
        self.captured = saved_captured
        self.dropped = saved_dropped

    def emit_fn_by_key(self, key):
        if key.startswith("fn:"):
            name = key[3:]
            params, body = self.fns[name]
            fast = None
            if name in self.fastc:
                fast = (self.fastc[name], self.fastkinds[name], name)
            self.define_fn(self.cfn(key), params, body, fast=fast)
        elif key.startswith("meth:"):
            tname, mname = key[5:].rsplit(".", 1)
            params, body = self.fns[tname + "." + mname]
            self.define_fn(self.cfn(key), params, body)
        else:
            raise RuntimeError("bad fn key " + key)

    def emit_prelude_tables(self):
        self.out.append("static const struct { const char *type, *meth; int code; } MTAB[] = {")
        for (tname, mname), code in sorted(self.mtab.items(), key=lambda kv: kv[1]):
            self.out.append('  {"%s", "%s", %d},' % (_clit(tname), _clit(mname), code))
        self.out.append("  {0, 0, 0}")
        self.out.append("};")
        self.out.append("static const struct { const char *tag, *type; } VTAB[] = {")
        for tag, tname in sorted(self.vtab.items()):
            self.out.append('  {"%s", "%s"},' % (_clit(tag), _clit(tname)))
        self.out.append("  {0, 0}")
        self.out.append("};")
        self.out.append("static Val vcall_any(Val fn, Vec *a);")
        self.out.append("static Val vcall_any(Val fn, Vec *a) {")
        self.out.append("  (void)a;")
        self.out.append(
            "  if (fn.tag != V_TAG || strcmp(fn.as.tv.tag, \"closure\") || fn.as.tv.args->len != 2)"
            " { vraise(\"not callable\"); return vunwind; }"
        )
        self.out.append("  int64_t code = vlong(fn.as.tv.args->items[0]);")
        self.out.append("  Val cp = fn.as.tv.args->items[1];")
        self.out.append("  Map *cap = cp.tag == V_STRUCT ? cp.as.map : NULL;")
        self.out.append("  (void)cap;")
        self.out.append("  switch (code) {")
        for key, code in sorted(self.codes.items(), key=lambda kv: kv[1]):
            self.out.append("    case %d: return %s(cap, a);" % (code, self.cfn(key)))
        self.out.append("  }")
        self.out.append('  vraise("bad code"); return vunwind;')
        self.out.append("}")
        self.out.append("static Val __attribute__((unused)) vcall_method(Val obj, const char *meth, Vec *a) {")
        self.out.append("  (void)a;")
        self.out.append("  const char *tn = NULL;")
        self.out.append("  int64_t i;")
        self.out.append("  if (obj.tag == V_STRUCT) tn = obj.as.map->type;")
        self.out.append("  else if (obj.tag == V_TAG) { for (i = 0; VTAB[i].tag; i++) if (!strcmp(VTAB[i].tag, obj.as.tv.tag)) tn = VTAB[i].type; }")
        self.out.append("  if (tn) for (i = 0; MTAB[i].type; i++) if (!strcmp(MTAB[i].type, tn) && !strcmp(MTAB[i].meth, meth)) {")
        self.out.append("    Vec *b = vvec(); vec_push(b, obj);")
        self.out.append("    for (int64_t j = 0; j < a->len; j++) vec_push(b, a->items[j]);")
        self.out.append("    int64_t code = MTAB[i].code;")
        self.out.append("    switch (code) {")
        for key, code in sorted(self.codes.items(), key=lambda kv: kv[1]):
            # `b` is this dispatcher's own argument vector. The callee copies
            # what it needs, so the container goes right after the call --
            # emptied first, because it never owned its items.
            if self.move_rt:
                self.out.append("    case %d: { Val r = %s(NULL, b); b->len = 0; "
                                "vrc_dec(vvecv(b)); return r; }" % (code, self.cfn(key)))
            else:
                self.out.append("    case %d: return %s(NULL, b);" % (code, self.cfn(key)))
        self.out.append("    }")
        self.out.append("  }")
        self.out.append("  return vbuiltin(obj, meth, a);")
        self.out.append("}")
        self.out.append("static Val __attribute__((unused)) vmap_with(Val obj, Val fn, "
                        "const char *meth) {")
        self.out.append("  Vec *w = vvec_of_m(obj, meth);")
        self.out.append("  Vec *o = vvec();")
        self.out.append("  Vec *one = vvec();")
        self.out.append("  for (int64_t i = 0; i < w->len; i++) {")
        self.out.append("    one->len = 0; vec_push(one, w->items[i]);")
        self.out.append('    Val r = vcall_any(fn, one);')
        self.out.append('    if (verr_pending()) {')
        self.out.append('      one->len = 0; vrc_dec(vvecv(one)); vrc_dec(fn); vrc_dec(vvecv(o));')
        self.out.append('      return r;')
        self.out.append('    }')
        # `one` borrows its item, so it is emptied before release; `o` owns what map
        # returned and co-owns what filter kept, which is why filter takes a reference.
        self.out.append('    if (meth[0] == \'m\') vec_push(o, r);')
        self.out.append('    else if (vis_true(r)) vec_push(o, vrc_inc(w->items[i]));')
        self.out.append("  }")
        self.out.append("  one->len = 0; vrc_dec(vvecv(one)); vrc_dec(fn);")
        self.out.append("  return vvecv(o);")
        self.out.append("}")

    def run_pipeline(self):
        for kind in self.order:
            if kind[0] == "fn":
                self.fn_code("fn:" + kind[1])
            else:
                self.fn_code("meth:" + kind[1] + "." + kind[2])
        for _, tname, mname in [k for k in self.order if k[0] == "meth"]:
            self.mtab[(tname, mname)] = self.fn_code("meth:" + tname + "." + mname)
        self.user_meths = set(m for _, m in self.mtab)
        self.compute_fast()
        if self.need_idiv:
            self.out.append("static int64_t bf_idiv(int64_t a, int64_t b) {")
            self.out.append("  if (!b) { vraise(\"div by zero\"); return 0; }")
            self.out.append("  if (a == INT64_MIN && b == -1) { return INT64_MIN; }")
            self.out.append("  int64_t q = a / b;")
            self.out.append("  if ((a % b) && ((a < 0) != (b < 0))) q--;")
            self.out.append("  return q;")
            self.out.append("}")
        if self.need_imod:
            self.out.append("static int64_t bf_imod(int64_t a, int64_t b) {")
            self.out.append("  if (!b) { vraise(\"div by zero\"); return 0; }")
            self.out.append("  if (a == INT64_MIN && b == -1) { return 0; }")
            self.out.append("  int64_t r = a % b;")
            self.out.append("  if (r && ((r < 0) != (b < 0))) r += b;")
            self.out.append("  return r;")
            self.out.append("}")
        if self.need_fdiv:
            self.out.append("static double bf_fdiv(double a, double b) {")
            self.out.append("  if (b == 0.0) { vraise(\"div by zero\"); return 0.0; }")
            self.out.append("  return a / b;")
            self.out.append("}")
        if self.need_fmod:
            self.out.append("static double bf_fmod(double a, double b) {")
            self.out.append("  if (b == 0.0) { vraise(\"div by zero\"); return 0.0; }")
            self.out.append("  double r = fmod(a, b);")
            self.out.append("  if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;")
            self.out.append("  return r;")
            self.out.append("}")
        if self.need_viat:
            self.out.append("static int64_t bf_viat(Vec *v, int64_t i) {")
            self.out.append("  if (i < 0 || i >= v->len) { vraise(\"index out of range\"); return 0; }")
            self.out.append("  Val x = v->items[i];")
            self.out.append("  if (x.tag != V_INT) { vraise(\"vec elem not int\"); return 0; }")
            self.out.append("  return x.as.ival;")
            self.out.append("}")
        if self.need_vset:
            self.out.append("static void bf_vset(Vec *v, int64_t i, int64_t x) {")
            self.out.append("  if (i < 0 || i >= v->len) { vraise(\"index out of range\"); return; }")
            self.out.append("  v->items[i] = vint(x);")
            self.out.append("}")
        self.emit_forward_decls()
        for kind in self.order:
            if kind[0] == "fn":
                self.emit_fn_by_key("fn:" + kind[1])
            else:
                self.emit_fn_by_key("meth:" + kind[1] + "." + kind[2])
        for name in sorted(self.fast):
            self.emit_fast_fn(name)
        self.emit_closures_queue()
        self.emit_late()

    def emit_late(self):
        """Emit the fns queued in `late` (nested fns), declaring each prototype
        as it becomes known: a nested fn's body can queue more of them."""
        i = getattr(self, "late_done", 0)
        while i < len(self.late):
            j = getattr(self, "late_declared", 0)
            while j < len(self.late):
                self.out.append("static Val %s(Map *cap, Vec *args);" % self.cfn("fn:" + self.late[j][1]))
                j += 1
            self.late_declared = j
            self.emit_fn_by_key("fn:" + self.late[i][1])
            i += 1
            self.late_done = i
        self.emit_closures_queue()

    def emit_main_open(self):
        self.sym = {}
        self.cap = None
        self.dfb = []
        self.fin = "__fin_top"
        if "fn:main" in self.codes:
            self.out.append("static const char *__brat_tagname(Val v) {")
            self.out.append("  switch (v.tag) {")
            self.out.append("    case V_NIL: return \"None\";")
            self.out.append("    case V_BOOL: return \"bool\";")
            self.out.append("    case V_INT: return \"i64\";")
            self.out.append("    case V_FLOAT: return \"f64\";")
            self.out.append("    case V_STR: return \"str\";")
            self.out.append("    case V_VEC: return \"vec\";")
            self.out.append("    case V_STRUCT: return \"struct\";")
            self.out.append("    default: return v.as.tv.tag ? v.as.tv.tag : \"tag\";")
            self.out.append("  }")
            self.out.append("}")
        self.out.append("int main(int argc, char **argv) {")
        self.out.append("  vmem_budget_apply();")
        self.out.append("  vargs_set(argc, argv);")
        self.out.append("  bdepth_init();")
        base = self.mod_bases[0] if self.mod_bases else "."
        self.out.append('  vbase_set("%s");' % _cstr_path(_o.path.abspath(base)))
        self.out.append("  Vec *_a = vvec();")
        names = set()

        def scan_stmts(stmts):
            for s in stmts:
                if not isinstance(s, tuple) or not s:
                    continue
                if s[0] == "pub":
                    scan_stmts([s[1]])
                elif s[0] in ("fn", "impl", "trait", "enum"):
                    pass
                else:
                    _collect_vars([s], names)

        scan_stmts([s for _, s in self.top_exe])
        self.dfb_locals = set(names)
        # A script's top-level `let` owns what it holds, like a binding in a
        # function body; naming them "copy" made a script leak every heap value
        # it bound at exit (`let a = args()` in syslib.brat).
        script_lets = set()
        for _k, _s in self.top_exe:
            if _k == "stmt" and isinstance(_s, tuple) and _s and _s[0] == "let":
                script_lets.add(_s[1])
        self.script_lets = script_lets
        for n in sorted(names):
            if n in self.global_lets:
                continue
            self.sym[n] = Sym(_cname(n), "owned" if n in script_lets else "copy")
            self.out.append("  Val BRAT_UNUSED %s = vnil();" % _cname(n))
        for n in sorted(self.global_lets):
            # A file-scope `let` in a script is still owned by main: released
            # once on the way out, which is where a script used to leak every
            # heap value it bound (`let a = args()` in syslib.brat).
            self.sym[n] = Sym(_cname(n), "owned" if n in script_lets else "copy")
        for n in sorted(self.relet & names):
            self.out.append("  %s = vnil();" % _cname(n))

    def emit_main_close(self):
        if "fn:main" in self.codes:
            self.out.append("  Val __r = %s(NULL, _a);" % self.cfn("fn:main"))
            self.out.append("  vrc_dec(vvecv(_a));")
            self.out.append(
                "  if (verr_pending()) { fprintf(stderr, \"unhandled: \"); "
                "vprint_err(verr_take()); fprintf(stderr, \"\\n\"); return 1; }"
            )
        else:
            # `_a` is the empty argument vector every main gets. A script has no
            # user `main` to hand it to, and the release used to live in the
            # branch above, so script mode leaked one Vec per run and every
            # LeakSanitizer run of a script reported it.
            self.out.append("  vrc_dec(vvecv(_a));")
        if self.move_rt:
            self._emit_scope_drops("  ")
        self.out.append("  vjoin_all();")
        self.out.append(
            "  if (verr_pending()) { fprintf(stderr, \"unhandled: \"); "
            "vprint_err(verr_take()); fprintf(stderr, \"\\n\"); return 1; }"
        )
        if "fn:main" in self.codes:
            self.out.append("  if (__r.tag == V_NIL) return 0;")
            self.out.append("  if (__r.tag == V_TAG) {")
            self.out.append("    if (__r.as.tv.tag && strcmp(__r.as.tv.tag, \"err\") == 0) {")
            self.out.append("      fprintf(stderr, \"unhandled: ('err', \");")
            self.out.append("      { Val __p = vnil();")
            self.out.append("        if (__r.as.tv.args && __r.as.tv.args->len > 0) __p = __r.as.tv.args->items[0];")
            self.out.append("        if (__p.tag == V_STR) fprintf(stderr, \"'%s'\", __p.as.str);")
            self.out.append("        else { Val __s = vstr_of(__p); fprintf(stderr, \"%s\", __s.as.str); } }")
            self.out.append("      fprintf(stderr, \")\\n\");")
            self.out.append("      return 1;")
            self.out.append("    }")
            self.out.append("    fprintf(stderr, \"TypeError: main must return i64, got %s\\n\", __r.as.tv.tag);")
            self.out.append("    return 1;")
            self.out.append("  }")
            # `main` may hand back a container or a string; the process is about
            # to exit, but a leak check still sees it. Read the code first.
            self.out.append("  if (__r.tag == V_INT) { int __code = (int)__r.as.ival; vrelease(__r); return __code; }")
            self.out.append("  fprintf(stderr, \"TypeError: main must return i64, got %s\\n\", __brat_tagname(__r));")
            self.out.append("  return 1;")
        self.out.append("  return 0;")
        self.out.append("}")

    def emit(self, entry):
        self.mod_bases = [_o.path.dirname(_o.path.abspath(entry))]
        self.collect_file(entry)
        return self.emit_program()

    def emit_src(self, src, base="."):
        self.mod_bases = [_o.path.abspath(base)]
        self.collect_src(src, self.mod_bases[0])
        return self.emit_program()

    # C width and alignment of one extern-struct field. A field that is not a
    # scalar, a pointer or a known extern struct cannot be laid out, and the
    # caller refuses the program instead of assuming a machine word.
    def _ffi_layout(self):
        """Size, alignment and field offsets of every extern struct, computed here.

        The C compiler is the only authority on layout, so this is not a second
        opinion that can win: the numbers are asserted against it with
        `_Static_assert` in the emitted C. What the computation buys is a floor
        on the target's rules -- a field BRAT spelled at the wrong width, or a
        target whose `long` is 4 bytes while `i64` claims 8, fails the build
        instead of corrupting the struct at run time. `ffi.struct_layout` holds
        the rule, because `brat ffi` must compute the same numbers.
        """
        ffi = _ffi_mod()
        return ffi.struct_layout(self.structs, self.packed_structs, ffi.field_shape)

    def _struct_order(self):
        """Extern structs, each after the ones it holds by value.

        C needs a struct defined before the one that contains it, and the
        declaration order in the file is whatever the author wrote.
        """
        order, seen = [], set()

        def visit(name):
            if name in seen or name not in self.structs:
                return
            seen.add(name)
            for _f, _ft in self.structs[name]:
                if _ft in self.structs and _ft != name:
                    visit(_ft)
            order.append(name)

        for _n in sorted(self.structs):
            visit(_n)
        return order

    def emit_program(self):
        """Two passes.

        A `fn` nested in a body, a closure inside a closure, and a spawn inside
        a nested fn are all discovered *while* emitting the surrounding body,
        which is after the point where C needs their prototypes. So pass one
        emits the whole program into a throwaway buffer purely to learn the
        complete set of functions, and pass two emits the real text with every
        prototype declared up front. `codes` survives between passes, so both
        passes number the functions identically.
        """
        self.out = []
        self.run_pipeline()
        self._cb_wrappers = []
        for _cb_name in sorted(self.fns):
            _info = self.fns[_cb_name]
            if not (isinstance(_info, tuple) and len(_info) == 2):
                continue
            if not _cb_name.isidentifier():
                continue
            _cb_params, _cb_body = _info
            _cb_ret = self.fn_ret.get(_cb_name)
            _ok = True
            _cparams = []
            for _i, _p in enumerate(_cb_params):
                _ann = _p[2] if isinstance(_p, tuple) and len(_p) > 2 else None
                _arg = "a%d" % _i
                if not _ann or _ann == "none":
                    # An unannotated parameter has no C width to write down, so
                    # the wrapper cannot be declared at all: skip it rather than
                    # emit `void a0`.
                    _ok = False
                    break
                try:
                    _cparams.append("%s %s" % (self._ffi_decl_type(_ann), _arg))
                except SyntaxError:
                    _ok = False
                    break
            if not _ok:
                continue
            try:
                _cret = self._ffi_decl_type(_cb_ret)
            except SyntaxError:
                continue
            self._cb_wrappers.append((_cb_name, _cret, _cparams))
        self.out = []
        self.nested_seq = 0
        self.clos_done = 0
        self.clos_declared = 0
        self.late_done = 0
        self.late_declared = 0
        self.tmp = 0
        # Ownership is re-derived in pass two; a stale `dropped` would suppress its drops.
        self.moved = set()
        self.captured = set()
        self.shared = set()
        self.dropped = set()
        self.scopes = []
        self.emit_prelude_headers()
        self.out.append("#include <stdlib.h>")
        self.out.append("#include <string.h>")
        self.out.append("#include <errno.h>")
        # `offsetof` in the extern-struct layout assertions below.
        self.out.append("#include <stddef.h>")
        _layout = self._ffi_layout()
        for _sname in self._struct_order():
            _sfields = self.structs[_sname]
            _smembers = []
            for _sfn, _sty in _sfields:
                if _sty == "bool":
                    _smembers.append("_Bool %s;" % _ffi_c(_sfn))
                elif _sty == "i64":
                    _smembers.append("int64_t %s;" % _ffi_c(_sfn))
                elif _sty == "i32":
                    _smembers.append("int %s;" % _ffi_c(_sfn))
                elif _sty == "u8":
                    # One byte, or every field after it sits at an offset the
                    # header never promised.
                    _smembers.append("unsigned char %s;" % _ffi_c(_sfn))
                elif _sty == "i8":
                    _smembers.append("signed char %s;" % _ffi_c(_sfn))
                elif _sty == "u16":
                    _smembers.append("unsigned short %s;" % _ffi_c(_sfn))
                elif _sty == "i16":
                    _smembers.append("short %s;" % _ffi_c(_sfn))
                elif _sty == "u32":
                    _smembers.append("unsigned %s;" % _ffi_c(_sfn))
                elif _sty == "u64":
                    _smembers.append("uint64_t %s;" % _ffi_c(_sfn))
                elif _sty == "usize":
                    _smembers.append("size_t %s;" % _ffi_c(_sfn))
                elif _sty == "f64":
                    _smembers.append("double %s;" % _ffi_c(_sfn))
                elif _sty == "f32":
                    _smembers.append("float %s;" % _ffi_c(_sfn))
                elif _sty == "str":
                    _smembers.append("const char *%s;" % _ffi_c(_sfn))
                elif _sty == "ptr":
                    _smembers.append("void *%s;" % _ffi_c(_sfn))
                elif _sty in self.structs:
                    _smembers.append("%s %s;" % (_ffi_c(_sty), _ffi_c(_sfn)))
                else:
                    _smembers.append("int64_t %s;" % _ffi_c(_sfn))
            # `extern packed struct`: no padding between fields. The attribute
            # is the difference between a header's record and a compiler's.
            _skw = ("typedef struct __attribute__((packed)) { "
                    if _sname in self.packed_structs else "typedef struct { ")
            self.out.append("%s%s } %s;" % (_skw, " ".join(_smembers), _ffi_c(_sname)))
            _lay = _layout.get(_sname)
            if _lay is not None:
                # Compile-time only: no code, no data, no run-time cost. A struct
                # whose C layout disagrees with the one BRAT emitted code for is
                # an ABI bug that would otherwise be found by a corrupted value
                # in production, or by nothing at all.
                self.out.append(
                    '_Static_assert(sizeof(%s) == %d, "extern struct %s: size differs '
                    'from the declared layout");' % (_ffi_c(_sname), _lay[0], _sname))
                for _fn, _foff in _lay[2]:
                    self.out.append(
                        '_Static_assert(offsetof(%s, %s) == %d, "extern struct %s.%s: '
                        'offset differs from the declared layout");'
                        % (_ffi_c(_sname), _ffi_c(_fn), _foff, _sname, _fn))
        self.out.append("")
        for _name, (_params, _ret, _lib, *_rest) in self.externs.items():
            _varargs = _rest[0] if _rest else None
            _effects = _rest[1] if len(_rest) > 1 else ()
            if not _lib:
                continue
            if "libc.so" in _lib or _lib == "libc":
                continue
            _cret = self._ffi_decl_type(_ret)
            _cparams = [self._ffi_decl_type(_p[2] if isinstance(_p, tuple) and len(_p) > 2 else None)
                        for _p in _params]
            if _varargs:
                _cparams.append("...")
            # The declared effects travel as attributes, which is the whole point
            # of declaring them: `const` lets the C compiler drop repeated calls
            # and hoist them out of a loop, `nothrow` lets it move a call across a
            # landing pad. libc declarations are skipped above -- those prototypes
            # come from the system headers and already carry their own
            # attributes, so a promise there would be a second, weaker one.
            _attr = ""
            if "pure" in _effects:
                _attr = " BRAT_ATTR_PURE" if any(e.startswith("reads:") for e in _effects) \
                    else " BRAT_ATTR_CONST"
            elif "nothrow" in _effects:
                _attr = " BRAT_ATTR_NOTHROW"
            self.out.append("extern %s %s(%s)%s;" % (_cret, _name,
                                                     ", ".join(_cparams) if _cparams else "void",
                                                     _attr))
        self.out.append("")
        for _cb_name, _cret, _cparams in self._cb_wrappers:
            self.out.append("%s brat_cb_%s(%s);" % (_cret, _cb_name, ", ".join(_cparams)))
        self.out.append("")
        if self.global_lets:
            for _gn in sorted(self.global_lets):
                self.out.append("Val %s;" % _cname(_gn))
            self.out.append("")
        self.run_pipeline()
        self.reclaim, self.relet = _reclaim_names(
            [s for k, s in self.top_exe if k == "stmt"], set(), self.heap_returns())
        # C-callable wrappers for BRAT functions used as callbacks.
        for _cb_name, _cret, _cparams in self._cb_wrappers:
            _cb_params, _cb_body = self.fns[_cb_name]
            _cb_ret = self.fn_ret.get(_cb_name)
            _pushes = []
            for _i, _p in enumerate(_cb_params):
                _ann = _p[2] if isinstance(_p, tuple) and len(_p) > 2 else None
                _arg = "a%d" % _i
                if _ann == "bool":
                    _pushes.append("vec_push(_a, vbool(%s));" % _arg)
                elif _ann in self.structs:
                    _pushes.append("vec_push(_a, %s);" % self._c_struct_to_val(_ann, _arg))
                elif _ann == "str":
                    _pushes.append("vec_push(_a, vstr(%s));" % _arg)
                elif _ann in ("f32", "f64"):
                    # `f32` arrives as a C float: widen once, here, because BRAT
                    # has a single float type and the callee expects one.
                    _pushes.append("vec_push(_a, vfloat((double)%s));" % _arg)
                elif ann_is_ptr(_ann):
                    _pushes.append("vec_push(_a, vint((int64_t)%s));" % _arg)
                else:
                    # Every integer width, sign-extended: a `short -1` is -1.
                    _pushes.append("vec_push(_a, vint((int64_t)%s));" % _arg)
            _cb_cname = self.cfn("fn:" + _cb_name)
            self.out.append("%s brat_cb_%s(%s) {" % (_cret, _cb_name, ", ".join(_cparams)))
            self.out.append("  Vec *_a = vvec();")
            for _push in _pushes:
                self.out.append("  " + _push)
            self.out.append("  Val _r = %s(NULL, _a);" % _cb_cname)
            # The packed arguments belong to this call: C holds nothing from them,
            # so a wrapper that skips this leaks a Vec per callback invocation.
            self.out.append("  vrc_dec(vvecv(_a));")
            self.out.append("  vcb_check(_r);")
            if _cb_ret in (None, "none"):
                self.out.append("  (void)_r;")
            elif _cb_ret == "bool":
                self.out.append("  return _r.as.b;")
            elif _cb_ret == "str":
                self.out.append("  return _r.as.str;")
            elif isinstance(_cb_ret, str) and _cb_ret.startswith("ptr"):
                self.out.append("  return (%s)_r.as.ival;" % self._ffi_decl_type(_cb_ret))
            elif _cb_ret in ("f32", "f64"):
                # BRAT has one float type; the union holds it in `fval`, and a
                # `float` return read out of `ival` is the float's bits.
                self.out.append("  return _r.as.fval;")
            elif _cb_ret in self.structs:
                self.out.append("  return %s;" % self._val_to_c_struct(_cb_ret, "_r"))
            else:
                self.out.append("  return _r.as.ival;")
            self.out.append("}")
            self.out.append("")
        self.emit_main_open()
        for _kind, payload in self.top_exe:
            self.st(payload, "  ")
        self.emit_main_close()
        self.emit_closures_queue()
        self.emit_prelude_tables()
        for _ex_name in sorted(self.exported):
            _info = self.fns.get(_ex_name)
            if not (isinstance(_info, tuple) and len(_info) == 2):
                continue
            _ex_params, _ex_body = _info
            _ex_ret = self.fn_ret.get(_ex_name)
            _cparams = []
            _pushes = []
            _ok = True
            for _i, _p in enumerate(_ex_params):
                _ann = _p[2] if isinstance(_p, tuple) and len(_p) > 2 else None
                _arg = "a%d" % _i
                if _ann == "bool":
                    _cparams.append("_Bool " + _arg)
                    _pushes.append("vec_push(_a, vbool(%s));" % _arg)
                elif _ann in ("i64", "i32"):
                    _cparams.append(("int64_t" if _ann == "i64" else "int") + " " + _arg)
                    _pushes.append("vec_push(_a, vint(%s));" % _arg)
                elif _ann == "f64":
                    _cparams.append("double " + _arg)
                    _pushes.append("vec_push(_a, vfloat(%s));" % _arg)
                elif _ann == "f32":
                    _cparams.append("float " + _arg)
                    _pushes.append("vec_push(_a, vfloat(%s));" % _arg)
                elif _ann == "str":
                    _cparams.append("const char * " + _arg)
                    _pushes.append("vec_push(_a, vstr(%s));" % _arg)
                elif _ann == "ptr":
                    _cparams.append("void * " + _arg)
                    _pushes.append("vec_push(_a, vint((int64_t)%s));" % _arg)
                elif isinstance(_ann, str) and _ann in self.structs:
                    _cparams.append("%s %s" % (_ffi_c(_ann), _arg))
                    _pushes.append("vec_push(_a, %s);" % self._c_struct_to_val(_ann, _arg))
                else:
                    _ok = False
                    break
            if not _ok:
                continue
            if _ex_ret in (None, "none"):
                _cret = "void"
            elif _ex_ret in self.structs:
                _cret = _ffi_c(_ex_ret)
            elif _ex_ret in ("i64", "i32"):
                _cret = "int64_t" if _ex_ret == "i64" else "int"
            elif _ex_ret == "f64":
                _cret = "double"
            elif _ex_ret == "f32":
                _cret = "float"
            elif _ex_ret == "str":
                _cret = "const char *"
            elif _ex_ret == "bool":
                _cret = "_Bool"
            elif _ex_ret == "ptr":
                _cret = "void *"
            else:
                continue
            _ex_cname = self.cfn("fn:" + _ex_name)
            self.out.append("")
            self.out.append("%s %s(%s) {" % (_cret, _ex_name, ", ".join(_cparams)))
            self.out.append("  Vec *_a = vvec();")
            for _push in _pushes:
                self.out.append("  " + _push)
            self.out.append("  Val _r = %s(NULL, _a);" % _ex_cname)
            if _ex_ret in (None, "none"):
                self.out.append("  (void)_r;")
            elif _ex_ret == "str":
                self.out.append("  return _r.as.str;")
            elif _ex_ret == "bool":
                self.out.append("  return _r.as.b;")
            elif _ex_ret == "ptr":
                self.out.append("  return (void *)_r.as.ival;")
            elif _ex_ret == "f64":
                self.out.append("  return _r.as.fval;")
            elif _ex_ret in self.structs:
                self.out.append("  return %s;" % self._val_to_c_struct(_ex_ret, "_r"))
            else:
                self.out.append("  return _r.as.ival;")
            self.out.append("}")
        return "\n".join(self.out) + "\n"

    def _val_to_c_struct(self, sname, valexpr):
        _mems = []
        for _f, _ft in self.structs[sname]:
            _base = "(*map_get((%s).as.map, \"%s\"))" % (valexpr, _f)
            if _ft == "bool":
                _mems.append(".%s = %s.as.b" % (_ffi_c(_f), _base))
            elif _ft == "str":
                _mems.append(".%s = %s.as.str" % (_ffi_c(_f), _base))
            elif _ft == "ptr":
                _mems.append(".%s = (void *)%s.as.ival" % (_ffi_c(_f), _base))
            elif _ft == "f64":
                _mems.append(".%s = %s.as.fval" % (_ffi_c(_f), _base))
            elif _ft == "f32":
                _mems.append(".%s = (float)%s.as.fval" % (_ffi_c(_f), _base))
            elif _ft in self.structs:
                _mems.append(".%s = %s" % (_ffi_c(_f), self._val_to_c_struct(_ft, _base)))
            else:
                _mems.append(".%s = %s.as.ival" % (_ffi_c(_f), _base))
        return "(%s){ %s }" % (_ffi_c(sname), ", ".join(_mems))

    def _c_struct_to_val(self, sname, cexpr):
        _sets = []
        for _f, _ft in self.structs[sname]:
            _fexpr = "%s.%s" % (cexpr, _ffi_c(_f))
            if _ft == "bool":
                _val = "vbool(%s)" % _fexpr
            elif _ft == "str":
                _val = "vstr(%s)" % _fexpr
            elif _ft == "ptr":
                _val = "vint((int64_t)%s)" % _fexpr
            elif _ft == "f64":
                _val = "vfloat(%s)" % _fexpr
            elif _ft == "f32":
                _val = "vfloat((double)%s)" % _fexpr
            elif _ft in self.structs:
                _val = self._c_struct_to_val(_ft, _fexpr)
            else:
                _val = "vint(%s)" % _fexpr
            _sets.append("map_set(_v.as.map, \"%s\", %s);" % (_f, _val))
        return "({ %s _r = %s; Val _v = vstruct(vmap(\"%s\")); %s; _v; })" % (
            _ffi_c(sname), cexpr, sname, " ".join(_sets))

    def emit_forward_decls(self):
        self.out.append("static Val vcall_any(Val fn, Vec *a);")
        self.out.append("static Val vcall_method(Val obj, const char *meth, Vec *a);")
        self.out.append("static Val vmap_with(Val obj, Val fn, const char *meth);")
        for key in sorted(self.codes, key=lambda k: self.codes[k]):
            self.out.append("static Val %s(Map *cap, Vec *args);" % self.cfn(key))
        for name in sorted(self.fast):
            params = self.fast[name][0]
            n = len(params)
            ct = "double" if self.fastkinds[name] == "f64" else "int64_t"
            if self.fastkinds[name] == "i64v":
                vv = self.fastvec.get(name, frozenset())
                ptypes = ["Vec *" if (p[0] if isinstance(p, tuple) else p) in vv else "int64_t" for p in params]
                self.out.append("static %s %s(%s);" % (ct, self.fastc[name], ", ".join(ptypes) if n else "void"))
            else:
                self.out.append("static %s %s(%s);" % (ct, self.fastc[name], ", ".join([ct] * n) if n else "void"))
        self.out.append("")

    def emit_closure_protos(self):
        """Declare every closure discovered so far.

        Bodies are emitted one at a time and each body can discover *more*
        closures (a `fn` nested inside a closure, a closure inside a closure),
        so the prototype list has to grow with the queue instead of being
        written once from a count that is not final yet.
        """
        i = getattr(self, "clos_declared", 0)
        while i < len(self.clos):
            self.out.append("static Val %s(Map *cap, Vec *args);" % self.cfn("clo:%d" % i))
            i += 1
        self.clos_declared = i

    def emit_closures_queue(self):
        i = getattr(self, "clos_done", 0)
        while i < len(self.clos):
            self.emit_closure_protos()
            params, body, capnames = self.clos[i]
            self.define_fn(self.cfn("clo:%d" % i), params, body, cap=capnames)
            i += 1
            self.clos_done = i
            self.emit_closure_protos()

    def emit_prelude_headers(self):
        version = tuple(int(part) for part in _ud.unidata_version.split("."))
        case_version = 1600 if version >= (16, 0) else 1500
        self.out.append("#define BRAT_CASE_MAP_VERSION %d" % case_version)
        self.out.append('#include "crt.h"')
        self.out.append("")


def emit(src, base="."):
    return Emitter().emit_src(src, base)


def emit_file(path, bundle=False):
    em = Emitter()
    em.mod_bases = [_o.path.dirname(_o.path.abspath(path))]
    src = em.emit(path)
    if bundle:
        crt_path = _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), "crt.h")
        with open(crt_path, encoding="utf-8") as f:
            crt_text = f.read()
        src = src.replace('#include "crt.h"', crt_text, 1)
    return src


# The C++ front end, once it is built: `brat-cxx cc` writes the same bytes this
# module writes (lang/tests/test_cxx_emit.py is the contract). A checkout that
# never built it, or built it before the last edit, gets the Python path.
_CXX = _o.environ.get("BRAT_CXX", _o.path.join(
    _o.path.dirname(_o.path.abspath(__file__)), "..", "cxx", "bin", "brat-cxx"))


def cxx_binary():
    """The built front end, or None when it is missing or older than its sources."""
    path = _o.path.abspath(_CXX)
    if not _o.path.exists(path):
        return None
    src_dir = _o.path.join(_o.path.dirname(path), "..", "src")
    try:
        newest = max(_o.path.getmtime(_o.path.join(src_dir, n)) for n in _o.listdir(src_dir))
    except OSError:
        return None
    return path if _o.path.getmtime(path) >= newest else None


def emit_file_c(path, bundle=False):
    """`emit_file` through the C++ emitter, or None when it cannot answer.

    Callers fall back to `emit_file`, so a machine without a C++ compiler, a
    stale binary or a program the front end refuses all keep working.
    """
    binp = cxx_binary()
    if binp is None:
        return None
    version = tuple(int(part) for part in _ud.unidata_version.split("."))
    case_version = 1600 if version >= (16, 0) else 1500
    env = dict(_o.environ, BRAT_CASE_MAP_VERSION=str(case_version),
               **_ffi_model_env())
    r = _sub.run([binp, "cc", _o.path.abspath(path)], capture_output=True, env=env)
    if r.returncode != 0:
        return None
    src = r.stdout.decode("utf-8", "surrogateescape")
    if bundle:
        crt_path = _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), "crt.h")
        with open(crt_path, encoding="utf-8") as f:
            crt_text = f.read()
        src = src.replace('#include "crt.h"', crt_text, 1)
    return src


def extern_libs(path):
    """Sorted unique library names referenced by `extern` decls in `path`."""
    em = Emitter()
    em.mod_bases = [_o.path.dirname(_o.path.abspath(path))]
    em.collect_file(path)
    libs = {t[2] for t in em.externs.values() if t[2]}
    return sorted(libs)
