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
  * when the program also declares the function (`extern fn`), the literal
    signature in `call(f, "i64(i64,i64)", ...)` is compared against that
    declaration instead of being believed.

The scan is a straight line through one body in program order, with no
fixpoint: a close that happens on any path is treated as having happened, which
is the conservative direction and the one that keeps this pass cheap.

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

DECLS = {"fn", "impl", "trait", "enum", "extern", "externstruct", "exportfn"}

# BRAT annotation -> the C type the call signature must name. `str` crosses the
# boundary as `ptr`, and every integer width crosses as `i64`, so a mismatch in
# the number of bytes is not silently blessed.
_C = {
    "str": "ptr", "ptr": "ptr", "none": "none",
    "i8": "i64", "i16": "i64", "i32": "i64", "i64": "i64",
    "u8": "i64", "u16": "i64", "u32": "i64", "u64": "i64",
    "isize": "i64", "usize": "i64", "bool": "i64",
    "f32": "f64", "f64": "f64",
}


def c_type(ann):
    return _C.get((ann or "").strip(), (ann or "").strip())


def parse_sig(text):
    """`"i64(i64,f64)"` -> ("i64", ["i64", "f64"]), or None when malformed."""
    t = text.replace(" ", "")
    if "(" not in t or not t.endswith(")"):
        return None
    ret, rest = t.split("(", 1)
    return ret, [p for p in rest[:-1].split(",") if p]


def line_of(node):
    return _LINES.get(id(node)) or None


def declarations(prog):
    """name -> (parameter C types, return C type) for every `extern` declared."""
    out = {}
    for s in prog[1]:
        node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if isinstance(node, tuple) and node and node[0] == "extern":
            params = [c_type(p[2] if len(p) > 2 else None) for p in node[3]]
            out[node[2]] = (params, c_type(node[4] if len(node) > 4 else None))
        elif isinstance(node, tuple) and node and node[0] == "exportfn":
            params = [c_type(p[2] if len(p) > 2 else None) for p in node[2]]
            out[node[1]] = (params, c_type(node[4] if len(node) > 4 else None))
    return out


class State:
    __slots__ = ("next_id", "handle", "closed", "tied", "sym")

    def __init__(self):
        self.next_id = 0
        self.handle = {}    # binding -> library id
        self.closed = {}    # library id -> line of the close
        self.tied = {}      # binding -> library id, for symbols and pointers
        self.sym = {}       # binding -> declared name it came from


def body_check(body, decls, out, st, line_hint=None):
    for stmt in body:
        if isinstance(stmt, list):
            body_check(stmt, decls, out, st, line_hint)
            continue
        if not isinstance(stmt, tuple) or not stmt:
            continue
        k = stmt[0]
        ln = line_of(stmt) or line_hint
        if k == "let":
            expr = stmt[2] if len(stmt) > 2 else None
            expr_check(expr, decls, out, st, ln)
            bind(stmt[1], expr, st)
        elif k == "set":
            expr_check(stmt[2] if len(stmt) > 2 else None, decls, out, st, ln)
        elif k in ("expr", "return", "defer", "setidx"):
            for part in stmt[1:]:
                if isinstance(part, (tuple, list)):
                    expr_check(part, decls, out, st, ln)
        elif k == "if":
            expr_check(stmt[1], decls, out, st, ln)
            body_check(stmt[2], decls, out, st, ln)
            els = stmt[3]
            if isinstance(els, list):
                body_check(els, decls, out, st, ln)
            elif isinstance(els, tuple):
                body_check([els], decls, out, st, ln)
        elif k == "while":
            expr_check(stmt[1], decls, out, st, ln)
            body_check(stmt[2], decls, out, st, ln)
        elif k == "forrange":
            expr_check(stmt[2], decls, out, st, ln)
            expr_check(stmt[3], decls, out, st, ln)
            body_check(stmt[4], decls, out, st, ln)
        elif k == "foriter":
            expr_check(stmt[2], decls, out, st, ln)
            body_check(stmt[3], decls, out, st, ln)
        elif k in ("spawn", "scope", "unsafe"):
            body_check(stmt[1], decls, out, st, ln)
        elif k == "try":
            body_check(stmt[1], decls, out, st, ln)
            if isinstance(stmt[2], list):
                body_check(stmt[2], decls, out, st, ln)


def bind(name, expr, st):
    """Record what a `let` minted: a handle, a symbol, or a pointer."""
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
    if callee == CALL and len(args) >= 2 and _is_var(args[0]):
        src = args[0][1]
        sig = args[1]
        if src in st.tied and isinstance(sig, tuple) and sig and sig[0] == "str":
            parsed = parse_sig(sig[1])
            if parsed and parsed[0] == "ptr":
                st.tied[name] = st.tied[src]


def _is_var(e):
    return isinstance(e, tuple) and len(e) > 1 and e[0] == "var"


def expr_check(e, decls, out, st, ln):
    if isinstance(e, list):
        for x in e:
            expr_check(x, decls, out, st, ln)
        return
    if not isinstance(e, tuple) or not e:
        return
    k = e[0]
    if k == "var":
        dead_use(e[1], out, st, ln)
        return
    if k != "call":
        for part in e[1:]:
            if isinstance(part, (tuple, list)):
                expr_check(part, decls, out, st, ln)
        return
    name, args = e[1], e[2]
    if name == CLOSE:
        close(args, out, st, ln)
        for a in args:
            expr_check(a, decls, out, st, ln)
        return
    if name == SYM:
        # A handle that was closed cannot produce a symbol.
        if args and _is_var(args[0]):
            lib = st.handle.get(args[0][1])
            if lib is not None and lib in st.closed:
                out.append((ln, CLOSED_VALUE % (args[0][1], st.closed[lib])))
        for a in args:
            expr_check(a, decls, out, st, ln)
        return
    if name == CALL and len(args) >= 2:
        plugin_call(args, decls, out, st, ln)
        for a in args[2:]:
            expr_check(a, decls, out, st, ln)
        return
    for a in args:
        expr_check(a, decls, out, st, ln)


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


def plugin_call(args, decls, out, st, ln):
    first, sig = args[0], args[1]
    if not _is_var(first) or first[1] not in st.tied:
        return
    var = first[1]
    lib = st.tied[var]
    if lib in st.closed:
        out.append((ln, DEAD % (var, _lib_name(st, lib), st.closed[lib])))
        return
    if not (isinstance(sig, tuple) and sig and sig[0] == "str"):
        return
    declared = decls.get(st.sym.get(var, ""))
    parsed = parse_sig(sig[1])
    if declared is None or parsed is None:
        return
    ret, params = parsed
    want_params, want_ret = declared
    if len(params) != len(want_params):
        out.append((ln, ARITY % (st.sym[var], len(want_params), len(params))))
        return
    for i, (got, want) in enumerate(zip(params, want_params), 1):
        if c_type(got) != want:
            out.append((ln, PARAM % (st.sym[var], i, want, c_type(got))))
            return
    if c_type(ret) != want_ret:
        out.append((ln, RET % (st.sym[var], want_ret, c_type(ret))))


def check(prog):
    """Every plugin diagnostic this pass has for one parsed program."""
    out = []
    if not isinstance(prog, tuple) or not prog or prog[0] != "prog":
        return out
    decls = declarations(prog)
    bodies = []
    top = []
    for s in prog[1]:
        node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if not isinstance(node, tuple) or not node:
            top.append(s)
            continue
        if node[0] == "fn":
            bodies.append(node[3])
        elif node[0] == "exportfn":
            bodies.append(node[3])
        elif node[0] == "impl":
            for f in node[3]:
                bodies.append(f[3])
        elif node[0] in DECLS:
            continue
        else:
            top.append(s)
    bodies.append(top)          # a script is one body, like `main`
    for body in bodies:
        body_check(body, decls, out, State())
    return out
