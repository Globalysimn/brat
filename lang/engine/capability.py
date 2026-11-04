"""Sharing & escape effects: reference cycles, `spawn` graph isolation, and cross-function escape summaries.

Questions answered across function boundaries via fixed-point summaries:

  * does an insert (direct, literal, field, or transitive callee/global insert)
    close a reference cycle? Refcounting cannot reclaim a cycle, so the owner
    graph must stay a forest;
  * is an object graph or raw pointer touched inside `spawn` raced against the
    parent or another `spawn` (directly, through nested containers, pointer
    offsets/slices, channels, or callees) without `join_all()`, or left
    unjoined when a function returns?
  * does an `arena { }` value escape into a module/global binding through a
    callee (`stash(v)`)?
"""

try:
    from brat.parser import _LINES
except ImportError:  # pragma: no cover - the module is imported from two roots
    try:
        from parser import _LINES
    except ImportError:
        _LINES = {}

INSERT_METH = {"push", "ins", "set"}
PTR_WRITE = {"ptr_write_i64", "ptr_write_f64", "ptr_write_f32", "ptr_write_u8",
             "ptr_write_str", "ptr_write_ptr", "ptr_write_struct", "slice_set", "fill"}
PTR_READ = {"ptr_read_i64", "ptr_read_f64", "ptr_read_f32", "ptr_read_u8",
            "ptr_read_str", "ptr_read_ptr", "ptr_read_struct"}
PTR_ALIAS_CALLS = {"ptr_offset", "slice_of", "slice_of_ptr", "slice_of_struct"}
RESOURCE_CLOSE = {"tcp_close", "lib_close"}
MUTATE_METH = {"pop", "sort", "sort_by", "del", "clear"}
JOIN = "join_all"
REPEATS = {"while", "forrange", "foriter"}
_NUM_OPS = {"-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||"}

CYCLE = ("reference cycle: `%s` goes into `%s`, which already owns it "
         "(refcounting cannot reclaim a cycle)")
RACE_JOIN = ("`%s` is written inside `spawn` and touched again with no `join_all()` "
             "in between (hint: send the value through a channel)")
RACE_READ = ("`%s` is read inside `spawn` and written here with no `join_all()` "
             "in between (hint: `join_all()` first, or send the value through a "
             "channel)")
RACE_LOOP = ("`%s` is written inside `spawn` launched in a loop that never joins "
             "(hint: join inside the loop, or send through a channel)")
RACE_UNJOINED = ("`%s` is touched inside `spawn` in a function that returns without "
                 "`join_all()` (hint: call `join_all()` before returning, or use "
                 "`scope { ... }`)")


def line_of(stmt):
    return _LINES.get(id(stmt)) or None


def root(e):
    """The variable a place starts at, or None when `e` computes a fresh value."""
    while isinstance(e, tuple) and e:
        if e[0] == "var":
            return e[1]
        if e[0] == "un" and len(e) > 2 and e[1] == "&":
            e = e[2]                 # `&x` is the place of x, not a fresh value
            continue
        if e[0] in ("move", "quest", "field", "idx"):
            e = e[1]
        else:
            return None
    return None


def _is_nested_place(e):
    """True when `e` indexes or projects into a container (`v[i]`, `s.f`) rather than naming `v` directly."""
    while isinstance(e, tuple) and e and e[0] in ("move", "quest"):
        e = e[1]
    return isinstance(e, tuple) and bool(e) and e[0] in ("idx", "field")


def _plugin_write_indices(sig_expr):
    """Indices into `call(sym, sig, a0, a1, ...)` (0-based from `a0`) that write/consume a pointer."""
    if not (isinstance(sig_expr, tuple) and len(sig_expr) > 1 and sig_expr[0] == "str"):
        return ()
    raw = sig_expr[1].strip()
    op = raw.find("(")
    if op <= 0 or not raw.endswith(")"):
        return ()
    rest = raw[op + 1:-1]
    out = []
    depth = 0
    start = 0
    idx = 0
    for i, ch in enumerate(rest):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            s = "".join(rest[start:i].split())
            if s in ("outptr", "ownptr", "moveptr"):
                out.append(idx)
            idx += 1
            start = i + 1
    tail = "".join(rest[start:].split())
    if tail in ("outptr", "ownptr", "moveptr"):
        out.append(idx)
    return out


def _nodes(e):
    """Every expression under `e`, outermost first."""
    out = []
    stack = [e]
    while stack:
        n = stack.pop()
        if not isinstance(n, tuple) or not n:
            continue
        out.append(n)
        if n[0] == "maplit":
            for k, v in (n[1] or ()):
                if isinstance(k, tuple):
                    stack.append(k)
                if isinstance(v, tuple):
                    stack.append(v)
            continue
        if n[0] == "struct":
            for _, v in (n[2] or ()):
                if isinstance(v, tuple):
                    stack.append(v)
            continue
        for sub in n[1:]:
            if isinstance(sub, tuple):
                stack.append(sub)
            elif isinstance(sub, list):
                stack.extend(x for x in sub if isinstance(x, tuple))
    return out


def _non_own_vars(e, acc=None):
    """Variables mentioned in `e` outside `own(...)` copies and scalar-only ops."""
    acc = set() if acc is None else acc
    if not isinstance(e, tuple) or not e:
        return acc
    k = e[0]
    if k in ("num", "str", "rstr", "bool", "none", "un"):
        return acc
    if k == "bin" and e[1] in _NUM_OPS:
        return acc
    if k == "call" and e[1] in ("own", "len", "is_err", "is_none", "is_vec", "is_str",
                                "is_map", "ptr_is_null", "ptr_read_i64", "ptr_read_f64",
                                "ptr_read_f32", "re_match"):
        return acc
    if k == "var" and isinstance(e[1], str):
        acc.add(e[1])
        return acc
    if k == "maplit":
        for mk, mv in (e[1] or ()):
            _non_own_vars(mk, acc)
            _non_own_vars(mv, acc)
        return acc
    if k == "struct":
        for _, fv in (e[2] or ()):
            _non_own_vars(fv, acc)
        return acc
    for sub in e[1:]:
        if isinstance(sub, tuple):
            _non_own_vars(sub, acc)
        elif isinstance(sub, list):
            for x in sub:
                if isinstance(x, tuple):
                    _non_own_vars(x, acc)
    return acc


def aliases(body, out=None):
    """`let c = a` or `let q = ptr_offset(p, ...)` makes another name for `a`/`p`'s storage."""
    out = {} if out is None else out
    for stmt in body:
        if not isinstance(stmt, tuple) or not stmt:
            continue
        if stmt[0] == "let" and isinstance(stmt[1], str) and isinstance(stmt[2], tuple):
            rhs = stmt[2]
            r = root(rhs)
            if not r and rhs and rhs[0] == "call" and rhs[1] in PTR_ALIAS_CALLS and rhs[2]:
                r = root(rhs[2][0])
            if r and r != stmt[1]:
                out[stmt[1]] = r
        for part in stmt[1:]:
            if isinstance(part, list):
                aliases(part, out)
            elif isinstance(part, tuple) and part and part[0] == "if":
                aliases([part], out)
        if stmt[0] == "try" and len(stmt) > 3 and isinstance(stmt[3], str):
            out.pop(stmt[3], None)
    return out


def resolve(name, alias):
    seen = 0
    while name in alias and seen < 64:
        name = alias[name]
        seen += 1
    return name


def _stmt_exprs(stmt):
    k = stmt[0]
    if k == "setidx":
        return [stmt[1], stmt[2], stmt[3]]
    if k == "setfield":
        return [stmt[1], stmt[3]]
    if k in ("set", "let"):
        return [stmt[2]] if isinstance(stmt[2], tuple) else []
    if k in ("if", "while"):
        return [stmt[1]]
    if k == "forrange":
        return [stmt[2], stmt[3]]
    if k == "foriter":
        return [stmt[2]]
    if len(stmt) > 1 and isinstance(stmt[1], tuple):
        return [stmt[1]]
    return []


def _lit_elements(e, fns=None):
    """Direct child expressions stored inside a fresh container/return expression `e`."""
    if not isinstance(e, tuple) or not e:
        return []
    k = e[0]
    if k == "vec":
        return list(e[1] or [])
    if k == "maplit":
        return [v for _, v in (e[1] or ())]
    if k == "struct":
        return [v for _, v in (e[2] or ())]
    if k == "call" and fns and e[1] in fns:
        args = list(e[2] or [])
        return [args[i] for i in sorted(fns[e[1]]["ret_params"]) if i < len(args)]
    if k == "mcall" and fns and e[2] in fns:
        args = [e[1]] + list(e[3] or [])
        return [args[i] for i in sorted(fns[e[2]]["ret_params"]) if i < len(args)]
    return []


def inserts(stmt, fns=None):
    """(container, value) pairs this statement inserts, as expressions."""
    out = []
    if stmt[0] == "setidx":
        out.append((stmt[1], stmt[3]))
    elif stmt[0] == "setfield":
        out.append((stmt[1], stmt[3]))
    elif stmt[0] == "let" and isinstance(stmt[1], str) and isinstance(stmt[2], tuple):
        lhs = ("var", stmt[1])
        for elem in _lit_elements(stmt[2], fns):
            rv = root(elem)
            if rv and rv != stmt[1]:
                out.append((lhs, elem))
    for e in _stmt_exprs(stmt):
        for n in _nodes(e):
            if n[0] == "mcall":
                if n[2] in INSERT_METH and n[3]:
                    out.append((n[1], n[3][-1] if n[2] == "set" else n[3][0]))
                elif fns and n[2] in fns:
                    args = [n[1]] + list(n[3] or [])
                    for ci, vi in fns[n[2]]["ins_pairs"]:
                        if ci < len(args) and vi < len(args):
                            out.append((args[ci], args[vi]))
                    for g, vi in fns[n[2]]["global_ins"]:
                        if vi < len(args):
                            out.append((("var", g), args[vi]))
            elif n[0] == "call" and fns and n[1] in fns:
                args = list(n[2] or [])
                for ci, vi in fns[n[1]]["ins_pairs"]:
                    if ci < len(args) and vi < len(args):
                        out.append((args[ci], args[vi]))
                for g, vi in fns[n[1]]["global_ins"]:
                    if vi < len(args):
                        out.append((("var", g), args[vi]))
    return out


def _touch_detail(stmt, fns=None):
    """(read, write, nested_write, ptr_touch, chan_sends, chan_recvs) for one statement."""
    read, write, nested_write, ptr_touch = set(), set(), set(), set()
    chan_sends = []
    chan_recvs = set()
    k = stmt[0]
    exprs = _stmt_exprs(stmt)
    for e in exprs:
        for n in _nodes(e):
            if n[0] == "mcall":
                r = root(n[1])
                if n[2] == "send" and n[3]:
                    cr = root(n[1])
                    vr = root(n[3][0])
                    if cr and vr:
                        chan_sends.append((cr, vr))
                elif n[2] == "recv":
                    cr = root(n[1])
                    if cr:
                        chan_recvs.add(cr)
                if r:
                    if n[2] in INSERT_METH or n[2] in MUTATE_METH:
                        write.add(r)
                        if _is_nested_place(n[1]):
                            nested_write.add(r)
                    elif fns and n[2] in fns and 0 in fns[n[2]]["mut_idx"]:
                        write.add(r)
                        if _is_nested_place(n[1]):
                            nested_write.add(r)
                    else:
                        read.add(r)
                if fns and n[2] in fns:
                    info = fns[n[2]]
                    args = [n[1]] + list(n[3] or [])
                    for idx in info["mut_idx"]:
                        if idx < len(args):
                            ar = root(args[idx])
                            if ar:
                                write.add(ar)
                                if _is_nested_place(args[idx]):
                                    nested_write.add(ar)
                    write.update(info["global_writes"])
                    read.update(info["global_reads"])
            elif n[0] == "call" and n[1] == "call":
                args = list(n[2] or [])
                if len(args) >= 2:
                    widxs = _plugin_write_indices(args[1])
                    for idx, a in enumerate(args[2:]):
                        ar = root(a)
                        if ar:
                            ptr_touch.add(ar)
                            if idx in widxs:
                                write.add(ar)
                            else:
                                read.add(ar)
            elif n[0] == "call" and fns and n[1] in fns:
                info = fns[n[1]]
                args = list(n[2] or [])
                for idx in info["mut_idx"]:
                    if idx < len(args):
                        ar = root(args[idx])
                        if ar:
                            write.add(ar)
                            if _is_nested_place(args[idx]):
                                nested_write.add(ar)
                for idx in info["ptr_idx"]:
                    if idx < len(args):
                        ar = root(args[idx])
                        if ar:
                            ptr_touch.add(ar)
                write.update(info["global_writes"])
                read.update(info["global_reads"])
            elif n[0] == "call" and n[1] in PTR_WRITE:
                args = n[2] or []
                r = root(args[0]) if args else None
                if r:
                    write.add(r)
                    ptr_touch.add(r)
            elif n[0] == "call" and n[1] in PTR_READ:
                args = n[2] or []
                r = root(args[0]) if args else None
                if r:
                    read.add(r)
                    ptr_touch.add(r)
            elif n[0] == "call" and n[1] in RESOURCE_CLOSE:
                args = n[2] or []
                r = root(args[0]) if args else None
                if r:
                    write.add(r)
                    if n[1] == "lib_close":
                        ptr_touch.add(r)
            elif n[0] == "move":
                r = root(n)
                if r:
                    write.add(r)
            elif n[0] == "set":
                if isinstance(n[1], str) and n[1]:
                    write.add(n[1].split(".", 1)[0])
            elif n[0] in ("setidx", "setfield"):
                r = root(n[1])
                if r:
                    write.add(r)
                    if _is_nested_place(n[1]):
                        nested_write.add(r)
            elif n[0] in ("var", "field", "idx"):
                r = root(n)
                if r:
                    read.add(r)
    if k == "setidx":
        r = root(stmt[1])
        if r:
            write.add(r)
            if _is_nested_place(stmt[1]):
                nested_write.add(r)
    if k == "setfield":
        r = root(stmt[1])
        if r:
            write.add(r)
            if _is_nested_place(stmt[1]):
                nested_write.add(r)
    return read - write, write, nested_write, ptr_touch, chan_sends, chan_recvs


def touched(stmt, fns=None):
    """(read, written) roots of one statement; written means mutated in place."""
    rd, wr, _, _, _, _ = _touch_detail(stmt, fns)
    return rd, wr


def _declared_expr(e, acc):
    if not isinstance(e, tuple) or not e:
        if isinstance(e, list):
            for x in e:
                _declared_expr(x, acc)
        return
    if e[0] == "closure":
        for p in e[2]:
            acc.add(p[0] if isinstance(p, tuple) else p)
        declared(e[3], acc)
        return
    if e[0] == "parfor":
        if isinstance(e[1], str):
            acc.add(e[1])
        _declared_expr(e[2], acc)
        _declared_expr(e[4], acc)
        declared(e[5], acc)
        return
    for x in e[1:]:
        if isinstance(x, (tuple, list)):
            _declared_expr(x, acc)


def declared(body, acc=None):
    """Every name the block binds, so a spawn's own locals are not shared."""
    acc = set() if acc is None else acc
    for stmt in body:
        if not isinstance(stmt, tuple) or not stmt:
            continue
        if stmt[0] in ("let", "forrange", "foriter"):
            acc.add(stmt[1])
        for se in _stmt_exprs(stmt):
            _declared_expr(se, acc)
        for part in stmt[1:]:
            if isinstance(part, list):
                declared(part, acc)
            elif isinstance(part, tuple) and part and part[0] in ("if", "block"):
                declared([part], acc)
        if stmt[0] == "try" and len(stmt) > 3 and stmt[3]:
            acc.add(stmt[3])
    return acc


class Event:
    """One statement's effect on sharing: what it mutates, what it looks at."""

    __slots__ = ("spawn", "join", "writes", "reads", "nested_writes",
                 "ptr_touches", "chan_sends", "chan_recvs", "line", "loop", "members")

    def __init__(self, spawn, writes, reads, line, loop=None,
                 nested_writes=None, ptr_touches=None, chan_sends=None, chan_recvs=None):
        self.spawn = spawn
        self.join = False
        self.members = range(0)   # events that run inside this spawn
        self.writes = writes
        self.reads = reads
        self.nested_writes = nested_writes or set()
        self.ptr_touches = ptr_touches or set()
        self.chan_sends = chan_sends or []
        self.chan_recvs = chan_recvs or set()
        self.line = line
        self.loop = loop

    def shares(self, other):
        return bool((self.writes & (other.writes | other.reads))
                    | (self.reads & other.writes))

    def written_side(self, other):
        return bool(self.writes & (other.writes | other.reads))


def _collect(body, events, loops, loop=None, fns=None):
    """Flatten a block into events; `loops` records each loop's event range."""
    for stmt in body:
        if not isinstance(stmt, tuple) or not stmt:
            continue
        k = stmt[0]
        line = line_of(stmt)
        if k == "spawn":
            inner = stmt[1]
            before = len(events)
            _collect(inner, events, loops, loop, fns)
            inside = events[before:]
            locals_ = declared(inner)
            write = set().union(*(e.writes for e in inside)) - locals_ if inside else set()
            read = set().union(*(e.reads for e in inside)) - locals_ if inside else set()
            nwrite = set().union(*(e.nested_writes for e in inside)) - locals_ if inside else set()
            ptrs = set().union(*(e.ptr_touches for e in inside)) - locals_ if inside else set()
            crecvs = set().union(*(e.chan_recvs for e in inside)) - locals_ if inside else set()
            event = Event(True, write, read, line, loop, nwrite, ptrs, [], crecvs)
            event.members = range(before + 1, len(events) + 1)
            events.insert(before, event)
            loops[:] = [(a + 1, b + 1) for a, b in loops]
            continue
        read, write, nwrite, ptrs, csends, crecvs = _touch_detail(stmt, fns)
        event = Event(False, write, read, line, loop, nwrite, ptrs, csends, crecvs)
        event.join = _is_join(stmt)
        events.append(event)
        if k in REPEATS:
            start = len(events)
            inner = stmt[2] if k == "while" else (stmt[4] if k == "forrange" else stmt[3])
            _collect(inner, events, loops, (start, None), fns)
            loops.append((start, len(events)))
        elif k == "if":
            _collect(stmt[2], events, loops, loop, fns)
            els = stmt[3]
            if isinstance(els, list):
                _collect(els, events, loops, loop, fns)
            elif isinstance(els, tuple):
                _collect([els], events, loops, loop, fns)
        elif k == "scope":
            _collect(stmt[1], events, loops, loop, fns)
            end_ev = Event(False, set(), set(), line, loop)
            end_ev.join = True
            events.append(end_ev)
        elif k == "try":
            _collect(stmt[1], events, loops, loop, fns)
            _collect(stmt[2], events, loops, loop, fns)


def _is_join(stmt):
    return (stmt[0] == "expr" and isinstance(stmt[1], tuple)
            and stmt[1][0] == "call" and stmt[1][1] == JOIN)


def _containment_graph(body, alias, fns=None):
    """Map `parent_root -> set(child_roots)` across `body`."""
    contains = {}
    for s in _all_stmts(body):
        for c, v in inserts(s, fns):
            rc, rv = root(c), root(v)
            if not rc or not rv:
                continue
            rc, rv = resolve(rc, alias), resolve(rv, alias)
            if rc != rv:
                contains.setdefault(rc, set()).add(rv)
    return contains


def _descendants(name, contains):
    out = set()
    stack = list(contains.get(name, ()))
    steps = 0
    while stack and steps < 256:
        cur = stack.pop()
        steps += 1
        if cur == name or cur in out:
            continue
        out.add(cur)
        stack.extend(contains.get(cur, ()))
    return out


def _cycles(body, errs, owners=None, alias=None, fns=None):
    """Build the owner forest from the inserts this block performs."""
    owners = {} if owners is None else owners
    alias = aliases(body) if alias is None else alias

    def owns(c, v):
        seen = 0
        while c is not None and seen < 4096:
            if c == v:
                return True
            c = owners.get(c)
            seen += 1
        return False

    for stmt in body:
        if not isinstance(stmt, tuple) or not stmt:
            continue
        for container, value in inserts(stmt, fns):
            rc, rv = root(container), root(value)
            if not rc or not rv:
                continue
            rc, rv = resolve(rc, alias), resolve(rv, alias)
            if owns(rc, rv):
                errs.append((line_of(stmt), CYCLE % (rv, rc)))
            elif rc != rv:
                owners.setdefault(rv, rc)
        k = stmt[0]
        if k in REPEATS:
            inner = stmt[2] if k == "while" else (stmt[4] if k == "forrange" else stmt[3])
            _cycles(inner, errs, owners, alias, fns)
        elif k == "if":
            _cycles(stmt[2], errs, owners, alias, fns)
            els = stmt[3]
            if isinstance(els, list):
                _cycles(els, errs, owners, alias, fns)
            elif isinstance(els, tuple):
                _cycles([els], errs, owners, alias, fns)
        elif k in ("spawn", "scope", "try"):
            _cycles(stmt[1], errs, owners, alias, fns)
            if k == "try":
                _cycles(stmt[2], errs, owners, alias, fns)
        elif k in ("fn", "exportfn"):
            _cycles(stmt[3], errs, {}, None, fns)
    return owners


def _races(body, errs, fns=None, is_fn_body=False):
    events, loops = [], []
    alias = aliases(body)
    contains = _containment_graph(body, alias, fns)
    _collect(body, events, loops, None, fns)

    chan_payloads = {}
    for event in events:
        for cr, vr in event.chan_sends:
            rc, rv = resolve(cr, alias), resolve(vr, alias)
            chan_payloads.setdefault(rc, set()).update({rv} | _descendants(rv, contains))

    for event in events:
        w = {resolve(x, alias) for x in event.writes}
        r = {resolve(x, alias) for x in event.reads}
        nw = {resolve(x, alias) for x in event.nested_writes}
        pt = {resolve(x, alias) for x in event.ptr_touches}
        for cr in event.chan_recvs:
            r.update(chan_payloads.get(resolve(cr, alias), ()))
        for x in list(nw):
            w.update(_descendants(x, contains))
        for x in list(r):
            r.update(_descendants(x, contains))
        event.writes = w
        event.reads = r - w
        event.ptr_touches = pt

    def joined_between(lo, hi):
        return any(events[k].join for k in range(lo + 1, min(hi + 1, len(events))))

    for i, e in enumerate(events):
        if not e.spawn or not (e.writes or e.reads or e.ptr_touches):
            continue
        raced = False
        for j in range(i + 1, len(events)):
            other = events[j]
            if j in e.members:          # the spawn's own body is not a second thread
                continue
            if e.loop != other.loop and other.spawn:
                continue
            if not e.shares(other) or joined_between(i, j):
                continue
            clash = (e.writes & (other.writes | other.reads)) | (e.reads & other.writes)
            shared = sorted(clash)[0]
            msg = RACE_JOIN if e.written_side(other) else RACE_READ
            errs.append((other.line or e.line, msg % shared))
            raced = True
            break
        for start, end in loops:
            if not e.writes:
                break
            if start <= i < end and not any(events[k].join for k in range(i + 1, end)):
                errs.append((e.line, RACE_LOOP % sorted(e.writes)[0]))
                raced = True
                break
        if is_fn_body and not raced and (e.writes or e.ptr_touches):
            if not any(events[k].join for k in range(i + 1, len(events))):
                who = sorted(e.writes | e.ptr_touches)[0]
                errs.append((e.line, RACE_UNJOINED % who))


DECLS = {"fn", "impl", "trait", "enum", "extern", "externstruct", "exportfn"}


def _all_stmts(body):
    for stmt in body:
        if not isinstance(stmt, tuple) or not stmt:
            continue
        yield stmt
        for part in stmt[1:]:
            if isinstance(part, list):
                for sub in _all_stmts(part):
                    yield sub
            elif isinstance(part, tuple) and part and part[0] in ("if", "block"):
                for sub in _all_stmts([part]):
                    yield sub


def _build_fn_summaries(stmts):
    fns = {}
    globals_ = set()
    for s in stmts:
        n = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if isinstance(n, tuple) and n and n[0] == "let" and isinstance(n[1], str):
            globals_.add(n[1])

    def register(node):
        if not isinstance(node, tuple) or not node:
            return
        if node[0] == "pub":
            register(node[1])
        elif node[0] in ("fn", "exportfn"):
            name, params, body = node[1], node[2], node[3]
            pnames = []
            mut_idx = set()
            ptr_idx = set()
            for idx, p in enumerate(params):
                if isinstance(p, tuple):
                    pnames.append(p[0])
                    if len(p) > 1 and p[1] in ("mut", "move"):
                        mut_idx.add(idx)
                    ann = p[2] if len(p) > 2 else None
                    if isinstance(ann, str) and (ann == "ptr" or ann.startswith("ptr[")):
                        ptr_idx.add(idx)
                else:
                    pnames.append(p)
            fns[name] = {
                "params": pnames,
                "mut_idx": mut_idx,
                "ptr_idx": ptr_idx,
                "ins_pairs": set(),
                "global_ins": set(),
                "global_writes": set(),
                "global_reads": set(),
                "ret_params": set(),
                "body": body,
                "alias": aliases(body),
            }
            for sub in body:
                register(sub)
        elif node[0] == "extern":
            name, params = node[2], node[3]
            pnames = [prm[0] for prm in params]
            effs = node[8] if len(node) > 8 else ()
            written = {e.split(":", 1)[1] for e in effs if e.startswith("writes:")}
            mut_idx = set()
            ptr_idx = set()
            for i, prm in enumerate(params):
                if prm[0] in written or (len(prm) > 1 and prm[1] == "move"):
                    mut_idx.add(i)
                ann = prm[2] if len(prm) > 2 else None
                if isinstance(ann, str) and (ann == "ptr" or ann.startswith("ptr[")):
                    ptr_idx.add(i)
            fns[name] = {
                "params": pnames,
                "mut_idx": mut_idx,
                "ptr_idx": ptr_idx,
                "ins_pairs": set(),
                "global_ins": set(),
                "global_writes": set(),
                "global_reads": set(),
                "ret_params": set(),
                "body": [],
                "alias": {},
            }
        elif node[0] == "impl":
            for m in node[3]:
                register(m)

    for s in stmts:
        register(s)

    for _ in range(6):
        for info in fns.values():
            if not info["body"]:
                continue
            pindex = {name: i for i, name in enumerate(info["params"])}
            alias = info["alias"]
            local = set(info["params"]) | declared(info["body"])
            contains = _containment_graph(info["body"], alias, fns)
            rev_contains = {}
            for par, kids in contains.items():
                for ch in kids:
                    rev_contains.setdefault(ch, set()).add(par)

            for s in _all_stmts(info["body"]):
                rd, wr, _, pt, csends, _ = _touch_detail(s, fns)
                for w in wr:
                    rw = resolve(w, alias)
                    if rw in pindex:
                        info["mut_idx"].add(pindex[rw])
                    elif rw in globals_ and rw not in local:
                        info["global_writes"].add(rw)
                for r in rd:
                    rr = resolve(r, alias)
                    if rr in globals_ and rr not in local:
                        info["global_reads"].add(rr)
                for p in pt:
                    rp = resolve(p, alias)
                    if rp in pindex:
                        info["ptr_idx"].add(pindex[rp])
                for cr, vr in csends:
                    rc, rv = resolve(cr, alias), resolve(vr, alias)
                    if rc in globals_ and rc not in local and rv in pindex:
                        info["global_ins"].add((rc, pindex[rv]))
                        info["global_writes"].add(rc)
                if s[0] == "set" and isinstance(s[1], str) and s[1] in globals_ and s[1] not in local:
                    info["global_writes"].add(s[1])
                    for vname in _non_own_vars(s[2]):
                        rv = resolve(vname, alias)
                        if rv in pindex:
                            info["global_ins"].add((s[1], pindex[rv]))
                        for ch in _descendants(rv, contains):
                            if ch in pindex:
                                info["global_ins"].add((s[1], pindex[ch]))
                if s[0] == "return" and len(s) > 1 and isinstance(s[1], tuple):
                    for vname in _non_own_vars(s[1]):
                        rv = resolve(vname, alias)
                        if rv in pindex:
                            info["ret_params"].add(pindex[rv])
                        for ch in _descendants(rv, contains):
                            if ch in pindex:
                                info["ret_params"].add(pindex[ch])
                for c, v in inserts(s, fns):
                    rc, rv = root(c), root(v)
                    if not rc or not rv:
                        continue
                    rc, rv = resolve(rc, alias), resolve(rv, alias)
                    if rc in pindex and rv in pindex and rc != rv:
                        info["ins_pairs"].add((pindex[rc], pindex[rv]))
                        info["mut_idx"].add(pindex[rc])
                    if rc in globals_ and rc not in local:
                        info["global_writes"].add(rc)
                        if rv in pindex:
                            info["global_ins"].add((rc, pindex[rv]))
                        for ch in _descendants(rv, contains):
                            if ch in pindex:
                                info["global_ins"].add((rc, pindex[ch]))
                    for anc in list(rev_contains.get(rc, ())):
                        if anc in pindex and rv in pindex and anc != rv:
                            info["ins_pairs"].add((pindex[anc], pindex[rv]))
    return fns


CT_THREAD_ANY = "thread:any"
CT_REENTER_NO = "reenter:no"

SCHEDULE_MSG = ("`%s` is registered as a callback of `%s`, which declares `thread(any)`, "
                "but it touches `%s`; a callback on another thread may not touch the "
                "caller's state (hint: pass it through the callback's parameters, or "
                "declare `thread(same)`)")
REENTER_MSG = ("`%s` is registered as a callback of `%s` and calls `%s` again; a callee "
               "may re-enter a callback unless the declaration says `reenter(no)` "
               "(hint: add `reenter(no)` to the declaration)")


def _callback_slots(stmts):
    """extern name -> (indices of callback parameters, contracts)."""
    out = {}
    for s in stmts:
        node = s[1] if isinstance(s, tuple) and s and s[0] == "pub" else s
        if not (isinstance(node, tuple) and node and node[0] == "extern"):
            continue
        idx = {i for i, p in enumerate(node[3])
               if isinstance(p, tuple) and len(p) > 2 and isinstance(p[2], str)
               and p[2].startswith("fn(")}
        if idx:
            out[node[2]] = (idx, node[9] if len(node) > 9 else ())
    return out


def _uses(body):
    """(names read, functions called) anywhere in a body."""
    names, calls = set(), set()
    for stmt in _all_stmts(body):
        for e in _stmt_exprs(stmt):
            for n in _nodes(e):
                if not isinstance(n, tuple) or not n:
                    continue
                if n[0] == "call" and isinstance(n[1], str):
                    calls.add(n[1])
                elif n[0] == "var" and isinstance(n[1], str):
                    names.add(n[1])
    return names, calls


def _first_global_touch(start, fns, globals_):
    """The first global a function reaches, following calls. None when it is clean."""
    seen, stack = set(), [start]
    while stack:
        cur = stack.pop()
        if cur in seen or cur not in fns:
            continue
        seen.add(cur)
        info = fns[cur]
        names, calls = _uses(info["body"])
        local = set(info["params"]) | declared(info["body"])
        hit = sorted(n for n in names - local if n in globals_)
        if hit:
            return hit[0]
        stack.extend(c for c in calls if c != cur)
    return None


def _reaches(start, target, fns):
    """Whether `start` calls `target`, directly or through other BRAT functions."""
    seen, stack = set(), [start]
    while stack:
        cur = stack.pop()
        if cur in seen or cur not in fns:
            continue
        seen.add(cur)
        _, calls = _uses(fns[cur]["body"])
        if target in calls:
            return True
        stack.extend(c for c in calls if c != cur)
    return False


def _callback_bodies(top, fns):
    """Every statement list a call site can appear in."""
    yield top
    for info in fns.values():
        if info["body"]:
            yield info["body"]


def callback_contracts(stmts, top, fns):
    slots = _callback_slots(stmts)
    if not slots:
        return []
    out = []
    globals_ = set()
    for n in top:
        cur = n
        while isinstance(cur, tuple) and cur and cur[0] == "pub" and len(cur) > 1:
            cur = cur[1][0] if isinstance(cur[1], list) and cur[1] else cur[1]
        if isinstance(cur, tuple) and cur and cur[0] == "let" and isinstance(cur[1], str):
            globals_.add(cur[1])
    for body in _callback_bodies(top, fns):
        for stmt in _all_stmts(body):
            line = line_of(stmt)
            for e in _stmt_exprs(stmt):
                for n in _nodes(e):
                    if not (isinstance(n, tuple) and n and n[0] == "call"):
                        continue
                    if n[1] not in slots:
                        continue
                    idx, contracts = slots[n[1]]
                    args = n[2] or []
                    for i in idx:
                        if i >= len(args) or not (isinstance(args[i], tuple)
                                                  and args[i][0] == "str"):
                            continue
                        cb = args[i][1]
                        if cb not in fns:
                            continue        # the callback gate owns that refusal
                        if CT_THREAD_ANY in contracts:
                            touch = _first_global_touch(cb, fns, globals_)
                            if touch:
                                out.append((line, SCHEDULE_MSG % (cb, n[1], touch)))
                        if CT_REENTER_NO not in contracts and _reaches(cb, n[1], fns):
                            out.append((line, REENTER_MSG % (cb, n[1], n[1])))
    return out


def check_sharing(prog):
    """Every diagnostic this pass has for one flattened program."""
    out = []
    top = []
    fns = _build_fn_summaries(prog[1])
    for stmt in prog[1]:
        node = stmt[1] if stmt[0] == "pub" else stmt
        if isinstance(node, tuple) and node and node[0] in DECLS:
            if node[0] in ("fn", "exportfn"):
                _cycles(node[3], out, None, None, fns)
                _races(node[3], out, fns, is_fn_body=True)
            elif node[0] == "impl":
                for m in node[3]:
                    if isinstance(m, tuple) and m and m[0] == "fn":
                        _cycles(m[3], out, None, None, fns)
                        _races(m[3], out, fns, is_fn_body=True)
            continue
        top.append(stmt)
    _cycles(top, out, None, None, fns)
    _races(top, out, fns, is_fn_body=False)
    out.extend(callback_contracts(prog[1], top, fns))
    return out
