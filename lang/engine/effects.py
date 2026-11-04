"""Effect analysis helpers used by the compiler and `par for` purity gate.

    raise   it can fail (`raise`, `?`, `unwrap`, `assert`, a runtime error)
    io      it talks to the outside world (print, files, env, network, time)
    alloc   it builds a heap value (a container or string literal, concatenation)
    spawn   it starts a thread
    ffi     it calls through the C boundary

Used exclusively where enforced: `borrow.parfor_rules` refuses impure `par for`
bodies and folds, and `may_fail` guards native scalar fast paths against
swallowing pending runtime errors.
"""

ORDER = ("raise", "io", "alloc", "spawn", "ffi")

RAISE_CALLS = {"raise", "unwrap", "assert", "exit"}
IO_CALLS = {"print", "eprint", "log.info", "log.warn", "log.error", "trace.span",
            "read_file", "write_file", "read_bytes", "write_bytes", "proc_run",
            "fs_rename", "fs_remove", "random_bytes", "https_request_raw",
            "sql_open", "sql_close", "sql_exec", "sql_query",
            "args", "env_get", "time_ms", "mono_ns", "sleep_ms",
            "tcp_listen", "tcp_connect", "tcp_send", "tcp_recv", "tcp_close"}
IO_HEADS = {"tcp", "log", "trace"}
PLUGIN_FFI_CALLS = {"lib_open", "lib_sym", "lib_close", "call",
                    "ptr_read_f32", "ptr_write_f32", "ptr_read_f64", "ptr_write_f64",
                    "ptr_read_i64", "ptr_write_i64", "ptr_read_i32", "ptr_write_i32",
                    "ptr_read_u8", "ptr_write_u8", "ptr_read_ptr", "ptr_write_ptr",
                    "ptr_read_str", "ptr_write_str", "ptr_read_struct", "ptr_write_struct",
                    "ptr_of_str", "str_from_ptr", "slice_of_ptr", "slice_of_struct",
                    "slice_set", "errno"}
ALLOC_NODES = {"vec", "maplit", "struct", "structlit", "closure"}
FAIL_HEADS = {"quest", "unwrap", "idx", "idxp", "setidx"}


def _row(*names):
    return frozenset(n for n in names if n)


def _walk(node, out):
    """Effects visible in one subtree, without following calls."""
    if isinstance(node, tuple):
        if not node:
            return
        head = node[0]
        if not isinstance(head, str):
            for sub in node:
                _walk(sub, out)
            return
        if head == "quest":
            out.add("raise")
        elif head in ("spawn", "scope"):
            out.add("spawn")
        elif head in ALLOC_NODES:
            out.add("alloc")
        elif head == "call":
            name = node[1]
            if name in RAISE_CALLS:
                out.add("raise")
            elif name in PLUGIN_FFI_CALLS:
                out.add("ffi")
            elif name in IO_CALLS or name.split(".")[0] in IO_HEADS:
                out.add("io")
        elif head == "mcall":
            meth = node[2]
            if meth in ("push", "ins", "set", "copy", "map", "filter", "chars",
                        "split", "replace", "trim", "upper", "lower", "join"):
                out.add("alloc")
            if meth in ("send", "close", "recv"):
                out.add("io")
        for sub in node[1:]:
            _walk(sub, out)
    elif isinstance(node, list):
        for sub in node:
            _walk(sub, out)


def _calls(node, fcalls, mcalls=None):
    """Names of the functions and methods a subtree calls."""
    if isinstance(node, tuple):
        if not node:
            return
        if not isinstance(node[0], str):
            for sub in node:
                _calls(sub, fcalls, mcalls)
            return
        if node[0] == "call" and isinstance(node[1], str):
            fcalls.add(node[1])
        elif node[0] == "mcall" and isinstance(node[2], str):
            (mcalls if mcalls is not None else fcalls).add(node[2])
        for sub in node[1:]:
            _calls(sub, fcalls, mcalls)
    elif isinstance(node, list):
        for sub in node:
            _calls(sub, fcalls, mcalls)


def _bodies(prog):
    """(key, params, body) for every function and method in the file."""
    out = []
    for stmt in prog[1]:
        node = stmt[1] if stmt[0] == "pub" else stmt
        if not isinstance(node, tuple) or not node:
            continue
        if node[0] in ("fn", "exportfn"):
            out.append((node[1], node[2], node[3]))
        elif node[0] == "impl":
            for fn in node[3]:
                out.append((node[2] + "." + fn[1], fn[2], fn[3]))
        elif node[0] == "extern":
            out.append((node[2], node[3], None))
    return out


def _heads(body):
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


def summarise(prog, rounds=None):
    """Map every function name to its effect row, as a sorted tuple of names."""
    bodies = _bodies(prog)
    local = {}
    for name, _params, body in bodies:
        if body is None:
            local[name] = _row("ffi")
            continue
        row = set()
        _walk(body, row)
        if "spawn" in _heads(body):
            row.add("spawn")
        local[name] = _row(*row)

    by_short = {}
    for name, _p, _b in bodies:
        if "." in name:
            by_short.setdefault(name.rsplit(".", 1)[-1], []).append(name)
    for _ in range(rounds or max(4, len(bodies) + 1)):
        changed = False
        for name, _params, body in bodies:
            if body is None:
                continue
            fcalls = set()
            mcalls = set()
            _calls(body, fcalls, mcalls)
            grown = set(local[name])
            for c in fcalls:
                if c in local:
                    grown |= local[c]
            for m in mcalls:
                for key in by_short.get(m, ()):
                    grown |= local[key]
            if len(grown) != len(local[name]):
                local[name] = _row(*grown)
                changed = True
        if not changed:
            break
    return {name: tuple(n for n in ORDER if n in row) for name, row in local.items()}


def pure(rows, name):
    """True when calling `name` cannot fail, touch the world, or allocate."""
    return not rows.get(name)


def may_fail(bodies):
    """Names whose body can leave an error pending: `raise`, `?`, div, index, unwrap."""
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
