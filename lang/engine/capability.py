"""Sharing effects: reference cycles, and writes that escape a `spawn`.

Two questions, answered in one pass over a function body:

  * does this insert close a reference cycle? A refcount cannot reclaim a cycle,
    and the runtime has no cycle collector, so the ownership graph must stay a
    forest;
  * is a place written inside `spawn` touched again before a `join_all()`? Then
    two threads write the same object, which is a data race whatever the checker
    says about mutability.

Neither question needs a fixpoint. The owner graph is a forest, so "does `c`
already own `v`" is a walk up one chain; the concurrency question is "is there a
join between these two statement positions".
"""

try:
    from brat.parser import _LINES
except ImportError:  # pragma: no cover - the module is imported from two roots
    try:
        from parser import _LINES
    except ImportError:
        _LINES = {}

INSERT_METH = {"push", "ins", "set"}
# Builtins that write through the pointer or slice they are given: the place the
# argument names is written, even though no `let` in the program says so.
PTR_WRITE = {"ptr_write_i64", "ptr_write_f64", "ptr_write_f32", "ptr_write_u8",
             "ptr_write_str", "ptr_write_ptr", "ptr_write_struct", "slice_set", "fill"}
RESOURCE_CLOSE = {"tcp_close"}
MUTATE_METH = {"pop", "sort", "sort_by", "del", "clear"}
JOIN = "join_all"
REPEATS = {"while", "forrange", "foriter"}

CYCLE = ("reference cycle: `%s` goes into `%s`, which already owns it "
         "(refcounting cannot reclaim a cycle)")
RACE_JOIN = ("`%s` is written inside `spawn` and touched again with no `join_all()` "
             "in between (hint: send the value through a channel)")
RACE_READ = ("`%s` is read inside `spawn` and written here with no `join_all()` "
             "in between (hint: `join_all()` first, or send the value through a "
             "channel)")
RACE_LOOP = ("`%s` is written inside `spawn` launched in a loop that never joins "
             "(hint: join inside the loop, or send through a channel)")


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


def _nodes(e):
    """Every expression under `e`, outermost first."""
    out = []
    stack = [e]
    while stack:
        n = stack.pop()
        if not isinstance(n, tuple) or not n:
            continue
        out.append(n)
        for sub in n[1:]:
            if isinstance(sub, tuple):
                stack.append(sub)
            elif isinstance(sub, list):
                stack.extend(x for x in sub if isinstance(x, tuple))
    return out


def aliases(body, out=None):
    """`let c = a` makes c another name for a's storage, at any depth."""
    out = {} if out is None else out
    for stmt in body:
        if not isinstance(stmt, tuple) or not stmt:
            continue
        if stmt[0] == "let" and isinstance(stmt[2], tuple):
            r = root(stmt[2])
            if r and r != stmt[1]:
                out[stmt[1]] = r
        for part in stmt[1:]:
            if isinstance(part, list):
                aliases(part, out)
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


def inserts(stmt, fns=None):
    """(container, value) pairs this statement inserts, as expressions."""
    out = []
    if stmt[0] == "setidx":
        out.append((stmt[1], stmt[3]))
    for e in _stmt_exprs(stmt):
        for n in _nodes(e):
            if n[0] == "mcall":
                if n[2] in INSERT_METH and n[3]:
                    # `push(x)` / `ins(i, x)` put the first argument in; `set(k, v)` the last.
                    out.append((n[1], n[3][-1] if n[2] == "set" else n[3][0]))
                elif fns and n[2] in fns:
                    args = [n[1]] + list(n[3] or [])
                    for ci, vi in fns[n[2]]["ins_pairs"]:
                        if ci < len(args) and vi < len(args):
                            out.append((args[ci], args[vi]))
            elif n[0] == "call" and fns and n[1] in fns:
                args = list(n[2] or [])
                for ci, vi in fns[n[1]]["ins_pairs"]:
                    if ci < len(args) and vi < len(args):
                        out.append((args[ci], args[vi]))
    return out


def touched(stmt, fns=None):
    """(read, written) roots of one statement; written means mutated in place."""
    read, write = set(), set()
    k = stmt[0]
    exprs = _stmt_exprs(stmt)
    for e in exprs:
        for n in _nodes(e):
            if n[0] == "mcall":
                r = root(n[1])
                if r:
                    if n[2] in INSERT_METH or n[2] in MUTATE_METH:
                        write.add(r)
                    elif fns and n[2] in fns and 0 in fns[n[2]]["mut_idx"]:
                        write.add(r)
                    else:
                        read.add(r)
                if fns and n[2] in fns:
                    args = [n[1]] + list(n[3] or [])
                    for idx in fns[n[2]]["mut_idx"]:
                        if idx < len(args):
                            ar = root(args[idx])
                            if ar:
                                write.add(ar)
            elif n[0] == "call" and fns and n[1] in fns:
                args = list(n[2] or [])
                for idx in fns[n[1]]["mut_idx"]:
                    if idx < len(args):
                        ar = root(args[idx])
                        if ar:
                            write.add(ar)
            elif n[0] == "call" and n[1] in PTR_WRITE:
                # C is told to write through this pointer: two threads that call
                # one of these on the same place are two writers of it.
                args = n[2] or []
                r = root(args[0]) if args else None
                if r:
                    write.add(r)
            elif n[0] == "call" and n[1] in RESOURCE_CLOSE:
                args = n[2] or []
                r = root(args[0]) if args else None
                if r:
                    write.add(r)
            elif n[0] == "move":
                # Handing the storage away is a write to the place it came from:
                # `free(move p)` and a channel send both end p's life here.
                r = root(n)
                if r:
                    write.add(r)
            elif n[0] in ("var", "field", "idx"):
                r = root(n)
                if r:
                    read.add(r)
    if k == "setidx":
        r = root(stmt[1])
        if r:
            write.add(r)
    # A plain `x = 2` rebinds the *binding*; it does not touch the object the
    # binding pointed at, so it is not a write this pass reports. That is only
    # sound because `spawn` copies the frame -- every name the child's body
    # mentions is snapshotted at the launch -- so a parent that rebinds a name
    # while the child reads it is two frames, not two threads on one object.
    # What *is* reported is a mutation through the object: `v.push`, `v[i] = x`,
    # `ptr_write_*`, `move`. Before the snapshot existed, the child read a
    # script-level binding straight out of the global and this pass was silent
    # about a real race (see docs/project/LEDGER.md, 2026-10-30).
    if k == "setfield":
        r = root(stmt[1])
        if r:
            write.add(r)
    return read - write, write


def declared(body, acc=None):
    """Every name the block binds, so a spawn's own locals are not shared."""
    acc = set() if acc is None else acc
    for stmt in body:
        if not isinstance(stmt, tuple) or not stmt:
            continue
        if stmt[0] in ("let", "forrange", "foriter"):
            acc.add(stmt[1])
        for part in stmt[1:]:
            if isinstance(part, list):
                declared(part, acc)
        if stmt[0] == "try" and len(stmt) > 3 and stmt[3]:
            acc.add(stmt[3])
    return acc


class Event:
    """One statement's effect on sharing: what it mutates, what it looks at."""

    __slots__ = ("spawn", "join", "writes", "reads", "line", "loop", "members")

    def __init__(self, spawn, writes, reads, line, loop=None):
        self.spawn = spawn
        self.join = False
        self.members = range(0)   # events that run inside this spawn
        self.writes = writes
        self.reads = reads
        self.line = line
        self.loop = loop

    def shares(self, other):
        """One of the two threads writes a place the other touches.

        A write on either side is a race: write/read and write/write both need
        a barrier. Two reads do not, so read-only sharing stays legal.
        """
        return bool((self.writes & (other.writes | other.reads))
                    | (self.reads & other.writes))

    def written_side(self, other):
        """True when this spawn's own write is what collides."""
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
            event = Event(True, write, read, line, loop)
            event.members = range(before + 1, len(events) + 1)
            events.insert(before, event)
            loops[:] = [(a + 1, b + 1) for a, b in loops]
            continue
        read, write = touched(stmt, fns)
        event = Event(False, write, read, line, loop)
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
        elif k in ("scope", "try"):
            _collect(stmt[1], events, loops, loop, fns)
            if k == "try":
                _collect(stmt[2], events, loops, loop, fns)


def _is_join(stmt):
    return (stmt[0] == "expr" and isinstance(stmt[1], tuple)
            and stmt[1][0] == "call" and stmt[1][1] == JOIN)


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


def _races(body, errs, fns=None):
    events, loops = [], []
    alias = aliases(body)
    _collect(body, events, loops, None, fns)
    for event in events:
        event.writes = {resolve(w, alias) for w in event.writes}
        event.reads = {resolve(r, alias) for r in event.reads}

    def joined_between(lo, hi):
        return any(events[k].join for k in range(lo + 1, min(hi + 1, len(events))))

    for i, e in enumerate(events):
        if not e.spawn or not (e.writes or e.reads):
            continue
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
            break
        for start, end in loops:
            # The loop rule is about writes only: a read-only fan-out that joins
            # after the loop is legal, and refusing it would be a false positive.
            if not e.writes:
                break
            if start <= i < end and not any(events[k].join for k in range(i + 1, end)):
                errs.append((e.line, RACE_LOOP % sorted(e.writes)[0]))
                break


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

    def register(node):
        if not isinstance(node, tuple) or not node:
            return
        if node[0] == "pub":
            register(node[1])
        elif node[0] in ("fn", "exportfn"):
            name, params, body = node[1], node[2], node[3]
            pnames = []
            mut_idx = set()
            for idx, p in enumerate(params):
                if isinstance(p, tuple):
                    pnames.append(p[0])
                    if len(p) > 1 and p[1] == "mut":
                        mut_idx.add(idx)
                else:
                    pnames.append(p)
            fns[name] = {
                "params": pnames,
                "mut_idx": mut_idx,
                "ins_pairs": set(),
                "body": body,
                "alias": aliases(body),
            }
            for sub in body:
                register(sub)
        elif node[0] == "extern":
            # `writes(p)` is the declaration's own account of what the C function
            # does to the memory it is handed. Registering it here is what makes
            # a foreign write a write: before effects existed, `free(p)` or
            # `memcpy` inside a `spawn` touched nothing this pass could see, so
            # the race it caused had nobody to report it.
            name, params = node[2], node[3]
            pnames = [prm[0] for prm in params]
            written = {e.split(":", 1)[1] for e in (node[8] if len(node) > 8 else ())
                       if e.startswith("writes:")}
            fns[name] = {
                "params": pnames,
                "mut_idx": {i for i, n in enumerate(pnames) if n in written},
                "ins_pairs": set(),
                "body": [],
                "alias": {},
            }
        elif node[0] == "impl":
            for m in node[3]:
                register(m)

    for s in stmts:
        register(s)

    for _ in range(3):
        for info in fns.values():
            pindex = {name: i for i, name in enumerate(info["params"])}
            alias = info["alias"]
            for s in _all_stmts(info["body"]):
                for c, v in inserts(s, fns):
                    rc, rv = root(c), root(v)
                    if not rc or not rv:
                        continue
                    rc, rv = resolve(rc, alias), resolve(rv, alias)
                    if rc in pindex and rv in pindex and rc != rv:
                        info["ins_pairs"].add((pindex[rc], pindex[rv]))
                        info["mut_idx"].add(pindex[rc])
    return fns


# What a C function may do with the callback it is handed (the contract is
# written on the callback parameter, stored on the declaration -- `parser.py`,
# `take_contracts`). Two defaults: the callee gets the caller's thread, and it
# may re-enter its callback. Both defaults are the dangerous half of the pair,
# because an unstated promise is a fact the checker cannot use.
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
    """A callback's two runtime promises, checked where it is handed over.

    `thread(any)` says the callee may call back from a thread the program never
    made. Then the callback's body must not reach anything the caller's thread
    also owns -- a global, or a function that touches one -- because the runtime
    is not thread-safe and the two threads would meet inside it.
    `reenter(no)` says the callee will not enter a callback while one of its own
    calls is on the stack. Without that promise, a callback that calls its own
    library again is a re-entry the callee never agreed to, which is the shape
    that crashes `qsort` and `sqlite3_step` from inside themselves.
    """
    slots = _callback_slots(stmts)
    if not slots:
        return []
    out = []
    globals_ = {n[1] for n in top
                if isinstance(n, tuple) and n and n[0] == "let" and isinstance(n[1], str)}
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
                _races(node[3], out, fns)
            elif node[0] == "impl":
                for m in node[3]:
                    if isinstance(m, tuple) and m and m[0] == "fn":
                        _cycles(m[3], out, None, None, fns)
                        _races(m[3], out, fns)
            continue
        top.append(stmt)
    _cycles(top, out, None, None, fns)          # a script is one body, like `main`
    _races(top, out, fns)
    out.extend(callback_contracts(prog[1], top, fns))
    return out
