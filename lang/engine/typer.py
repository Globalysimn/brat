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

    def note(self, msg):
        self.append((self.line, msg))


def report(f, errs, src):
    sys.stdout.write(to_human(f, src, _entries_to_diags(f, errs, "error")))


_WORD = {"i64": "int", "i32": "int", "u32": "int", "u64": "int", "usize": "int",
         "u8": "int", "f32": "float", "f64": "float", "str": "str", "ptr": "raw", "bool": "bool"}


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
    if ann in ("i32", "i64", "u64"):
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
    "is_vec": "bool", "is_str": "bool", "is_map": "bool",
    "join_all": "int", "Ok": "tag", "Err": "tag", "Some": "tag",
    "args": ("vec", "str"), "exit": "dyn", "eprint": "dyn", "raise": "dyn",
    "time_ms": "int", "sleep_ms": "dyn", "env_get": "dyn",
    "__arena_enter": "int", "__arena_leave": "int",
    "re_match": "bool",
    "tcp_listen": "tag", "tcp_connect": "tag", "tcp_accept": "tag",
    "tcp_send": "tag", "tcp_recv": "tag", "tcp_close": "tag",
    "sbuf": "int", "sbuf_add": "int", "sbuf_len": "int", "sbuf_str": "str",
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


_ARENA_ENTER = ("expr", ("call", "__arena_enter", []))
_INTERP = re.compile(r'\{([A-Za-z_][A-Za-z0-9_]*)')


def is_arena_block(s):
    """`if true { __arena_enter(); defer __arena_leave(); ... }` from parse_arena."""
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


class FnTyper:
    # Types whose values cannot reference arena memory.
    SCALAR = ("int", "float", "bool", "none")

    def __init__(self, sigs, ret, errs, structs=None):
        self.sigs = sigs
        self.ret = ret
        self.errs = errs
        # name -> {field: type}. `ptr[T]` dereferences through this table, so a
        # read of a field that `extern struct T` never declared is a type error
        # and not a crash at run time.
        self.structs = structs or {}
        self.env = {}
        # An `&x` is legitimate only inside an extern's `ptr[T]` argument.
        self._amp_ok = False
        self.ptr_slots = {}
        self.extern_names = set()
        self.tainted = []
        self.region = 0
        self.regions = {}
        self.region_stack = [0]

    def current_region(self):
        return self.region_stack[-1]

    def inner(self, name):
        r = self.regions.get(name, 0)
        return self.current_region() != 0 and r != 0 and r == self.current_region()

    def declare(self, name):
        if isinstance(name, str):
            if self.current_region() != 0:
                self.regions[name] = self.current_region()
            else:
                self.regions[name] = 0

    def taint(self, name):
        if (self.current_region() != 0 and isinstance(name, str)
                and self.regions.get(name, 0) != self.current_region()):
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

    def non_scalar_inner(self, names):
        out = []
        for n in names:
            r = self.regions.get(n, 0)
            if r != 0 and self.env.get(n, "dyn") not in self.SCALAR:
                out.append(n)
        return out

    NUM_OPS = ("-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||")

    def escapes(self, e):
        """True when value e may carry arena memory out of the innermost block."""
        if self.current_region() == 0 or not isinstance(e, tuple) or not e:
            return False
        ms = mentions(e, set())
        if not self.non_scalar_inner(ms) and not any(self.is_tainted(n) for n in ms):
            return False
        # Arithmetic and comparisons yield numbers or bools whatever the operand
        # types; `+` does too when one operand is a number.
        if e[0] == "bin":
            if e[1] in self.NUM_OPS:
                return False
            if e[1] == "+" and any(self.expr(x) in ("int", "float") for x in e[2:4]):
                return False
        if e[0] == "un":
            return False
        return self.expr(e) not in self.SCALAR

    def arena_note(self, msg, ownable=True):
        if ownable:
            msg += "  (hint: hand out a heap copy with `own(...)`, e.g. `return Err(own(msg))`)"
        else:
            msg += "  (hint: keep the value inside the block, or copy it with `own(...)` first)"
        self.errs.note("arena: " + msg)

    def _closure_body(self, e):
        """Type-check the body of an inline closure passed as an argument.

        `trace.span("x") { ... }` puts real statements in argument position;
        skipping them would leave that code outside every static island.
        """
        _, _cname, cparams, cbody = e
        saved = dict(self.env)
        saved_ret = self.ret
        for p in cparams:
            pn = p[0] if isinstance(p, tuple) else p
            ann = p[2] if isinstance(p, tuple) and len(p) > 2 else None
            self.env[pn] = ann_type(ann)
        # A closure has no annotated return: `v.filter(fn(x) { return x > 1 })`
        # returns a bool, and typing it against the enclosing `ret` rejected it.
        self.ret = "dyn"
        self.block(cbody)
        self.ret = saved_ret
        self.env = saved

    def nested_fn(self, params, body, ret):
        """A `fn` declared inside a body: its own parameters and return."""
        saved_env, saved_ret = dict(self.env), self.ret
        for p in params:
            pn = p[0] if isinstance(p, tuple) else p
            ann = p[2] if isinstance(p, tuple) and len(p) > 2 else None
            self.env[pn] = ann_type(ann)
        self.ret = ann_type(ret)
        self.block(body)
        self.env, self.ret = saved_env, saved_ret

    def field_names(self, t):
        """`ip, code, size` for a struct type, or an empty string."""
        return ", ".join(t[1]) if isinstance(t, tuple) and t[0] == "struct" else ""

    def struct_field_err(self, e2, bt):
        fields = self.field_names(bt)
        if fields:
            return "no field `%s` on struct (has %s)" % (e2, fields)
        return "no field `%s` on struct" % e2

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
            return ("struct", {f: self.expr(v) for f, v in e[2]})
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
            for a in e[3]:
                self.expr(a)
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
                if elem in ("i64", "i32", "u32", "u64", "usize", "u8"):
                    return "int"
                if elem in ("f32", "f64"):
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
                return BRES[name]
            if name in ("slice_of", "slice_of_ptr", "slice_of_struct", "slice_struct_of",
                        "slice_len", "slice_get", "slice_set", "str_from_ptr"):
                return self.slice_call(name, e[2])
            if name in ("lib_open", "lib_sym", "lib_close", "call"):
                return self.plugin_call(name, e[2])
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
                return rtype
            return "dyn"
        return "dyn"

    _SIG_RET = {"i64": "int", "f32": "float", "f64": "float", "str": "str", "ptr": "int", "none": "dyn"}
    _SIG_WANT = {"i64": "int", "f32": "float", "f64": "float", "str": "str", "ptr": "int"}

    def plugin_call(self, name, args):
        """A plugin call carries its C signature as a literal, so it is checked here."""
        if name == "lib_open":
            if len(args) != 1:
                self.errs.note("`lib_open` wants 1 arg, got %d" % len(args))
            elif not ok("str", self.expr(args[0])):
                self.errs.note("lib_open: want str, got %s" % tname(self.expr(args[0])))
            return "int"
        if name == "lib_sym":
            if len(args) != 2:
                self.errs.note("`lib_sym` wants 2 args, got %d" % len(args))
                return "int"
            if not ok("int", self.expr(args[0])):
                self.errs.note("lib_sym: handle wants ptr, got %s" % tname(self.expr(args[0])))
            if not ok("str", self.expr(args[1])):
                self.errs.note("lib_sym: name wants str, got %s" % tname(self.expr(args[1])))
            return "int"
        if name == "lib_close":
            if len(args) != 1:
                self.errs.note("`lib_close` wants 1 arg, got %d" % len(args))
            elif not ok("int", self.expr(args[0])):
                self.errs.note("lib_close: want ptr, got %s" % tname(self.expr(args[0])))
            return "int"
        if len(args) < 2:
            self.errs.note("`call` wants a function, a signature and the arguments")
            return "dyn"
        sig = args[1]
        if not (isinstance(sig, tuple) and sig and sig[0] == "str"):
            self.errs.note("call: signature must be a string literal")
            return "dyn"
        text = sig[1].replace(" ", "")
        if "(" not in text or not text.endswith(")"):
            self.errs.note("call: bad signature `%s`" % text)
            return "dyn"
        ret_t, rest = text.split("(", 1)
        params = [p for p in rest[:-1].split(",") if p]
        if ret_t not in self._SIG_RET:
            self.errs.note("call: unsupported return type `%s`" % ret_t)
            return "dyn"
        if not ok("int", self.expr(args[0])):
            self.errs.note("call: want ptr, got %s" % tname(self.expr(args[0])))
        if len(params) != len(args) - 2:
            self.errs.note("call: signature wants %d argument(s), got %d"
                           % (len(params), len(args) - 2))
            return self._SIG_RET[ret_t]
        for i, (t, a) in enumerate(zip(params, args[2:]), 1):
            if t not in self._SIG_WANT:
                self.errs.note("call: unsupported argument type `%s`" % t)
                continue
            got = self.expr(a)
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
                elem = self.expr(kid)
                break
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
        if k == "let":
            ann = s[5] if len(s) > 5 else None
            want = ann_type(ann)
            got = self.expr(s[2])
            if want != "dyn" and not ok(want, got):
                self.errs.note("let `%s`: annotated %s, got %s" % (s[1], tname(want), tname(got)))
            self.env[s[1]] = want if want != "dyn" else got
            outer_alias = any(not self.inner(n) and self.env.get(n, "dyn") not in self.SCALAR
                              for n in mentions(s[2], set()) if n != s[1])
            if not outer_alias:
                self.declare(s[1])
            if self.current_region() != 0 and isinstance(s[1], str) and any(self.is_tainted(n) for n in mentions(s[2], set())):
                self.taint(s[1])
            if self.current_region() != 0 and has_node(s[2], "closure") and self.non_scalar_inner(mentions(s[2], set())):
                self.arena_note("a closure inside the block captures a value made in it",
                                ownable=False)
        elif k == "set":
            got = self.expr(s[2])
            target = s[1].split(".", 1)[0]
            if self.current_region() != 0 and not self.inner(target) and self.escapes(s[2]):
                self.arena_note("a value made inside the block is assigned to `%s`, "
                                "which lives outside it" % target)
                self.taint(target)
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
            if self.current_region() != 0 and root and not self.inner(root) and self.escapes(s[3]):
                self.arena_note("a value made inside the block is stored into `%s`, "
                                "which lives outside it" % root)
                self.taint(root)
        elif k == "setfield":
            bt = self.expr(s[1])
            got = self.expr(s[3])
            root = first_var(s[1])
            if self.current_region() != 0 and root and not self.inner(root) and self.escapes(s[3]):
                self.arena_note("a value made inside the block is stored into `%s`, "
                                "which lives outside it" % root)
                self.taint(root)
            if isinstance(bt, tuple) and bt[0] == "struct":
                if s[2] not in bt[1]:
                    self.errs.note(self.struct_field_err(s[2], bt))
                elif not ok(bt[1][s[2]], got):
                    self.errs.note("assign to `%s`: want %s, got %s"
                                   % (s[2], tname(bt[1][s[2]]), tname(got)))
        elif k == "return":
            got = self.expr(s[1])
            if self.escapes(s[1]):
                self.arena_note("a value made inside the block is returned out of it")
            if not ok(self.ret, got):
                self.errs.note("return: want %s, got %s" % (tname(self.ret), tname(got)))
        elif k == "expr":
            self.expr(s[1])
            e = s[1]
            if self.current_region() != 0 and isinstance(e, tuple) and e:
                if e[0] == "mcall":
                    root = first_var(e[1])
                    args = [a for a in e[3] if not (isinstance(a, tuple) and a and a[0] == "closure")]
                    if root and root in self.env and not self.inner(root) and any(self.escapes(a) for a in args):
                        self.arena_note("a value made inside the block is stored into `%s`, "
                                        "which lives outside it" % root)
                        self.taint(root)
                elif e[0] == "call" and e[1] in self.sigs:
                    args = e[2]
                    if any(self.escapes(a) for a in args) and any(
                            first_var(a) and not self.inner(first_var(a)) for a in args):
                        self.arena_note("a value made inside the block is passed to `%s` "
                                        "next to an outer binding" % e[1])
                elif e[0] == "closure" and self.non_scalar_inner(mentions(e, set())):
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
            self.declare(s[1])
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
            self.declare(s[1])
            self.env[s[1]] = st[1] if isinstance(st, tuple) else ("str" if st == "str" else "dyn")
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
            if self.current_region() != 0 and self.non_scalar_inner(mentions(s[1], set())):
                self.arena_note("a spawn inside the block captures a value made in it")
            if self.current_region() != 0 and any(self.is_tainted(n) for n in mentions(s[1], set())):
                self.arena_note("a spawn inside the block captures a value made in it",
                                ownable=False)
            self.block(s[1])
        elif k == "defer":
            self.expr(s[1])
        elif k == "fn":
            self.nested_fn(s[2], s[3], fn_ret(s))
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
        # The variadic annotation is kept as written: `..ptr` and `..i64` are both
        # `int` to the type system, but only `..ptr` may take a `str` (`%s`).
        sigs[name] = (ptypes, ann_type(ret), ndef,
              (varargs, ann_type(varargs)) if varargs else None,
              (tuple(p[2] if isinstance(p, tuple) and len(p) > 2 else None
                     for p in params), ret))
    return sigs


def check(prog, extra=None):
    errs = Errs()
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
    # `extern struct` fields, so a `ptr[T]` can be dereferenced by name.
    structs = {}
    for s in prog[1]:
        node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if isinstance(node, tuple) and node and node[0] == "externstruct":
            structs[node[1]] = {f: ann_type(a) for f, a in node[2]}
    for s in prog[1]:
        node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if not (isinstance(node, tuple) and node and node[0] in ("fn", "extern", "exportfn")):
            continue
        if node[0] == "extern":
            continue
        ret = ann_type(fn_ret(node))
        if ret == "dyn":
            continue
        tp = FnTyper(sigs, ret, errs, structs)
        tp.ptr_slots = ptr_slots
        tp.extern_names = extern_names
        for p in node[2]:
            pn = p[0] if isinstance(p, tuple) else p
            ann = p[2] if isinstance(p, tuple) and len(p) > 2 else None
            tp.env[pn] = ann_type(ann)
        tp.block(node[3])
    top = FnTyper(sigs, "dyn", errs, structs)
    for s in prog[1]:
        node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if isinstance(node, tuple) and node and node[0] in ("fn", "impl", "trait", "enum"):
            continue
        top.stmt(s)
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
    for f in argv[1:]:
        try:
            loaded = brat.load_file(f)
        except (OSError, brat.ModuleError) as e:
            print("%s: load error %s" % (f, e))
            return 2
        for source in loaded.files:
            srcs[source.path] = source.src
            progs.append((source.path, source.prog))
    merged = {}
    for idx, (_, prog) in enumerate(progs):
        if idx == 0:
            continue
        collect_sigs(prog, merged)
    code = 0
    for f, prog in progs:
        errs = check(prog, merged)
        print("%s: %d type error(s)" % (f, len(errs)))
        report(f, errs, srcs.get(f, ""))
        if errs:
            code = 1
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))