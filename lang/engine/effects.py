"""Inferred effect rows: what a function can do besides return a value.

    raise   it can fail (`raise`, `?`, `unwrap`, `assert`, a runtime error)
    io      it talks to the outside world (print, files, env, network, time)
    alloc   it builds a heap value (a container or string literal, concatenation)
    spawn   it starts a thread
    ffi     it calls through the C boundary

The row is inferred from the body and from the rows of the functions it calls,
so a caller inherits what its callees do. Nothing here rejects a program: the
row is information first. It is what the emitter needs in order to know a fast
path is safe (`raise` absent), what `?` checking will rest on, and what
`par for` needs in order to call a body pure.
"""

try:
    from brat.parser import _LINES
except ImportError:  # pragma: no cover - the engine directory is also a plain path
    try:
        from parser import _LINES
    except ImportError:
        _LINES = {}

ORDER = ("raise", "io", "alloc", "spawn", "ffi")

RAISE_CALLS = {"raise", "unwrap", "assert", "exit"}
IO_CALLS = {"print", "eprint", "log.info", "log.warn", "log.error", "trace.span",
            "read_file", "write_file", "args", "env_get", "time_ms", "sleep_ms",
            "tcp_listen", "tcp_connect", "tcp_send", "tcp_recv", "tcp_close"}
IO_HEADS = {"tcp", "log", "trace"}
ALLOC_NODES = {"vec", "maplit", "structlit", "closure"}
DECLS = {"fn", "impl", "trait", "enum", "extern", "externstruct", "exportfn"}


def _row(*names):
    return frozenset(n for n in names if n)


def _walk(node, out):
    """Effects visible in one subtree, without following calls."""
    if isinstance(node, tuple):
        if not node:
            return
        head = node[0]
        if head == "quest":
            out.add("raise")
        elif head in ALLOC_NODES:
            out.add("alloc")
        elif head == "str" and "{" in str(node[1]):
            pass
        elif head == "call":
            name = node[1]
            if name in RAISE_CALLS:
                out.add("raise")
            elif name in IO_CALLS or name.split(".")[0] in IO_HEADS:
                out.add("io")
        elif head == "mcall":
            meth = node[2]
            if meth in ("push", "ins", "set", "copy", "map", "filter", "chars",
                        "split", "replace", "trim", "upper", "lower", "join"):
                out.add("alloc")
        for sub in node[1:]:
            _walk(sub, out)
    elif isinstance(node, list):
        for sub in node:
            _walk(sub, out)


def _calls(node, out):
    """Names of the functions a subtree calls, methods by their bare name too."""
    if isinstance(node, tuple):
        if not node:
            return
        if node[0] == "call" and isinstance(node[1], str):
            out.add(node[1])
        elif node[0] == "mcall" and isinstance(node[2], str):
            out.add(node[2])
        for sub in node[1:]:
            _calls(sub, out)
    elif isinstance(node, list):
        for sub in node:
            _calls(sub, out)


def _bodies(prog):
    """(key, params, body) for every function and method in the file."""
    out = []
    for stmt in prog[1]:
        node = stmt[1] if stmt[0] == "pub" else stmt
        if not isinstance(node, tuple) or not node:
            continue
        if node[0] == "fn":
            out.append((node[1], node[2], node[3]))
        elif node[0] == "impl":
            for fn in node[3]:
                out.append((node[2] + "." + fn[1], fn[2], fn[3]))
        elif node[0] == "extern":
            out.append((node[2], node[3], None))
    return out


def summarise(prog, rounds=None):
    """Map every function name to its effect row, as a sorted tuple of names."""
    bodies = _bodies(prog)
    local = {}
    externs = set()
    for name, _params, body in bodies:
        if body is None:
            externs.add(name)
            local[name] = _row("ffi")
            continue
        row = set()
        _walk(body, row)
        if "spawn" in _heads(body):
            row.add("spawn")
        local[name] = _row(*row)

    alias = {name.rsplit(".", 1)[-1]: name for name, _p, _b in bodies}
    for _ in range(rounds or max(4, len(bodies) + 1)):
        changed = False
        for name, _params, body in bodies:
            if body is None:
                continue
            called = set()
            _calls(body, called)
            grown = set(local[name])
            for c in called:
                key = c if c in local else alias.get(c)
                if key:
                    grown |= local[key]
            if len(grown) != len(local[name]):
                local[name] = _row(*grown)
                changed = True
        if not changed:
            break
    return {name: tuple(n for n in ORDER if n in row) for name, row in local.items()}


def _heads(body):
    """Statement kinds anywhere in a body."""
    out = set()

    def walk(node):
        if isinstance(node, tuple):
            if node and isinstance(node[0], str):
                out.add(node[0])
            for sub in node[1:]:
                walk(sub)
        elif isinstance(node, list):
            for sub in node:
                walk(sub)

    walk(body)
    return out


def format_rows(rows, title="effects"):
    """One line per function, effects in a stable order."""
    lines = []
    for name in sorted(rows):
        row = ", ".join(rows[name]) or "pure"
        lines.append("%-28s %s: %s" % (name, title, row))
    return "\n".join(lines)


def pure(rows, name):
    """True when calling `name` cannot fail, cannot touch the world and cannot allocate."""
    return not rows.get(name)

FAIL_HEADS = {"quest", "unwrap", "idx", "idxp"}


def may_fail(bodies):
    """Names whose body can leave an error pending: `raise`, `?`, div, index, unwrap.

    The user-facing row stays quiet about `10 / x` (it is a normal arithmetic
    expression, not a promise of failure), but the emitter cannot: a fast-path
    function returning a bare int64 has no room to carry a pending error, so a
    call to anything in this set must be followed by `verr_pending()`.
    """
    def touches(node):
        if isinstance(node, tuple):
            if not node:
                return False
            head = node[0]
            if head in FAIL_HEADS:
                return True
            if head == "bin" and node[1] in ("/", "%"):
                return True
            if head == "call" and node[1] in RAISE_CALLS:
                return True
            return any(touches(sub) for sub in node[1:])
        if isinstance(node, list):
            return any(touches(sub) for sub in node)
        return False

    def calls(node, out):
        if isinstance(node, tuple):
            if node and node[0] == "call" and isinstance(node[1], str):
                out.add(node[1])
            for sub in node[1:]:
                calls(sub, out)
        elif isinstance(node, list):
            for sub in node:
                calls(sub, out)

    fail = set()
    edges = {}
    for name, _params, body in bodies:
        if body is None:
            fail.add(name)
            continue
        if touches(body):
            fail.add(name)
        got = set()
        calls(body, got)
        edges[name] = got
    changed = True
    while changed:
        changed = False
        for name, callees in edges.items():
            if name in fail:
                continue
            if callees & fail:
                fail.add(name)
                changed = True
    return fail
