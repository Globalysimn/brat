"""Plugins at run time: a value belongs to the library that made it.

`lib_open` hands out a handle, `lib_sym` turns a name into a symbol inside that
handle, and `call` runs a symbol. Nothing in that chain is a type in BRAT -- the
handle and the symbol are both `ptr` -- so the checker used to see three
unrelated integers, and `call` after `lib_close` was accepted and crashed at
run time. This pass gives the chain a shape:

  * a symbol or a pointer made through a symbol belongs to exactly one open
    library, and `lib_close` ends every value made from it;
  * using one afterwards is an error that names the value, the library and the
    line of the close -- the shape of a linear token, checked where the token is
    minted and where it dies;
  * pointer ownership modes (`own ptr`, `&ptr`, `out ptr`) in `call` signatures
    enforce move-in/move-out transfer, read-only borrows, mutable out-parameters,
    and leak-free consumption of owned pointers;
  * callback arguments (`fn(...) -> ...`) in `call` are checked for function-name
    validity, arity, C widths, `thread(any)` global-touch, `reenter(no)` re-entry,
    and retention across `lib_close`;
  * when the program also declares the function (`extern fn`), the literal
    signature in `call(f, "i64(i64,i64)", ...)` is compared against that
    declaration (including `move` ownership) instead of being believed.

Mirrored in `lang/cxx/src/plugin.cpp`; the suite diffs the messages byte for
byte, so both files carry the same wording.
"""

try:
    from brat.parser import _LINES
except ImportError:  # pragma: no cover - imported from two roots
    try:
        from parser import _LINES
    except ImportError:
        _LINES = {}

OPEN = "lib_open"
SYM = "lib_sym"
CLOSE = "lib_close"
CALL = "call"

DEAD = "`%s` is from library `%s`, closed on line %d (hint: close it after the " \
       "last call, or open the library again)"
TWICE = "`%s` is closed twice (the first close is on line %d)"
CLOSED_VALUE = "`%s` is closed on line %d (hint: open the library again)"
ARITY = "call: `%s` is declared to take %d argument(s), the signature has %d"
PARAM = "call: `%s` parameter %d is %s, the signature says %s"
RET = "call: `%s` returns %s, the signature says %s"
PARAM_OWN = "call: `%s` parameter %d takes ownership (`own ptr`), the signature says %s"
RET_OWN = "call: `%s` returns an owned pointer (`own ptr`), the signature says %s"

NEED_MOVE = "call: arg %d is `own ptr`, pass `move %s` to transfer ownership"
NO_MOVE_BORROW = "call: arg %d is `&ptr` (borrowed), remove `move`"
NO_MOVE_OUT = "call: arg %d is `out ptr`, remove `move`"
OUT_NEED_MUT_ADDR = "call: arg %d is `out ptr`, `&%s` requires `let mut %s`"
OUT_NEED_MUT_VAR = "call: arg %d is `out ptr`, `%s` must be declared `let mut`"
USED_AFTER_MOVE = "`%s` is used after `move` on line %d"
MOVED_TWICE = "`%s` was already moved on line %d"
OWN_LEAK = "`%s` is an owned pointer from `call` on line %d and is never moved or " \
           "returned (hint: pass `move %s` to its destructor or `free`)"
OWN_DROP = "call: `own ptr` result is dropped without freeing"

CB_NOT_NAME = "arg %d of `call`: callback must be a function name"
CB_UNKNOWN = "unknown callback `%s`"
CB_ARITY = "callback `%s` takes %d parameter(s), %s wants %d"
CB_SIG = "callback `%s` has signature fn(%s)->%s, expected %s"
CB_SCHEDULE = ("`%s` is registered as a callback of `%s`, which declares `thread(any)`, "
               "but it touches `%s`; a callback on another thread may not touch the "
               "caller's state (hint: pass it through the callback's parameters, or "
               "declare `thread(same)`)")
CB_REENTER = ("`%s` is registered as a callback of `%s` and calls `%s` again; a callee "
              "may re-enter a callback unless the declaration says `reenter(no)` "
              "(hint: add `reenter(no)` to the declaration)")
CB_CLOSE_RETAINED = ("cannot close library `%s`: callback `%s` registered on line %d "
                     "declares `thread(any)` and may outlive `lib_close`")

DECLS = {"fn", "impl", "trait", "enum", "extern", "externstruct", "exportfn"}

_C = {
    "str": "ptr", "ptr": "ptr", "own ptr": "ptr", "&ptr": "ptr", "out ptr": "ptr",
    "none": "none",
    "i8": "i64", "i16": "i64", "i32": "i64", "i64": "i64",
    "u8": "i64", "u16": "i64", "u32": "i64", "u64": "i64",
    "isize": "i64", "usize": "i64", "bool": "i64",
    "f32": "f64", "f64": "f64",
}


def norm_slot(raw):
    """Canonicalize a return/parameter token from a `call` signature string."""
    s = "".join((raw or "").split())
    if s in ("ownptr", "moveptr"):
        return "own ptr"
    if s == "outptr":
        return "out ptr"
    if s in ("&ptr", "refptr"):
        return "&ptr"
    return s


def strip_cb_contracts(slot):
    """Split trailing `thread(same|any)` / `reenter(yes|no)` off a `fn(...)` slot."""
    s = slot
    contracts = []
    if not s.startswith("fn("):
        return s, ()
    while True:
        hit = None
        for word, vals in (("thread", ("same", "any")), ("reenter", ("yes", "no"))):
            for val in vals:
                suf = "%s(%s)" % (word, val)
                if s.endswith(suf):
                    hit = (word, val, suf)
                    break
            if hit:
                break
        if not hit:
            break
        word, val, suf = hit
        s = s[:-len(suf)]
        contracts.append("%s:%s" % (word, val))
    return s, tuple(reversed(contracts))


def parse_cb_slot(slot):
    """`fn(i64,f64)->i64` -> (['i64', 'f64'], 'i64', contracts), or None."""
    clean, contracts = strip_cb_contracts(slot)
    if not clean.startswith("fn(") or ")->" not in clean:
        return None
    idx = clean.rfind(")->")
    inner = clean[3:idx]
    ret = clean[idx + 3:]
    if not ret:
        return None
    params = split_top_commas(inner) if inner else []
    return params, ret, contracts


def split_top_commas(text):
    out = []
    depth = 0
    start = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            piece = norm_slot(text[start:i])
            if piece:
                out.append(piece)
            start = i + 1
    tail = norm_slot(text[start:])
    if tail:
        out.append(tail)
    return out


def c_type(ann):
    s = norm_slot(ann)
    if s.startswith("fn("):
        return "ptr"
    if s.startswith("ptr["):
        return "ptr"
    return _C.get(s, s)


def parse_sig(text):
    """`"own ptr(&ptr,out ptr)"` -> ("own ptr", ["&ptr", "out ptr"]), or None."""
    if not isinstance(text, str):
        return None
    raw = text.strip()
    open_i = raw.find("(")
    if open_i <= 0 or not raw.endswith(")"):
        return None
    # Ensure the outer `(` matches the final `)`.
    depth = 0
    for i in range(open_i, len(raw)):
        if raw[i] == "(":
            depth += 1
        elif raw[i] == ")":
            depth -= 1
            if depth == 0 and i != len(raw) - 1:
                return None
    if depth != 0:
        return None
    ret = norm_slot(raw[:open_i])
    rest = raw[open_i + 1:-1]
    if not ret:
        return None
    return ret, split_top_commas(rest)


def line_of(node):
    return _LINES.get(id(node)) or None


def _top_nodes(stmts):
    for s in stmts:
        node = s
        while isinstance(node, tuple) and len(node) > 1 and node[0] == "pub":
            node = node[1]
        if isinstance(node, tuple) and len(node) > 1 and node[0] == "block" and isinstance(node[1], list):
            for sub in _top_nodes(node[1]):
                yield sub
            continue
        yield node


def declarations(prog):
    """name -> (param C types, ret C type, param move flags, ret owned) for externs."""
    out = {}
    for node in _top_nodes(prog[1]):
        if isinstance(node, tuple) and node and node[0] == "extern":
            params = [c_type(p[2] if len(p) > 2 else None) for p in node[3]]
            pmoves = [bool(len(p) > 1 and p[1] == "move") for p in node[3]]
            ret_owned = bool(len(node) > 6 and node[6])
            out[node[2]] = (params, c_type(node[4] if len(node) > 4 else None), pmoves, ret_owned)
        elif isinstance(node, tuple) and node and node[0] == "exportfn":
            params = [c_type(p[2] if len(p) > 2 else None) for p in node[2]]
            pmoves = [bool(len(p) > 1 and p[1] == "move") for p in node[2]]
            out[node[1]] = (params, c_type(node[4] if len(node) > 4 else None), pmoves, False)
    return out


def fn_table(prog):
    """Top-level BRAT functions: name -> (param_names, param_anns, ret_ann, body)."""
    out = {}
    globals_ = set()
    for node in _top_nodes(prog[1]):
        if not isinstance(node, tuple) or not node:
            continue
        if node[0] in ("fn", "exportfn"):
            pnames = [p[0] if isinstance(p, tuple) else p for p in node[2]]
            panns = [norm_slot(p[2]) if isinstance(p, tuple) and len(p) > 2 and p[2] else ""
                     for p in node[2]]
            ret = norm_slot(node[5]) if node[0] == "fn" and len(node) > 5 and node[5] else \
                  (norm_slot(node[4]) if node[0] == "exportfn" and len(node) > 4 and node[4] else "none")
            out[node[1]] = (pnames, panns, ret or "none", node[3])
        elif node[0] == "let" and isinstance(node[1], str):
            globals_.add(node[1])
    return out, globals_


def _all_stmts(body):
    for s in body:
        if isinstance(s, list):
            for sub in _all_stmts(s):
                yield sub
            continue
        if not isinstance(s, tuple) or not s:
            continue
        yield s
        for part in s[1:]:
            if isinstance(part, list):
                for sub in _all_stmts(part):
                    yield sub
            elif isinstance(part, tuple) and part and part[0] == "if":
                for sub in _all_stmts([part]):
                    yield sub


def _declared_locals(body):
    out = set()
    for s in _all_stmts(body):
        if s[0] in ("let", "lettmp", "forrange", "foriter") and isinstance(s[1], str):
            out.add(s[1])
        elif s[0] == "try" and len(s) > 3 and isinstance(s[3], str):
            out.add(s[3])
    return out


def _expr_nodes(e):
    stack = [e]
    while stack:
        n = stack.pop()
        if isinstance(n, list):
            stack.extend(n)
            continue
        if not isinstance(n, tuple) or not n:
            continue
        yield n
        for sub in n[1:]:
            if isinstance(sub, (tuple, list)):
                stack.append(sub)


def _body_uses(body):
    names, calls, sym_calls = set(), set(), set()
    for s in _all_stmts(body):
        for part in s[1:]:
            if isinstance(part, list):
                continue
            if isinstance(part, tuple):
                for n in _expr_nodes(part):
                    if n[0] == "var" and isinstance(n[1], str):
                        names.add(n[1])
                    elif n[0] == "call" and isinstance(n[1], str):
                        if n[1] == CALL and n[2] and _is_var(n[2][0]):
                            sym_calls.add(n[2][0][1])
                        else:
                            calls.add(n[1])
    return names, calls, sym_calls


def _cb_first_global(start, fns, globals_):
    seen, stack = set(), [start]
    while stack:
        cur = stack.pop()
        if cur in seen or cur not in fns:
            continue
        seen.add(cur)
        pnames, _panns, _ret, body = fns[cur]
        names, calls, _ = _body_uses(body)
        local = set(pnames) | _declared_locals(body)
        hits = sorted(n for n in (names - local) if n in globals_)
        if hits:
            return hits[0]
        stack.extend(c for c in calls if c != cur)
    return None


def _cb_reenters(start, sym_var, sym_decl_name, fns):
    seen, stack = set(), [start]
    while stack:
        cur = stack.pop()
        if cur in seen or cur not in fns:
            continue
        seen.add(cur)
        _pnames, _panns, _ret, body = fns[cur]
        _names, calls, sym_calls = _body_uses(body)
        if sym_var and sym_var in sym_calls:
            return True
        if sym_decl_name and sym_decl_name in calls:
            return True
        stack.extend(c for c in calls if c != cur)
    return False


class State:
    __slots__ = ("next_id", "handle", "closed", "tied", "sym",
                 "muts", "owned_ptr", "moved", "consumed", "retained_cb")

    def __init__(self, init_muts=()):
        self.next_id = 0
        self.handle = {}        # binding -> library id
        self.closed = {}        # library id -> line of the close
        self.tied = {}          # binding -> library id, for symbols and pointers
        self.sym = {}           # binding -> declared name it came from
        self.muts = set(init_muts)
        self.owned_ptr = {}     # binding -> line minted via `own ptr(...)`
        self.moved = {}         # binding -> line moved
        self.consumed = set()   # owned_ptr bindings moved or returned
        self.retained_cb = {}   # library id -> list of (cb_name, line) with thread(any)


def body_check(body, decls, fns, globals_, out, st, line_hint=None):
    for stmt in body:
        if isinstance(stmt, list):
            body_check(stmt, decls, fns, globals_, out, st, line_hint)
            continue
        if not isinstance(stmt, tuple) or not stmt:
            continue
        k = stmt[0]
        ln = line_of(stmt) or line_hint or 1
        if k in ("let", "lettmp"):
            expr = stmt[2] if len(stmt) > 2 else None
            if len(stmt) > 3 and bool(stmt[3]) and isinstance(stmt[1], str):
                st.muts.add(stmt[1])
            expr_check(expr, decls, fns, globals_, out, st, ln)
            bind(stmt[1], expr, st, ln)
        elif k == "set":
            target = stmt[1] if len(stmt) > 1 else None
            expr = stmt[2] if len(stmt) > 2 else None
            expr_check(expr, decls, fns, globals_, out, st, ln)
            if isinstance(target, str):
                if target in st.owned_ptr and target not in st.consumed:
                    prev_ln = st.owned_ptr[target]
                    out.append((prev_ln, OWN_LEAK % (target, prev_ln, target)))
                st.moved.pop(target, None)
                st.owned_ptr.pop(target, None)
                st.consumed.discard(target)
                st.tied.pop(target, None)
                st.sym.pop(target, None)
                st.handle.pop(target, None)
                bind(target, expr, st, ln)
        elif k == "return":
            expr = stmt[1] if len(stmt) > 1 else None
            if isinstance(expr, tuple):
                if expr[0] == "var" and expr[1] in st.owned_ptr:
                    st.consumed.add(expr[1])
                elif expr[0] == "move" and _is_var(expr[1]) and expr[1][1] in st.owned_ptr:
                    st.consumed.add(expr[1][1])
                expr_check(expr, decls, fns, globals_, out, st, ln)
        elif k == "expr":
            expr = stmt[1] if len(stmt) > 1 else None
            if (isinstance(expr, tuple) and expr and expr[0] == "call"
                    and expr[1] == CALL and len(expr[2]) >= 2
                    and isinstance(expr[2][1], tuple) and expr[2][1][0] == "str"):
                ps = parse_sig(expr[2][1][1])
                if ps and ps[0] == "own ptr":
                    out.append((ln, OWN_DROP))
            if isinstance(expr, (tuple, list)):
                expr_check(expr, decls, fns, globals_, out, st, ln)
        elif k in ("defer", "setidx", "setfield"):
            for part in stmt[1:]:
                if isinstance(part, (tuple, list)):
                    expr_check(part, decls, fns, globals_, out, st, ln)
        elif k == "if":
            expr_check(stmt[1], decls, fns, globals_, out, st, ln)
            body_check(stmt[2], decls, fns, globals_, out, st, ln)
            els = stmt[3]
            if isinstance(els, list):
                body_check(els, decls, fns, globals_, out, st, ln)
            elif isinstance(els, tuple):
                body_check([els], decls, fns, globals_, out, st, ln)
        elif k == "while":
            expr_check(stmt[1], decls, fns, globals_, out, st, ln)
            body_check(stmt[2], decls, fns, globals_, out, st, ln)
        elif k == "forrange":
            expr_check(stmt[2], decls, fns, globals_, out, st, ln)
            expr_check(stmt[3], decls, fns, globals_, out, st, ln)
            body_check(stmt[4], decls, fns, globals_, out, st, ln)
        elif k == "foriter":
            expr_check(stmt[2], decls, fns, globals_, out, st, ln)
            body_check(stmt[3], decls, fns, globals_, out, st, ln)
        elif k in ("spawn", "scope", "unsafe", "block"):
            body_check(stmt[1], decls, fns, globals_, out, st, ln)
        elif k == "try":
            body_check(stmt[1], decls, fns, globals_, out, st, ln)
            if len(stmt) > 3 and isinstance(stmt[3], list):
                body_check(stmt[3], decls, fns, globals_, out, st, ln)
            elif len(stmt) > 2 and isinstance(stmt[2], list):
                body_check(stmt[2], decls, fns, globals_, out, st, ln)


def bind(name, expr, st, ln):
    """Record what a `let` minted: a handle, a symbol, or a pointer."""
    if not name:
        return
    if _is_var(expr):
        src = expr[1]
        if src in st.handle:
            st.handle[name] = st.handle[src]
        if src in st.tied:
            st.tied[name] = st.tied[src]
        if src in st.sym:
            st.sym[name] = st.sym[src]
        return
    if isinstance(expr, tuple) and expr and expr[0] == "move" and _is_var(expr[1]):
        src = expr[1][1]
        if src in st.tied:
            st.tied[name] = st.tied[src]
        if src in st.owned_ptr:
            st.consumed.add(src)
            st.owned_ptr[name] = ln
        return
    if not isinstance(expr, tuple) or expr[0] != "call":
        return
    callee, args = expr[1], expr[2]
    if callee == OPEN:
        st.handle[name] = st.next_id
        st.next_id += 1
        return
    if callee == SYM and len(args) == 2 and _is_var(args[0]):
        src = args[0][1]
        if src in st.handle:
            st.tied[name] = st.handle[src]
            if isinstance(args[1], tuple) and args[1] and args[1][0] == "str":
                st.sym[name] = args[1][1]
        return
    if callee == CALL and len(args) >= 2:
        sig = args[1]
        parsed = parse_sig(sig[1]) if (isinstance(sig, tuple) and sig and sig[0] == "str") else None
        if parsed and parsed[0] in ("ptr", "own ptr"):
            if _is_var(args[0]) and args[0][1] in st.tied:
                st.tied[name] = st.tied[args[0][1]]
            if parsed[0] == "own ptr":
                st.owned_ptr[name] = ln


def _is_var(e):
    return isinstance(e, tuple) and len(e) > 1 and e[0] == "var"


def _consume_move(var, out, st, ln):
    dead_use(var, out, st, ln)
    if var not in st.owned_ptr:
        return
    if var in st.moved:
        out.append((ln, MOVED_TWICE % (var, st.moved[var])))
        return
    st.moved[var] = ln
    st.consumed.add(var)


def expr_check(e, decls, fns, globals_, out, st, ln):
    if isinstance(e, list):
        for x in e:
            expr_check(x, decls, fns, globals_, out, st, ln)
        return
    if not isinstance(e, tuple) or not e:
        return
    k = e[0]
    if k == "var":
        dead_use(e[1], out, st, ln)
        if e[1] in st.moved:
            out.append((ln, USED_AFTER_MOVE % (e[1], st.moved[e[1]])))
        return
    if k == "move":
        if len(e) > 1 and _is_var(e[1]):
            _consume_move(e[1][1], out, st, ln)
        elif len(e) > 1:
            expr_check(e[1], decls, fns, globals_, out, st, ln)
        return
    if k == "closure":
        init_muts = [p[0] for p in (e[2] or ()) if isinstance(p, tuple) and len(p) > 1 and p[1] == "mut"]
        sub = State(init_muts)
        sub.handle = dict(st.handle)
        sub.tied = dict(st.tied)
        sub.sym = dict(st.sym)
        sub.closed = dict(st.closed)
        sub.moved = dict(st.moved)
        sub.retained_cb = {hk: list(hv) for hk, hv in st.retained_cb.items()}
        sub.next_id = st.next_id
        body_check(e[3], decls, fns, globals_, out, sub, ln)
        _finish_body(sub, out)
        return
    if k == "parfor":
        if len(e) > 2:
            expr_check(e[2], decls, fns, globals_, out, st, ln)
        if len(e) > 4:
            expr_check(e[4], decls, fns, globals_, out, st, ln)
        if len(e) > 5 and isinstance(e[5], list):
            sub = State(())
            sub.handle = dict(st.handle)
            sub.tied = dict(st.tied)
            sub.sym = dict(st.sym)
            sub.closed = dict(st.closed)
            sub.moved = dict(st.moved)
            sub.retained_cb = {hk: list(hv) for hk, hv in st.retained_cb.items()}
            sub.next_id = st.next_id
            body_check(e[5], decls, fns, globals_, out, sub, ln)
            _finish_body(sub, out)
        return
    if k != "call":
        for part in e[1:]:
            if isinstance(part, (tuple, list)):
                expr_check(part, decls, fns, globals_, out, st, ln)
        return
    name, args = e[1], e[2]
    if name == CLOSE:
        close(args, out, st, ln)
        for a in args:
            expr_check(a, decls, fns, globals_, out, st, ln)
        return
    if name == SYM:
        if args and _is_var(args[0]):
            lib = st.handle.get(args[0][1])
            if lib is not None and lib in st.closed:
                out.append((ln, CLOSED_VALUE % (args[0][1], st.closed[lib])))
        for a in args:
            expr_check(a, decls, fns, globals_, out, st, ln)
        return
    if name == CALL and len(args) >= 2:
        handled_slots = plugin_call(args, decls, fns, globals_, out, st, ln)
        for idx, a in enumerate(args[2:]):
            if idx in handled_slots:
                continue
            expr_check(a, decls, fns, globals_, out, st, ln)
        return
    for a in args:
        expr_check(a, decls, fns, globals_, out, st, ln)


def close(args, out, st, ln):
    if len(args) != 1 or not _is_var(args[0]):
        return
    var = args[0][1]
    if var not in st.handle:
        return
    lib = st.handle[var]
    if lib in st.closed:
        out.append((ln, TWICE % (var, st.closed[lib])))
        return
    retained = st.retained_cb.get(lib)
    if retained:
        cb_name, cb_ln = retained[0]
        out.append((ln, CB_CLOSE_RETAINED % (var, cb_name, cb_ln)))
        return
    st.closed[lib] = ln


def dead_use(var, out, st, ln):
    if var not in st.tied:
        return
    lib = st.tied[var]
    if lib in st.closed:
        out.append((ln, DEAD % (var, _lib_name(st, lib), st.closed[lib])))


def _lib_name(st, lib):
    for var, ident in st.handle.items():
        if ident == lib:
            return var
    return "?"


def _check_cb_slot(pos, slot, arg, sym_var, sym_name, lib, fns, globals_, out, st, ln):
    cb_info = parse_cb_slot(slot)
    if cb_info is None:
        return
    want_params, want_ret, contracts = cb_info
    if not (isinstance(arg, tuple) and arg and arg[0] == "str"):
        out.append((ln, CB_NOT_NAME % pos))
        return
    cb_name = arg[1]
    if cb_name not in fns:
        out.append((ln, CB_UNKNOWN % cb_name))
        return
    _pnames, panns, ret_ann, _body = fns[cb_name]
    clean_slot = "fn(%s)->%s" % (",".join(want_params), want_ret)
    if len(panns) != len(want_params):
        out.append((ln, CB_ARITY % (cb_name, len(panns), clean_slot, len(want_params))))
        return
    if tuple(panns) != tuple(want_params) or ret_ann != want_ret:
        out.append((ln, CB_SIG % (cb_name, ",".join(panns), ret_ann, clean_slot)))
        return
    label = sym_name or sym_var or "call"
    if "thread:any" in contracts:
        touch = _cb_first_global(cb_name, fns, globals_)
        if touch:
            out.append((ln, CB_SCHEDULE % (cb_name, label, touch)))
        if lib is not None:
            st.retained_cb.setdefault(lib, []).append((cb_name, ln))
    if "reenter:no" not in contracts and _cb_reenters(cb_name, sym_var, sym_name, fns):
        out.append((ln, CB_REENTER % (cb_name, label, label)))


def plugin_call(args, decls, fns, globals_, out, st, ln):
    handled = set()
    first, sig = args[0], args[1]
    sym_var = first[1] if _is_var(first) else None
    lib = st.tied.get(sym_var) if sym_var else None
    if sym_var is not None and lib is not None and lib in st.closed:
        out.append((ln, DEAD % (sym_var, _lib_name(st, lib), st.closed[lib])))
        return handled
    if not (isinstance(sig, tuple) and sig and sig[0] == "str"):
        return handled
    parsed = parse_sig(sig[1])
    if parsed is None:
        return handled
    ret, params = parsed
    sym_name = st.sym.get(sym_var, "") if sym_var else ""

    # Pointer ownership & callback rules apply whether or not an `extern fn` exists.
    for idx, (slot, arg) in enumerate(zip(params, args[2:])):
        pos = idx + 1
        if slot == "own ptr":
            handled.add(idx)
            if isinstance(arg, tuple) and arg and arg[0] == "move" and _is_var(arg[1]):
                _consume_move(arg[1][1], out, st, ln)
            elif _is_var(arg):
                dead_use(arg[1], out, st, ln)
                out.append((ln, NEED_MOVE % (pos, arg[1])))
            else:
                expr_check(arg, decls, fns, globals_, out, st, ln)
        elif slot == "&ptr":
            handled.add(idx)
            if isinstance(arg, tuple) and arg and arg[0] == "move":
                out.append((ln, NO_MOVE_BORROW % pos))
            else:
                expr_check(arg, decls, fns, globals_, out, st, ln)
        elif slot == "out ptr":
            handled.add(idx)
            if isinstance(arg, tuple) and arg and arg[0] == "move":
                out.append((ln, NO_MOVE_OUT % pos))
            elif (isinstance(arg, tuple) and len(arg) > 2
                    and arg[0] == "un" and arg[1] == "&" and _is_var(arg[2])):
                v = arg[2][1]
                dead_use(v, out, st, ln)
                if v in st.moved:
                    out.append((ln, USED_AFTER_MOVE % (v, st.moved[v])))
                elif v not in st.muts:
                    out.append((ln, OUT_NEED_MUT_ADDR % (pos, v, v)))
            elif _is_var(arg):
                v = arg[1]
                dead_use(v, out, st, ln)
                if v in st.moved:
                    out.append((ln, USED_AFTER_MOVE % (v, st.moved[v])))
                elif v not in st.muts:
                    out.append((ln, OUT_NEED_MUT_VAR % (pos, v)))
            else:
                expr_check(arg, decls, fns, globals_, out, st, ln)
        elif slot.startswith("fn("):
            handled.add(idx)
            _check_cb_slot(pos, slot, arg, sym_var, sym_name, lib, fns, globals_, out, st, ln)

    declared = decls.get(sym_name) if sym_name else None
    if declared is None:
        return handled
    want_params, want_ret, want_moves, want_ret_owned = declared
    if len(params) != len(want_params):
        out.append((ln, ARITY % (sym_name, len(want_params), len(params))))
        return handled
    for i, (got, want, wmove) in enumerate(zip(params, want_params, want_moves), 1):
        if c_type(got) != want:
            out.append((ln, PARAM % (sym_name, i, want, c_type(got))))
            return handled
        if wmove and want == "ptr" and got != "own ptr":
            out.append((ln, PARAM_OWN % (sym_name, i, got)))
            return handled
    if c_type(ret) != want_ret:
        out.append((ln, RET % (sym_name, want_ret, c_type(ret))))
        return handled
    if want_ret_owned and want_ret == "ptr" and ret != "own ptr":
        out.append((ln, RET_OWN % (sym_name, ret)))
    return handled


def _finish_body(st, out):
    for var in sorted(st.owned_ptr, key=lambda k: (st.owned_ptr[k], k)):
        if var not in st.consumed:
            ln = st.owned_ptr[var]
            out.append((ln, OWN_LEAK % (var, ln, var)))


def check(prog):
    """Every plugin diagnostic this pass has for one parsed program."""
    out = []
    if not isinstance(prog, tuple) or not prog or prog[0] != "prog":
        return out
    decls = declarations(prog)
    fns, globals_ = fn_table(prog)
    bodies = []
    top = []
    for node in _top_nodes(prog[1]):
        if not isinstance(node, tuple) or not node:
            top.append(node)
            continue
        if node[0] in ("fn", "exportfn"):
            init_muts = [p[0] for p in node[2]
                         if isinstance(p, tuple) and len(p) > 1 and p[1] == "mut"]
            bodies.append((node[3], init_muts))
        elif node[0] == "impl":
            for f in node[3]:
                init_muts = [p[0] for p in f[2]
                             if isinstance(p, tuple) and len(p) > 1 and p[1] == "mut"]
                bodies.append((f[3], init_muts))
        elif node[0] in DECLS:
            continue
        else:
            top.append(node)
    bodies.append((top, ()))     # a script is one body, like `main`
    for body, init_muts in bodies:
        st = State(init_muts)
        body_check(body, decls, fns, globals_, out, st)
        _finish_body(st, out)
    return out
