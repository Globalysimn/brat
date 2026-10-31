r"""Adaptive ownership: which mechanism a binding gets, and why.

For every binding the same four questions are asked — how often it is read, how
often it is written, whether it leaves its frame, and whether it crosses a thread
— and the answer picks one of five mechanisms:

```
    (reads, writes, escapes, thread)      strategy
    ----------------------------------    ------------------------
    reads = 0, never escapes              drop      (new_value at once)
    writes = 0, readable, escapes         move      (hand it over, no clone)
    writes = 0, readable, stays           view      (a pointer, no refcount traffic)
    writes > 0, shared with another name  cow       (clone at the first write)
    writes > 0, one owner                 own       (mutate in place)
    crosses a `spawn`                     transfer  (brick 2 discipline)
    anything unprovable                   dynamic   (checked at run time)
```

The first match wins, and the reason is printed with the choice — a compiler that
silently picks a mechanism is a compiler that cannot be argued with. This is what
`brat check --ownership` shows:

```
$ brat check --ownership examples/own_paths.brat
p        own      two writes, never shared: mutate in place
a        moved    read once and moved out: hand it over
```

The choice is not a semantic change: `view` and `move` are what the language
already does, and `cow` is named but not enabled (see the Roadmap — a shallow
clone aliases the elements inside the container, so accepting a two-writer program
by cloning would quietly change what the reader sees).
"""

try:
    from brat.parser import _LINES
except ImportError:  # pragma: no cover - the module is imported from two roots
    try:
        from parser import _LINES
    except ImportError:
        _LINES = {}

COPY_ANNS = {"i32", "i64", "u64", "f64", "bool"}
MUTATORS = {"push", "pop", "sort", "sort_by", "ins", "del", "set", "clear"}
DECLS = {"fn", "impl", "trait", "enum", "extern", "externstruct", "exportfn"}

DROP = "drop"
MOVE = "move"
VIEW = "view"
COW = "cow"
OWN = "own"
TRANSFER = "transfer"
DYNAMIC = "dynamic"


class Uses:
    """Everything the analysis needs to know about one binding."""

    def __init__(self, name, line, copy):
        self.name = name
        self.line = line
        self.copy = copy
        self.reads = 0
        self.writes = 0
        self.escapes = 0
        self.thread = 0
        self.shared_with = []       # [(name, last read line)] — names over this storage
        self.write_lines = []
        self.last_read = 0

    def strategy(self):
        if self.copy:
            return OWN, "a Copy value: passed by value, nothing to free"
        if self.thread:
            return TRANSFER, "used inside `spawn`: handed over, never shared mutably"
        if self.reads == 0 and self.escapes == 0:
            return DROP, "never read: free it at the value, no binding needed"
        if self.writes == 0:
            if self.escapes:
                return MOVE, "read and then leaves the frame: hand it over, no clone"
            return VIEW, ("read %d time(s), never written: a pointer, no refcount "
                          "traffic" % self.reads)
        first_write = min(self.write_lines) if self.write_lines else 0
        alive = [(n, last) for n, line, last in self.shared_with
                 if line < first_write and (not last or last >= first_write)]
        if alive:
            name, last = alive[0]
            return COW, ("`%s` still reads it at line %s and the write is at line %s: "
                         "clone-on-write would copy the buffer, but the values inside "
                         "would still be shared, so the write is refused (channel it, "
                         "or stop reading through `%s` first)"
                         % (name, last or "?", first_write or "?", name))
        if self.shared_with:
            return OWN, "the other name is gone by line %d: mutate in place" % first_write
        return OWN, "written %d time(s), never shared: mutate in place%s" % (
            self.writes, ", then handed over" if self.escapes else "")


def _root(e):
    while isinstance(e, tuple) and e:
        if e[0] == "var":
            return e[1]
        if e[0] in ("move", "quest", "field", "idx"):
            e = e[1]
        else:
            return None
    return None


def _walk_expr(e, on_read, on_write):
    if not isinstance(e, tuple) or not e:
        return
    kind = e[0]
    if kind == "var":
        on_read(e[1])
        return
    if kind == "move":
        on_read(_root(e[1]))
        on_write(_root(e[1]))
        return
    if kind == "mcall" and e[2] in MUTATORS:
        on_write(_root(e[1]))
        for a in e[3]:
            _walk_expr(a, on_read, on_write)
        return
    if kind == "call":
        for a in e[2]:
            _walk_expr(a, on_read, on_write)
        return
    for sub in e[1:]:
        if isinstance(sub, tuple):
            _walk_expr(sub, on_read, on_write)
        elif isinstance(sub, list):
            for x in sub:
                _walk_expr(x, on_read, on_write)


def analyse_body(body):
    """Uses of every binding, in declaration order."""
    uses = {}

    def slot(name, line=None, copy=False):
        if not name or not isinstance(name, str):
            return None
        if name not in uses:
            uses[name] = Uses(name, line, copy)
        return uses[name]

    line = [0]

    threading_ = [False]

    def on_read(name):
        u = slot(name)
        if u:
            u.reads += 1
            u.last_read = max(u.last_read, line[0])
            if threading_[0]:
                u.thread += 1

    def on_write(name):
        u = slot(name)
        if u:
            u.writes += 1
            u.write_lines.append(line[0])
            if threading_[0]:
                u.thread += 1

    def scan(stmts, in_thread=False):
        for s in stmts if isinstance(stmts, list) else []:
            if not isinstance(s, tuple) or not s:
                continue
            kind = s[0]
            if kind in DECLS:
                continue
            line[0] = _LINES.get(id(s)) or line[0]
            if kind == "let":
                name, e = s[1], s[2]
                ann = s[5] if len(s) > 5 else None
                u = slot(name, _LINES.get(id(s)), ann in COPY_ANNS)
                if u and ann in COPY_ANNS:
                    u.copy = True
                _walk_expr(e, on_read, on_write)
                src = _root(e)
                if src and src != name:
                    other = slot(src)
                    if other:
                        other.shared_with.append(name)
                continue
            if kind == "set":
                on_write(s[1])
                _walk_expr(s[2], on_read, on_write)
                continue
            if kind in ("setidx", "setfield"):
                on_write(_root(s[1]))
                for sub in s[1:]:
                    if isinstance(sub, (tuple, list)):
                        _walk_expr(sub, on_read, on_write) if isinstance(sub, tuple) else None
                continue
            if kind == "return":
                for name in _names(s[1]):
                    u = slot(name)
                    if u:
                        u.escapes += 1
                        u.reads += 1
                continue
            if kind == "spawn":
                was = threading_[0]
                threading_[0] = True
                scan(s[1], True)
                threading_[0] = was
                continue
            if kind in ("while", "foriter", "forrange", "if", "try", "scope", "unsafe",
                        "block", "defer"):
                for sub in s[1:]:
                    if isinstance(sub, list):
                        scan(sub, in_thread)
                    elif isinstance(sub, tuple):
                        _walk_expr(sub, on_read, on_write)
                continue
            for sub in s[1:]:
                if isinstance(sub, list):
                    scan(sub, in_thread)
                elif isinstance(sub, tuple):
                    _walk_expr(sub, on_read, on_write)

    scan(body)
    for u in uses.values():
        u.shared_with = [(n, uses[n].line, uses[n].last_read) for n in u.shared_with
                         if n in uses]
    return list(uses.values())


def _names(e):
    out = []
    if isinstance(e, tuple):
        if e and e[0] == "var":
            return [e[1]]
        for sub in e[1:]:
            if isinstance(sub, (tuple, list)):
                out.extend(_names(sub))
    elif isinstance(e, list):
        for sub in e:
            out.extend(_names(sub))
    return out


def strategies(prog):
    """[(body index, name, strategy, reason, line)] for one file."""
    rows = []
    for i, body in enumerate(_bodies(prog)):
        for u in analyse_body(body):
            name, why = u.strategy()
            rows.append((i, u.name, name, why, u.line))
    return rows


def _bodies(prog):
    yield prog[1]
    for stmt in prog[1]:
        node = stmt[1] if stmt[0] == "pub" else stmt
        if isinstance(node, tuple) and node:
            if node[0] == "fn":
                yield node[3]
            elif node[0] == "impl":
                for fn in node[3]:
                    yield fn[3]


def format_strategies(prog):
    """One line per binding, and a count per strategy."""
    rows = strategies(prog)
    lines = []
    seen = set()
    for _i, name, how, why, line in rows:
        if (name, how) in seen:
            continue
        seen.add((name, how))
        where = ("line %d" % line) if line else ""
        lines.append("%-16s %-9s %-10s %s" % (name, how, where, why))
    counts = {}
    for _i, _n, how, _w, _l in rows:
        counts[how] = counts.get(how, 0) + 1
    if counts:
        summary = ", ".join("%d %s" % (n, how) for how, n in sorted(counts.items()))
        lines.append("strategies: " + summary)
    return "\n".join(lines)
