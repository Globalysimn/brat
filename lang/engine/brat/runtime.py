import contextlib
import copy
import ctypes
_STR_PTRS = []
import json
import math
import os
import re
import socket
import sys
import threading
import time

try:
    from .lexer import Cur, lex
    from .parser import parse_prog, _LINES
    from .modules import load_source
except ImportError:
    from parser import parse_prog, _LINES
    from modules import load_source

class Done(Exception):
    """An uncaught error travelling to the nearest `catch`.

    A function boundary turns it into the caller's value, which is what
    `match f() { Ok(v) => ..., Err(e) => ... }` reads.
    """

    def __init__(self, v):
        self.v = v


class Propagate(Done):
    """`?` and `unwrap`: an error that leaves the function, not its result.

    `try` in the caller catches it; a plain caller never sees a value.
    """


class Ret(Exception):
    def __init__(self, v):
        self.v = v


class LoopBreak(Exception):
    pass


class LoopCont(Exception):
    pass
_MISS = object()
_MOVED = object()  # sentinel: reading a moved binding errors under BRAT_MOVE=1
_MOVE = os.environ.get('BRAT_MOVE', '1') not in ('0', 'false', 'False', '')

_INTERP_RE = re.compile(r'\{([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\}')

_I64 = 1 << 63

# Language recursion budget: BRAT frames, not Python frames (DESIGN-fast-fail A1).
# 2000: measured refusal 0.4 s interpreter / instant C; a higher default is
# unreachable in the interpreter and overflows the C stack (~29k frames).
RECURSION_BUDGET = int(os.environ.get('BRAT_RECURSION_BUDGET', '2000'))

_DEPTH = threading.local()


@contextlib.contextmanager
def _frame():
    """Enter one BRAT frame; refuse past the budget so the error is a value."""
    d = getattr(_DEPTH, 'd', 0) + 1
    if d > RECURSION_BUDGET:
        raise RuntimeError('recursion too deep')
    _DEPTH.d = d
    try:
        yield
    finally:
        _DEPTH.d = d - 1


def wrap64(n):
    if -_I64 <= n < _I64:
        return n
    n &= (1 << 64) - 1
    return n - (1 << 64) if n >= _I64 else n


_CASE_GROUP = ('upper', 'lower')
_AFFIX_GROUP = ('contains', 'starts_with', 'ends_with')


def _meth_name(meth):
    """The name a missing method reports. `crt.h` groups `upper`/`lower` under
    `case` and the affix tests under `affix`, and the two runtimes must print
    the same bytes because `catch e` hands the message to the program."""
    if meth in _CASE_GROUP:
        return 'case'
    if meth in _AFFIX_GROUP:
        return 'affix'
    return meth


def _no_method(meth):
    return RuntimeError('no method ' + _meth_name(meth))


def _kind(v):
    """Comparable class of a value, mirroring `vcmp` in `crt.h`."""
    if isinstance(v, (bool, int, float)):
        return 'n'
    if isinstance(v, str):
        return 's'
    if isinstance(v, list):
        return 'v'
    if isinstance(v, dict):
        return 'm'
    if v is None:
        return 'z'
    if isinstance(v, tuple):
        return 'g'
    return 'x'


_MAX_VALUE_DEPTH = 2000


def _too_deep(*vals):
    """True when a nested container is deeper than the C runtime's budget.

    `==`, `!=` and the ordering operators walk a value graph recursively in the
    C backend (`veq`/`vcmp`), which stops with "recursion too deep" past
    `BRAT_MAX_VALUE_DEPTH` instead of overflowing the machine stack. Python's
    `==` on two deep lists is recursive too, but silently: it either answers or
    dies inside CPython. Measuring the nesting here first keeps the two backends
    on the same answer, and is the reason this walks iteratively."""
    stack = [(v, 1) for v in vals if isinstance(v, (list, dict, tuple))]
    while stack:
        v, d = stack.pop()
        if d > _MAX_VALUE_DEPTH:
            return True
        if isinstance(v, dict):
            kids = list(v.values())
        elif isinstance(v, tuple):
            kids = [x for x in v[1:]]      # the first slot is the tag's name
        else:
            kids = v
        stack.extend((x, d + 1) for x in kids if isinstance(x, (list, dict, tuple)))
    return False


def _eq_deep(a, b, _depth=0):
    """`==` against `veq`'s recursion budget, for values past the C one.

    Only reached when `_too_deep` said the pair is deeper than the budget, so
    the shallow path keeps Python's C-speed comparison. Tag arguments, keys and
    the struct type are compared, because that is what `veq_` compares.
    """
    if _depth >= _MAX_VALUE_DEPTH:
        raise RuntimeError('recursion too deep')
    an = isinstance(a, (bool, int, float))
    bn = isinstance(b, (bool, int, float))
    if an and bn:
        return a == b
    if _kind(a) != _kind(b):
        return False
    if isinstance(a, dict):
        if a.get('$type', '') != b.get('$type', '') or len(a) != len(b):
            return False
        for kx, ky in zip(a.keys(), b.keys()):
            if not _eq_deep(kx, ky, _depth + 1) or not _eq_deep(a[kx], b[ky], _depth + 1):
                return False
        return True
    if isinstance(a, (list, tuple)):
        if len(a) != len(b):
            return False
        for x, y in zip(a, b):
            if not _eq_deep(x, y, _depth + 1):
                return False
        return True
    return a == b


def _ncmp(op, a, b, _depth=0):
    """The three-way order `vcmp` gives, applied all the way down.

    Mirrors `vcmp_` in `crt.h` case for case, because `str < i64` is refused at
    the top level *and* inside a vector, a tag or a map, and the answer must be
    the same three-way comparison the native side made: an element-wise order,
    a length tie-break, `None` below everything and a tag compared by name
    first. Python's own operators cannot stand in for it -- `[1, "x"] < [1, 2]`
    is a `TypeError` there, whose text used to reach the program as its error.
    """
    if isinstance(a, str) and isinstance(b, str):
        return (a > b) - (a < b)
    an = isinstance(a, (bool, int, float))
    bn = isinstance(b, (bool, int, float))
    if an and bn:
        # Exact: converting to float first answered `2^60 + 1 > 2^60` with
        # "equal", and Python's own operators already compare an `int` against a
        # `float` by value.
        return (a > b) - (a < b)
    if a is None or b is None:
        if a is None and b is None:
            return 0
        return -1 if a is None else 1
    if _kind(a) != _kind(b) or _kind(a) == 'x':
        raise RuntimeError('type error in ' + op)
    if _depth + 1 >= _MAX_VALUE_DEPTH:
        raise RuntimeError('recursion too deep')
    if isinstance(a, list):
        for x, y in zip(a, b):
            c = _ncmp(op, x, y, _depth + 1)
            if c:
                return c
        return (len(a) > len(b)) - (len(a) < len(b))
    if isinstance(a, dict):
        at, bt = a.get('$type', ''), b.get('$type', '')
        c = _ncmp(op, at, bt)
        if c:
            return c
        ka, kb = list(a.keys()), list(b.keys())
        for kx, ky in zip(ka, kb):
            c = _ncmp(op, kx, ky, _depth + 1)
            if c:
                return c
            c = _ncmp(op, a[kx], b[ky], _depth + 1)
            if c:
                return c
        return (len(a) > len(b)) - (len(a) < len(b))
    if isinstance(a, tuple):
        # A tag: its name, then its arguments, then how many there are.
        c = _ncmp(op, a[0], b[0], _depth + 1)
        if c:
            return c
        for x, y in zip(a[1:], b[1:]):
            c = _ncmp(op, x, y, _depth + 1)
            if c:
                return c
        return (len(a) > len(b)) - (len(a) < len(b))
    return 0


def _compare(op, a, b):
    """`<`, `<=`, `>`, `>=` inside one comparable class, or the same error the
    native runtime raises. Mixed types are refused: a total order across `str`
    and `i64` would answer a question the program never asked."""
    c = _ncmp(op, a, b)
    if op == '<':
        return c < 0
    if op == '>':
        return c > 0
    if op == '<=':
        return c <= 0
    return c >= 0


_NAN = float('nan')

# A map key is typed, as it is in `crt.h`: `1`, `1.0` and `true` are three keys.
# Python folds them into one, so a numeric key is stored under a tag and unwrapped
# on the way out. String keys are left alone: `$type` marks a struct.
def _mkey(k):
    if isinstance(k, (list, dict)):
        # No hash the program cannot change under the map. Every caller checks
        # with `check_map_key` first and answers an err value; this guard is
        # the backstop that keeps CPython's own `TypeError: unhashable type`
        # from reaching the program, where no `try`/`catch` could see it.
        raise RuntimeError('unhashable map key')
    if isinstance(k, bool):
        return ('b', k)
    if isinstance(k, int):
        return ('i', k)
    if isinstance(k, float):
        # One NaN is one key, as in `vkey_hash`: Python would answer by object
        # identity, so two NaNs from two `parse_float` calls were two keys here
        # and one in the C backend.
        return ('f', _NAN if k != k else k)
    return k


def _munkey(k):
    return k[1] if isinstance(k, tuple) and len(k) == 2 and k[0] in ('b', 'i', 'f') else k


# Case mapping follows `vcase` in crt.h: a per-code-point simple mapping, plus
# the two expansions it spells out (ß -> SS, İ -> i + combining dot) and the
# context rule for final sigma. Code points whose full mapping is several chars
# stay put in both runtimes.
_CASE_EXPAND = {('ß', True): 'SS', ('İ', False): 'i\u0307'}


def _cased(cp, lower_ok):
    greek = (0x3b1 <= cp <= 0x3c9) if lower_ok else (0x391 <= cp <= 0x3a9)
    cyr = (0x430 <= cp <= 0x44f) if lower_ok else (0x410 <= cp <= 0x44f)
    return (0x41 <= cp <= 0x5a) or greek or cyr or (lower_ok and 0x61 <= cp <= 0x7a)


def _case_map(s, up):
    out = []
    prev = 0
    for i, ch in enumerate(s):
        cp = ord(ch)
        got = _CASE_EXPAND.get((ch, up))
        if got is not None:
            out.append(got)
        else:
            m = ch.upper() if up else ch.lower()
            if len(m) == 1:
                if not up and m == '\u03c3' and _cased(prev, False):
                    nxt = ord(s[i + 1]) if i + 1 < len(s) else 0
                    if not _cased(nxt, True):
                        m = '\u03c2'
                out.append(m)
            else:
                out.append(ch)
        prev = cp
    return ''.join(out)


def _checked_sort(v, key=None):
    """Sort one comparable class; `crt.h` `vsort` refuses a mixed container.

    The order is `_ncmp`'s, not Python's: a container whose elements are not all
    of one class is refused here, and a nested one is refused by the comparator
    itself -- `[[1], ["a"]].sort()` used to answer with a Python traceback where
    the native side answered `type error in <`.
    """
    import functools
    if len(set(_kind(x) for x in v)) > 1:
        raise RuntimeError('type error in <')
    if key is None:
        v.sort(key=functools.cmp_to_key(lambda a, b: _ncmp('<', a, b)))
    else:
        # `sort_by` compares the keys, as `vsort_by` does; the key function is
        # still called once per element, which is what the interpreter promised.
        v.sort(key=functools.cmp_to_key(lambda a, b: _ncmp('<', key(a), key(b))))
    return v


def val_type(v):
    if v is None:
        return 'none'
    if isinstance(v, bool):
        return 'bool'
    if isinstance(v, int):
        return 'i64'
    if isinstance(v, float):
        return 'f64'
    if isinstance(v, str):
        return 'str'
    if isinstance(v, list):
        return 'vec'
    if isinstance(v, dict):
        return 'struct ' + v['$type'] if v.get('$type') is not None else 'map'
    if isinstance(v, tuple):
        return str(v[0]) if v else 'tuple'
    return type(v).__name__


def _bstr(v):
    """Render a value like crt.h vprint_f does for `print`.

    Escapes only backslash, the chosen quote and \n \r \t, byte-wise: Python's
    repr escapes control characters and non-printables C cannot reproduce."""
    if isinstance(v, str):
        return v
    return _brepr(v)


def _brepr(v):
    if isinstance(v, bool):
        return 'True' if v else 'False'
    if v is None:
        return 'None'
    if isinstance(v, (int, float)):
        return str(v)
    if isinstance(v, str):
        quote = '"' if ("'" in v and '"' not in v) else "'"
        out = [quote]
        for ch in v:
            if ch == quote or ch == '\\':
                out.append('\\' + ch)
            elif ch == '\n':
                out.append('\\n')
            elif ch == '\r':
                out.append('\\r')
            elif ch == '\t':
                out.append('\\t')
            else:
                out.append(ch)
        out.append(quote)
        return ''.join(out)
    if isinstance(v, list):
        return '[' + ', '.join(_brepr(x) for x in v) + ']'
    if isinstance(v, dict):
        parts = []
        if v.get('$type') is not None:
            parts.append("'$type': '%s'" % v['$type'])
        for k, x in v.items():
            if k != '$type':
                parts.append('%s: %s' % (_brepr(_munkey(k)), _brepr(x)))
        return '{' + ', '.join(parts) + '}'
    if isinstance(v, tuple):
        # A closure is an internal object, not a value the language can write
        # down; `crt.h` says `fn` for the same reason, and the interpreter used
        # to print its AST here (`('closure', None, [('x', 'ref', ...)], ...)`),
        # which was the last known place where two backends printed different
        # bytes for one value.
        if v and v[0] == 'closure':
            return 'fn'
        parts = ["'%s'" % v[0]] + [_brepr(x) for x in v[1:]]
        return '(' + ', '.join(parts) + (',' if len(parts) == 1 else '') + ')'
    return str(v)


def find_path(env, path):
    parts = path.split('.')
    v = find(env, parts[0])
    for f in parts[1:]:
        if v is _MISS or not isinstance(v, dict) or f not in v:
            return _MISS
        v = v[f]
    return v


def grow(s, env):
    def sub(m):
        v = find_path(env, m.group(1))
        return m.group(0) if v is _MISS else _bstr(v)
    return _INTERP_RE.sub(sub, s)


def name_path(e):
    """Dotted name of a var/field chain, or None when `e` computes a value."""
    if isinstance(e, tuple) and e:
        if e[0] == 'var':
            return e[1]
        if e[0] == 'field':
            b = name_path(e[1])
            return b + '.' + e[2] if b else None
    return None


def find(env, n):
    for s in reversed(env):
        if n in s:
            v = s[n]
            if _MOVE and v is _MOVED:
                raise NameError('use of moved value ' + n)
            return v
    return _MISS

def _invalidate_move(env, node):
    if not _MOVE:
        return
    if isinstance(node, tuple) and node and node[0] == 'var':
        name = node[1]
        for s in reversed(env):
            if name in s:
                s[name] = _MOVED
                return

def put(env, n, v):
    for s in reversed(env):
        if n in s:
            s[n] = v
            return
    env[-1][n] = v

def vdrop(v):
    return None

def vmove(src):
    return src


_RC = {}
_MANAGED = bool(os.environ.get('BRAT_MANAGED', '1'))


def vrc_inc(v):
    if _MANAGED and isinstance(v, (str, list, dict)):
        e = _RC.get(id(v))
        if e is None:
            _RC[id(v)] = [1, v]
        else:
            e[0] += 1
    return v


def vrc_dec(v):
    if _MANAGED and isinstance(v, (str, list, dict)):
        e = _RC.get(id(v))
        if e is not None:
            e[0] -= 1
            if e[0] <= 0:
                _RC.pop(id(v), None)
    return None


def _drop_all(frame):
    if _MANAGED:
        for val in list(frame.values()):
            vrc_dec(val)


# Frame -> {binding: element type}. The element a `ptr[T]` promises is part of
# the declaration, and the interpreter needs it at `p[i]` to know the stride and
# the layout behind it. Keyed by the frame dict, so a binding in an inner
# function cannot inherit the outer one's element type, and dropped with it.
_PTRS = {}

# A window over C records remembers which struct it walks, so an element reads
# with the declared layout. Keyed by the tuple the window is, which is a value
# the interpreter passes around by identity.
_SLICE_STRUCTS = {}

# Records declared `extern packed struct`: no padding, so ctypes must not add any.
_PACKED = set()

# The element a call's result carries, in call order. A `ptr[T]` return says
# what the value points at; the `let` that binds it records that, so
# `let p = node_new(...)` needs no annotation and `ptr[Node]` still means Node.
_FFI_RETS = []


def _note_ptr(frame, name, ann):
    """Remember what `let p: ptr[T]` promises about `p`."""
    if not (isinstance(ann, str) and ann.startswith('ptr[') and ann.endswith(']')):
        return
    elem = ann[4:-1].strip()
    if elem:
        _PTRS.setdefault(id(frame), {})[name] = elem


def _note_ptr_params(frame, params):
    """The same for a function's parameters, which carry annotations too."""
    for p in params:
        if isinstance(p, tuple) and len(p) > 2:
            _note_ptr(frame, p[0], p[2])


def _forget_ptr_frame(frame):
    _PTRS.pop(id(frame), None)


def _bind(frame, name, val):
    if _MANAGED:
        old = frame.get(name)
        if old is not None:
            vrc_dec(old)
        vrc_inc(val)
    frame[name] = val


MAX_STR = 1 << 30
_TYPE_KEY = '$type'


def check_map_key(k):
    """The sentence a key a map cannot hold is refused with, or None.

    Both shapes answer with an `('err', ...)` *value*, never a raise:
    `test_forge.py` pins that for `$type` ("a caught error is a value in this
    language"), and `crt.h` answers `unhashable map key` for the other one.
    """
    if k == _TYPE_KEY:
        return 'map key "$type" is reserved'
    if isinstance(k, (list, dict)):
        return 'unhashable map key'
    return None


def ev(e, env, funcs):
    k = e[0]
    if k == 'num' or k == 'bool':
        return e[1]
    if k == 'none':
        return None
    if k == 'str':
        return grow(e[1], env)
    if k == 'rstr':
        return e[1]
    if k == 'vec':
        return [ev(x, env, funcs) for x in e[1]]
    if k == 'maplit':
        out = {}
        for ke, ve in e[1]:
            kv = ev(ke, env, funcs)
            bad = check_map_key(kv)
            if bad:
                return ('err', bad)
            # `_mkey` is what every read, store and method goes through: a
            # literal that skipped it stored `1`, `1.0` and `true` under the
            # keys Python folds together, so `{1: "a"}[1]` was "no key 1" here
            # while the C backend answered "a".
            out[_mkey(kv)] = ev(ve, env, funcs)
        return out
    if k == 'struct':
        return {'$type': e[1], **{f: ev(v, env, funcs) for f, v in e[2]}}
    if k == 'field':
        base = ev(e[1], env, funcs)
        if not isinstance(base, dict) or e[2] not in base:
            raise RuntimeError('no field ' + e[2])
        return base[e[2]]
    if k == 'idx':
        base = ev(e[1], env, funcs)
        ix = ev(e[2], env, funcs)
        if isinstance(base, int) and not isinstance(base, bool) and _ptr_kind(env, e[1]):
            return _ptr_at(_ptr_kind(env, e[1]), base, ix)
        if isinstance(base, dict):
            # A tag on the map makes the read a *field* read, which is the word
            # both backends already use for `p.zz`; the tag itself stays
            # unreadable. Asking first whether the map is tagged raised even
            # when the field was there: `let p = Point { x: 1 }` then
            # `p["x"]` was `no field x` here and `1` from every other backend.
            k = _mkey(ix)
            if k == _TYPE_KEY:
                raise RuntimeError('no field ' + str(ix))
            if k not in base:
                raise RuntimeError(('no field ' if '$type' in base else 'no key ')
                                   + str(ix))
            return base[k]
        if not isinstance(ix, (int, float)) or int(ix) != ix:
            raise RuntimeError('index must be int')
        if not isinstance(base, (list, str)):
            raise RuntimeError('index of non-indexable')
        ix = int(ix)
        if ix < 0 or ix >= len(base):
            raise RuntimeError('index out of range')
        return base[ix]
    if k == 'var':
        v = find(env, e[1])
        if v is _MISS:
            raise NameError('no var ' + e[1])
        return v
    if k == 'un':
        if e[1] == '&':
            # An out-parameter: only meaningful where an extern takes a pointer.
            if not _AMP[0]:
                raise RuntimeError('`&` is an out-parameter: write it where the extern '
                                   'takes a `ptr[T]`, as in `fill(&n, 21)`')
            # The address of a binding: a number a C function writes through.
            # The name is how the write gets back to the value, so `&` takes a
            # name, a field of one, or an element of one.
            src = e[2]
            if src[0] == 'var':
                _out_names(env)[src[1]] = True
                return ('addr', env, src[1], 0, None)
            if src[0] == 'field' and isinstance(src[1], tuple) and src[1][0] == 'var':
                whole = ev(src, env, funcs)
                _out_names(env)[src[1][1]] = True
                return ('addr', env, src[1][1], 0, None)
            if src[0] == 'idx' and isinstance(src[1], tuple) and src[1][0] == 'var':
                whole = ev(src, env, funcs)
                _out_names(env)[src[1][1]] = True
                return ('addr', env, src[1][1], 0, None)
            raise RuntimeError('`&` takes a binding: `&x`, `&x.f` or `&x[i]`')
        v = ev(e[2], env, funcs)
        if e[1] == '!':
            return not v
        if isinstance(v, int):
            return wrap64(-v)
        if isinstance(v, float):
            return -v
        raise RuntimeError('type error in unary -')
    if k == 'move':
        v = ev(e[1], env, funcs)
        _invalidate_move(env, e[1])
        return v
    if k == 'closure':
        _, name, params, body = e
        cap = dict(env[-1])
        node = ('closure', name, params, body, cap)
        if name:
            cap[name] = node
        return node
    if k == 'bin':
        op = e[1]
        a = ev(e[2], env, funcs)
        if op == '&&':
            return ev(e[3], env, funcs) if a else a
        if op == '||':
            return a if a else ev(e[3], env, funcs)
        b = ev(e[3], env, funcs)
        if op == '+':
            if isinstance(a, str) or isinstance(b, str):
                return str(a) + str(b)
            if isinstance(a, list) and isinstance(b, list):
                return a + b
            if isinstance(a, int) and isinstance(b, int):
                return wrap64(a + b)
            if isinstance(a, (int, float)) and isinstance(b, (int, float)):
                return a + b
            raise RuntimeError('type error in +')
        if op == '-':
            if isinstance(a, int) and isinstance(b, int):
                return wrap64(a - b)
            if isinstance(a, (int, float)) and isinstance(b, (int, float)):
                return a - b
            raise RuntimeError('type error in -')
        if op == '*':
            if isinstance(a, int) and isinstance(b, int):
                return wrap64(a * b)
            # str * int / int * str — same ceiling as .repeat / crt.h vrepeat_n
            if isinstance(a, str) and isinstance(b, int):
                if b <= 0 or not a:
                    return ''
                if len(a) * b > MAX_STR:
                    raise RuntimeError('out of memory')
                try:
                    return a * b
                except MemoryError:
                    raise RuntimeError('out of memory')
            if isinstance(b, str) and isinstance(a, int):
                if a <= 0 or not b:
                    return ''
                if len(b) * a > MAX_STR:
                    raise RuntimeError('out of memory')
                try:
                    return b * a
                except MemoryError:
                    raise RuntimeError('out of memory')
            if isinstance(a, (int, float)) and isinstance(b, (int, float)):
                return a * b
            raise RuntimeError('type error in *')
        if op == '/':
            if b == 0:
                raise RuntimeError('div by zero')
            if isinstance(a, float) or isinstance(b, float):
                return a / b
            if isinstance(a, int) and isinstance(b, int):
                if a == -_I64 and b == -1:
                    return -_I64
                return wrap64(a // b)
            if isinstance(a, (int, float)) and isinstance(b, (int, float)):
                return a / b
            raise RuntimeError('type error in /')
        if op == '%':
            if b == 0:
                raise RuntimeError('div by zero')
            if isinstance(a, int) and isinstance(b, int):
                if a == -_I64 and b == -1:
                    return 0
                return wrap64(a % b)
            if isinstance(a, (int, float)) and isinstance(b, (int, float)):
                r = math.fmod(a, b)
                if r != 0 and ((r < 0) != (b < 0)):
                    r += b
                return r
            raise RuntimeError('type error in %')
        if op == '==' or op == '!=':
            if (_kind(a) == _kind(b) and isinstance(a, (list, dict, tuple))
                    and _too_deep(a, b)):
                # Deep enough that the native `veq` would stop with its budget:
                # walk it here with the same budget instead of asking Python,
                # whose answer for a 2500-deep pair is a `RecursionError`.
                same = _eq_deep(a, b)
            else:
                same = a == b
            return same if op == '==' else not same
        if op in ('<', '>', '<=', '>='):
            # The budget is `_ncmp`'s now: it counts as it descends, so a deep
            # value compared against `None` answers instead of raising, exactly
            # as `vcmp_` does when the two classes differ.
            return _compare(op, a, b)
        if op == 'in':
            if isinstance(b, dict):
                return _mkey(a) in b and a != '$type'
            if isinstance(b, (list, str)):
                return a in b
            raise RuntimeError('in of non-container')
        raise RuntimeError('bad bin op ' + op)
    if k == 'or':
        a = ev(e[1], env, funcs)
        return a if a is not None else ev(e[2], env, funcs)
    if k == 'quest':
        v = ev(e[1], env, funcs)
        if isinstance(v, tuple) and len(v) == 2 and v[0] in ('ok', 'some'):
            return v[1]
        if isinstance(v, tuple) and len(v) == 2 and v[0] == 'err':
            raise Propagate(v)
        return v
    if k == 'mcall':
        path = name_path(e[1])
        if path is not None and find(env, path.split('.')[0]) is _MISS:
            # Static path: `log.info`, `Type.method`, `Type.Variant`. The callee
            # is looked up by name, so nothing is evaluated as a receiver.
            return call(path + '.' + e[2], e[3], env, funcs)
        obj = ev(e[1], env, funcs)
        if e[2] in ('map', 'filter', 'any', 'all', 'find', 'sort_by') and len(e[3]) == 1:
            return apply_hof(obj, e[2], e[3], env, funcs)
        if e[2] == 'fold' and len(e[3]) == 2:
            return apply_hof(obj, e[2], e[3], env, funcs)
        vals = [ev(a, env, funcs) for a in e[3]]
        fname = method_of(obj, e[2], funcs)
        if fname is not None:
            return call_method_fn(fname, obj, vals, env, funcs)
        return do_method(obj, e[2], vals)
    if k == 'parfor':
        return do_parfor(e, env, funcs)
    if k == 'call':
        if e[2] == [] and e[1][:1].isupper() and '.' not in e[1]:
            v = find(env, e[1])
            if v is not _MISS and e[1] not in funcs:
                return v
        return call(e[1], e[2], env, funcs)
    raise RuntimeError('bad node ' + k)

# One line per write under one lock: print() writes the text and the newline as
# two calls, so concurrent spawns could tear a line apart ("childchild").
_OUT_LOCK = threading.Lock()


def _out_line(line):
    with _OUT_LOCK:
        sys.stdout.write(line + '\n')


def emit_hit(level, msg):
    _out_line(json.dumps({'ts': time.time(), 'lvl': level, 'msg': msg}, ensure_ascii=False))


_RE_STEPS = threading.local()
_RE_MAX_STEPS = 2000000


def _re_parse(pat):
    pos = [0]
    n = len(pat)

    def peek():
        return pat[pos[0]] if pos[0] < n else None

    def parse_alt():
        alts = [parse_seq()]
        while peek() == '|':
            pos[0] += 1
            alts.append(parse_seq())
        if len(alts) == 1:
            return alts[0]
        return ('alt', alts)

    def parse_seq():
        items = []
        while True:
            c = peek()
            if c is None or c in ('|', ')'):
                return items
            atom = parse_atom()
            q = peek()
            if q in ('*', '+', '?'):
                pos[0] += 1
                atom = ({'*': 'star', '+': 'plus', '?': 'opt'}[q], atom)
            items.append(atom)

    def parse_atom():
        c = pat[pos[0]]
        if c == '(':
            pos[0] += 1
            inner = parse_alt()
            if peek() != ')':
                raise SyntaxError('regex: unbalanced ( in ' + pat)
            pos[0] += 1
            return ('group', inner)
        if c == '[':
            j = pos[0] + 1
            body = ''
            if j < n and pat[j] == '^':
                body += '^'
                j += 1
            while j < n and pat[j] != ']':
                body += pat[j]
                j += 1
            pos[0] = j + 1
            return ('set', body)
        if c == '.':
            pos[0] += 1
            return ('dot', None)
        if c == '^':
            pos[0] += 1
            return ('bol', None)
        if c == '$':
            pos[0] += 1
            return ('eol', None)
        if c == '\\':
            e = pat[pos[0] + 1] if pos[0] + 1 < n else '\\'
            pos[0] += 2
            if e == 'd':
                return ('set', '0-9')
            if e == 'D':
                return ('set', '^0-9')
            if e == 'w':
                return ('set', 'a-zA-Z0-9_')
            if e == 'W':
                return ('set', '^a-zA-Z0-9_')
            if e == 's':
                return ('set', ' \t\n\r')
            if e == 'S':
                return ('set', '^ \t\n\r')
            if e == 'n':
                return ('lit', '\n')
            if e == 't':
                return ('lit', '\t')
            if e == 'r':
                return ('lit', '\r')
            return ('lit', e)
        pos[0] += 1
        return ('lit', c)

    tree = parse_alt()
    if pos[0] != n:
        raise SyntaxError('regex: unbalanced ) in ' + pat)
    return tree


def _re_one(node, ch):
    k = node[0]
    if k == 'lit':
        return ch == node[1]
    if k == 'dot':
        return True
    if k == 'set':
        body = node[1]
        neg = body.startswith('^')
        if neg:
            body = body[1:]
        found = False
        i = 0
        while i < len(body):
            if i + 2 < len(body) and body[i + 1] == '-':
                if body[i] <= ch <= body[i + 2]:
                    found = True
                    break
                i += 3
            else:
                if body[i] == ch:
                    found = True
                    break
                i += 1
        return (not found) if neg else found
    return False


def _re_node_ends(node, s, si):
    k = node[0]
    if k == 'lit' or k == 'set' or k == 'dot':
        if si < len(s) and _re_one(node, s[si]):
            yield si + 1
        return
    if k == 'bol':
        if si == 0:
            yield si
        return
    if k == 'eol':
        if si == len(s):
            yield si
        return
    if k == 'group':
        inner = node[1]
        if isinstance(inner, tuple) and inner and inner[0] == 'alt':
            for alt in inner[1]:
                for e in _re_items_ends(alt, s, si):
                    yield e
        else:
            for e in _re_items_ends(inner, s, si):
                yield e
        return
    if k == 'alt':
        for alt in node[1]:
            for e in _re_items_ends(alt, s, si):
                yield e
        return
    if k in ('star', 'plus', 'opt'):
        ends = [si]
        i = si
        while i < len(s):
            nxt = None
            for e in _re_node_ends(node[1], s, i):
                nxt = e
                break
            if nxt is None or nxt == i:
                break
            i = nxt
            ends.append(i)
            if len(ends) > 100000:
                break
        if k == 'opt':
            ends = [si + 1, si] if len(ends) > 1 else [si, si]
        else:
            if k == 'plus':
                ends = ends[1:]
            ends = list(reversed(ends))
        for e in ends:
            yield e
        return


def _re_items_ends(items, s, si):
    _RE_STEPS.n = getattr(_RE_STEPS, 'n', 0) + 1
    if _RE_STEPS.n > _RE_MAX_STEPS:
        raise RuntimeError('regex too complex')
    if not items:
        yield si
        return
    for end in _re_node_ends(items[0], s, si):
        for e in _re_items_ends(items[1:], s, end):
            yield e


def re_match(s, pat):
    _RE_STEPS.n = 0
    # C matches raw UTF-8 bytes; latin-1 maps each byte to one code point so the
    # tree-walker indexes and matches by byte, keeping `.`/classes in parity.
    s = s.encode('utf-8').decode('latin-1')
    pat = pat.encode('utf-8').decode('latin-1')
    try:
        tree = _re_parse(pat)
    except SyntaxError:
        raise RuntimeError('bad regex pattern')
    for i in range(len(s) + 1):
        ends = _re_node_ends(tree, s, i) if isinstance(tree, tuple) else _re_items_ends(tree, s, i)
        for _ in ends:
            return True
    return False

def _coerce_arg(ann, val):
    """`fn f(x: f64)` binds a double in the emitted C, so an `i64` literal has to
    arrive as a float here too; otherwise `f(2)` prints `2` from the interpreter
    and `2.0` from the native build. Only the annotated-float case is coerced:
    every other annotation is either managed (str, vec, map) or already the same
    Python type, and `dyn` must stay untouched. A closure literal is not coerced:
    the C emitter types its parameters dynamically (the `fn(x: f64)` in
    `lang/examples/closure.brat` receives whatever it is called with), so
    coercing there would trade this divergence for its mirror image."""
    if ann == 'f64' and isinstance(val, int) and not isinstance(val, bool):
        return float(val)
    return val


def _coerce_params(params, vals):
    if not any(p[2] == 'f64' for p in params):
        return vals
    return [_coerce_arg(p[2], v) for p, v in zip(params, vals)]


def fill_defaults(params, vals, env, funcs, name):
    if len(vals) == len(params):
        return vals
    ndef = 0
    for p in params:
        if p[3] is not None:
            ndef += 1
    if len(vals) < len(params) - ndef or len(vals) > len(params):
        raise TypeError(name + ' wants %d args' % len(params))
    vals = list(vals)
    for i in range(len(vals), len(params)):
        vals.append(ev(params[i][3], env, funcs))
    return vals


def run_closure(node, vals, env, funcs):
    _, cname, params, body, cap = node
    names = [p[0] for p in params]
    vals = fill_defaults(params, vals, env, funcs, cname or 'closure')
    frame = dict(cap)
    if _MANAGED:
        for val in vals:
            vrc_inc(val)
    frame.update(zip(names, vals))
    _note_ptr_params(frame, params)
    if cname:
        frame[cname] = node
    # A closure sees the script frame, like a named function does: `find` walks
    # `inner` from the top, so the closure's own frame still shadows a binding of
    # the same name, and a script-level `let` is read live rather than copied at
    # creation. It used to be `inner = [frame]`, which cut the closure off from
    # the script: `let base = 5` + `fn(x) { return x + base }` raised `NameError:
    # no var base` in the interpreter while both emitters printed the answer.
    inner = ([env[0]] if env else []) + [frame]
    with _frame():
        try:
            run_block(body, inner, funcs, frame=True)
        except Propagate:
            if _MANAGED:
                for nm in names:
                    vrc_dec(frame.get(nm))
            raise
        except (Done, Ret) as r:
            if _MANAGED:
                for nm in names:
                    vrc_dec(frame.get(nm))
            return r.v
        if _MANAGED:
            for nm in names:
                vrc_dec(frame.get(nm))
        return 0


def resolve_fn(argnode, env, funcs, meth):
    if (isinstance(argnode, tuple) and argnode and argnode[0] == 'var'
            and argnode[1] not in env[-1] and argnode[1] in funcs
            and isinstance(funcs[argnode[1]], tuple) and len(funcs[argnode[1]]) == 2):
        params, body = funcs[argnode[1]]
        return ('closure', argnode[1], params, body, {})
    fnval = ev(argnode, env, funcs)
    if not (isinstance(fnval, tuple) and fnval and fnval[0] == 'closure'):
        raise _no_method(meth)
    return fnval


def apply_iter(obj, meth, argnode, env, funcs):
    if not isinstance(obj, list):
        raise _no_method(meth)
    fnval = resolve_fn(argnode, env, funcs, meth)
    out = []
    for x in obj:
        r = run_closure(fnval, [x], env, funcs)
        if meth == 'map':
            out.append(r)
        elif r:
            out.append(x)
    return out


def apply_hof(obj, meth, argnodes, env, funcs):
    if meth in ('map', 'filter'):
        return apply_iter(obj, meth, argnodes[0], env, funcs)
    if not isinstance(obj, list):
        raise _no_method(meth)
    if meth in ('any', 'all', 'find'):
        fnval = resolve_fn(argnodes[0], env, funcs, meth)
        for x in obj:
            r = run_closure(fnval, [x], env, funcs)
            if meth == 'any' and r:
                return True
            if meth == 'all' and not r:
                return False
            if meth == 'find' and r:
                return x
        if meth == 'any':
            return False
        if meth == 'all':
            return True
        return None
    if meth == 'fold':
        fnval = resolve_fn(argnodes[1], env, funcs, 'fold')
        acc = ev(argnodes[0], env, funcs)
        for x in obj:
            acc = run_closure(fnval, [acc, x], env, funcs)
        return acc
    if meth == 'sort_by':
        fnval = resolve_fn(argnodes[0], env, funcs, 'sort_by')
        return _checked_sort(obj, key=lambda x: run_closure(fnval, [x], env, funcs))
    raise NameError('no method ' + meth)


_DFR = threading.local()


def _dfr_stack():
    st = getattr(_DFR, 'st', None)
    if st is None:
        st = _DFR.st = []
    return st


class _DfrScope(list):
    """One defer frame.

    `frame` marks the body of a function so its defers run after the BRAT depth
    is released, matching the C backend's `bdepth_pop(); vdfreset(...)` order.
    A nested block keeps its frame, so its defers still cost a depth level.
    """
    __slots__ = ('frame',)

    def __init__(self, frame=False):
        super().__init__()
        self.frame = frame


def run_defers(env, funcs):
    scope = _dfr_stack().pop()
    saved = getattr(_DEPTH, 'd', 0)
    if scope.frame:
        # Release this frame before the deferred call, as __fin does in C,
        # so a defer running exactly at the budget is not itself refused.
        _DEPTH.d = saved - 1
    try:
        for clo in reversed(scope):
            run_closure(clo, [], env, funcs)
    finally:
        _DEPTH.d = saved


_STR_METH = {'split', 'lines', 'trim', 'lower', 'upper', 'starts_with', 'ends_with', 'contains', 'parse_int', 'replace', 'index_of', 'repeat', 'split_once', 'copy', 'chars', 'parse_float', 'slice'}
_FLOAT_RE = re.compile(r'[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?\Z')
_FLOAT_WORD = re.compile(r'[+-]?(?:inf(?:inity)?|nan)\Z', re.IGNORECASE)
_VEC_METH = {'join', 'push', 'pop', 'first', 'last', 'sort', 'rev', 'ins', 'del', 'slice', 'sum', 'min', 'max', 'enumerate', 'copy', 'zip', 'chunks', 'pos'}
_MAP_METH = {'get', 'has', 'keys', 'del', 'set', 'items', 'vals', 'copy', 'get_or'}

# How many arguments each builtin method takes. The dispatch below reads
# `vals[0]` / `vals[1]`, so a wrong count used to leave the interpreter as a
# CPython `IndexError: list index out of range` -- a host exception printed in a
# BRAT program's place, naming a list nobody wrote. The counts are the checker's
# ("method `set` wants 2 args"), and the emitters refuse the same calls.
_METH_ARITY = {
    'split': (0, 1), 'lines': (0, 0), 'trim': (0, 0), 'lower': (0, 0),
    'upper': (0, 0), 'starts_with': (1, 1), 'ends_with': (1, 1),
    'contains': (1, 1), 'parse_int': (0, 0), 'replace': (2, 2),
    'index_of': (1, 1), 'repeat': (1, 1), 'split_once': (1, 1),
    'parse_float': (0, 0), 'slice': (2, 2), 'join': (1, 1),
    'push': (1, 1), 'pop': (0, 0), 'first': (0, 0), 'last': (0, 0),
    'sort': (0, 0), 'rev': (0, 0), 'ins': (2, 2), 'del': (1, 1),
    'sum': (0, 0), 'min': (0, 0), 'max': (0, 0), 'enumerate': (0, 0),
    'zip': (1, 1), 'chunks': (1, 1), 'pos': (1, 1), 'get': (1, 1),
    'has': (1, 1), 'keys': (0, 0), 'set': (2, 2), 'items': (0, 0),
    'vals': (0, 0), 'get_or': (2, 2), 'copy': (0, 0), 'chars': (0, 0),
    'send': (1, 1), 'recv': (0, 0), 'close': (0, 0),
}


def _check_arity(meth, vals):
    want = _METH_ARITY.get(meth)
    if want is None:
        return
    lo, hi = want
    if lo <= len(vals) <= hi:
        return
    if lo == hi:
        raise RuntimeError('method `%s` wants %d args' % (meth, lo))
    raise RuntimeError('method `%s` wants %d to %d args' % (meth, lo, hi))


def do_method(obj, meth, vals):
    _check_arity(meth, vals)
    try:
        return _do_method(obj, meth, vals)
    except (TypeError, AttributeError, IndexError, KeyError):
        # A host exception means the call was wrong for this receiver -- an i64
        # where a separator belongs, a record with no such field. BRAT reports
        # that in its own words; the CPython text names nothing the program has.
        raise RuntimeError('method `%s`: bad argument' % meth)


def _do_method(obj, meth, vals):
    if isinstance(obj, _Chan):
        if meth == 'send':
            return obj.send(vals[0] if vals else None)
        if meth == 'recv':
            return obj.recv()
        if meth == 'close':
            return obj.close()
        raise TypeError('channel has no method ' + meth)
    if isinstance(obj, str) and meth in _STR_METH:
        if meth == 'split':
            return obj.split(vals[0] if vals else None)
        if meth == 'lines':
            return obj.splitlines()
        if meth == 'trim':
            return obj.strip()
        if meth == 'lower':
            return _case_map(obj, False)
        if meth == 'upper':
            return _case_map(obj, True)
        if meth == 'starts_with':
            return obj.startswith(vals[0])
        if meth == 'ends_with':
            return obj.endswith(vals[0])
        if meth == 'contains':
            return vals[0] in obj
        if meth == 'parse_int':
            t = obj.strip()
            body = t[1:] if t[:1] in ('+', '-') else t
            if body and all('0' <= ch <= '9' for ch in body):
                return ('ok', wrap64(int(t)))
            return ('err', 'not a number')
        if meth == 'replace':
            return obj.replace(vals[0], vals[1])
        if meth == 'index_of':
            r = obj.find(vals[0])
            return None if r < 0 else r
        if meth == 'repeat':
            if len(obj) * int(vals[0]) > MAX_STR:
                raise RuntimeError('out of memory')
            try:
                return obj * vals[0]
            except MemoryError:
                raise RuntimeError('out of memory')
        if meth == 'split_once':
            h, sep, t = obj.partition(vals[0])
            return None if not sep and vals[0] else [h, t]
        if meth == 'copy':
            return obj
        if meth == 'chars':
            return list(obj)
        if meth == 'slice':
            return obj[int(vals[0]):int(vals[1])]
        if meth == 'parse_float':
            t = obj.strip()
            if _FLOAT_RE.match(t) or _FLOAT_WORD.match(t):
                try:
                    return ('ok', float(t))
                except ValueError:
                    pass
            return ('err', 'not a float')
    if isinstance(obj, dict) and meth in _MAP_METH:
        if meth == 'get':
            if len(vals) != 1:
                raise RuntimeError('no method get')
            bad = check_map_key(vals[0])
            if bad:
                return ('err', bad)
            return obj.get(_mkey(vals[0]))
        if meth == 'get_or':
            bad = check_map_key(vals[0])
            if bad:
                return ('err', bad)
            return obj.get(_mkey(vals[0]), vals[1])
        if meth == 'has':
            bad = check_map_key(vals[0])
            if bad:
                return ('err', bad)
            return _mkey(vals[0]) in obj
        if meth == 'keys':
            return [_munkey(k) for k in obj.keys() if k != '$type']
        if meth == 'del':
            bad = check_map_key(vals[0])
            if bad:
                return ('err', bad)
            k = _mkey(vals[0])
            if k in obj:
                del obj[k]
                return True
            return False
        if meth == 'set':
            bad = check_map_key(vals[0])
            if bad:
                return ('err', bad)
            obj[_mkey(vals[0])] = vals[1]
            return vals[1]
        if meth == 'items':
            return [[_munkey(k), v] for k, v in obj.items() if k != '$type']
        if meth == 'vals':
            return [v for k, v in obj.items() if k != '$type']
        if meth == 'copy':
            return dict(obj)
    if isinstance(obj, list) and meth in _VEC_METH:
        if meth == 'join':
            return vals[0].join(str(x) for x in obj)
        if meth == 'push':
            obj.append(vals[0])
            return len(obj)
        if meth == 'pop':
            return obj.pop() if obj else None
        if meth == 'first':
            return obj[0] if obj else None
        if meth == 'last':
            return obj[-1] if obj else None
        if meth == 'sort':
            return _checked_sort(obj)
        if meth == 'rev':
            return list(reversed(obj))
        if meth == 'ins':
            obj.insert(int(vals[0]), vals[1])
            return len(obj)
        if meth == 'del':
            ix = int(vals[0])
            if ix < 0:
                ix += len(obj)
            if ix < 0 or ix >= len(obj):
                raise RuntimeError('index out of range')
            return obj.pop(ix)
        if meth == 'slice':
            return obj[int(vals[0]):int(vals[1])]
        if meth == 'sum':
            if not all(isinstance(x, (int, float, bool)) for x in obj):
                raise RuntimeError('sum of non-numeric')
            t = 0
            for x in obj:
                if isinstance(x, int):
                    t = wrap64(t + x)
                else:
                    t = t + x
            return t
        if meth == 'min' or meth == 'max':
            if len(set(_kind(x) for x in obj)) > 1:
                raise RuntimeError('type error in <')
            if not obj:
                return None
            best = obj[0]
            for x in obj[1:]:
                c = _ncmp('<', x, best)
                if (c < 0) if meth == 'min' else (c > 0):
                    best = x
            return best
        if meth == 'enumerate':
            return [[i, x] for i, x in enumerate(obj)]
        if meth == 'copy':
            return list(obj)
        if meth == 'zip':
            return [list(p) for p in zip(obj, vals[0])]
        if meth == 'chunks':
            n = int(vals[0])
            if n <= 0:
                raise RuntimeError('chunks wants n > 0')
            return [obj[i:i + n] for i in range(0, len(obj), n)]
        if meth == 'pos':
            try:
                return obj.index(vals[0])
            except ValueError:
                return None
    raise _no_method(meth)


_FFI_LIBS = {}


def _ffi_lib(libname):
    """dlopen each library once; CDLL on every call was the hot cost."""
    import ctypes
    lib = _FFI_LIBS.get(libname)
    if lib is None:
        lib = ctypes.CDLL(libname, use_errno=True) if libname else ctypes.CDLL(None, use_errno=True)
        _FFI_LIBS[libname] = lib
    return lib


# Integer FFI types: ctypes name and inclusive range. BRAT ints are i64, so a
# u64/usize result above i64::MAX cannot be represented and is rejected.
_FFI_INT = {
    'i8': ('c_byte', -2**7, 2**7 - 1),
    'u8': ('c_ubyte', 0, 255),
    'i16': ('c_short', -2**15, 2**15 - 1),
    'u16': ('c_ushort', 0, 65535),
    'i64': ('c_longlong', -2**63, 2**63 - 1),
    'i32': ('c_int', -2**31, 2**31 - 1),
    'u32': ('c_uint32', 0, 2**32 - 1),
    'u64': ('c_uint64', 0, 2**63 - 1),
    'usize': ('c_size_t', 0, 2**63 - 1),
}


_OUT_NAMES = []          # one stack of {name: True} per frame that has an `&`


def _out_names(env):
    """The set of names in `env` that `&` handed to a callee."""
    if not _OUT_NAMES:
        _OUT_NAMES.append({})
    top = _OUT_NAMES[-1]
    if id(env) not in top:
        top[id(env)] = {}
    return top[id(env)]


def _out_back(val, env, funcs):
    """Write through an `&`: the callee has been called, read what it left."""
    import ctypes
    _, oenv, name, width, whole = val
    if whole is not None:
        oenv[name] = whole
        return
    cur = oenv.get(name, 0)
    if width == 8:
        new = ctypes.c_longlong(cur).value if not isinstance(cur, float) else cur
    elif width == 4:
        new = ctypes.c_int(cur).value
    elif width == 2:
        new = ctypes.c_short(cur).value
    else:
        new = ctypes.c_ubyte(cur).value
    oenv[name] = new


def _ffi_invoke(name, info, vals, funcs):
    """Call a BRAT `extern` through ctypes. One path for both call sites."""
    import ctypes
    libname = info[3]
    lib = _ffi_lib(libname)
    fn = getattr(lib, name)
    params, ret = info[1], info[2]
    varargs = info[4] if len(info) > 4 else None
    checked = len(info) > 5 and info[5]

    def scalar_ctype(ty):
        if ty in _FFI_INT:
            return getattr(ctypes, _FFI_INT[ty][0])
        return {'f64': ctypes.c_double, 'f32': ctypes.c_float, 'str': ctypes.c_char_p,
                'ptr': ctypes.c_void_p}.get(ty, _struct_class(ty) if ty in _STRUCTS else None)

    boxes = []

    def to_c(ty, v, i, variadic_slot=False):
        if isinstance(ty, str) and ty.startswith('ptr['):
            ty = 'ptr'      # the element is a promise to BRAT, not to C
        if isinstance(ty, str) and ty.startswith('fn'):
            return _callback_ptr(ty, v, funcs, i, name)
        if ty in _STRUCTS:
            cls = _struct_class(ty)
            inst = cls()
            for f, ft in _STRUCTS[ty]:
                fv = v.get(f) if isinstance(v, dict) else v
                # A scalar field assigns through the ctypes descriptor, which
                # refuses a c_float where it declared c_long: unwrap first.
                cv = to_c(ft, fv, i)
                setattr(inst, f, cv.value if hasattr(cv, 'value') else cv)
            return inst
        if ty is None:
            return v   # untyped `...` argument
        if ty in _FFI_INT:
            _, lo, hi = _FFI_INT[ty]
            if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
                raise RuntimeError('ffi: arg %d of %s wants %s' % (i, name, ty))
            return scalar_ctype(ty)(v)
        if ty in ('f64', 'f32'):
            if not isinstance(v, (int, float)) or isinstance(v, bool):
                raise RuntimeError('ffi: arg %d of %s wants %s' % (i, name, ty))
            return ctypes.c_float(v) if ty == 'f32' else ctypes.c_double(v)
        if ty == 'str':
            if not isinstance(v, str):
                raise RuntimeError('ffi: arg %d of %s wants str' % (i, name))
            if '\0' in v:
                # C would silently end the string at the NUL byte.
                raise RuntimeError('ffi: arg %d of %s has a NUL byte' % (i, name))
            return ctypes.c_char_p(v.encode('utf-8'))
        if ty == 'ptr' and isinstance(v, tuple) and v and v[0] == 'addr':
            # `&x`: the callee gets storage of the width its `ptr[T]` declares,
            # seeded with the binding's value, and the binding takes the value
            # back after the call.
            _tag, oenv, oname, _w, _whole = v
            # Where the name actually lives: `env` is a list of scopes, and the
            # binding may have been made in an outer one.
            _scope = next((sc for sc in reversed(oenv) if oname in sc), None)
            _pointee = params[i - 1][2] if i - 1 < len(params) else None
            # The storage is the width of the pointee, not the width of a word:
            # an `i16 *` out-parameter writes two bytes and reads two.
            ct = {'u8': ctypes.c_ubyte, 'i8': ctypes.c_byte,
                  'u16': ctypes.c_ushort, 'i16': ctypes.c_short,
                  'i32': ctypes.c_int, 'u32': ctypes.c_uint,
                  'f32': ctypes.c_float, 'f64': ctypes.c_double,
                  'i64': ctypes.c_longlong, 'u64': ctypes.c_ulonglong,
                  'usize': ctypes.c_size_t}.get(
                      _pointee[4:-1] if isinstance(_pointee, str) and _pointee.startswith('ptr[') else '',
                      ctypes.c_longlong)
            cur = _scope.get(oname, 0) if _scope else 0
            try:
                box = ct(int(cur))
            except (TypeError, ValueError):
                box = ct(cur)
            boxes.append((_scope, oname, box))
            return ctypes.byref(box)
        if ty == 'ptr':
            if isinstance(v, str) and variadic_slot:
                # A `str` fills a variadic `..ptr` slot as `const char *`.
                return ctypes.c_char_p(v.encode('utf-8'))
            if not isinstance(v, int) or isinstance(v, bool) or v < 0:
                raise RuntimeError('ffi: arg %d of %s wants ptr' % (i, name))
            return ctypes.c_void_p(v)
        raise RuntimeError('ffi: unsupported type %s in extern %s' % (ty, name))

    cargs = [to_c(p[2] if isinstance(p, tuple) and len(p) > 2 else None, v, i + 1)
             for i, (p, v) in enumerate(zip(params, vals))]
    cargs += [to_c(varargs, v, len(params) + j + 1, True)
              for j, v in enumerate(vals[len(params):])]

    fn.argtypes = None
    if varargs is None:
        fixed = [scalar_ctype(p[2]) if isinstance(p, tuple) and len(p) > 2 else None
                 for p in params]
        if all(t is not None for t in fixed):
            fn.argtypes = fixed
    if ret in _FFI_INT:
        fn.restype = scalar_ctype(ret)
    elif ret == 'str':
        fn.restype = ctypes.c_char_p
    elif ret == 'ptr' or (isinstance(ret, str) and ret.startswith('ptr[')):
        fn.restype = ctypes.c_void_p
    elif ret == 'f64':
        fn.restype = ctypes.c_double
    elif ret == 'f32':
        fn.restype = ctypes.c_float
    elif ret in _STRUCTS:
        fn.restype = _struct_class(ret)
    elif ret is None:
        fn.restype = None
    else:
        raise RuntimeError('ffi: unsupported return type %s in extern %s' % (ret, name))

    if checked:
        ctypes.set_errno(0)
    r = fn(*cargs)
    for oenv, oname, box in boxes:
        # The name is the only way back to the value; a `f32`/`f64` binding
        # takes a float, everything else an integer.
        oenv[oname] = box.value
    if checked:
        # NULL for ptr, -1 for integers: same sentinels as the native backend.
        failed = (r is None or r == 0) if ret == 'ptr' else r == -1
        if failed:
            raise RuntimeError('%s: %s' % (name, os.strerror(ctypes.get_errno())))
    def _struct_val(type_name, inst):
        """A ctypes struct as a BRAT struct value, nested fields included.

        A `#[repr(C)]` struct usually has another one inside it; leaving the
        inner field as a ctypes object made `r.origin.y` a runtime error and
        `print(r.origin)` a Python repr.
        """
        out = {'$type': type_name}
        for f, ft in _STRUCTS[type_name]:
            val = getattr(inst, f)
            if ft in _STRUCTS:
                out[f] = _struct_val(ft, val)
            elif ft == 'str':
                out[f] = val.decode('utf-8') if isinstance(val, bytes) else val
            elif ft == 'ptr':
                out[f] = val.value if hasattr(val, 'value') else (val or 0)
            elif hasattr(val, 'value'):
                out[f] = val.value      # a scalar read back is a ctypes wrapper
            else:
                out[f] = val
        return out

    if isinstance(ret, str) and ret.startswith('ptr[') and ret.endswith(']') and r is not None:
        # The pointer carries its element, so `let p = node_new(...)` can be
        # indexed without a second annotation: the declaration made the promise.
        _FFI_RETS.append(ret[4:-1].strip())
    elif ret == 'ptr' and r is not None:
        _FFI_RETS.append(None)
    if ret in _STRUCTS:
        return _struct_val(ret, r)
    if ret in _FFI_INT:
        if r > 2**63 - 1:
            raise RuntimeError('ffi: result of %s does not fit i64' % name)
        return r
    if ret == 'str':
        if r is None:
            raise RuntimeError('ffi: C returned NULL where str expected')
        return r.decode('utf-8') if isinstance(r, bytes) else r
    if ret == 'ptr' or (isinstance(ret, str) and ret.startswith('ptr[')):
        if hasattr(r, 'value'):
            return r.value or 0
        return r if r is not None else 0
    if ret in ('f64', 'f32'):
        # `c_float` already rounded the value; BRAT has one float type.
        return float(r)
    return None


def do_parfor(e, env, funcs):
    """`par for x in v reduce(op, unit) { return f(x) }` — a checked monoid fold.

    The interpreter folds in index order without threads: a pure body has no
    observable order, and the fold order is fixed, so the result is the same
    value the native backend produces with a thread pool. Speed is the native
    backend's job; agreeing with it exactly is this one's.
    """
    _, var, it_e, op, unit_e, body = e
    seq = ev(it_e, env, funcs)
    if isinstance(seq, dict):
        seq = [_munkey(k) for k in seq if k != '$type']
    if not isinstance(seq, list):
        raise RuntimeError('par for wants a vec')
    results = []
    for item in seq:
        frame = {var: item}
        with _frame():
            env.append(frame)
            try:
                run_block(body, env, funcs, frame=True)
            except (Ret, Done) as r:
                results.append(r.v)
                env.pop()
                continue
            except Exception:
                env.pop()
                raise
            env.pop()
    acc = ev(unit_e, env, funcs)
    for r in results:
        acc = call_value(op, [acc, r], env, funcs)
    return acc


def _ptr_kind(env, node):
    """The element type of `p` in `p[i]`, when `p` is a `ptr[T]` binding.

    The annotation lives with the frame that declared it: the innermost frame
    holding the name wins, so a shadowing binding without `ptr[T]` keeps the
    plain `ptr` behaviour.
    """
    if not (isinstance(node, tuple) and node and node[0] == 'var'):
        return None
    name = node[1]
    for frame in reversed(env):
        if name in frame:
            kinds = _PTRS.get(id(frame))
            return kinds.get(name) if kinds else None
    return None


_PTR_WORD = {'i64': ('q', 8), 'i32': ('i', 4), 'u8': ('B', 1),
             'f64': ('d', 8), 'f32': ('f', 4)}


def _ptr_at(elem, base, ix):
    """Read the element `p[i]` names, at the width the element type says."""
    import ctypes
    if base <= 0:
        raise RuntimeError('null pointer dereference' if base == 0 else 'pointer is negative')
    if not isinstance(ix, (int, float)) or int(ix) != ix:
        raise RuntimeError('index must be int')
    ix = int(ix)
    if elem in _STRUCTS:
        cls = _struct_class(elem)
        if cls is None:
            raise RuntimeError('ptr[%s]: no layout for %s' % (elem, elem))
        size = ctypes.sizeof(cls)
        cell = ctypes.cast(base + ix * size, ctypes.POINTER(cls))
        return _read_struct(cell.contents, elem)
    if elem not in _PTR_WORD:
        raise RuntimeError('ptr[%s]: index a pointer needs a unit' % elem)
    code, width = _PTR_WORD[elem]
    cell = ctypes.cast(base + ix * width, ctypes.POINTER(_ctypes_of(code)))
    v = cell.contents.value
    return float(v) if code in ('d', 'f') else int(v)


def _ptr_put(elem, base, ix, v):
    """Write the element `p[i] = v` names. A table of records is read-only."""
    import ctypes
    if base <= 0:
        raise RuntimeError('null pointer dereference' if base == 0 else 'pointer is negative')
    if not isinstance(ix, (int, float)) or int(ix) != ix:
        raise RuntimeError('index must be int')
    ix = int(ix)
    if elem in _STRUCTS:
        raise RuntimeError('a table of C structs is read-only: '
                           'write the record with ptr_write_struct')
    if elem not in _PTR_WORD:
        raise RuntimeError('ptr[%s]: index a pointer needs a unit' % elem)
    code, width = _PTR_WORD[elem]
    if code in ('q', 'i', 'B'):
        if not isinstance(v, int) or isinstance(v, bool):
            raise RuntimeError('ptr[%s]: wants an integer, got other' % elem)
        lo, hi = {'q': (-2 ** 63, 2 ** 63 - 1), 'i': (-2 ** 31, 2 ** 31 - 1),
                  'B': (0, 255)}[code]
        if v < lo or v > hi:
            raise RuntimeError('ptr[%s]: value %r out of range' % (elem, v))
    elif not isinstance(v, (int, float)) or isinstance(v, bool):
        raise RuntimeError('ptr[%s]: wants a number, got other' % elem)
    cell = ctypes.cast(base + ix * width, ctypes.POINTER(_ctypes_of(code)))
    cell.contents.value = float(v) if code in ('d', 'f') else int(v)


def _ctypes_of(code):
    import ctypes
    return {'q': ctypes.c_int64, 'i': ctypes.c_int32, 'B': ctypes.c_ubyte,
            'd': ctypes.c_double, 'f': ctypes.c_float}[code]


def call_value(name, vals, env, funcs):
    info = funcs.get(name)
    if isinstance(info, tuple) and info and info[0] == 'extern':
        return _ffi_invoke(name, info, vals, funcs)
    if name in funcs:
        params, body = funcs[name]
        vals = fill_defaults(params, vals, env, funcs, name)
        vals = _coerce_params(params, vals)
        names = [p[0] for p in params]
        frame = {}
        for nm, val in zip(names, vals):
            if _MANAGED:
                vrc_inc(val)
            frame[nm] = val
        with _frame():
            env.append(frame)
            try:
                run_block(body, env, funcs, frame=True)
            except Propagate:
                _drop_all(frame)
                env.pop()
                raise
            except (Done, Ret) as r:
                _drop_all(frame)
                env.pop()
                return r.v
            except Exception:
                _drop_all(frame)
                env.pop()
                raise
            _drop_all(frame)
            env.pop()
            return None
    raise NameError(name)


_PLUGINS = {}
_PLUGIN_LIBS = {}
_PLUGIN_SYMBOL_OWNERS = {}
_PLUGIN_SEQ = [0]
_PLUGIN_LOCK = threading.RLock()


def _sig_parts(text):
    t = text.replace(' ', '')
    if '(' not in t or not t.endswith(')'):
        raise RuntimeError('call: bad signature `%s`' % t)
    ret, rest = t.split('(', 1)
    return ret, [p for p in rest[:-1].split(',') if p]


_CTYPES = {'i64': 'c_longlong', 'f64': 'c_double', 'str': 'c_char_p', 'ptr': 'c_void_p'}


def _plugin(name, vals):
    import ctypes

    if os.name == 'nt':
        raise RuntimeError('%s: not supported on Windows' % name)

    def next_id():
        if _PLUGIN_SEQ[0] >= (1 << 63) - 1:
            raise RuntimeError('plugin handle id space exhausted')
        _PLUGIN_SEQ[0] += 1
        return _PLUGIN_SEQ[0]

    if name == 'lib_open':
        if len(vals) != 1:
            raise TypeError('lib_open wants 1 arg')
        if not isinstance(vals[0], str):
            raise RuntimeError('lib_open: want str')
        try:
            cdll = ctypes.CDLL(vals[0], mode=ctypes.RTLD_LOCAL)
        except OSError as e:
            raise RuntimeError('lib_open: %s' % e)
        with _PLUGIN_LOCK:
            handle = next_id()
            _PLUGIN_LIBS[handle] = {
                'library': cdll,
                'active': 0,
                'closing': False,
            }
        return handle
    if name == 'lib_sym':
        if len(vals) != 2:
            raise TypeError('lib_sym wants 2 args')
        if type(vals[0]) is not int or vals[0] <= 0:
            raise RuntimeError('lib_sym: invalid library handle')
        if not isinstance(vals[1], str):
            raise RuntimeError('lib_sym: want str')
        with _PLUGIN_LOCK:
            entry = _PLUGIN_LIBS.get(vals[0])
            if entry is None or entry['closing']:
                raise RuntimeError('lib_sym: no such handle %s' % vals[0])
            try:
                fn = getattr(entry['library'], vals[1])
            except AttributeError:
                raise RuntimeError('lib_sym: no symbol %s' % vals[1])
            symbol = next_id()
            _PLUGINS[symbol] = fn
            _PLUGIN_SYMBOL_OWNERS[symbol] = vals[0]
        return symbol
    if name == 'lib_close':
        if len(vals) != 1:
            raise TypeError('lib_close wants 1 arg')
        if type(vals[0]) is not int or vals[0] <= 0:
            raise RuntimeError('lib_close: invalid library handle')
        with _PLUGIN_LOCK:
            entry = _PLUGIN_LIBS.get(vals[0])
            if entry is None or (entry['closing'] and entry['active']):
                raise RuntimeError('lib_close: no such handle %s' % vals[0])
            entry['closing'] = True
            for symbol, owner in list(_PLUGIN_SYMBOL_OWNERS.items()):
                if owner == vals[0]:
                    _PLUGIN_SYMBOL_OWNERS.pop(symbol, None)
                    _PLUGINS.pop(symbol, None)
            if not entry['active']:
                del _PLUGIN_LIBS[vals[0]]
        return 0
    if len(vals) < 2:
        raise TypeError('call wants a function, a signature and arguments')
    ret, params = _sig_parts(vals[1])
    for t in params + [ret]:
        if t not in _CTYPES and t != 'none':
            raise RuntimeError('call: unsupported type `%s`' % t)
    with _PLUGIN_LOCK:
        if type(vals[0]) is not int or vals[0] <= 0:
            raise RuntimeError('call: want function symbol')
        fn = _PLUGINS.get(vals[0])
        owner = _PLUGIN_SYMBOL_OWNERS.get(vals[0])
        entry = _PLUGIN_LIBS.get(owner)
        if fn is None or entry is None or entry['closing']:
            raise RuntimeError('call: no such function %s' % vals[0])
        if len(vals) - 2 != len(params):
            raise RuntimeError('call: signature wants %d argument(s), got %d'
                               % (len(params), len(vals) - 2))
        entry['active'] += 1
    try:
        fn.argtypes = [getattr(ctypes, _CTYPES[t]) for t in params]
        fn.restype = getattr(ctypes, _CTYPES[ret]) if ret != 'none' else None
        argv = []
        for t, v in zip(params, vals[2:]):
            argv.append(v.encode('utf-8') if t == 'str' and isinstance(v, str) else v)
        r = fn(*argv)
        if ret == 'none':
            return 0
        if ret == 'str':
            if r is None:
                raise RuntimeError('call: NULL str result')
            return r.decode('utf-8', 'replace') if isinstance(r, bytes) else r
        if ret == 'ptr':
            return r or 0
        return r
    finally:
        with _PLUGIN_LOCK:
            entry['active'] -= 1
            if entry['closing'] and not entry['active']:
                del _PLUGIN_LIBS[owner]


_AMP = [0]          # >0 while the arguments of an extern call are evaluated


def call(name, args, env, funcs):
    info = funcs.get(name)
    if isinstance(info, tuple) and info and info[0] == 'extern':
        _AMP[0] += 1
        try:
            vals = [ev(a, env, funcs) for a in args]
        finally:
            _AMP[0] -= 1
    else:
        vals = [ev(a, env, funcs) for a in args]
    if isinstance(info, tuple) and info and info[0] == 'extern':
        return _ffi_invoke(name, info, vals, funcs)
    if name == 'print':
        _out_line(' '.join(_bstr(v) for v in vals))
        return 0
    if name in ('log.info', 'log.warn', 'log.error'):
        emit_hit(name.split('.')[1].upper(), ' '.join(str(v) for v in vals))
        return 0
    if name == 'trace.span':
        if vals and isinstance(vals[-1], tuple) and vals[-1] and vals[-1][0] == 'closure':
            clo = vals[-1]
            vals = vals[:-1]
            print(json.dumps({'ts': time.time(), 'span': vals}, ensure_ascii=False))
            return run_closure(clo, [], env, funcs)
        print(json.dumps({'ts': time.time(), 'span': vals}, ensure_ascii=False))
        return 0
    if name in ('Ok', 'Err', 'Some'):
        if len(vals) != 1:
            raise TypeError(name + ' wants 1 arg')
        return (name.lower(), vals[0])
    if name == 'unwrap':
        if len(vals) != 1:
            raise TypeError('unwrap wants 1 arg')
        v = vals[0]
        if isinstance(v, tuple) and len(v) == 2 and v[0] in ('ok', 'some'):
            return v[1]
        if isinstance(v, tuple) and len(v) == 2 and v[0] == 'err':
            raise Propagate(v)
        raise RuntimeError('unwrap of ' + val_type(v) + ' (want Ok/Err/Some)')
    if name == 'is_err':
        v = vals[0] if len(vals) == 1 else None
        return isinstance(v, tuple) and len(v) == 2 and v[0] == 'err'
    if name == 'is_none':
        v = vals[0] if len(vals) == 1 else None
        return v is None
    if name == 'is_vec':
        v = vals[0] if len(vals) == 1 else None
        return isinstance(v, list)
    if name == 'is_str':
        v = vals[0] if len(vals) == 1 else None
        return isinstance(v, str)
    if name == 'is_map':
        v = vals[0] if len(vals) == 1 else None
        return isinstance(v, dict)
    if name == 'len':
        if len(vals) != 1:
            raise TypeError('len wants 1 arg')
        if isinstance(vals[0], (list, str)):
            return len(vals[0])
        if isinstance(vals[0], dict):
            return len(vals[0]) - (1 if '$type' in vals[0] else 0)
        raise RuntimeError('len of non-sized')
    if name == '__at':
        if len(vals) != 2:
            raise TypeError('__at wants 2 args')
        if not isinstance(vals[1], (int, float)):
            raise TypeError('__at index must be num')
        return vals[0][int(vals[1])]
    if name == 'tagof':
        v = vals[0] if vals else None
        if v is None:
            return 'none'
        if isinstance(v, tuple) and v and isinstance(v[0], str):
            return v[0]
        return 'plain'
    if name == 'assert':
        if len(vals) not in (1, 2):
            raise TypeError('assert wants 1-2 args')
        if not vals[0]:
            raise RuntimeError(vals[1] if len(vals) == 2 else 'assert failed')
        return 0
    if name == 'str':
        if len(vals) != 1:
            raise TypeError('str wants 1 arg')
        v = vals[0]
        if isinstance(v, str):
            return v
        if _too_deep(v):
            # `vsprint` walks the value recursively and stops at the same
            # budget; without this the interpreter answered a 2500-deep value
            # where the native side said `recursion too deep`.
            raise RuntimeError('recursion too deep')
        return _brepr(v)
    if name == 'read_file':
        if len(vals) != 1:
            raise TypeError('read_file wants 1 arg')
        if not isinstance(vals[0], str):
            return ('err', 'read_file wants a string path')
        try:
            path = confined_path(vals[0])
            if path is None:
                return ('err', 'path outside the program directory')
            with open(path, encoding='utf-8') as f:
                data = f.read()
            if '\x00' in data:
                return ('err', 'invalid utf-8')
            return ('ok', data)
        except UnicodeDecodeError:
            return ('err', 'invalid utf-8')
        except OSError as e:
            return ('err', str(e))
    if name == 'sbuf':
        if len(vals) != 0:
            raise TypeError('sbuf wants 0 args')
        return ['sb', '']
    if name == 'sbuf_add':
        if len(vals) != 2:
            raise TypeError('sbuf_add wants 2 args')
        h, part = vals
        if not (isinstance(h, list) and h and h[0] == 'sb'):
            raise TypeError('sbuf_add wants a builder')
        h[1] = h[1] + part
        return len(h[1])
    if name == 'sbuf_len':
        if len(vals) != 1:
            raise TypeError('sbuf_len wants 1 arg')
        h, = vals
        if not (isinstance(h, list) and h and h[0] == 'sb'):
            raise TypeError('sbuf_len wants a builder')
        return len(h[1])
    if name == 'sbuf_str':
        if len(vals) != 1:
            raise TypeError('sbuf_str wants 1 arg')
        h, = vals
        if not (isinstance(h, list) and h and h[0] == 'sb'):
            raise TypeError('sbuf_str wants a builder')
        return ('ok', h[1])
    if name == 'tcp_listen':
        if len(vals) != 1:
            raise TypeError('tcp_listen wants 1 arg')
        addr = _parse_addr(vals[0])
        if addr is None:
            return ('err', 'address wants host:port with port 1..65535')
        try:
            s_ = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        except OSError:
            return ('err', 'socket failed')
        s_.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s_.bind(addr)
        except OSError:
            s_.close()
            return ('err', 'bind failed')
        try:
            s_.listen(16)
        except OSError:
            s_.close()
            return ('err', 'listen failed')
        return ('ok', _fd_for(s_))
    if name == 'tcp_accept':
        if len(vals) != 1:
            raise TypeError('tcp_accept wants 1 arg')
        s_ = _sock_get(vals[0])
        if s_ is None:
            return ('err', 'bad socket slot')
        try:
            conn, _ = s_.accept()
        except OSError:
            return ('err', 'accept failed')
        conn.settimeout(_SOCK_TIMEOUT)
        return ('ok', _fd_for(conn))
    if name == 'tcp_connect':
        if len(vals) != 1:
            raise TypeError('tcp_connect wants 1 arg')
        addr = _parse_addr(vals[0])
        if addr is None:
            return ('err', 'address wants host:port with port 1..65535')
        try:
            s_ = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        except OSError:
            return ('err', 'socket failed')
        s_.settimeout(_SOCK_TIMEOUT)
        try:
            s_.connect(addr)
        except OSError:
            s_.close()
            return ('err', 'connect failed')
        return ('ok', _fd_for(s_))
    if name == 'tcp_recv':
        if len(vals) != 2:
            raise TypeError('tcp_recv wants 2 args')
        n = vals[1]
        if not isinstance(n, int) or not 0 <= n <= _RECV_MAX:
            return ('err', 'recv wants 0 <= n <= 1048576')
        s_ = _sock_get(vals[0])
        if s_ is None:
            return ('err', 'bad socket slot')
        try:
            b = s_.recv(n)
        except OSError:
            return ('err', 'recv failed')
        try:
            s = b.decode('utf-8')
            if '\x00' in s:
                return ('err', 'invalid utf-8')
            return ('ok', s)
        except UnicodeDecodeError:
            return ('err', 'invalid utf-8')
    if name == 'tcp_send':
        if len(vals) != 2:
            raise TypeError('tcp_send wants 2 args')
        data = vals[1]
        if not isinstance(data, str):
            return ('err', 'tcp_send wants string data')
        s_ = _sock_get(vals[0])
        if s_ is None:
            return ('err', 'bad socket slot')
        payload = data.encode('utf-8')
        try:
            s_.sendall(payload)
        except OSError:
            return ('err', 'send failed')
        return ('ok', len(payload))
    if name == 'tcp_close':
        if len(vals) != 1:
            raise TypeError('tcp_close wants 1 arg')
        s_ = _sock_take(vals[0])
        if s_ is None:
            return ('err', 'bad socket slot')
        s_.close()
        return ('ok', 0)
    if name == 'write_file':
        if len(vals) != 2:
            raise TypeError('write_file wants 2 args')
        if not isinstance(vals[0], str) or not isinstance(vals[1], str):
            return ('err', 'write_file wants string path and text')
        try:
            path = confined_path(vals[0])
            if path is None:
                return ('err', 'path outside the program directory')
            with open(path, 'w', encoding='utf-8') as f:
                f.write(vals[1])
            return ('ok', len(vals[1]))
        except OSError as e:
            return ('err', str(e))
    if name in funcs and isinstance(funcs[name], tuple) and funcs[name] and funcs[name][0] == 'ctor':
        _, tag, arity = funcs[name]
        if len(vals) != arity:
            raise TypeError(name + ' wants %d args' % arity)
        if tag == 'none':
            return None
        return (tag,) + tuple(vals)
    if '.' in name:
        parts = name.split('.')
        obj = find(env, parts[0])
        if obj is _MISS:
            if len(parts) == 2 and parts[0][:1].isupper():
                f0 = funcs.get(parts[0])
                if (isinstance(f0, tuple) and f0 and f0[0] == 'ctor' and f0[2] == 0):
                    tag = f0[1]
                    obj = None if tag == 'none' else (tag,)
                else:
                    fname = parts[0] + '.' + parts[1]
                    fn = funcs.get(fname)
                    if isinstance(fn, tuple) and len(fn) == 2:
                        params, body = fn
                        names = [p[0] for p in params]
                        if len(vals) != len(names):
                            raise TypeError(fname + ' wants %d args' % len(names))
                        if names and names[0] == 'self':
                            raise TypeError(fname + ' needs instance, use value.method()')
                        frame = {}
                        for nm, val in zip(names, vals):
                            if _MANAGED:
                                vrc_inc(val)
                            frame[nm] = val
                        with _frame():
                            env.append(frame)
                            try:
                                run_block(body, env, funcs, frame=True)
                            except Propagate:
                                _drop_all(frame)
                                env.pop()
                                raise
                            except (Done, Ret) as r:
                                _drop_all(frame)
                                env.pop()
                                return r.v
                            _drop_all(frame)
                            env.pop()
                            return 0
                    raise NameError('no var ' + parts[0])
            else:
                raise NameError('no var ' + parts[0])
        for f in parts[1:-1]:
            if not isinstance(obj, dict) or f not in obj:
                raise RuntimeError('no field ' + f)
            obj = obj[f]
        meth = parts[-1]
        if meth in ('map', 'filter') and len(args) == 1:
            return apply_iter(obj, meth, args[0], env, funcs)
        fname = method_of(obj, meth, funcs)
        if fname is not None:
            return call_method_fn(fname, obj, vals, env, funcs)
        return do_method(obj, meth, vals)
    v = find(env, name)
    if isinstance(v, tuple) and v and v[0] == 'closure':
        return run_closure(v, vals, env, funcs)
    if v is not _MISS and name not in funcs:
        # A binding that is not a closure is not callable: `let g = 5; g(1)`.
        # The C backend answers `('err', 'not callable')` for it, which a
        # `try`/`catch` can catch; here the same program fell through to the
        # builtin table and left a Python `NameError: no fn g` that no BRAT
        # handler could see.
        raise RuntimeError('not callable')
    if name in funcs:
        params, body = funcs[name]
        vals = fill_defaults(params, vals, env, funcs, name)
        vals = _coerce_params(params, vals)
        names = [p[0] for p in params]
        frame = {}
        for nm, val in zip(names, vals):
            if _MANAGED:
                vrc_inc(val)
            frame[nm] = val
        _note_ptr_params(frame, params)
        with _frame():
            env.append(frame)
            try:
                run_block(body, env, funcs, frame=True)
            except Propagate:
                _drop_all(frame)
                _forget_ptr_frame(frame)
                env.pop()
                raise
            except (Done, Ret) as r:
                _drop_all(frame)
                _forget_ptr_frame(frame)
                env.pop()
                return r.v
            _drop_all(frame)
            _forget_ptr_frame(frame)
            env.pop()
            return 0
    if name == 'chan':
        if len(vals) != 1 or not isinstance(vals[0], int) or isinstance(vals[0], bool):
            raise TypeError('chan wants 1 int arg')
        if vals[0] < 1:
            return ('err', 'chan: capacity must be at least 1')
        if vals[0] > 1048576:
            # The same bound as crt.h: the C queue is preallocated, so an
            # enormous capacity was an allocation failure there and a working
            # channel here.
            return ('err', 'chan: capacity too large')
        return _Chan(vals[0])
    if name == 'join_all':
        if vals:
            raise TypeError('join_all wants 0 args')
        return join_all()
    if name == 'args':
        return list(_ARGS[0])
    if name == 'exit':
        code = vals[0] if vals else 0
        if not isinstance(code, (int, float)):
            raise TypeError('exit wants int')
        sys.exit(int(code))
    if name == 'eprint':
        print(' '.join(_bstr(v) for v in vals), file=sys.stderr)
        return 0
    if name == 'raise':
        if len(vals) != 1:
            raise TypeError('raise wants 1 arg')
        raise Done(('err', vals[0]))
    if name == 'own':
        return copy.deepcopy(vals[0])
    if name in ('__arena_enter', '__arena_leave'):
        # The interpreter has no arena memory; the blocks only need to run.
        return 0
    if name == 'time_ms':
        return int(time.time() * 1000)
    if name == 'sleep_ms':
        if len(vals) != 1 or not isinstance(vals[0], (int, float)):
            raise TypeError('sleep_ms wants 1 int arg')
        time.sleep(max(0, vals[0]) / 1000.0)
        return 0
    if name in ('lib_open', 'lib_sym', 'lib_close', 'call'):
        return _plugin(name, vals)
    if name == 'slice_of_struct':
        if len(vals) != 3:
            raise TypeError('slice_of_struct wants 3 args')
        p0, n0, sname = vals
        if not isinstance(sname, str) or sname not in _STRUCTS:
            raise RuntimeError('slice_of_struct: %s is not an extern struct' % (sname,))
        if n0 < 0:
            raise RuntimeError('slice_of_struct: n must be >= 0')
        if p0 <= 0 and n0 > 0:
            raise RuntimeError('null pointer dereference')
        import ctypes
        sl = (p0, 0, n0, 6, ctypes.sizeof(_struct_class(sname)))
        _SLICE_STRUCTS[id(sl)] = sname
        return sl
    if name == 'slice_struct_of':
        if len(vals) != 1 or not isinstance(vals[0], list):
            raise TypeError('slice_struct_of wants a vector literal')
        elems = list(vals[0])
        # Slot 5 is the storage itself: a window over built elements indexes
        # the elements, and a store lands where the program can see it.
        return (elems, 0, len(elems), 7, 0, elems)
    if name in ('slice_of', 'slice_of_ptr'):
        want = 3
        if len(vals) != want:
            raise TypeError('%s wants %d args' % (name, want))
        if name == 'slice_of':
            back, a, b = vals
            if not isinstance(back, list):
                raise RuntimeError('slice_of wants a vec')
            if a < 0 or b < a or b > len(back):
                raise RuntimeError('slice range %d..%d out of range 0..%d' % (a, b, len(back)))
            return [back, a, b - a, 0]
        p, n, kind = vals
        if kind not in ('i64', 'i32', 'f64', 'f32', 'u8'):
            raise RuntimeError('slice_of_ptr: kind must be i64, i32, f64, f32 or u8')
        if n < 0:
            raise RuntimeError('slice_of_ptr: n must be >= 0')
        if p == 0 and n > 0:
            raise RuntimeError('null pointer dereference')
        return [p, 0, n, {'i64': 1, 'i32': 4, 'f64': 2, 'f32': 5, 'u8': 3}[kind]]
    if name == 'slice_len':
        if len(vals) != 1:
            raise TypeError('slice_len wants 1 slice')
        return vals[0][2]
    if name in ('slice_get', 'slice_set'):
        want = 2 if name == 'slice_get' else 3
        if len(vals) != want:
            raise TypeError('%s wants %d args' % (name, want))
        back, start, n, kind = vals[0][:4] if isinstance(vals[0], tuple) else vals[0]
        i = vals[1]
        if i < 0 or i >= n:
            raise RuntimeError('slice index %d out of range 0..%d' % (i, n))
        if kind == 7:
            # A window over elements built in the program: the elements are the
            # storage, so a store lands where the program can see it.
            elems = vals[0][5]
            if name == 'slice_get':
                return elems[start + i]
            elems[start + i] = vals[2]
            return 0
        if kind == 6:
            # A window over a table of C records: an element is the address of
            # the record, read the way `ptr_read_struct` reads one.
            import ctypes
            p0 = vals[0][0]
            esize = vals[0][4]
            if p0 <= 0:
                raise RuntimeError('null pointer dereference' if p0 == 0 else 'pointer is negative')
            if name == 'slice_set':
                raise RuntimeError('slice_set: a window of C structs is read-only')
            cls = _struct_class(_SLICE_STRUCTS[id(vals[0])])
            cell = ctypes.cast(p0 + (start + i) * esize, ctypes.POINTER(cls))
            return _read_struct(cell.contents, _SLICE_STRUCTS[id(vals[0])])
        if kind == 0:
            if name == 'slice_get':
                return back[start + i]
            back[start + i] = vals[2]
            return 0
        import ctypes
        # The element width is the kind's: a byte window steps by one byte, a
        # 32-bit block by four. Treating every kind but `u8` as eight bytes is
        # what made an i32 register read the neighbouring one.
        _WIDTH = {1: 8, 2: 8, 3: 1, 4: 4, 5: 4}
        if kind == 3:
            cell = ctypes.cast(back + start + i, ctypes.POINTER(ctypes.c_ubyte))
            if name == 'slice_get':
                return cell.contents.value
            v = vals[2]
            if v < 0 or v > 255:
                raise RuntimeError('slice_set: byte %d out of range 0..255' % v)
            cell.contents.value = v
            return 0
        width = {1: ctypes.c_int64, 2: ctypes.c_double,
                 4: ctypes.c_int32, 5: ctypes.c_float}[kind]
        cell = ctypes.cast(back + (start + i) * _WIDTH[kind], ctypes.POINTER(width))
        if name == 'slice_get':
            v = cell.contents.value
            return float(v) if kind == 5 else v
        v = vals[2]
        if kind == 4:
            if not isinstance(v, int) or isinstance(v, bool) or v < -2**31 or v > 2**31 - 1:
                raise RuntimeError('slice_set: i32 value %r out of range' % (v,))
        elif kind == 5:
            if not isinstance(v, (int, float)) or isinstance(v, bool):
                raise RuntimeError('slice_set: want f32, got other')
            v = float(v)
        cell.contents.value = v
        return 0
    if name == 'str_from_ptr':
        if len(vals) != 2:
            raise TypeError('str_from_ptr wants 2 args')
        p, n = vals
        if n < 0:
            raise RuntimeError('str_from_ptr: n must be >= 0')
        if p == 0:
            if n == 0:
                return ''
            raise RuntimeError('null pointer dereference')
        import ctypes
        data = ctypes.string_at(p, n)
        nul = data.find(b'\x00')
        if nul >= 0:
            raise RuntimeError('str_from_ptr: NUL byte at %d' % nul)
        try:
            return data.decode('utf-8')
        except UnicodeDecodeError:
            raise RuntimeError('str_from_ptr: invalid UTF-8')
    if name == 'ptr_null':
        return 0
    if name == 'ptr_is_null':
        if len(vals) != 1:
            raise TypeError('ptr_is_null wants 1 ptr arg')
        return vals[0] == 0
    if name == 'ptr_offset':
        if len(vals) != 2:
            raise TypeError('ptr_offset wants 2 args')
        return vals[0] + vals[1]
    if name == 'ptr_read_i64':
        if len(vals) != 1:
            raise TypeError('ptr_read_i64 wants 1 ptr arg')
        import ctypes
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        return ctypes.cast(vals[0], ctypes.POINTER(ctypes.c_longlong)).contents.value
    if name == 'ptr_write_i64':
        if len(vals) != 2:
            raise TypeError('ptr_write_i64 wants 2 args')
        import ctypes
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        ctypes.cast(vals[0], ctypes.POINTER(ctypes.c_longlong)).contents.value = vals[1]
        return 0
    if name in ('ptr_read_struct', 'ptr_write_struct'):
        import ctypes
        want = 3 if name == 'ptr_write_struct' else 2
        if len(vals) != want:
            raise TypeError('%s wants %d args' % (name, want))
        tname = vals[-1]
        if not isinstance(tname, str) or tname not in _STRUCTS:
            raise RuntimeError('%s: %s is not an extern struct' % (name, tname))
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        cls = _struct_class(tname)
        ptr = ctypes.cast(vals[0], ctypes.POINTER(cls))
        if name == 'ptr_write_struct':
            def fill(inst, tname_, v):
                if not isinstance(v, dict):
                    raise RuntimeError('ptr_write_struct: %s wants a struct value' % tname_)
                for f, ft in _STRUCTS[tname_]:
                    if f not in v:
                        raise RuntimeError('ptr_write_struct: %s has no field %s' % (tname_, f))
                    if ft in _STRUCTS:
                        # A nested struct is a ctypes object inside another one:
                        # assign into it rather than replacing it.
                        fill(getattr(inst, f), ft, v[f])
                        continue
                    if ft == 'str':
                        if not isinstance(v[f], str):
                            raise RuntimeError('ptr_write_struct: %s.%s wants str' % (tname_, f))
                        setattr(inst, f, v[f].encode('utf-8'))
                    elif ft in ('f32', 'f64'):
                        setattr(inst, f, float(v[f]))
                    elif ft == 'ptr':
                        setattr(inst, f, int(v[f]))
                    else:
                        setattr(inst, f, int(v[f]))

            fill(ptr.contents, tname, vals[1])
            return 0
        return _read_struct(ptr.contents, tname)
    if name == 'ptr_read_f32':
        if len(vals) != 1:
            raise TypeError('ptr_read_f32 wants 1 ptr arg')
        import ctypes
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        return float(ctypes.cast(vals[0], ctypes.POINTER(ctypes.c_float)).contents.value)
    if name == 'ptr_write_f32':
        if len(vals) != 2:
            raise TypeError('ptr_write_f32 wants 2 args')
        if not isinstance(vals[1], (int, float)) or isinstance(vals[1], bool):
            raise TypeError('ptr_write_f32 wants a number')
        import ctypes
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        ctypes.cast(vals[0], ctypes.POINTER(ctypes.c_float)).contents.value = float(vals[1])
        return 0
    if name == 'ptr_read_f64':
        if len(vals) != 1:
            raise TypeError('ptr_read_f64 wants 1 ptr arg')
        import ctypes
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        return ctypes.cast(vals[0], ctypes.POINTER(ctypes.c_double)).contents.value
    if name == 'ptr_write_f64':
        if len(vals) != 2:
            raise TypeError('ptr_write_f64 wants 2 args')
        import ctypes
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        ctypes.cast(vals[0], ctypes.POINTER(ctypes.c_double)).contents.value = vals[1]
        return 0
    if name == 'ptr_read_ptr':
        if len(vals) != 1:
            raise TypeError('ptr_read_ptr wants 1 ptr arg')
        import ctypes
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        return ctypes.cast(vals[0], ctypes.POINTER(ctypes.c_void_p)).contents.value or 0
    if name == 'ptr_read_str':
        if len(vals) != 1:
            raise TypeError('ptr_read_str wants 1 ptr arg')
        import ctypes
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        return ctypes.cast(vals[0], ctypes.c_char_p).value.decode('utf-8')
    if name == 'ptr_of_str':
        if len(vals) != 1:
            raise TypeError('ptr_of_str wants 1 arg')
        _ct = __import__('ctypes')
        b = vals[0].encode('utf-8') if isinstance(vals[0], str) else vals[0]
        buf = _ct.create_string_buffer(b)
        _STR_PTRS.append(buf)
        return _ct.cast(buf, _ct.c_void_p).value or 0
    if name == 'ptr_write_str':
        if len(vals) != 2:
            raise TypeError('ptr_write_str wants 2 args')
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        _ct = __import__('ctypes')
        b = vals[1].encode('utf-8') if isinstance(vals[1], str) else vals[1]
        buf = _ct.create_string_buffer(b)
        _STR_PTRS.append(buf)
        _ct.cast(vals[0], _ct.POINTER(_ct.c_void_p)).contents.value = _ct.cast(buf, _ct.c_void_p).value
        return 0
    if name == 'ptr_write_ptr':
        if len(vals) != 2:
            raise TypeError('ptr_write_ptr wants 2 args')
        import ctypes
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        ctypes.cast(vals[0], ctypes.POINTER(ctypes.c_void_p)).contents.value = vals[1]
        return 0
    if name == 'errno':
        return __import__('ctypes').get_errno()
    if name == 'strerror':
        if len(vals) != 1:
            raise TypeError('strerror wants 1 arg')
        return os.strerror(int(vals[0]))
    if name == 'env_get':
        if len(vals) != 1:
            raise TypeError('env_get wants 1 arg')
        key = str(vals[0])
        allow = os.environ.get('BRAT_ENV_ALLOW', '')
        allowed = {x.strip() for x in allow.split(',') if x.strip()} if allow else None
        if allowed is not None and key not in allowed:
            return ('err', 'env_get refused: %r' % key)
        upper = key.upper()
        for bad in ("SECRET", "KEY", "TOKEN", "PASS", "PASSWORD", "CREDENTIAL",
                    "PRIVATE", "DATABASE", "DB_", "AUTH", "DSN", "CONN", "COOKIE",
                    "SESSION", "BEARER", "REDIS", "MONGO", "POSTGRES", "MYSQL",
                    "AMQP", "AWS_", "GCP_", "AZURE_", "GITHUB_", "OPENAI_",
                    "ANTHROPIC_", "STRIPE_", "WEBHOOK", "SIGNING", "CERT",
                    "SALT", "SEED"):
            if bad in upper:
                return ('err', 'env_get refused: %r' % key)
        val = os.environ.get(key)
        if val is not None and (('://' in val and '@' in val) or '-----BEGIN' in val):
            return ('err', 'env_get refused: %r' % key)
        return val
    if name == 're_match':
        if len(vals) != 2:
            raise TypeError('re_match wants 2 args')
        if not isinstance(vals[0], str) or not isinstance(vals[1], str):
            raise RuntimeError('re_match wants str')
        return re_match(vals[0], vals[1])
    raise NameError('no fn ' + name)

def run_block(body, env, funcs, frame=False):
    st = _dfr_stack()
    st.append(_DfrScope(frame))
    try:
        for s in body:
            run_stmt(s, env, funcs)
    finally:
        run_defers(env, funcs)

_MOD_BASE = ['.']

# `BRAT_FILES=1` is the runner's own opt-out, never the program's: `0`, `no` or
# a typo must leave the sandbox on. Mirrors `_vfiles_free` in crt.h.
_FILES_FREE = os.environ.get('BRAT_FILES', '').strip().lower() in ('1', 'yes', 'true', 'on')


def confined_path(p):
    # Non-str resolves to None: a raw int would otherwise reach open() as an fd.
    if not isinstance(p, str):
        return None
    if _FILES_FREE:
        return p
    base = os.path.realpath(_MOD_BASE[0])
    if not base or base == os.sep:
        return None  # refuse to run with an empty or filesystem-root base
    full = os.path.realpath(os.path.join(base, p))
    if full == base or full.startswith(base + os.sep):
        return full
    return None

_ARGS = [[]]

_SOCKETS = {}
_NEXT_FD = [1]
_SOCK_LOCK = threading.Lock()
_SOCK_TIMEOUT = 30.0      # connected sockets: a silent peer frees its thread
_RECV_MAX = 1 << 20       # one tcp_recv returns at most 1 MiB; mirrors crt.h


def _parse_addr(text):
    """'host:port' -> (host, port), or None. Mirrors crt.h _vhost_port."""
    if not isinstance(text, str):
        return None
    host, sep, port = text.rpartition(':')
    if not sep or not host or len(host.encode('utf-8')) > 255:
        return None
    if not (port.isascii() and port.isdigit()) or not 1 <= int(port) <= 65535:
        return None
    try:
        packed = socket.inet_aton(host)
    except OSError:
        return None
    if packed == b'\xff\xff\xff\xff':
        return None
    return socket.inet_ntoa(packed), int(port)


def _fd_for(sock):
    with _SOCK_LOCK:
        fd = _NEXT_FD[0]
        _NEXT_FD[0] += 1
        _SOCKETS[fd] = sock
    return fd


def _sock_get(fd):
    if not isinstance(fd, int):
        return None
    with _SOCK_LOCK:
        return _SOCKETS.get(fd)


def _sock_take(fd):
    if not isinstance(fd, int):
        return None
    with _SOCK_LOCK:
        return _SOCKETS.pop(fd, None)

_POOL = {'cond': threading.Condition(), 'q': [], 'active': 0, 'parked': 0, 'done': []}
_SPAWN_WORKERS = None
_CURRENT = threading.local()


def _spawn_worker():
    while True:
        with _POOL['cond']:
            while not _POOL['q']:
                _POOL['cond'].wait()
            task = _POOL['q'].pop(0)
        _run_task(task)


class _Chan:
    """A bounded queue where a value has one holder at a time.

    `send` takes the value over and `recv` hands it back, so a channel is the
    runtime half of the checker's rule that sending a value moves it.
    """
    __slots__ = ('cap', 'buf', 'closed', 'cond')

    def __init__(self, cap):
        self.cap = cap
        self.buf = []
        self.closed = False
        self.cond = threading.Condition()

    def send(self, v):
        with self.cond:
            while len(self.buf) >= self.cap and not self.closed:
                self.cond.wait()
            if self.closed:
                return ('err', 'send on closed channel')
            self.buf.append(v)
            self.cond.notify_all()
        return None

    def recv(self):
        with self.cond:
            while not self.buf and not self.closed:
                self.cond.wait()
            if not self.buf:
                return None
            v = self.buf.pop(0)
            self.cond.notify_all()
            return v

    def close(self):
        with self.cond:
            self.closed = True
            self.cond.notify_all()
        return None


class _Spawn:
    __slots__ = ('err', 'done')

    def __init__(self):
        self.err = None
        self.done = threading.Event()


def _run_task(task):
    block, snap, funcs, handle, scope = task
    outer = getattr(_CURRENT, 'h', None)
    outer_scope = getattr(_CURRENT, 'scope', None)
    _CURRENT.h = handle
    _CURRENT.scope = scope
    try:
        run_block(block, snap, funcs)
    except Exception as ex:
        handle.err = ex
    finally:
        _CURRENT.h = outer
        _CURRENT.scope = outer_scope
        with _POOL['cond']:
            _POOL['active'] -= 1
            _POOL['done'].append(handle)
            _POOL['cond'].notify_all()
        handle.done.set()


def _ensure_workers():
    global _SPAWN_WORKERS
    if _SPAWN_WORKERS is None:
        n = min(2 * (os.cpu_count() or 1), 32)
        _SPAWN_WORKERS = []
        for _ in range(n):
            t = threading.Thread(target=_spawn_worker, daemon=True)
            t.start()
            _SPAWN_WORKERS.append(t)


def _spawn(block, snap, funcs):
    handle = _Spawn()
    _ensure_workers()
    scope = getattr(_CURRENT, 'scope', None)
    if scope is not None:
        scope.append(handle)
    with _POOL['cond']:
        _POOL['q'].append((block, snap, funcs, handle, scope))
        _POOL['active'] += 1
        _POOL['cond'].notify()
    return handle


def _scope_join(handles):
    """Wait for every task this scope started and re-raise its first error.

    The list can grow while we wait: a spawned block that spawns again runs in
    a thread that inherited the same scope.
    """
    err = None
    i = 0
    while i < len(handles):
        h = handles[i]
        i += 1
        h.done.wait()
        if err is None and h.err is not None:
            err = h.err
    if err is not None:
        raise err


def join_all():
    # Wait until every task but the parked join_all callers is done, running
    # queued work here: a bounded pool that only blocked would starve a nested
    # join_all's own children. Mirrors crt.h vjoin_all.
    in_task = getattr(_CURRENT, 'h', None) is not None
    with _POOL['cond']:
        if in_task:
            _POOL['parked'] += 1
    try:
        while True:
            with _POOL['cond']:
                target = _POOL['parked'] if in_task else 0
                if not _POOL['q'] and _POOL['active'] <= target:
                    break
                task = _POOL['q'].pop(0) if _POOL['q'] else None
                if task is None:
                    _POOL['cond'].wait()
            if task is not None:
                _run_task(task)
        with _POOL['cond']:
            done = _POOL['done']
            _POOL['done'] = []
    finally:
        with _POOL['cond']:
            if in_task:
                _POOL['parked'] -= 1
    err = None
    for h in done:
        if err is None and h.err is not None:
            err = h.err
    if err is not None:
        raise err
    return 0


_TRAITS = {}
_VARIANTS = {}
_STRUCTS = {}
# Declared return type per function (`fn f() -> f64`). Recorded for the readers
# of this file and for `docs/project/LEDGER.md`: the interpreter keeps its values
# untyped, so `fn f() -> f64 { return 3 }` prints `3` while the C backend, whose
# fast-path signature is `double`, prints `3.0`. Coercing here would break the
# other half of the same rule -- `fn f() -> f64 { return 1 / 2 }` stays integer
# division in the emitted C, because that subexpression is not fast -- so the
# honest place for this is the LEDGER, not a one-sided patch in the interpreter.
_RET = {}
_STRUCT_CACHE = {}
_CALLBACK_REFS = []


# The width table for a C record, in the same units as the emitter's `_FFI_C`.
# `i8`/`i16` were missing, so a struct field declared at one or two bytes fell
# through to `c_longlong`: the ctypes record was laid out eight bytes per field,
# and every field after the first was read from the wrong offset. The native
# backend was right, so the two implementations disagreed on a value -- the
# failure this file exists to prevent.
_STRUCT_CTYPES = {
    'i8': 'c_int8', 'u8': 'c_ubyte', 'i16': 'c_int16', 'u16': 'c_ushort',
    'i32': 'c_int', 'u32': 'c_uint', 'i64': 'c_longlong', 'u64': 'c_ulonglong',
    'usize': 'c_size_t', 'f32': 'c_float', 'f64': 'c_double',
    'str': 'c_char_p', 'ptr': 'c_void_p',
}


def _struct_ctype(ty):
    if isinstance(ty, str) and ty in _STRUCT_CTYPES:
        return getattr(ctypes, _STRUCT_CTYPES[ty])
    if isinstance(ty, str) and ty.startswith('ptr['):
        return ctypes.c_void_p      # a typed pointer is still one word wide
    if ty in _STRUCTS:
        return _struct_class(ty)
    return ctypes.c_longlong


def _read_struct(inst, tname_):
    """One level of a C struct as a BRAT struct value, nested included."""
    got = {'$type': tname_}
    for f, ft in _STRUCTS[tname_]:
        v = getattr(inst, f)
        if ft in _STRUCTS:
            got[f] = _read_struct(v, ft)
        elif isinstance(v, bytes):
            got[f] = v.decode('utf-8')
        elif hasattr(v, 'value'):
            got[f] = v.value
        else:
            got[f] = v
    return got


def _struct_class(name):
    if name in _STRUCT_CACHE:
        return _STRUCT_CACHE[name]
    import ctypes
    fields = _STRUCTS[name]
    attrs = {'_fields_': [(f, _struct_ctype(t)) for f, t in fields]}
    if name in _PACKED:
        # `extern packed struct`: ctypes pads by default, which would put the
        # second field at the wrong byte and read the neighbouring one.
        attrs['_pack_'] = 1
    cls = type(name, (ctypes.Structure,), attrs)
    _STRUCT_CACHE[name] = cls
    return cls


def _parse_cb_sig(sig):
    s = sig.strip()
    i = s.index('(')
    j = s.index(')')
    params_str = s[i + 1:j].strip()
    rest = s[j + 1:].strip()
    if rest.startswith('->'):
        ret = rest[2:].strip()
    else:
        ret = 'none'
    if params_str == '':
        params = []
    else:
        params = [p.strip() for p in params_str.split(',')]
    return params, ret


_CB_SCALARS = {'i8': 'c_byte', 'u8': 'c_ubyte', 'i16': 'c_short',
               'u16': 'c_ushort', 'i32': 'c_int', 'u32': 'c_uint',
               'i64': 'c_longlong', 'u64': 'c_ulonglong', 'usize': 'c_size_t',
               'f32': 'c_float', 'f64': 'c_double', 'str': 'c_char_p',
               'ptr': 'c_void_p'}


def _cb_ctype(ty):
    """The ctypes type a callback slot really has.

    A width the table does not know used to fall back to `c_longlong`, which is
    an ABI lie of exactly the kind a callback pays for at the call: `fn(f32)`
    declared as taking a long long passes the float in the wrong register class.
    """
    import ctypes
    if isinstance(ty, str) and ty.startswith('ptr'):
        return ctypes.c_void_p
    if ty in _CB_SCALARS:
        return getattr(ctypes, _CB_SCALARS[ty])
    if ty in _STRUCTS:
        return _struct_class(ty)
    return ctypes.c_longlong


def _filled_struct(name, v):
    """A ctypes struct instance for a BRAT struct value.

    This is what lets a callback hand a record back to C: the interpreter used to
    declare a struct-returning callback as returning nothing, so the C side read
    whatever was in the return register.
    """
    import ctypes  # noqa: F401 - the field descriptors need it
    inst = _struct_class(name)()
    for f, ft in _STRUCTS[name]:
        fv = None
        if isinstance(v, dict):
            fv = v.get(f)
        if fv is None:
            continue
        if ft in _STRUCTS:
            fv = _filled_struct(ft, fv)
        elif ft == 'str' and isinstance(fv, str):
            fv = fv.encode('utf-8')
        elif ft == 'ptr' and isinstance(fv, int):
            fv = ctypes.c_void_p(fv)
        elif hasattr(fv, 'value'):
            fv = fv.value
        setattr(inst, f, fv)
    return inst


def _cb_run(fname, brat_args, funcs):
    """Call the BRAT function behind a callback, dying the way C expects.

    A raise inside a callback cannot unwind through the C frames between it and
    the caller, so it stops the process, exactly like the native wrapper's
    vcb_panic: same words, same exit code, no garbage result.
    """
    try:
        r = call(fname, brat_args, [{}], funcs)
    except Done as d:  # noqa: B902 - BRAT's own raise, see below
        sys.stderr.write('unhandled: ' + str(d.v) + '\n')
        sys.stderr.flush()
        os._exit(1)
    if isinstance(r, tuple) and len(r) == 2 and r[0] == 'err':
        sys.stderr.write('unhandled: ' + str(r) + '\n')
        sys.stderr.flush()
        os._exit(1)
    return r


# ctypes refuses a struct as a *callback result* outright ("invalid result type
# for callback function"): it has no libffi path for it. But a callback is just an
# address, and C can be told anything -- so when a callback returns a record by
# value, the interpreter compiles a three-line C trampoline: the shim keeps the
# native prototype (so the record really comes back in the registers or through
# the hidden pointer the ABI chose) and hands Python only `void *`. One source of
# truth for the layout: the struct is emitted with the *same* ctypes widths and
# the shim `_Static_assert`s size and every offset, so a disagreement is a
# compile error here, never a wrong value at run time.
_CTYPE_C = {'c_int8': 'int8_t', 'c_ubyte': 'uint8_t', 'c_int16': 'int16_t',
            'c_ushort': 'uint16_t', 'c_int': 'int32_t', 'c_uint': 'uint32_t',
            'c_longlong': 'int64_t', 'c_ulonglong': 'uint64_t',
            'c_size_t': 'size_t', 'c_float': 'float', 'c_double': 'double',
            'c_char_p': 'char *', 'c_void_p': 'void *'}

_CB_SHIMS = {}


def _cb_c_field(ty):
    if isinstance(ty, str) and ty in _STRUCTS:
        return '__brat_s_' + ty
    if ty in ('str', 'ptr'):
        return _CTYPE_C[_STRUCT_CTYPES[ty]]
    if isinstance(ty, str) and ty.startswith('ptr['):
        return 'void *'
    if ty in _STRUCT_CTYPES:
        return _CTYPE_C[_STRUCT_CTYPES[ty]]
    return 'void *'


def _cb_c_slot(ty):
    """The C spelling of one callback parameter, for the shim's prototype."""
    if isinstance(ty, str) and ty in _STRUCTS:
        return '__brat_s_' + ty
    if ty in _CB_SCALARS:
        return _CTYPE_C[_STRUCT_CTYPES.get(ty, 'c_void_p')]
    if ty in _STRUCTS:
        return '__brat_s_' + ty
    return 'void *'


def _cb_shim_src(ptypes, slots, ret, nbytes):
    import ctypes
    out = ['#include <stdint.h>', '#include <stddef.h>', '#include <string.h>', '']
    seen = []

    def emit(name):
        if name in seen:
            return
        for _f, _ft in _STRUCTS[name]:
            if isinstance(_ft, str) and _ft in _STRUCTS:
                emit(_ft)       # a record holding a record must come second
        seen.append(name)
        pack = ' __attribute__((packed))' if name in _PACKED else ''
        out.append('typedef struct%s {' % pack)
        for f, ft in _STRUCTS[name]:
            out.append('    %s %s;' % (_cb_c_field(ft), f))
        out.append('} __brat_s_%s;' % name)
        out.append('_Static_assert(sizeof(__brat_s_%s) == %d, '
                   '"BRAT callback shim: `%s` size");' % (name, ctypes.sizeof(_struct_class(name)), name))
        for f, ft in _STRUCTS[name]:
            inst = _struct_class(name)
            off = getattr(inst, f).offset
            out.append('_Static_assert(offsetof(__brat_s_%s, %s) == %d, '
                       '"BRAT callback shim: `%s.%s` offset");' % (name, f, off, name, f))
        out.append('')

    emit(ret)
    out.append('static void (*__brat_fill)(unsigned char *, const unsigned char *);')
    out.append('void __brat_cb_bind(void (*f)(unsigned char *, const unsigned char *)) '
               '{ __brat_fill = f; }')
    proto = ', '.join('%s a%d' % (_cb_c_slot(t), i) for i, t in enumerate(ptypes)) or 'void'
    out.append('__brat_s_%s __brat_cb_call(%s) {' % (ret, proto))
    out.append('    unsigned char in[%d];' % max(nbytes, 1))
    out.append('    unsigned char out[%d];' % ctypes.sizeof(_struct_class(ret)))
    out.append('    memset(in, 0, sizeof in);')
    for i, (t, (_off, size, _big)) in enumerate(zip(ptypes, slots)):
        out.append('    { %s t = a%d; memcpy(in + %d, &t, sizeof t); }'
                   % (_cb_c_slot(t), i, _off))
    out.append('    __brat_fill(out, in);')
    out.append('    __brat_s_%s r; memcpy(&r, out, sizeof r); return r;' % ret)
    out.append('}')
    return '\n'.join(out) + '\n'


def _cb_shim_lib(src):
    import hashlib
    import subprocess
    import tempfile
    key = hashlib.sha1(src.encode('utf-8')).hexdigest()[:20]
    if key in _CB_SHIMS:
        return _CB_SHIMS[key]
    d = os.path.join(tempfile.gettempdir(), 'brat-cb-shim')
    os.makedirs(d, exist_ok=True)
    so = os.path.join(d, 'shim-%s.so' % key)
    if not os.path.exists(so):
        cpath = os.path.join(d, 'shim-%s.c' % key)
        with open(cpath, 'w', encoding='utf-8') as f:
            f.write(src)
        cc = os.environ.get('CC') or 'cc'
        try:
            r = subprocess.run([cc, '-shared', '-fPIC', '-O1', '-w', cpath, '-o', so + '.tmp'],
                               capture_output=True, text=True, timeout=120)
        except OSError as e:
            raise RuntimeError("ffi: a callback returning a struct by value needs a C "
                               "compiler to reach C; `%s` could not run (%s): run "
                               "`brat build`" % (cc, e))
        if r.returncode != 0:
            raise RuntimeError(
                "ffi: a callback returning a struct by value needs a C trampoline and it "
                "did not compile -- the record `%s` is not laid out the same way twice, so "
                "returning it would be a guess: run `brat build`\n%s"
                % (src.split('__brat_s_')[1].split(';')[0], (r.stderr or '').strip().splitlines()[:3]))
        os.replace(so + '.tmp', so)
    lib = ctypes.CDLL(so)
    _CB_SHIMS[key] = lib
    return lib


def _norm_abi(text):
    return ''.join(text.split()) if isinstance(text, str) else 'none'


def _check_cb_value(sig, fname, funcs, pos=None, where=None):
    """Refuse a callback slot the call cannot honour, before ctypes sees it.

    Without this, an argument that is not a function name reaches
    `ctypes.CFUNCTYPE` and the failure comes back as a Python traceback from
    inside a callback C is in the middle of (`Exception ignored on calling
    ctypes callback function`), several frames from the line that made the
    mistake. The words are the checker's, so a program that `check` rejects
    fails here saying the same thing.
    """
    prefix = ('arg %d of `%s`: ' % (pos, where)) if pos is not None else ''
    if not isinstance(fname, str):
        raise RuntimeError(prefix + 'callback must be a function name')
    f = funcs.get(fname)
    if f is None or not (isinstance(f, tuple) and len(f) == 2):
        raise RuntimeError(prefix + 'unknown callback `%s`' % fname)
    params = f[0]
    if not isinstance(params, list):
        raise RuntimeError(prefix + 'unknown callback `%s`' % fname)
    want = _norm_abi(sig)
    try:
        parsed = _parse_cb_sig(want)
    except ValueError:
        raise RuntimeError('callback `%s` does not have a verifiable C ABI signature' % fname)
    if len(params) != len(parsed[0]):
        raise RuntimeError('callback `%s` takes %d parameter(s), %s wants %d'
                           % (fname, len(params), want, len(parsed[0])))
    anns = [_norm_abi(p[2] if isinstance(p, tuple) and len(p) > 2 else None) for p in params]
    actual_ret = _norm_abi(_RET.get(fname))
    if tuple(anns) != tuple(_norm_abi(t) for t in parsed[0]) or actual_ret != _norm_abi(parsed[1]):
        raise RuntimeError('callback `%s` has signature fn(%s)->%s, expected %s'
                           % (fname, ','.join(anns), actual_ret, want))


def _callback_ptr_struct(fname, funcs, ptypes, ret):
    """A callback that returns a record by value, through a compiled trampoline."""
    slots = []
    off = 0
    for t in ptypes:
        size = ctypes.sizeof(_cb_ctype(t))
        big = max(8, (size + 7) // 8 * 8)
        slots.append((off, size, big))
        off += big
    lib = _cb_shim_lib(_cb_shim_src(ptypes, slots, ret, off))
    FILL = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)

    def wrapper(outp, inp):     # pragma: no cover - exercised from C
        brat_args = []
        for t, (o, size, _big) in zip(ptypes, slots):
            raw = _cb_ctype(t).from_buffer_copy(ctypes.string_at(inp + o, size))
            if isinstance(t, str) and t in _STRUCTS:
                brat_args.append(('num', _read_struct(raw, t)))
            elif t == 'str':
                v = raw.value
                brat_args.append(('str', v.decode('utf-8') if isinstance(v, bytes) else (v or '')))
            elif t == 'ptr':
                brat_args.append(('num', raw.value if hasattr(raw, 'value') else (raw or 0)))
            else:
                brat_args.append(('num', raw.value if hasattr(raw, 'value') else raw))
        inst = _filled_struct(ret, _cb_run(fname, brat_args, funcs))
        ctypes.memmove(outp, ctypes.byref(inst), ctypes.sizeof(inst))
        # A `str` field is a pointer into a bytes object: keep the object alive
        # until C has read the record it points at.
        _CALLBACK_REFS.append(inst)

    ptr = FILL(wrapper)
    _CALLBACK_REFS.append(ptr)
    lib.__brat_cb_bind.argtypes = [ctypes.c_void_p]
    lib.__brat_cb_bind(ctypes.cast(ptr, ctypes.c_void_p))
    return ctypes.cast(lib.__brat_cb_call, ctypes.c_void_p)


def _callback_ptr(sig, fname, funcs, pos=None, where=None):
    import ctypes
    _check_cb_value(sig, fname, funcs, pos, where)
    ptypes, ret = _parse_cb_sig(sig)
    argtypes = [_cb_ctype(t) for t in ptypes]
    if ret in _STRUCTS:
        return _callback_ptr_struct(fname, funcs, ptypes, ret)
    elif ret in _CB_SCALARS:
        restype = _cb_ctype(ret)
    else:
        restype = None

    def _c_to_brat(ty, cv):
        if ty == 'str':
            return cv.decode('utf-8') if isinstance(cv, bytes) else cv
        if ty == 'ptr':
            return cv.value if hasattr(cv, 'value') else (cv or 0)
        return cv

    def wrapper(*cvals):
        brat_args = []
        for t, cv in zip(ptypes, cvals):
            v = _c_to_brat(t, cv)
            if t == 'str':
                brat_args.append(('str', v))
            elif t in _STRUCTS:
                brat_args.append(('num', _read_struct(cv, t)))
            else:
                brat_args.append(('num', v))
        r = _cb_run(fname, brat_args, funcs)
        if ret in _STRUCTS:
            return _filled_struct(ret, r)
        if ret in ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64', 'usize', 'ptr'):
            return int(r) if not isinstance(r, bool) else int(r)
        if ret in ('f32', 'f64'):
            return float(r)
        if ret == 'str':
            return r.encode('utf-8') if isinstance(r, str) else r
        return None

    if argtypes:
        CFUNCTYPE = ctypes.CFUNCTYPE(restype, *argtypes)
    else:
        CFUNCTYPE = ctypes.CFUNCTYPE(restype)
    ptr = CFUNCTYPE(wrapper)
    _CALLBACK_REFS.append(ptr)
    return ptr


def hoist(s, funcs):
    if s[0] == 'pub':
        s = s[1]
    if s[0] in ('fn', 'exportfn'):
        if s[1] not in funcs:
            funcs[s[1]] = (s[2], s[3])
            _RET[s[1]] = s[5] if len(s) > 5 else None
    elif s[0] == 'extern':
        if s[2] not in funcs:
            funcs[s[2]] = ('extern', s[3], s[4], s[1], s[5] if len(s) > 5 else None,
                           bool(len(s) > 7 and s[7]))
    elif s[0] == 'externstruct':
        _STRUCTS[s[1]] = s[2]
        if len(s) > 3 and s[3]:
            _PACKED.add(s[1])
    elif s[0] == 'enum':
        for vname, fields in s[2]:
            tag = 'none' if vname == 'None' and not fields else vname.lower()
            if vname not in funcs:
                funcs[vname] = ('ctor', tag, len(fields))
            _VARIANTS.setdefault(tag, s[1])
    elif s[0] == 'trait':
        if s[1] not in _TRAITS:
            _TRAITS[s[1]] = [g[1] for g in s[2]]
    elif s[0] == 'impl':
        _, trait, tname, fns = s
        if trait is not None:
            if trait not in _TRAITS:
                raise RuntimeError('unknown trait ' + trait)
            missing = [m for m in _TRAITS[trait] if m not in [f[1] for f in fns]]
            if missing:
                raise RuntimeError('impl %s for %s missing %s' % (trait, tname, ','.join(missing)))
        seen = set()
        for f in fns:
            if f[1] in seen:
                raise RuntimeError('duplicate method %s.%s' % (tname, f[1]))
            seen.add(f[1])
            if tname + '.' + f[1] not in funcs:
                funcs[tname + '.' + f[1]] = (f[2], f[3])


def method_of(obj, meth, funcs):
    if isinstance(obj, dict) and '$type' in obj:
        fname = obj['$type'] + '.' + meth
        if fname in funcs:
            return fname
    if isinstance(obj, tuple) and obj and isinstance(obj[0], str):
        tname = _VARIANTS.get(obj[0], obj[0].capitalize())
        fname = tname + '.' + meth
        if fname in funcs:
            return fname
    return None


def call_method_fn(fname, this, vals, env, funcs):
    params, body = funcs[fname]
    names = [p[0] for p in params]
    vals = fill_defaults(params, [this] + list(vals), env, funcs, fname)[1:]
    vals = _coerce_params(params[1:], vals)
    if len(vals) + 1 != len(names):
        raise TypeError(fname + ' wants %d args' % (len(names) - 1))
    frame = {}
    for nm, val in zip(names, [this] + vals):
        if _MANAGED:
            vrc_inc(val)
        frame[nm] = val
    _note_ptr_params(frame, params)
    with _frame():
        env.append(frame)
        try:
            run_block(body, env, funcs, frame=True)
        except Propagate:
            _drop_all(frame)
            env.pop()
            raise
        except (Done, Ret) as r:
            _drop_all(frame)
            env.pop()
            return r.v
        _drop_all(frame)
        env.pop()
        return 0


def run_stmt(s, env, funcs):
    try:
        _run_stmt(s, env, funcs)
    except (Done, LoopBreak, LoopCont):
        raise
    except Exception as ex:
        ln = _LINES.get(id(s))
        if ln is not None and getattr(ex, '_brat_ln', None) is None:
            ex._brat_ln = ln
        raise


def _run_stmt(s, env, funcs):
    k = s[0]
    if k == 'unsafe':
        run_block(s[1], env, funcs)
    elif k == 'let':
        _FFI_RETS.clear()
        _bind(env[-1], s[1], ev(s[2], env, funcs))
        # The annotation is the sixth element; a `let` that carries none still
        # has the slot, so it is the value that decides, not the length.
        ann = s[5] if len(s) > 5 else None
        if ann:
            _note_ptr(env[-1], s[1], ann)
        elif _FFI_RETS:
            # `let p = node_new(...)`: the callee's `ptr[T]` return is the only
            # declaration of what `p` points at.
            elem = _FFI_RETS[-1]
            if elem:
                _PTRS.setdefault(id(env[-1]), {})[s[1]] = elem
        _FFI_RETS.clear()
    elif k == 'set':
        name = s[1]
        v = ev(s[2], env, funcs)
        if _MANAGED:
            tgt = None
            for f in reversed(env):
                if name in f:
                    tgt = f
                    break
            if tgt is not None:
                vrc_dec(tgt[name])
            vrc_inc(v)
        put(env, name, v)
    elif k == 'setidx':
        base = ev(s[1], env, funcs)
        ix = ev(s[2], env, funcs)
        v = ev(s[3], env, funcs)
        _pk = _ptr_kind(env, s[1])
        if _pk is not None and isinstance(base, int) and not isinstance(base, bool):
            _ptr_put(_pk, base, ix, v)
            return
        if isinstance(base, dict):
            if '$type' in base:
                raise RuntimeError('no field ' + str(ix))
            bad = check_map_key(ix)
            if bad:
                return ('err', bad)
            base[_mkey(ix)] = v
        elif isinstance(base, list):
            if not isinstance(ix, (int, float)) or int(ix) != ix:
                raise RuntimeError('index must be int')
            ix = int(ix)
            if ix < 0 or ix >= len(base):
                raise RuntimeError('index out of range')
            base[ix] = v
        else:
            raise RuntimeError('setidx of non-indexable')
    elif k == 'setfield':
        base = ev(s[1], env, funcs)
        v = ev(s[3], env, funcs)
        if not isinstance(base, dict):
            raise RuntimeError('no field ' + s[2])
        base[s[2]] = v
    elif k in ('fn', 'exportfn'):
        funcs[s[1]] = (s[2], s[3])
        _RET[s[1]] = s[5] if len(s) > 5 else None
    elif k == 'if':
        _, cond, then, els = s
        if ev(cond, env, funcs):
            run_block(then, env, funcs)
        elif els:
            run_block(els if isinstance(els, list) else [els], env, funcs)
    elif k == 'while':
        while ev(s[1], env, funcs):
            try:
                run_block(s[2], env, funcs)
            except LoopCont:
                pass
            except LoopBreak:
                break
    elif k == 'foriter':
        _, vn, ve, body = s
        seq = ev(ve, env, funcs)
        if isinstance(seq, dict):
            seq = [_munkey(k) for k in seq if k != '$type']
        frame = env[-1]
        had = vn in frame
        old = frame.get(vn)
        try:
            for x in seq:
                _bind(frame, vn, x)
                try:
                    run_block(body, env, funcs)
                except LoopCont:
                    continue
                except LoopBreak:
                    break
        finally:
            if had:
                frame[vn] = old
            else:
                frame.pop(vn, None)
    elif k == 'forrange':
        _, vn, se, ee, body = s
        a, b = ev(se, env, funcs), ev(ee, env, funcs)
        if not isinstance(a, (int, float)) or not isinstance(b, (int, float)):
            raise RuntimeError('range bounds must be num')
        frame = env[-1]
        had = vn in frame
        old = frame.get(vn)
        i = int(a)
        try:
            while i < int(b):
                _bind(frame, vn, i)
                try:
                    run_block(body, env, funcs)
                except LoopCont:
                    pass
                except LoopBreak:
                    break
                i += 1
        finally:
            if had:
                frame[vn] = old
            else:
                frame.pop(vn, None)
    elif k == 'return':
        raise Ret(ev(s[1], env, funcs))
    elif k == 'try':
        try:
            run_block(s[1], env, funcs)
        except Done as r:
            v = r.v
            payload = v[1] if isinstance(v, tuple) and len(v) == 2 else v
            if s[3]:
                _bind(env[-1], s[3], payload)
            run_block(s[2], env, funcs)
        except (Ret, LoopBreak, LoopCont):
            raise
        except Exception as ex:
            if s[3]:
                _bind(env[-1], s[3], str(ex))
            run_block(s[2], env, funcs)
    elif k == 'scope':
        handles = []
        outer = getattr(_CURRENT, 'scope', None)
        _CURRENT.scope = handles
        try:
            run_block(s[1], env, funcs)
        finally:
            _CURRENT.scope = outer
            _scope_join(handles)
    elif k == 'spawn':
        snap = [dict(f) for f in env]
        _spawn(s[1], snap, funcs)
    elif k == 'defer':
        st = _dfr_stack()
        # defer is a snapshot closure (SPEC 9); outside any block the C backend skips it too.
        if st:
            st[-1].append(('closure', None, [], [('expr', s[1])], dict(env[-1])))
    elif k == 'break':
        raise LoopBreak()
    elif k == 'continue':
        raise LoopCont()
    elif k == 'externstruct':
        pass
    elif k == 'impl':
        hoist(s, funcs)
    elif k == 'enum':
        hoist(s, funcs)
    elif k == 'pub':
        run_stmt(s[1], env, funcs)
    elif k == 'expr':
        ev(s[1], env, funcs)
    else:
        raise RuntimeError('bad stmt ' + k)

def run(src, base='.', argv=None):
    _MOD_BASE[0] = base
    _ARGS[0] = list(argv) if argv else []
    _TRAITS.clear()
    _VARIANTS.clear()
    prog = load_source(src, base).prog
    env = [{}]
    funcs = {}
    for s in prog[1]:
        hoist(s, funcs)
    for s in prog[1]:
        if s[0] not in ('fn', 'impl', 'trait', 'enum', 'pub', 'extern', 'externstruct'):
            run_stmt(s, env, funcs)
    out = call('main', [], env, funcs) if 'main' in funcs else 0
    join_all()
    return out