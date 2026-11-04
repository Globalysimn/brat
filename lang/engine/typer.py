import importlib.util
import os
import re
import sys

try:
    from brat.parser import _LINES
except ImportError:
    try:
        from parser import _LINES
    except ImportError:
        _LINES = {}

try:
    from diag import to_human, _entries_to_diags
except ImportError:  # pragma: no cover
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from diag import to_human, _entries_to_diags


class Errs(list):
    def __init__(self):
        super().__init__()
        self.line = None
        self.arena_only = False
        self.silent = False

    def note(self, msg):
        if self.silent:
            return
        if self.arena_only and not msg.startswith("arena: "):
            return
        self.append((self.line, msg))


def report(f, errs, src):
    sys.stdout.write(to_human(f, src, _entries_to_diags(f, errs, "error")))


_WORD = {"i8": "int", "u8": "int", "i16": "int", "u16": "int",
         "i32": "int", "u32": "int", "i64": "int", "u64": "int",
         "usize": "int", "isize": "int",
         "f32": "float", "f64": "float", "str": "str", "ptr": "raw", "bool": "bool"}
_G_STRUCTS = None
_G_UNIONS = None


def _word(inner):
    """The element of a `ptr[T]`/`slice[T]`: a scalar name, or T itself.

    A name that is not a scalar is kept as written rather than erased to `dyn`:
    `ptr[Node]` has to reach the struct table, and a check between `ptr[Node]`
    and `ptr[Reg]` is the one a driver needs. `ok()` still accepts `dyn` on
    either side, so an unannotated `ptr` keeps matching anything.
    """
    w = inner.strip()
    return _WORD.get(w, w)


def _cb_arity(ann):
    parsed = _cb_signature(ann)
    if parsed is None:
        return 0
    return len(parsed[0])


def _split_type_list(text):
    parts = []
    start = 0
    depth = 0
    for i, char in enumerate(text):
        if char in "([":
            depth += 1
        elif char in ")]":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def _abi_type(text):
    return re.sub(r"\s+", "", text or "none")


def _cb_signature(ann):
    if not isinstance(ann, str) or not ann.startswith("fn"):
        return None
    start = ann.find("(")
    if start < 0:
        return None
    depth = 0
    end = -1
    for i in range(start, len(ann)):
        if ann[i] == "(":
            depth += 1
        elif ann[i] == ")":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end < 0:
        return None
    params = _split_type_list(ann[start + 1:end])
    result = ann[end + 1:].strip()
    if result.startswith("->"):
        result = result[2:].strip()
    elif result:
        return None
    else:
        result = "none"
    return params, result


def ann_type(ann):
    if isinstance(ann, str) and ann.startswith("fn"):
        return ("cb", _cb_arity(ann), ann)
    if ann in ("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "isize"):
        return "int"
    if ann in ("f64", "f32"):
        return "float"
    if ann == "bool":
        return "bool"
    if ann == "str":
        return "str"
    if isinstance(ann, str) and ann.startswith("ptr["):
        return ("ptr", _word(ann[4:-1]))
    if ann == "ptr":
        return "int"
    if isinstance(ann, str) and ann.startswith("slice["):
        return ("slice", _word(ann[6:-1]))
    if ann == "slice":
        return ("slice", "dyn")
    if ann and ann.startswith("vec["):
        inner = ann[4:].rstrip("]")
        return ("vec", ann_type(inner) if inner else "dyn")
    if ann == "vec":
        return ("vec", "dyn")
    if ann and (ann == "map" or ann.startswith("map[")):
        return "map"
    if isinstance(ann, str) and "[" in ann and ann.endswith("]"):
        lb = ann.find("[")
        elem, cnt = ann[:lb].strip(), ann[lb + 1:-1].strip()
        if elem and cnt.isdigit():
            return ("vec", ann_type(elem))
    if isinstance(ann, str) and _G_UNIONS is not None and ann in _G_UNIONS:
        return ("struct", (_G_STRUCTS or {}).get(ann, {}), "union")
    return "dyn"


def tname(t):
    if isinstance(t, tuple):
        if t[0] == "cb":
            return t[2]
        if t[0] == "ptr":
            return "ptr[%s]" % tname(t[1])
        if t[0] == "slice":
            return "slice[%s]" % tname(t[1])
        if t[0] == "vec":
            return "vec[" + tname(t[1]) + "]" if t[1] != "dyn" else "vec"
        return t[0]
    return {"int": "i64", "float": "f64", "dyn": "?"}.get(t, t)


def ok(want, got):
    if want == "dyn" or got in ("dyn", "tag", "none"):
        return True
    if want == got:
        return True
    if want == "float" and got == "int":
        return True
    if isinstance(want, tuple) and want[0] == "cb" and got == "str":
        return True
    if isinstance(want, tuple) and want[0] == "struct" and len(want) > 2 and want[2] == "union" and got == "map":
        return True
    if isinstance(want, tuple) and isinstance(got, tuple):
        if want[0] == "struct" or got[0] == "struct":
            return want[0] == "struct" and got[0] == "struct"
        if want[0] == "vec" and got[0] == "vec":
            return want[1] == "dyn" or got[1] == "dyn" or want[1] == got[1]
        if want[0] == "ptr" and got[0] == "ptr":
            return want[1] == "dyn" or got[1] == "dyn" or want[1] == got[1]
        if want[0] == "slice" and got[0] == "slice":
            return want[1] == "dyn" or got[1] == "dyn" or want[1] == got[1]
    if isinstance(want, tuple) and want[0] == "ptr" and got == "int":
        return True
    if want == "int" and isinstance(got, tuple) and got[0] == "ptr":
        return True
    return False


STR_METH = {
    "split": (["str"], ("vec", "str"), True),
    "lines": ([], ("vec", "str"), False),
    "trim": ([], "str", False),
    "lower": ([], "str", False),
    "upper": ([], "str", False),
    "starts_with": (["str"], "bool", False),
    "ends_with": (["str"], "bool", False),
    "contains": (["str"], "bool", False),
    "parse_int": ([], "tag", False),
    "replace": (["str", "str"], "str", False),
    "index_of": (["str"], "dyn", False),
    "repeat": (["int"], "str", False),
    "split_once": (["str"], "dyn", False),
    "copy": ([], "str", False),
    "chars": ([], ("vec", "str"), False),
    "parse_float": ([], "tag", False),
    "slice": (["int", "int"], "str", False),
}

VEC_METH = {
    "join": (["str"], "str", False),
    "push": (["$E"], "int", False),
    "pop": ([], "dyn", False),
    "first": ([], "dyn", False),
    "last": ([], "dyn", False),
    "sort": ([], "$S", False),
    "rev": ([], "$S", False),
    "ins": (["int", "$E"], "int", False),
    "del": (["int"], "dyn", False),
    "slice": (["int", "int"], "$S", False),
    "sum": ([], "$SUM", False),
    "min": ([], "dyn", False),
    "max": ([], "dyn", False),
    "enumerate": ([], ("vec", "dyn"), False),
    "copy": ([], "$S", False),
    "zip": (["dyn"], ("vec", "dyn"), False),
    "chunks": (["int"], "$S", False),
    "pos": (["dyn"], "dyn", False),
    "any": (["dyn"], "bool", False),
    "all": (["dyn"], "bool", False),
    "find": (["dyn"], "dyn", False),
    "fold": (["dyn", "dyn"], "dyn", False),
    "sort_by": (["dyn"], "$S", False),
}

MAP_METH = {
    "get": (["dyn"], "dyn", False),
    "has": (["dyn"], "bool", False),
    "keys": ([], ("vec", "str"), False),
    "del": (["dyn"], "bool", False),
    "set": (["dyn", "dyn"], "dyn", False),
    "items": ([], ("vec", "dyn"), False),
    "vals": ([], ("vec", "dyn"), False),
    "copy": ([], "map", False),
    "get_or": (["dyn", "dyn"], "dyn", False),
}


def numeric(t):
    return t in ("int", "float", "bool")


BRES = {
    "len": "int", "str": "str", "is_err": "bool", "is_none": "bool",
    "tagof": "str", "read_file": "tag", "write_file": "tag",
    "read_bytes": "tag", "write_bytes": "tag", "proc_run": "tag",
    "utf8_bytes": ("vec", "int"), "utf8_str": "tag",
    "fs_rename": "tag", "fs_remove": "tag",
    "sha256_bytes": "tag", "random_bytes": "tag", "https_request_raw": "tag",
    "sql_open": "tag", "sql_close": "tag", "sql_exec": "tag", "sql_query": "tag",
    "is_vec": "bool", "is_str": "bool", "is_map": "bool",
    "join_all": "int", "Ok": "tag", "Err": "tag", "Some": "tag",
    "args": ("vec", "str"), "exit": "dyn", "eprint": "dyn", "raise": "dyn",
    "time_ms": "int", "mono_ns": "int", "sleep_ms": "dyn", "env_get": "dyn",
    "$arena_enter": "int", "$arena_leave": "int",
    "re_match": "bool", "re_find": "dyn", "re_captures": "dyn", "re_replace": "str",
    "tcp_listen": "tag", "tcp_connect": "tag", "tcp_accept": "tag",
    "tcp_send": "tag", "tcp_recv": "tag", "tcp_close": "tag",
    "sbuf": "dyn", "sbuf_add": "int", "sbuf_len": "int", "sbuf_str": "str",
    "chan": "int",
    "ptr_null": "int", "ptr_is_null": "bool",
    "ptr_read_i64": "int", "ptr_write_i64": "int",
    "ptr_read_f64": "float", "ptr_write_f64": "int",
    "ptr_read_f32": "float", "ptr_write_f32": "int",
    "ptr_read_struct": "dyn", "ptr_write_struct": "int",
    "ptr_read_ptr": "int", "ptr_offset": "int",
    "ptr_read_str": "str", "ptr_write_ptr": "int",
    "ptr_of_str": "int", "ptr_write_str": "int",
    "errno": "int", "strerror": "str",
}


_ARENA_ENTER = ("expr", ("call", "$arena_enter", []))
_INTERP = re.compile(r'\{([A-Za-z_][A-Za-z0-9_]*)')


def is_arena_block(s):
    """`if true { $arena_enter(); defer $arena_leave(); ... }` from parse_arena."""
    return (len(s) > 2 and s[1] == ("bool", True) and isinstance(s[2], list)
            and s[2] and s[2][0] == _ARENA_ENTER)


def first_var(e):
    """The first variable a place expression is rooted at, or None."""
    if isinstance(e, tuple) and e:
        if e[0] == "var":
            return e[1]
        for x in e[1:]:
            r = first_var(x)
            if r:
                return r
    elif isinstance(e, list):
        for x in e:
            r = first_var(x)
            if r:
                return r
    return None


def mentions(node, acc):
    """Every name a subtree reads, including names inside string interpolation.
    The argument of own() is not read: own() makes it a heap copy first."""
    if isinstance(node, tuple):
        if node and node[0] == "call" and node[1] == "own":
            return acc
        if node and node[0] == "var" and len(node) > 1 and isinstance(node[1], str):
            acc.add(node[1])
        elif node and node[0] == "str" and len(node) > 1 and isinstance(node[1], str):
            for m in _INTERP.finditer(node[1]):
                acc.add(m.group(1))
        for x in node:
            mentions(x, acc)
    elif isinstance(node, list):
        for x in node:
            mentions(x, acc)
    return acc


def has_node(node, tag):
    if isinstance(node, tuple):
        if node and node[0] == tag:
            return True
        return any(has_node(x, tag) for x in node)
    if isinstance(node, list):
        return any(has_node(x, tag) for x in node)
    return False


_RO_MCALL_NAMES = frozenset({
    "get", "has", "keys", "vals", "len", "contains", "starts_with", "ends_with",
    "index_of", "split", "split_once", "replace", "slice", "join", "zip", "pos",
    "chunks", "enumerate", "copy", "trim", "lower", "upper", "lines", "chars",
    "repeat", "parse_int", "parse_float", "first", "last", "min", "max", "sum",
    "rev", "map", "filter", "fold", "each", "any", "all", "find", "count",
    "sort_by", "flat_map", "take", "skip"
})

_PURE_READ_BUILTINS = frozenset({
    "len", "str", "print", "eprint", "is_err", "is_none", "is_vec", "is_str",
    "is_map", "tagof", "own", "sbuf_add", "tcp_send", "write_file", "write_bytes"
})

_SCALAR_ONLY_CALLS = frozenset({
    "own", "len", "is_err", "is_none", "is_vec", "is_str", "is_map",
    "ptr_is_null", "ptr_read_i64", "ptr_read_f64", "ptr_read_f32", "ptr_read_u8",
    "re_match", "sbuf_len", "slice_len", "time_ms", "mono_ns", "errno"
})

_FLOW_NUM_OPS = frozenset({
    "-", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||", "in"
})


def _has_chan_call(e):
    if isinstance(e, tuple) and e:
        if e[0] == "call" and len(e) > 1 and e[1] == "chan":
            return True
        return any(_has_chan_call(x) for x in e[1:])
    if isinstance(e, list):
        return any(_has_chan_call(x) for x in e)
    return False


def _non_scalar_flow_vars(e, acc):
    if not isinstance(e, tuple) or not e:
        if isinstance(e, list):
            for x in e:
                _non_scalar_flow_vars(x, acc)
        return
    k = e[0]
    if k in ("num", "bool", "none", "un"):
        return
    if k == "bin" and e[1] in _FLOW_NUM_OPS:
        return
    if k == "call" and e[1] in _SCALAR_ONLY_CALLS:
        return
    if k == "var":
        if isinstance(e[1], str) and e[1]:
            acc.add(e[1])
        return
    if k in ("str", "rstr"):
        if isinstance(e[1], str):
            for m in _INTERP.finditer(e[1]):
                acc.add(m.group(1))
        return
    if k == "closure":
        inner_ms = mentions(e[3], set())
        bnd = {p[0] if isinstance(p, tuple) else p for p in e[2]}
        acc.update(inner_ms - bnd)
        return
    if k == "parfor":
        _non_scalar_flow_vars(e[2], acc)
        _non_scalar_flow_vars(e[4], acc)
        inner_ms = mentions(e[5], set())
        acc.update(inner_ms - {e[1]})
        return
    for x in e[1:]:
        if isinstance(x, (tuple, list)):
            _non_scalar_flow_vars(x, acc)


def _deps_of_expr(e, deps):
    vs = set()
    _non_scalar_flow_vars(e, vs)
    out = set()
    for v in vs:
        if v in deps:
            out.update(deps[v])
    return out


def _scan_sink_expr(e, sinks, local_names, deps, sink_params, edges):
    if not isinstance(e, tuple) or not e:
        if isinstance(e, list):
            for x in e:
                _scan_sink_expr(x, sinks, local_names, deps, sink_params, edges)
        return
    k = e[0]
    if k == "mcall" and e[2] not in _RO_MCALL_NAMES:
        root = first_var(e[1])
        d = set()
        for a in e[3]:
            d.update(_deps_of_expr(a, deps))
        if e[2] == "send":
            sink_params.update(d)
        elif root:
            if root in sinks and root not in local_names:
                sink_params.update(d)
            else:
                deps.setdefault(root, set()).update(d)
    elif k == "call":
        has_sink_arg = False
        all_d = set()
        for j, a in enumerate(e[2]):
            dj = _deps_of_expr(a, deps)
            all_d.update(dj)
            if isinstance(e[1], str) and e[1] and dj:
                edges.append((e[1], j, frozenset(dj)))
            fv = first_var(a)
            if fv and fv in sinks and fv not in local_names:
                has_sink_arg = True
        if has_sink_arg and e[1] not in _PURE_READ_BUILTINS:
            sink_params.update(all_d)
    if k == "closure":
        sub = set(local_names)
        for p in e[2]:
            sub.add(p[0] if isinstance(p, tuple) else p)
        _scan_sink_stmts(e[3], sinks, sub, deps, sink_params, edges)
        return
    if k == "parfor":
        _scan_sink_expr(e[2], sinks, local_names, deps, sink_params, edges)
        _scan_sink_expr(e[4], sinks, local_names, deps, sink_params, edges)
        sub = set(local_names)
        sub.add(e[1])
        _scan_sink_stmts(e[5], sinks, sub, deps, sink_params, edges)
        return
    for x in e[1:]:
        if isinstance(x, (tuple, list)):
            _scan_sink_expr(x, sinks, local_names, deps, sink_params, edges)


def _scan_sink_stmts(stmts, sinks, local_names, deps, sink_params, edges):
    cur_locals = set(local_names)
    for s in stmts:
        if not isinstance(s, tuple) or not s:
            continue
        k = s[0]
        if k == "pub":
            s = s[1]
            if not isinstance(s, tuple) or not s:
                continue
            k = s[0]
        if k == "let":
            _scan_sink_expr(s[2], sinks, cur_locals, deps, sink_params, edges)
            d = _deps_of_expr(s[2], deps)
            if isinstance(s[1], str) and s[1]:
                cur_locals.add(s[1])
                deps.setdefault(s[1], set()).update(d)
        elif k == "set":
            _scan_sink_expr(s[2], sinks, cur_locals, deps, sink_params, edges)
            d = _deps_of_expr(s[2], deps)
            head = s[1].split(".", 1)[0]
            if head:
                if head in sinks and head not in cur_locals:
                    sink_params.update(d)
                else:
                    deps.setdefault(head, set()).update(d)
        elif k == "setidx":
            for x in s[1:4]:
                _scan_sink_expr(x, sinks, cur_locals, deps, sink_params, edges)
            d = _deps_of_expr(s[3], deps)
            root = first_var(s[1])
            if root:
                if root in sinks and root not in cur_locals:
                    sink_params.update(d)
                else:
                    deps.setdefault(root, set()).update(d)
        elif k == "setfield":
            _scan_sink_expr(s[1], sinks, cur_locals, deps, sink_params, edges)
            _scan_sink_expr(s[3], sinks, cur_locals, deps, sink_params, edges)
            d = _deps_of_expr(s[3], deps)
            root = first_var(s[1])
            if root:
                if root in sinks and root not in cur_locals:
                    sink_params.update(d)
                else:
                    deps.setdefault(root, set()).update(d)
        elif k in ("return", "expr", "defer"):
            _scan_sink_expr(s[1], sinks, cur_locals, deps, sink_params, edges)
        elif k == "if":
            _scan_sink_expr(s[1], sinks, cur_locals, deps, sink_params, edges)
            _scan_sink_stmts(s[2], sinks, cur_locals, deps, sink_params, edges)
            if s[3]:
                els = s[3] if isinstance(s[3], list) else [s[3]]
                _scan_sink_stmts(els, sinks, cur_locals, deps, sink_params, edges)
        elif k == "while":
            _scan_sink_expr(s[1], sinks, cur_locals, deps, sink_params, edges)
            _scan_sink_stmts(s[2], sinks, cur_locals, deps, sink_params, edges)
        elif k == "forrange":
            _scan_sink_expr(s[2], sinks, cur_locals, deps, sink_params, edges)
            _scan_sink_expr(s[3], sinks, cur_locals, deps, sink_params, edges)
            sub = set(cur_locals)
            if isinstance(s[1], str):
                sub.add(s[1])
            _scan_sink_stmts(s[4], sinks, sub, deps, sink_params, edges)
        elif k == "foriter":
            _scan_sink_expr(s[2], sinks, cur_locals, deps, sink_params, edges)
            sub = set(cur_locals)
            if isinstance(s[1], str):
                sub.add(s[1])
                deps.setdefault(s[1], set()).update(_deps_of_expr(s[2], deps))
            _scan_sink_stmts(s[3], sinks, sub, deps, sink_params, edges)
        elif k == "try":
            _scan_sink_stmts(s[1], sinks, cur_locals, deps, sink_params, edges)
            sub = set(cur_locals)
            if len(s) > 3 and isinstance(s[3], str) and s[3]:
                sub.add(s[3])
            _scan_sink_stmts(s[2], sinks, sub, deps, sink_params, edges)
        elif k in ("spawn", "unsafe", "scope", "block"):
            _scan_sink_stmts(s[1], sinks, cur_locals, deps, sink_params, edges)
        elif k in ("fn", "exportfn"):
            sub = set(cur_locals)
            for p in s[2]:
                sub.add(p[0] if isinstance(p, tuple) else p)
            _scan_sink_stmts(s[3], sinks, sub, deps, sink_params, edges)


def _compute_sink_summary(body, params, sinks):
    deps = {}
    local_names = set()
    for i, p in enumerate(params):
        pn = p[0] if isinstance(p, tuple) else p
        local_names.add(pn)
        deps[pn] = {i}
    sink_params = set()
    edges = []
    for _ in range(2):
        edges = []
        _scan_sink_stmts(body, sinks, local_names, deps, sink_params, edges)
    return sink_params, edges


class FnTyper:
    # Types whose values cannot reference arena memory.
    SCALAR = ("int", "float", "bool", "none")

    def __init__(self, sigs, ret, errs, structs=None, unions=None):
        self.sigs = sigs
        self.ret = ret
        self.errs = errs
        # name -> {field: type}. `ptr[T]` dereferences through this table, so a
        # read of a field that `extern struct T` never declared is a type error
        # and not a crash at run time.
        self.structs = structs or {}
        self.unions = set(unions) if unions else set()
        self.env = {}
        self.outer_globals = set()
        # An `&x` is legitimate only inside an extern's `ptr[T]` argument.
        self._amp_ok = False
        self.ptr_slots = {}
        self.extern_names = set()
        self.tainted = []
        self.region = 0
        self.decl_regions = {}
        self.regions = {}
        self.arena_ptrs = set()
        self.closure_sinks = {}
        self.local_fn_sinks = {}
        self.region_stack = [0]
        self._arena_checked = set()

    def current_region(self):
        return self.region_stack[-1]

    def decl_region(self, name):
        return self.decl_regions.get(name, 0)

    def val_region(self, name):
        return self.regions.get(name, 0)

    def inner(self, name):
        return self.current_region() != 0 and self.decl_region(name) >= self.current_region()

    def declare(self, name, vreg=-1):
        if isinstance(name, str) and name:
            cur = self.current_region()
            self.decl_regions[name] = cur
            self.regions[name] = cur if vreg < 0 else vreg

    def taint(self, name):
        if (self.current_region() != 0 and isinstance(name, str)
                and name and not self.inner(name)):
            if not self.tainted:
                self.tainted.append(set())
            self.tainted[-1].add(name)

    def is_tainted(self, name):
        if not isinstance(name, str):
            return False
        for s in self.tainted:
            if name in s:
                return True
        return False

    def _is_non_scalar_var(self, name):
        if name in self.arena_ptrs:
            return True
        return self.env.get(name, "dyn") not in self.SCALAR

    def non_scalar_inner(self, names, min_reg=1):
        out = []
        for n in names:
            if self.val_region(n) >= min_reg and self._is_non_scalar_var(n):
                out.append(n)
        return out

    def _peek_expr(self, e):
        saved = self.errs.silent
        self.errs.silent = True
        try:
            return self.expr(e)
        finally:
            self.errs.silent = saved

    NUM_OPS = ("-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||")
    _ELEM_MCALL = frozenset({
        "get", "get_or", "pop", "first", "last", "min", "max", "find", "del", "unwrap", "recv"
    })
    _HOF_MCALL = frozenset({
        "each", "map", "filter", "flat_map", "find", "any", "all", "count", "sort_by", "fold"
    })

    def expr_region(self, e):
        cur = self.current_region()
        if cur == 0 or not isinstance(e, tuple) or not e:
            return 0
        k = e[0]
        if k in ("num", "bool", "none", "str", "rstr", "un"):
            return 0
        if k in ("vec", "maplit", "struct"):
            return cur
        if k == "var":
            if self.is_tainted(e[1]):
                return cur
            if not self._is_non_scalar_var(e[1]):
                return 0
            return self.val_region(e[1])
        if k == "closure":
            r = 0
            for n in mentions(e, set()):
                if self.is_tainted(n):
                    return cur
                if self._is_non_scalar_var(n):
                    v = self.val_region(n)
                    if v > r:
                        r = v
            return r
        if k == "parfor":
            return 0 if self._peek_expr(e) in self.SCALAR else cur
        if k == "bin":
            op = e[1]
            if op == "*":
                if self._peek_expr(e[2]) == "str" or self._peek_expr(e[3]) == "str":
                    return cur
                return 0
            if op in self.NUM_OPS or op == "in":
                return 0
            if op == "+":
                ta = self._peek_expr(e[2])
                tb = self._peek_expr(e[3])
                if ta in ("int", "float") and tb != "str":
                    return 0
                if tb in ("int", "float") and ta != "str":
                    return 0
                return cur
            return 0
        if k == "or":
            if self._peek_expr(e) in self.SCALAR:
                return 0
            return max(self.expr_region(e[1]), self.expr_region(e[2]))
        if k in ("quest", "move", "field", "idx"):
            if self._peek_expr(e) in self.SCALAR:
                return 0
            return self.expr_region(e[1])
        if k == "mcall":
            if self._peek_expr(e) in self.SCALAR:
                return 0
            if e[2] in self._ELEM_MCALL:
                r = self.expr_region(e[1])
                for a in e[3]:
                    ar = self.expr_region(a)
                    if ar > r:
                        r = ar
                return r
            return cur
        if k == "call":
            name = e[1]
            if name == "own":
                return 0
            if name == "ptr_of_str" and e[2]:
                return self.expr_region(e[2][0])
            if name in ("slice_of", "slice_get") and e[2]:
                if name == "slice_get" and self._peek_expr(e) in self.SCALAR:
                    return 0
                return self.expr_region(e[2][0])
            if isinstance(name, str) and name[:1].isupper() and name not in self.sigs:
                r = 0
                for a in e[2]:
                    ar = self.expr_region(a)
                    if ar > r:
                        r = ar
                return r
            if self._peek_expr(e) in self.SCALAR:
                return 0
            return cur
        return 0

    def escapes(self, e, min_reg=-1):
        """True when value e may carry arena memory out of the target region."""
        cur = self.current_region()
        if cur == 0 or not isinstance(e, tuple) or not e:
            return False
        threshold = cur if min_reg < 0 else min_reg
        return self.expr_region(e) >= threshold

    def arena_note(self, msg, ownable=True):
        if ownable:
            msg += "  (hint: hand out a heap copy with `own(...)`, e.g. `return Err(own(msg))`)"
        else:
            msg += "  (hint: keep the value inside the block, or copy it with `own(...)` first)"
        self.errs.note("arena: " + msg)

    def _body_sink_params(self, body, params):
        sinks = set(self.outer_globals)
        for nm in self.env:
            if not self.inner(nm):
                sinks.add(nm)
        sp, edges = _compute_sink_summary(body, params, sinks)
        for callee, arg_idx, caller_ps in edges:
            sig = self.sigs.get(callee)
            if sig is not None and len(sig) > 5 and arg_idx in sig[5]:
                sp.update(caller_ps)
            elif callee in self.local_fn_sinks and arg_idx in self.local_fn_sinks[callee]:
                sp.update(caller_ps)
            elif callee in self.closure_sinks and arg_idx in self.closure_sinks[callee]:
                sp.update(caller_ps)
            elif callee not in self.sigs and callee in self.env and not self.inner(callee):
                sp.update(caller_ps)
        return sp

    def _closure_body(self, e, elem_reg=0, elem_ty="dyn", elem_idx=0):
        """Type-check the body of an inline closure passed as an argument.

        `trace.span("x") { ... }` puts real statements in argument position;
        skipping them would leave that code outside every static island.
        """
        _, _cname, cparams, cbody = e
        saved = dict(self.env)
        saved_decl = dict(self.decl_regions)
        saved_reg = dict(self.regions)
        saved_ret = self.ret
        for idx, p in enumerate(cparams):
            pn = p[0] if isinstance(p, tuple) else p
            ann = p[2] if isinstance(p, tuple) and len(p) > 2 else None
            pt = ann_type(ann)
            if pt == "dyn" and elem_reg > 0 and idx == elem_idx:
                pt = elem_ty
            self.env[pn] = pt
            self.decl_regions[pn] = self.current_region()
            self.regions[pn] = elem_reg if (elem_reg > 0 and idx == elem_idx and pt not in self.SCALAR) else 0
        # A closure has no annotated return: `v.filter(fn(x) { return x > 1 })`
        # returns a bool, and typing it against the enclosing `ret` rejected it.
        self.ret = "dyn"
        self.block(cbody)
        self.ret = saved_ret
        self.regions = saved_reg
        self.decl_regions = saved_decl
        self.env = saved

    def nested_fn(self, name, params, body, ret):
        """A `fn` declared inside a body: its own parameters and return."""
        if isinstance(name, str) and name:
            sp = self._body_sink_params(body, params)
            if sp:
                self.local_fn_sinks[name] = sp
            else:
                self.local_fn_sinks.pop(name, None)
        saved_env, saved_ret = dict(self.env), self.ret
        saved_decl = dict(self.decl_regions)
        saved_reg = dict(self.regions)
        for p in params:
            pn = p[0] if isinstance(p, tuple) else p
            ann = p[2] if isinstance(p, tuple) and len(p) > 2 else None
            self.env[pn] = ann_type(ann)
            self.decl_regions[pn] = self.current_region()
            self.regions[pn] = 0
        self.ret = ann_type(ret)
        self.block(body)
        self.regions = saved_reg
        self.decl_regions = saved_decl
        self.env, self.ret = saved_env, saved_ret

    def field_names(self, t):
        """`ip, code, size` for a struct type, or an empty string."""
        return ", ".join(t[1]) if isinstance(t, tuple) and t[0] == "struct" else ""

    def struct_field_err(self, e2, bt):
        fields = self.field_names(bt)
        if fields:
            return "no field `%s` on struct (has %s)" % (e2, fields)
        return "no field `%s` on struct" % e2

    _RO_MCALL = _RO_MCALL_NAMES

    def _closure_captures_arena(self, e):
        if not (isinstance(e, tuple) and e and e[0] == "closure"):
            return False
        ms = mentions(e, set())
        return bool(self.non_scalar_inner(ms, 1)) or any(self.is_tainted(n) for n in ms)

    def _check_arena_call_expr(self, e):
        if self.current_region() == 0 or id(e) in self._arena_checked:
            return
        self._arena_checked.add(id(e))
        if e[0] == "mcall":
            if e[2] in self._RO_MCALL:
                return
            root = first_var(e[1])
            max_arg_reg = 0
            for a in e[3]:
                if isinstance(a, tuple) and a and a[0] == "closure":
                    if self._closure_captures_arena(a):
                        max_arg_reg = self.current_region()
                    continue
                ar = self.expr_region(a)
                if ar > max_arg_reg:
                    max_arg_reg = ar
            if max_arg_reg > 0 and e[2] == "send" and max_arg_reg >= self.current_region():
                if root and (root in self.env or root in self.outer_globals) and not self.inner(root):
                    self.arena_note("a value made inside the block is stored into `%s`, "
                                    "which lives outside it" % root)
                    self.taint(root)
                else:
                    self.arena_note("a value made inside the block is sent through a channel")
            elif root and max_arg_reg > 0:
                if ((root in self.env or root in self.outer_globals)
                        and not self.inner(root) and max_arg_reg >= self.current_region()):
                    self.arena_note("a value made inside the block is stored into `%s`, "
                                    "which lives outside it" % root)
                    self.taint(root)
                elif self.inner(root) and max_arg_reg > self.val_region(root):
                    self.regions[root] = max_arg_reg
        elif e[0] == "call" and e[1] != "own":
            name = e[1]
            args = e[2]
            if name in ("slice_set", "ptr_write_str", "ptr_write_ptr", "ptr_write_struct") and len(args) >= 2:
                root = first_var(args[0])
                if root and not self.inner(root) and self.escapes(args[-1]):
                    self.arena_note("a value made inside the block is stored into `%s`, "
                                    "which lives outside it" % root)
                    self.taint(root)
                return
            if name in self.sigs or name in self.env or name in self.local_fn_sinks:
                escapes_any = any(self.escapes(a) or self._closure_captures_arena(a) for a in args)
                if not escapes_any:
                    return
                outer = any(first_var(a) and not self.inner(first_var(a)) for a in args)
                if outer:
                    self.arena_note("a value made inside the block is passed to `%s` "
                                    "next to an outer binding" % name)
                    return
                sig = self.sigs.get(name)
                sig_sinks = sig[5] if (sig is not None and len(sig) > 5) else ()
                unknown_outer_cb = (name not in self.sigs and name not in self.local_fn_sinks
                                    and name not in self.closure_sinks and name in self.env
                                    and not self.inner(name))
                hit_sink = False
                for j, a in enumerate(args):
                    if not (self.escapes(a) or self._closure_captures_arena(a)):
                        continue
                    if (unknown_outer_cb or j in sig_sinks
                            or j in self.local_fn_sinks.get(name, ())
                            or j in self.closure_sinks.get(name, ())):
                        hit_sink = True
                        break
                if hit_sink:
                    self.arena_note("a value made inside the block is passed to `%s`, "
                                    "which may store into outer state" % name)

    def expr(self, e):
        if not isinstance(e, tuple) or not e:
            return "dyn"
        k = e[0]
        if k == "num":
            return "float" if isinstance(e[1], float) else "int"
        if k == "bool":
            return "bool"
        if k == "str":
            return "str"
        if k == "rstr":
            return "str"
        if k == "none":
            return "none"
        if k == "vec":
            ts = [self.expr(x) for x in e[1]]
            t = ts[0] if ts and all(x == ts[0] for x in ts) else "dyn"
            return ("vec", t)
        if k == "maplit":
            for ke, ve in e[1]:
                self.expr(ke)
                self.expr(ve)
            return "map"
        if k == "struct":
            fmap = {f: self.expr(v) for f, v in e[2]}
            if e[1] in self.unions:
                ufields = self.structs.get(e[1], {})
                for fn, fv in fmap.items():
                    if fn not in ufields:
                        self.errs.note("extern union `%s` has no arm `%s`" % (e[1], fn))
                    elif not ok(ufields[fn], fv):
                        self.errs.note("arm `%s` of extern union `%s` wants %s, got %s"
                                       % (fn, e[1], tname(ufields[fn]), tname(fv)))
                if len(e[2]) != 1:
                    self.errs.note("extern union `%s` literal initializes %d arms (want 1)"
                                   % (e[1], len(e[2])))
                return ("struct", ufields, "union")
            return ("struct", fmap)
        if k == "var":
            return self.env.get(e[1], "dyn")
        if k == "un":
            t = self.expr(e[2])
            if e[1] == "&":
                # `&x` is a `ptr`: a number whose element type the extern names.
                if not (isinstance(e[2], tuple) and e[2][0] == "var"):
                    self.errs.note("`&` takes a binding: `let mut p = &x`")
                elif not self._amp_ok:
                    self.errs.note("`&` is an out-parameter: write it where the extern "
                                   "takes a `ptr[T]`, as in `fill(&n, 21)`")
                return "int"
            if e[1] == "!":
                return "bool"
            return t if numeric(t) else "dyn"
        if k == "move":
            return self.expr(e[1])
        if k == "closure":
            self._closure_body(e)
            return "dyn"
        if k == "bin":
            op = e[1]
            a = self.expr(e[2])
            b = self.expr(e[3])
            if op in ("==", "!=", "<", ">", "<=", ">=", "in"):
                return "bool"
            if op in ("&&", "||"):
                return a if a == b else "dyn"
            if op == "+":
                if a == "str" or b == "str":
                    return "str"
                if isinstance(a, tuple) and isinstance(b, tuple):
                    return ("vec", "dyn")
                if numeric(a) and numeric(b):
                    return "float" if "float" in (a, b) else "int"
                return "dyn"
            if numeric(a) and numeric(b):
                if op == "*" and (a == "str" or b == "str"):
                    return "str"
                return "float" if "float" in (a, b) else "int"
            if op == "*" and (a == "str" or b == "str") and (a == "int" or b == "int"):
                return "str"
            return "dyn"
        if k == "or":
            a = self.expr(e[1])
            b = self.expr(e[2])
            return a if a == b else "dyn"
        if k == "quest":
            return self.expr(e[1])
        if k == "mcall":
            ot = self.expr(e[1])
            recv_reg = self.expr_region(e[1]) if self.current_region() != 0 else 0
            elem_ty = ot[1] if (isinstance(ot, tuple) and ot[0] == "vec") else ("str" if ot == "str" else "dyn")
            for a in e[3]:
                if recv_reg > 0 and e[2] in self._HOF_MCALL and isinstance(a, tuple) and a and a[0] == "closure":
                    self._closure_body(a, recv_reg, elem_ty, 1 if e[2] == "fold" else 0)
                else:
                    self.expr(a)
            self._check_arena_call_expr(e)
            return self._meth(ot, e[2], e[3])
        if k == "field":
            bt = self.expr(e[1])
            if isinstance(bt, tuple) and bt[0] == "struct":
                if e[2] not in bt[1]:
                    self.errs.note(self.struct_field_err(e[2], bt))
                    return "dyn"
                return bt[1][e[2]]
            return "dyn"
        if k == "idx":
            b = self.expr(e[1])
            self.expr(e[2])
            if isinstance(b, tuple) and b[0] == "ptr":
                elem = b[1]
                if elem in self.structs:
                    return ("struct", self.structs[elem])
                if elem in ("int", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "isize"):
                    return "int"
                if elem in ("float", "f32", "f64"):
                    return "float"
                self.errs.note("index a `ptr` needs a unit: declare it `ptr[T]` "
                               "with T an extern struct, i32, i64, f32, f64 or u8")
                return "dyn"
            if isinstance(b, tuple) and b[0] == "vec":
                return b[1]
            if b == "str":
                return "str"
            if b == "int":
                self.errs.note("index a `ptr` needs a unit: declare it `ptr[T]` "
                               "with T an extern struct, i32, i64, f32, f64 or u8")
            return "dyn"
        if k == "call":
            name = e[1]
            # An `&x` is checked inside the argument slot that takes a `ptr[T]`,
            # and nowhere else: it is not a general address-of.
            # `fill(&n, 21)` -- and no annotation at all still means "an extern
            # with pointer parameters", which the emitter checks against the
            # prototype. A call to a BRAT function has no out-parameter slot.
            slots = self.ptr_slots.get(name)
            if slots is None and name in self.extern_names:
                slots = set(range(len(e[2])))
            if name not in ("lib_open", "lib_sym", "lib_close", "call"):
                for _i, a in enumerate(e[2]):
                    _prev, self._amp_ok = self._amp_ok, (_i in (slots or ()))
                    self.expr(a)
                    self._amp_ok = _prev
            if name == "own":
                if len(e[2]) != 1:
                    self.errs.note("`own` wants 1 arg, got %d" % len(e[2]))
                    return "dyn"
                return self.expr(e[2][0])
            if name in BRES:
                self._check_arena_call_expr(e)
                return BRES[name]
            if name in ("slice_of", "slice_of_ptr", "slice_of_struct", "slice_struct_of",
                        "slice_len", "slice_get", "slice_set", "str_from_ptr"):
                st = self.slice_call(name, e[2])
                self._check_arena_call_expr(e)
                return st
            if name in ("lib_open", "lib_sym", "lib_close", "call"):
                pt = self.plugin_call(name, e[2])
                self._check_arena_call_expr(e)
                return pt
            if name in self.sigs:
                ptypes, rtype, ndef, varargs = self.sigs[name][:4]
                # (The per-slot walk above already typed each argument once;
                # this pass is the contract check on the result.)
                if varargs is None:
                    if len(e[2]) < len(ptypes) - ndef or len(e[2]) > len(ptypes):
                        self.errs.note("`%s` wants %d args, got %d" % (name, len(ptypes), len(e[2])))
                else:
                    if len(e[2]) < len(ptypes) - ndef:
                        self.errs.note("`%s` wants at least %d args, got %d" % (name, len(ptypes) - ndef, len(e[2])))
                for i, pt in enumerate(ptypes):
                    if i < len(e[2]):
                        # The contract pass: an `&x` that reached this point was
                        # already accepted (or refused) by the slot walk above,
                        # so it is judged as the pointer it is.
                        _prev, self._amp_ok = self._amp_ok, (name in self.extern_names)
                        gt = self.expr(e[2][i])
                        self._amp_ok = _prev
                        if isinstance(pt, tuple) and pt[0] == "cb":
                            self.check_cb(pt, e[2][i], name, i + 1)
                            continue
                        if not ok(pt, gt):
                            self.errs.note("arg %d of `%s`: want %s, got %s" % (i + 1, name, tname(pt), tname(gt)))
                if varargs is not None:
                    raw, vt = varargs
                    for i in range(len(ptypes), len(e[2])):
                        gt = self.expr(e[2][i])
                        # A `str` fills a variadic `..ptr` slot: that is how `%s`
                        # reads it, as `const char *`.
                        if raw == "ptr" and gt == "str":
                            continue
                        if not ok(vt, gt):
                            self.errs.note("arg %d of `%s`: want %s, got %s" % (i + 1, name, tname(vt), tname(gt)))
                self._check_arena_call_expr(e)
                return rtype
            self._check_arena_call_expr(e)
            return "dyn"
        return "dyn"

    _SIG_RET = {"i64": "int", "f32": "float", "f64": "float", "str": "str",
                "ptr": "int", "own ptr": "int", "none": "dyn"}
    _SIG_WANT = {"i64": "int", "f32": "float", "f64": "float", "str": "str",
                 "ptr": "int", "own ptr": "int", "&ptr": "int", "out ptr": "int"}

    @staticmethod
    def _norm_plugin_slot(raw):
        s = "".join((raw or "").split())
        if s in ("ownptr", "moveptr"):
            return "own ptr"
        if s == "outptr":
            return "out ptr"
        if s in ("&ptr", "refptr"):
            return "&ptr"
        return s

    @classmethod
    def _parse_plugin_sig(cls, raw_text):
        t = raw_text.strip()
        open_i = t.find("(")
        if open_i <= 0 or not t.endswith(")"):
            return None
        depth = 0
        for i in range(open_i, len(t)):
            if t[i] == "(":
                depth += 1
            elif t[i] == ")":
                depth -= 1
                if depth == 0 and i != len(t) - 1:
                    return None
        if depth != 0:
            return None
        ret_t = cls._norm_plugin_slot(t[:open_i])
        rest = t[open_i + 1:-1]
        params = []
        depth = 0
        start = 0
        for i, ch in enumerate(rest):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif ch == "," and depth == 0:
                piece = cls._norm_plugin_slot(rest[start:i])
                if piece:
                    params.append(piece)
                start = i + 1
        tail = cls._norm_plugin_slot(rest[start:])
        if tail:
            params.append(tail)
        return ret_t, params

    def plugin_call(self, name, args):
        """A plugin call carries its C signature as a literal, so it is checked here."""
        if name == "lib_open":
            if len(args) != 1:
                self.errs.note("`lib_open` wants 1 arg, got %d" % len(args))
            else:
                got = self.expr(args[0])
                if not ok("str", got):
                    self.errs.note("lib_open: want str, got %s" % tname(got))
            return "int"
        if name == "lib_sym":
            if len(args) != 2:
                self.errs.note("`lib_sym` wants 2 args, got %d" % len(args))
                return "int"
            got0 = self.expr(args[0])
            if not ok("int", got0):
                self.errs.note("lib_sym: handle wants ptr, got %s" % tname(got0))
            got1 = self.expr(args[1])
            if not ok("str", got1):
                self.errs.note("lib_sym: name wants str, got %s" % tname(got1))
            return "int"
        if name == "lib_close":
            if len(args) != 1:
                self.errs.note("`lib_close` wants 1 arg, got %d" % len(args))
            else:
                got = self.expr(args[0])
                if not ok("int", got):
                    self.errs.note("lib_close: want ptr, got %s" % tname(got))
            return "int"
        if len(args) < 2:
            self.errs.note("`call` wants a function, a signature and the arguments")
            return "dyn"
        sig = args[1]
        if not (isinstance(sig, tuple) and sig and sig[0] == "str"):
            self.errs.note("call: signature must be a string literal")
            return "dyn"
        parsed = self._parse_plugin_sig(sig[1])
        if parsed is None:
            self.errs.note("call: bad signature `%s`" % sig[1].replace(" ", ""))
            return "dyn"
        ret_t, params = parsed
        if ret_t not in self._SIG_RET:
            self.errs.note("call: unsupported return type `%s`" % ret_t)
            return "dyn"
        fn_ty = self.expr(args[0])
        if not ok("int", fn_ty):
            self.errs.note("call: want ptr, got %s" % tname(fn_ty))
        if len(params) != len(args) - 2:
            self.errs.note("call: signature wants %d argument(s), got %d"
                           % (len(params), len(args) - 2))
            return self._SIG_RET[ret_t]
        for i, (t, a) in enumerate(zip(params, args[2:]), 1):
            if t.startswith("fn(") and ")->" in t:
                # Validated in detail (name, arity, widths, contracts) by plugin.py.
                continue
            if t not in self._SIG_WANT:
                self.errs.note("call: unsupported argument type `%s`" % t)
                continue
            prev_amp, self._amp_ok = self._amp_ok, (t == "out ptr")
            got = self.expr(a)
            self._amp_ok = prev_amp
            is_ptr_slot = t in ("ptr", "own ptr", "&ptr", "out ptr")
            if is_ptr_slot and isinstance(got, tuple) and got and got[0] == "ptr":
                continue
            if not ok(self._SIG_WANT[t], got):
                self.errs.note("call: arg %d wants %s, got %s" % (i, t, tname(got)))
        return self._SIG_RET[ret_t]

    def _resolve_ptr(self, t):
        """A pointer type with the record behind it resolved, not just named.

        Extern annotations arrive as `("ptr", "Reg")` -- the name, because
        `ann_type` has no table of records -- while an index into one answers
        `("ptr", ("struct", fields))`. Both are the same type, and comparing them
        as written refuses a program that is right.
        """
        if isinstance(t, tuple) and t and t[0] == "ptr" and isinstance(t[1], str) \
                and t[1] in self.structs:
            return ("ptr", ("struct", self.structs[t[1]]))
        return t

    def slice_call(self, name, args):
        """`slice[T]` is a checked window, over a vec or over `(ptr[T], n)` from C."""
        def elem(node):
            t = self.expr(node)
            return t[1] if isinstance(t, tuple) and t[0] in ("vec", "slice") else "dyn"

        if name == "slice_of_struct":
            # slice_of_struct(p, n, "T"): a window over a table of records. The
            # layout must be declared, or reading an element would be a guess.
            if len(args) != 3:
                self.errs.note("`slice_of_struct` wants 3 args, got %d" % len(args))
                return ("slice", "dyn")
            sname = args[2]
            if not (isinstance(sname, tuple) and sname[0] == "str"):
                self.errs.note("slice_of_struct wants a struct name as a string literal")
                return ("slice", "dyn")
            if sname[1] not in self.structs:
                self.errs.note("slice_of_struct: %s is not an extern struct" % sname[1])
                return ("slice", "dyn")
            # `ptr[Reg]` carries the name; an index read has already resolved it
            # to the record. Compare the two spellings of one type, or the
            # checker reports `want ptr[Reg], got ptr[Reg]`.
            got = self._resolve_ptr(self.expr(args[0]))
            if not ok(("ptr", ("struct", self.structs[sname[1]])), got):
                self.errs.note("slice_of_struct: want ptr[%s], got %s"
                               % (sname[1], tname(got)))
            if not ok("int", self.expr(args[1])):
                self.errs.note("slice_of_struct: n wants i64, got %s"
                               % tname(self.expr(args[1])))
            return ("slice", ("struct", self.structs[sname[1]]))
        if name == "slice_struct_of":
            # slice_struct_of([...]): a window over elements built in the
            # program. Its element type is whatever the literal holds, taken
            # from the first one; an empty literal has none.
            if len(args) != 1:
                self.errs.note("`slice_struct_of` wants 1 arg, got %d" % len(args))
                return ("slice", "dyn")
            if not (isinstance(args[0], tuple) and args[0][0] == "vec"):
                self.errs.note("slice_struct_of wants a vector literal: "
                               "a window that is written cannot start empty")
                return ("slice", "dyn")
            elem = "dyn"
            for kid in args[0][1]:
                kt = self.expr(kid)
                if elem == "dyn":
                    elem = kt
                elif kt != "dyn" and not ok(elem, kt):
                    self.errs.note("slice_struct_of: want %s, got %s"
                                   % (tname(elem), tname(kt)))
            return ("slice", elem)
        want = {"slice_of": 3, "slice_of_ptr": 3, "slice_len": 1, "slice_get": 2,
                "slice_set": 3, "str_from_ptr": 2}[name]
        if len(args) != want:
            self.errs.note("`%s` wants %d args, got %d" % (name, want, len(args)))
            return "dyn"
        if name == "str_from_ptr":
            if not ok("int", self.expr(args[0])):
                self.errs.note("str_from_ptr: want ptr, got %s" % tname(self.expr(args[0])))
            if not ok("int", self.expr(args[1])):
                self.errs.note("str_from_ptr: n wants i64, got %s" % tname(self.expr(args[1])))
            return "str"
        if name == "slice_len":
            return "int"
        if name == "slice_of":
            e = elem(args[0])
            for i in (1, 2):
                got = self.expr(args[i])
                if not ok("int", got):
                    self.errs.note("slice_of: bound %d wants i64, got %s" % (i, tname(got)))
            return ("slice", e)
        if name == "slice_of_ptr":
            kind = args[2]
            if not (isinstance(kind, tuple) and kind[0] == "str"
                    and kind[1] in ("i64", "i32", "f64", "f32", "u8")):
                self.errs.note('slice_of_ptr: kind must be the literal '
                               '"i64", "i32", "f64", "f32" or "u8"')
                return ("slice", "dyn")
            want_t = ("ptr", _WORD[kind[1]])
            got = self.expr(args[0])
            if not ok(want_t, got):
                self.errs.note("slice_of_ptr: want ptr[%s], got %s" % (kind[1], tname(got)))
            if not ok("int", self.expr(args[1])):
                self.errs.note("slice_of_ptr: n wants i64, got %s" % tname(self.expr(args[1])))
            return ("slice", _WORD[kind[1]])
        e = elem(args[0])
        if not ok("int", self.expr(args[1])):
            self.errs.note("%s: index wants i64, got %s" % (name, tname(self.expr(args[1]))))
        if name == "slice_get":
            return e
        got = self.expr(args[2])
        if not ok(e, got):
            self.errs.note("slice_set: want %s, got %s" % (tname(e), tname(got)))
        return "int"

    def check_cb(self, pt, arg, name, pos):
        """A callback argument is the name of a function with the declared arity."""
        if not (isinstance(arg, tuple) and arg and arg[0] == "str"):
            self.errs.note("arg %d of `%s`: callback must be a function name" % (pos, name))
            return
        fname = arg[1]
        sig = self.sigs.get(fname)
        if sig is None:
            self.errs.note("unknown callback `%s`" % fname)
            return
        got = len(sig[0])
        if got != pt[1]:
            self.errs.note("callback `%s` takes %d parameter(s), %s wants %d"
                           % (fname, got, pt[2], pt[1]))
            return
        expected = _cb_signature(pt[2])
        actual = sig[4] if len(sig) > 4 else None
        if expected is None or actual is None:
            self.errs.note("callback `%s` does not have a verifiable C ABI signature" % fname)
            return
        actual_params, actual_ret = actual
        expected_params, expected_ret = expected
        if (tuple(_abi_type(t) for t in actual_params) !=
                tuple(_abi_type(t) for t in expected_params) or
                _abi_type(actual_ret) != _abi_type(expected_ret)):
            actual_text = "fn(%s)->%s" % (
                ",".join(_abi_type(t) for t in actual_params), _abi_type(actual_ret))
            self.errs.note("callback `%s` has signature %s, expected %s"
                           % (fname, actual_text, pt[2]))

    def _meth(self, ot, meth, args):
        tab = None
        if ot == "str":
            tab = STR_METH
        elif isinstance(ot, tuple) and ot[0] == "vec":
            tab = VEC_METH
        elif ot == "map":
            tab = MAP_METH
        if tab is None:
            return "dyn"
        if meth in ("map", "filter"):
            return ("vec", "dyn")
        sig = tab.get(meth)
        if sig is None:
            self.errs.note("no method `%s` for %s" % (meth, tname(ot)))
            return "dyn"
        ptypes, rtype, flex = sig
        if len(args) != len(ptypes) and not (flex and len(args) == len(ptypes) - 1):
            self.errs.note("method `%s` wants %d args" % (meth, len(ptypes)))
            return "dyn"
        for i, pt in enumerate(ptypes):
            if i >= len(args):
                break
            want = ot[1] if pt == "$E" else pt
            gt = self.expr(args[i])
            if not ok(want, gt):
                self.errs.note("method `%s` arg %d: want %s, got %s" % (meth, i + 1, tname(want), tname(gt)))
        if rtype == "$SUM":
            return ot[1] if isinstance(ot, tuple) and numeric(ot[1]) else "dyn"
        return ot if rtype == "$S" else rtype

    def stmt(self, s):
        ln = _LINES.get(id(s))
        if ln is not None:
            self.errs.line = ln
        k = s[0]
        if k == "unsafe":
            for ss in s[1]:
                self.stmt(ss)
            return
        if k == "scope":
            if self.current_region() != 0 or _contains_arena(s[1]):
                saved_ao = self.errs.arena_only
                self.errs.arena_only = True
                self.block(s[1])
                self.errs.arena_only = saved_ao
            return
        if k == "let":
            ann = s[5] if len(s) > 5 else None
            want = ann_type(ann)
            got = self.expr(s[2])
            if want != "dyn" and not ok(want, got):
                self.errs.note("let `%s`: annotated %s, got %s" % (s[1], tname(want), tname(got)))
            vty = want if want != "dyn" else got
            self.env[s[1]] = vty
            ms = mentions(s[2], set())
            outer_alias = False
            alias_decl = self.current_region()
            for n in ms:
                if n != s[1] and not self.inner(n) and self._is_non_scalar_var(n):
                    outer_alias = True
                    alias_decl = min(alias_decl, self.decl_region(n))
            er = self.expr_region(s[2])
            is_arena_ptr = self.current_region() != 0 and er > 0 and vty in self.SCALAR
            if is_arena_ptr:
                self.arena_ptrs.add(s[1])
            else:
                self.arena_ptrs.discard(s[1])
            if isinstance(s[1], str) and s[1]:
                self.decl_regions[s[1]] = alias_decl if outer_alias else self.current_region()
                self.regions[s[1]] = 0 if (vty in self.SCALAR and not is_arena_ptr) else er
                if isinstance(s[2], tuple) and s[2] and s[2][0] == "closure":
                    sp = self._body_sink_params(s[2][3], s[2][2])
                    if sp:
                        self.closure_sinks[s[1]] = sp
                    else:
                        self.closure_sinks.pop(s[1], None)
                else:
                    self.closure_sinks.pop(s[1], None)
            if self.current_region() != 0 and isinstance(s[1], str) and any(self.is_tainted(n) for n in ms):
                self.taint(s[1])
            if self.current_region() != 0 and has_node(s[2], "closure") and self.non_scalar_inner(ms, 1):
                self.arena_note("a closure inside the block captures a value made in it",
                                ownable=False)
        elif k == "set":
            got = self.expr(s[2])
            target = s[1].split(".", 1)[0]
            er = self.expr_region(s[2])
            if self.current_region() != 0:
                if not self.inner(target) and self.escapes(s[2]):
                    self.arena_note("a value made inside the block is assigned to `%s`, "
                                    "which lives outside it" % target)
                    self.taint(target)
                elif self.inner(target) and er > self.val_region(target):
                    self.regions[target] = er
                    if self.env.get(target, "dyn") in self.SCALAR:
                        self.arena_ptrs.add(target)
                if has_node(s[2], "closure") and self.non_scalar_inner(mentions(s[2], set()), 1):
                    self.arena_note("a closure inside the block captures a value made in it",
                                    ownable=False)
            if ("." not in s[1] and isinstance(s[2], tuple) and s[2]
                    and s[2][0] == "closure"):
                sp = self._body_sink_params(s[2][3], s[2][2])
                if sp:
                    self.closure_sinks.setdefault(target, set()).update(sp)
            if "." in s[1]:
                head, field = s[1].split(".", 1)
                bt = self.env.get(head)
                if isinstance(bt, tuple) and bt[0] == "struct" and "." not in field:
                    if field not in bt[1]:
                        self.errs.note(self.struct_field_err(field, bt))
                    elif not ok(bt[1][field], got):
                        self.errs.note("assign to `%s`: want %s, got %s" % (s[1], tname(bt[1][field]), tname(got)))
            else:
                cur = self.env.get(s[1])
                if cur is not None and cur != "dyn" and not ok(cur, got):
                    self.errs.note("assign to `%s`: want %s, got %s" % (s[1], tname(cur), tname(got)))
        elif k == "setidx":
            self.expr(s[1])
            self.expr(s[2])
            self.expr(s[3])
            root = first_var(s[1])
            if self.current_region() != 0 and root:
                if not self.inner(root) and self.escapes(s[3]):
                    self.arena_note("a value made inside the block is stored into `%s`, "
                                    "which lives outside it" % root)
                    self.taint(root)
                elif self.inner(root):
                    er = self.expr_region(s[3])
                    if er > self.val_region(root):
                        self.regions[root] = er
        elif k == "setfield":
            bt = self.expr(s[1])
            got = self.expr(s[3])
            root = first_var(s[1])
            if self.current_region() != 0 and root:
                if not self.inner(root) and self.escapes(s[3]):
                    self.arena_note("a value made inside the block is stored into `%s`, "
                                    "which lives outside it" % root)
                    self.taint(root)
                elif self.inner(root):
                    er = self.expr_region(s[3])
                    if er > self.val_region(root):
                        self.regions[root] = er
            if isinstance(bt, tuple) and bt[0] == "struct":
                if s[2] not in bt[1]:
                    self.errs.note(self.struct_field_err(s[2], bt))
                elif not ok(bt[1][s[2]], got):
                    self.errs.note("assign to `%s`: want %s, got %s"
                                   % (s[2], tname(bt[1][s[2]]), tname(got)))
        elif k == "return":
            got = self.expr(s[1])
            if self.escapes(s[1], 1):
                self.arena_note("a value made inside the block is returned out of it")
            if not ok(self.ret, got):
                self.errs.note("return: want %s, got %s" % (tname(self.ret), tname(got)))
        elif k == "expr":
            self.expr(s[1])
            e = s[1]
            if self.current_region() != 0 and isinstance(e, tuple) and e:
                if e[0] == "closure" and self.non_scalar_inner(mentions(e, set())):
                    self.arena_note("a closure inside the block captures a value made in it",
                                    ownable=False)
                elif e[0] == "closure" and any(self.is_tainted(n) for n in mentions(e, set())):
                    self.arena_note("a closure inside the block captures a value made in it",
                                    ownable=False)
        elif k == "if":
            self.expr(s[1])
            if is_arena_block(s):
                self.region += 1
                self.region_stack.append(self.region)
                self.tainted.append(set())
                self.block(s[2])
                self.tainted.pop()
                self.region_stack.pop()
            else:
                self.block(s[2])
            if s[3]:
                self.block(s[3] if isinstance(s[3], list) else [s[3]])
        elif k == "while":
            self.expr(s[1])
            self.block(s[2])
        elif k == "forrange":
            self.declare(s[1], 0)
            for b in (s[2], s[3]):
                t = self.expr(b)
                if t not in ("int", "bool", "dyn"):
                    self.errs.note("forrange bound: want i64, got %s" % tname(t))
            old = self.env.get(s[1])
            self.env[s[1]] = "int"
            self.block(s[4])
            if old is None:
                self.env.pop(s[1], None)
            else:
                self.env[s[1]] = old
        elif k == "foriter":
            st = self.expr(s[2])
            old = self.env.get(s[1])
            elem = st[1] if isinstance(st, tuple) else ("str" if st == "str" else "dyn")
            er = (self.current_region() if st == "str"
                  else (self.expr_region(s[2]) if elem not in self.SCALAR else 0)) if self.current_region() != 0 else 0
            self.declare(s[1], er)
            self.env[s[1]] = elem
            self.block(s[3])
            if old is None:
                self.env.pop(s[1], None)
            else:
                self.env[s[1]] = old
        elif k in ("break", "continue", "import", "trait", "enum"):
            pass
        elif k == "try":
            self.block(s[1])
            old = self.env.get(s[3]) if len(s) > 3 and s[3] else None
            if len(s) > 3 and s[3]:
                self.declare(s[3])
                self.env[s[3]] = "dyn"
            self.block(s[2])
            if len(s) > 3 and s[3]:
                if old is None:
                    self.env.pop(s[3], None)
                else:
                    self.env[s[3]] = old
        elif k == "spawn":
            if self.current_region() != 0 and self.non_scalar_inner(mentions(s[1], set()), 1):
                self.arena_note("a spawn inside the block captures a value made in it")
            if self.current_region() != 0 and any(self.is_tainted(n) for n in mentions(s[1], set())):
                self.arena_note("a spawn inside the block captures a value made in it",
                                ownable=False)
            self.block(s[1])
        elif k == "defer":
            self.expr(s[1])
        elif k in ("fn", "exportfn"):
            self.nested_fn(s[1], s[2], s[3], fn_ret(s))
        elif k == "impl":
            pass
        elif k == "pub":
            self.stmt(s[1])

    def block(self, body):
        for s in body:
            if isinstance(s, tuple) and s:
                self.stmt(s)


def fn_ret(node):
    if isinstance(node, tuple) and node and node[0] == "extern":
        return node[4] if len(node) > 4 else None
    return node[5] if len(node) > 5 else None


def collect_sigs(prog, sigs):
    mod_sinks = set()
    for s in prog[1]:
        node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if isinstance(node, tuple) and node and node[0] == "let" and isinstance(node[1], str) and node[1]:
            is_const = bool(len(node) > 4 and node[4])
            if not is_const or _has_chan_call(node[2]):
                mod_sinks.add(node[1])
    for s in prog[1]:
        node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if not (isinstance(node, tuple) and node and node[0] in ("fn", "extern", "exportfn")):
            continue
        if node[0] == "extern":
            params = node[3]
            ret = node[4]
            varargs = node[5] if len(node) > 5 else None
        else:
            params = node[2]
            ret = fn_ret(node)
            varargs = None
        ptypes = []
        ndef = 0
        for p in params:
            ann = p[2] if isinstance(p, tuple) and len(p) > 2 else None
            ptypes.append(ann_type(ann))
            if isinstance(p, tuple) and len(p) > 3 and p[3] is not None:
                ndef += 1
        name = node[2] if node[0] == "extern" else node[1]
        sink_params = set()
        sink_edges = ()
        if node[0] != "extern":
            sink_params, raw_edges = _compute_sink_summary(node[3], params, mod_sinks)
            sink_edges = tuple(raw_edges)
        # The variadic annotation is kept as written: `..ptr` and `..i64` are both
        # `int` to the type system, but only `..ptr` may take a `str` (`%s`).
        sigs[name] = (ptypes, ann_type(ret), ndef,
              (varargs, ann_type(varargs)) if varargs else None,
              (tuple(p[2] if isinstance(p, tuple) and len(p) > 2 else None
                     for p in params), ret),
              frozenset(sink_params), sink_edges)
    changed = True
    while changed:
        changed = False
        for nm, sg in list(sigs.items()):
            if len(sg) <= 6 or not sg[6]:
                continue
            cur_sp = set(sg[5])
            for callee, arg_idx, caller_ps in sg[6]:
                csg = sigs.get(callee)
                if csg is not None and len(csg) > 5 and arg_idx in csg[5]:
                    if not caller_ps <= cur_sp:
                        cur_sp.update(caller_ps)
                        changed = True
            if len(cur_sp) != len(sg[5]):
                sigs[nm] = sg[:5] + (frozenset(cur_sp), sg[6])
    return sigs


def collect_exports(prog, consts, structs):
    dummy_errs = Errs()
    tp = FnTyper({}, "dyn", dummy_errs, structs)
    tp.env.update(consts)
    for s in prog[1]:
        node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if isinstance(node, tuple) and node and node[0] == "externstruct":
            structs[node[1]] = {f: ann_type(a) for f, a in node[2]}
            if len(node) > 4 and node[4] == "union":
                structs.setdefault("__unions__", set()).add(node[1])
        elif (isinstance(s, tuple) and s and s[0] == "pub"
              and isinstance(node, tuple) and node and node[0] == "let"
              and len(node) > 4 and node[4]):
            ann = node[5] if len(node) > 5 else None
            want = ann_type(ann)
            got = tp.expr(node[2])
            cty = want if want != "dyn" else got
            consts[node[1]] = cty
            tp.env[node[1]] = cty


def _contains_arena(node):
    if isinstance(node, tuple) and node:
        if is_arena_block(node):
            return True
        for item in node[1:]:
            if _contains_arena(item):
                return True
    elif isinstance(node, list):
        for item in node:
            if _contains_arena(item):
                return True
    return False


def _walk_union_lits(node, tp):
    if isinstance(node, tuple) and node:
        if node[0] == "struct" and len(node) > 2 and node[1] in tp.unions:
            tp.errs.line = _LINES.get(id(node))
            tp.expr(node)
            return
        for item in node[1:]:
            _walk_union_lits(item, tp)
    elif isinstance(node, list):
        for item in node:
            _walk_union_lits(item, tp)


def check(prog, extra=None, extra_consts=None, extra_structs=None):
    global _G_STRUCTS, _G_UNIONS
    errs = Errs()
    structs = dict(extra_structs) if extra_structs else {}
    unions = set(structs.pop("__unions__", ()))
    for s in prog[1]:
        node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if isinstance(node, tuple) and node and node[0] == "externstruct":
            structs.setdefault(node[1], {})
            if len(node) > 4 and node[4] == "union":
                unions.add(node[1])
    saved_gs, saved_gu = _G_STRUCTS, _G_UNIONS
    _G_STRUCTS, _G_UNIONS = structs, unions
    try:
        for s in prog[1]:
            node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
            if isinstance(node, tuple) and node and node[0] == "externstruct":
                structs[node[1]] = {f: ann_type(a) for f, a in node[2]}
        sigs = dict(extra) if extra else {}
        collect_sigs(prog, sigs)
        # An `&x` is only meaningful where an extern takes a pointer: everywhere
        # else it would be an address the program cannot write back through.
        ptr_slots = {}
        extern_names = set()
        for _st in prog[1]:
            _node = _st[1] if (isinstance(_st, tuple) and _st and _st[0] == "pub") else _st
            if isinstance(_node, tuple) and _node and _node[0] == "extern" and len(_node) > 2:
                extern_names.add(_node[2])
        for _name, _sig in sigs.items():
            # The fifth slot is `(annotations, ret)` -- see `collect_sigs`.
            if len(_sig) > 4 and _sig[4]:
                _anns = _sig[4][0] or ()
                ptr_slots[_name] = {i for i, a in enumerate(_anns)
                                    if isinstance(a, str) and a.startswith("ptr")}
        const_env = dict(extra_consts) if extra_consts else {}
        outer_globals = set(const_env)
        for s in prog[1]:
            node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
            if isinstance(node, tuple) and node and node[0] == "let" and isinstance(node[1], str) and node[1]:
                outer_globals.add(node[1])
        const_probe = FnTyper(sigs, "dyn", Errs(), structs, unions)
        const_probe.env.update(const_env)
        for s in prog[1]:
            node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
            if isinstance(node, tuple) and node and node[0] == "let" and len(node) > 4 and node[4]:
                ann = node[5] if len(node) > 5 else None
                want = ann_type(ann)
                got = const_probe.expr(node[2])
                cty = want if want != "dyn" else got
                const_env[node[1]] = cty
                const_probe.env[node[1]] = cty
        for s in prog[1]:
            node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
            if isinstance(node, tuple) and node and node[0] == "impl":
                for m in node[3]:
                    if not (isinstance(m, tuple) and m and m[0] == "fn"):
                        continue
                    tp = FnTyper(sigs, ann_type(fn_ret(m)), errs, structs, unions)
                    tp.env.update(const_env)
                    tp.outer_globals = outer_globals
                    tp.ptr_slots = ptr_slots
                    tp.extern_names = extern_names
                    for p in m[2]:
                        pn = p[0] if isinstance(p, tuple) else p
                        ann = p[2] if isinstance(p, tuple) and len(p) > 2 else None
                        tp.env[pn] = ann_type(ann)
                    if unions:
                        _walk_union_lits(m[3], tp)
                    if _contains_arena(m[3]):
                        tp.errs.arena_only = True
                        tp.block(m[3])
                        tp.errs.arena_only = False
                continue
            if not (isinstance(node, tuple) and node and node[0] in ("fn", "extern", "exportfn")):
                continue
            if node[0] == "extern":
                continue
            ret = ann_type(fn_ret(node))
            tp = FnTyper(sigs, ret, errs, structs, unions)
            tp.env.update(const_env)
            tp.outer_globals = outer_globals
            tp.ptr_slots = ptr_slots
            tp.extern_names = extern_names
            for p in node[2]:
                pn = p[0] if isinstance(p, tuple) else p
                ann = p[2] if isinstance(p, tuple) and len(p) > 2 else None
                tp.env[pn] = ann_type(ann)
            if ret == "dyn":
                if unions:
                    _walk_union_lits(node[3], tp)
                if _contains_arena(node[3]):
                    tp.errs.arena_only = True
                    tp.block(node[3])
                    tp.errs.arena_only = False
                continue
            tp.block(node[3])
        top = FnTyper(sigs, "dyn", errs, structs, unions)
        top.env.update(const_env)
        top.outer_globals = outer_globals
        for s in prog[1]:
            node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
            if isinstance(node, tuple) and node and node[0] in ("fn", "exportfn", "impl", "trait", "enum", "extern"):
                continue
            top.stmt(s)
    finally:
        _G_STRUCTS, _G_UNIONS = saved_gs, saved_gu
    out = []
    for m in errs:
        if m not in out:
            out.append(m)
    return out


def load_brat(path=None):
    if path is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "brat.py")
    spec = importlib.util.spec_from_file_location("brat_oracle", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main(argv):
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(encoding='utf-8', errors='replace')
        except (AttributeError, ValueError):
            pass
    brat = load_brat()
    progs = []
    srcs = {}
    arena_only = False
    for f in argv[1:]:
        if f == "--arena-only":
            arena_only = True
            continue
        try:
            loaded = brat.load_file(f)
        except (OSError, brat.ModuleError) as e:
            print("%s: load error %s" % (f, e))
            return 2
        for source in loaded.files:
            srcs[source.path] = source.src
            progs.append((source.path, source.prog))
    code = 0
    for idx, (f, prog) in enumerate(progs):
        merged = {}
        merged_consts = {}
        merged_structs = {}
        for j, (_, other_prog) in enumerate(progs):
            if j == idx:
                continue
            collect_exports(other_prog, merged_consts, merged_structs)
            global _G_STRUCTS
            _saved_gs = _G_STRUCTS
            _G_STRUCTS = {k: v for k, v in merged_structs.items() if k != "__unions__"}
            try:
                collect_sigs(other_prog, merged)
            finally:
                _G_STRUCTS = _saved_gs
        errs = check(prog, merged, merged_consts, merged_structs)
        if arena_only:
            errs = [e for e in errs if e[1].startswith("arena: ")]
        print("%s: %d type error(s)" % (f, len(errs)))
        report(f, errs, srcs.get(f, ""))
        if errs:
            code = 1
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))