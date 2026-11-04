import contextlib
import copy
import ctypes
import errno as _errno
import hashlib
import json
import math
import os
import re
import select
import signal
import socket
import sqlite3
import ssl
import stat
import sys
import threading
import time

_STR_PIN_LOCK = threading.Lock()
_STR_PTRS = {}
_STR_PIN_CAP = 32768
_STR_PIN_BYTES_CAP = 64 << 20
_STR_PIN_BYTES = 0


def _pin_str_buf(raw_bytes):
    global _STR_PIN_BYTES
    with _STR_PIN_LOCK:
        buf = _STR_PTRS.get(raw_bytes)
        if buf is not None:
            return buf
        if (len(_STR_PTRS) >= _STR_PIN_CAP or
                len(raw_bytes) + 1 > _STR_PIN_BYTES_CAP - _STR_PIN_BYTES):
            raise RuntimeError('string pin limit exceeded')
        buf = ctypes.create_string_buffer(raw_bytes)
        _STR_PTRS[raw_bytes] = buf
        _STR_PIN_BYTES += len(raw_bytes) + 1
        return buf


_MAX_FILE_BYTES = 64 << 20
_MAX_READ_BYTES = 16 << 20
_MAX_BYTE_VEC = 16 << 20
_MAX_STR_BYTES = 1 << 30
_MAX_SQL_ROWS = 100000
_MAX_SQL_COLS = 1024
_MAX_SQL_BLOB = 4 << 20
_MAX_SQL_RESULT_BYTES = 64 << 20
_MAX_TOTAL_CHAN_SLOTS = 4194304
_CHAN_BUDGET_LOCK = threading.Lock()
_CHAN_TOTAL_SLOTS = [0]

_SQL_LOCK = threading.Lock()
_SQL_DBS = {}
_NEXT_SQL = [1]


class _SqlEntry:
    __slots__ = ('conn', 'lock', 'active', 'closing')

    def __init__(self, conn):
        self.conn = conn
        self.lock = threading.Lock()
        self.active = 0
        self.closing = False


def _sql_acquire(sv):
    with _SQL_LOCK:
        ent = _SQL_DBS.get(sv)
        if ent is None or ent.closing or ent.conn is None:
            return None
        ent.active += 1
    ent.lock.acquire()
    return ent


def _sql_release(sv, ent):
    ent.lock.release()
    to_close = None
    with _SQL_LOCK:
        ent.active -= 1
        if ent.closing and ent.active == 0:
            to_close = ent.conn
            ent.conn = None
            _SQL_DBS.pop(sv, None)
    if to_close is not None:
        try:
            to_close.close()
        except Exception:
            pass

try:
    from .parser import _LINES
    from .modules import load_source
except ImportError:
    from parser import _LINES
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

# Language recursion budget: BRAT frames, not Python frames. 2000: measured
# refusal 0.4 s interpreter / instant C; a higher default is unreachable in the
# interpreter and overflows the C stack.
_DEFAULT_RECURSION_BUDGET = 2000


def _recursion_budget():
    """BRAT_RECURSION_BUDGET under the rule bdepth_init in crt.h applies: an integer in
    1..1000000, anything else is ignored."""
    raw = os.environ.get('BRAT_RECURSION_BUDGET', '')
    if not re.fullmatch(r'[ \t\n\v\f\r]*[+-]?[0-9]+', raw):
        return _DEFAULT_RECURSION_BUDGET
    value = int(raw)
    return value if 0 < value <= 1000000 else _DEFAULT_RECURSION_BUDGET


RECURSION_BUDGET = _recursion_budget()

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


def _os_arg_bytes(v):
    if isinstance(v, bytes):
        return v
    return os.fsencode(v)


def _os_text(raw):
    try:
        s = raw.decode('utf-8')
    except UnicodeDecodeError:
        return None
    return None if '\x00' in s else s


def _valid_text(msg):
    if re.search(r'\\udc[0-9a-fA-F]{2}', msg):
        return False
    try:
        raw = msg.encode('utf-8', 'surrogateescape')
    except UnicodeEncodeError:
        return False
    return _os_text(raw) is not None


def _err_text(msg):
    if _valid_text(msg):
        return msg
    m = re.match(r'^\[Errno (\d+)\]', msg)
    return '[Errno %s] os error' % m.group(1) if m else 'os error'


def _os_error_text(e):
    msg = str(e)
    if _valid_text(msg):
        return msg
    err_no = getattr(e, 'errno', None)
    return '[Errno %s] os error' % (err_no if err_no is not None else 0)


def _fs_path(v, fname):
    if isinstance(v, str):
        if '\x00' in v:
            return None, 'embedded NUL in file path'
        return v, None
    if isinstance(v, list):
        raw = bytearray()
        for b in v:
            if isinstance(b, bool) or not isinstance(b, int) or b < 0 or b > 255:
                return None, '%s byte out of range 0..255' % fname
            if b == 0:
                return None, 'embedded NUL in file path'
            raw.append(b)
        return bytes(raw).decode('utf-8', 'surrogateescape'), None
    return None, ''


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


def _too_deep(*vals, skip_closure=True):
    """True when a nested container is deeper than the C runtime's budget.

    `==`, `!=` and the ordering operators walk a value graph recursively in the
    C backend (`veq`/`vcmp`), which stops with "recursion too deep" past
    `BRAT_MAX_VALUE_DEPTH` instead of overflowing the machine stack. Python's
    `==` on two deep lists is recursive too, but silently: it either answers or
    dies inside CPython. Measuring the nesting here first keeps the two backends
    on the same answer, and is the reason this walks iteratively."""
    stack = [(v, 1) for v in vals if isinstance(v, (list, dict, tuple))
             and not (skip_closure and isinstance(v, tuple) and v and v[0] == 'closure')]
    while stack:
        v, d = stack.pop()
        if d > _MAX_VALUE_DEPTH:
            return True
        if isinstance(v, dict):
            kids = list(v.values())
        elif isinstance(v, tuple):
            if v and v[0] == 'closure':
                if skip_closure:
                    continue
                kids = [v[4]] if len(v) > 4 and isinstance(v[4], dict) else []
            else:
                kids = [x for x in v[1:]]      # the first slot is the tag's name
        else:
            kids = v
        stack.extend((x, d + 1) for x in kids if isinstance(x, (list, dict, tuple)))
    return False


def _eq_deep(a, b, _depth=0):
    """Iterative `==` against `veq`'s recursion budget (`_MAX_VALUE_DEPTH`)."""
    stk = []
    while True:
        if _depth + len(stk) >= _MAX_VALUE_DEPTH:
            raise RuntimeError('recursion too deep')
        an = isinstance(a, (bool, int, float))
        bn = isinstance(b, (bool, int, float))
        if an and bn:
            if a != b:
                return False
        elif _kind(a) != _kind(b):
            return False
        elif isinstance(a, dict):
            if a.get('$type', '') != b.get('$type', '') or len(a) != len(b):
                return False
            ks = [k for k in a.keys() if k != '$type']
            if ks:
                k0 = ks[0]
                if k0 not in b:
                    return False
                stk.append(('d', a, b, ks, 0))
                a, b = a[k0], b[k0]
                continue
        elif isinstance(a, tuple):
            if len(a) != len(b) or (a and a[0] != b[0]):
                return False
            if len(a) > 1:
                stk.append(('t', a, b, None, 1))
                a, b = a[1], b[1]
                continue
        elif isinstance(a, list):
            if len(a) != len(b):
                return False
            if a:
                stk.append(('l', a, b, None, 0))
                a, b = a[0], b[0]
                continue
        else:
            if a != b:
                return False
        advanced = False
        while stk:
            knd, pa, pb, ks, idx = stk[-1]
            nx = idx + 1
            if knd == 'l':
                if nx < len(pa):
                    stk[-1] = ('l', pa, pb, None, nx)
                    a, b = pa[nx], pb[nx]
                    advanced = True
                    break
                stk.pop()
            elif knd == 't':
                if nx < len(pa):
                    stk[-1] = ('t', pa, pb, None, nx)
                    a, b = pa[nx], pb[nx]
                    advanced = True
                    break
                stk.pop()
            else:
                if nx < len(ks):
                    kn = ks[nx]
                    if kn not in pb:
                        return False
                    stk[-1] = ('d', pa, pb, ks, nx)
                    a, b = pa[kn], pb[kn]
                    advanced = True
                    break
                stk.pop()
        if not advanced:
            return True


def _ncmp(op, a, b, _depth=0):
    """Iterative three-way order matching `vcmp_` in `crt.h`."""
    stk = []
    while True:
        if _depth + len(stk) >= _MAX_VALUE_DEPTH:
            raise RuntimeError('recursion too deep')
        if isinstance(a, str) and isinstance(b, str):
            c = (a > b) - (a < b)
            if c:
                return c
        elif isinstance(a, (bool, int, float)) and isinstance(b, (bool, int, float)):
            if a != a or b != b:
                c = (a != a) - (b != b)
                if c:
                    return c
            else:
                c = (a > b) - (a < b)
                if c:
                    return c
        elif a is None or b is None:
            if a is not b:
                return -1 if a is None else 1
        elif _kind(a) != _kind(b) or _kind(a) == 'x':
            raise RuntimeError('type error in ' + op)
        elif isinstance(a, list):
            nn = min(len(a), len(b))
            if nn == 0:
                if len(a) != len(b):
                    return 1 if len(a) > len(b) else -1
            else:
                stk.append(('l', a, b, None, 0, 0))
                a, b = a[0], b[0]
                continue
        elif isinstance(a, dict):
            at, bt = a.get('$type', ''), b.get('$type', '')
            if at != bt:
                return 1 if at > bt else -1
            ka = [k for k in a.keys() if k != '$type']
            kb = [k for k in b.keys() if k != '$type']
            nn = min(len(ka), len(kb))
            if nn == 0:
                if len(ka) != len(kb):
                    return 1 if len(ka) > len(kb) else -1
            else:
                stk.append(('d', a, b, (ka, kb), 0, 0))
                a, b = _munkey(ka[0]), _munkey(kb[0])
                continue
        elif isinstance(a, tuple):
            if a[0] != b[0]:
                return 1 if a[0] > b[0] else -1
            nn = min(len(a), len(b))
            if nn <= 1:
                if len(a) != len(b):
                    return 1 if len(a) > len(b) else -1
            else:
                stk.append(('t', a, b, None, 1, 0))
                a, b = a[1], b[1]
                continue
        advanced = False
        while stk:
            knd, pa, pb, ks, idx, ph = stk[-1]
            if knd == 'l':
                nn = min(len(pa), len(pb))
                nx = idx + 1
                if nx < nn:
                    stk[-1] = ('l', pa, pb, None, nx, 0)
                    a, b = pa[nx], pb[nx]
                    advanced = True
                    break
                if len(pa) != len(pb):
                    return 1 if len(pa) > len(pb) else -1
                stk.pop()
            elif knd == 't':
                nn = min(len(pa), len(pb))
                nx = idx + 1
                if nx < nn:
                    stk[-1] = ('t', pa, pb, None, nx, 0)
                    a, b = pa[nx], pb[nx]
                    advanced = True
                    break
                if len(pa) != len(pb):
                    return 1 if len(pa) > len(pb) else -1
                stk.pop()
            else:
                ka, kb = ks
                if ph == 0:
                    stk[-1] = ('d', pa, pb, ks, idx, 1)
                    a, b = pa[ka[idx]], pb[kb[idx]]
                    advanced = True
                    break
                nn = min(len(ka), len(kb))
                nx = idx + 1
                if nx < nn:
                    stk[-1] = ('d', pa, pb, ks, nx, 0)
                    a, b = _munkey(ka[nx]), _munkey(kb[nx])
                    advanced = True
                    break
                if len(ka) != len(kb):
                    return 1 if len(ka) > len(kb) else -1
                stk.pop()
        if not advanced:
            return 0


def _compare(op, a, b):
    """`<`, `<=`, `>`, `>=` inside one comparable class, or the same error the
    native runtime raises. Mixed types are refused: a total order across `str`
    and `i64` would answer a question the program never asked."""
    if isinstance(a, (bool, int, float)) and isinstance(b, (bool, int, float)):
        if a != a or b != b:
            return False
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
    if key is None:
        if len(set(_kind(x) for x in v)) > 1:
            raise RuntimeError('type error in <')
        v.sort(key=functools.cmp_to_key(lambda a, b: _ncmp('<', a, b)))
    else:
        # O(N) Schwartzian transform: evaluate `key(x)` once per element
        # (including len == 1) and sort `(key, index, item)` stably, matching
        # `vsort_by` in `crt.h`.
        n = len(v)
        if n <= 1:
            if n == 1:
                key(v[0])
            return v
        ents = [(key(v[i]), i, v[i]) for i in range(n)]
        if len(set(_kind(e[0]) for e in ents)) > 1:
            raise RuntimeError('type error in <')
        def _cmp_ent(a, b):
            c = _ncmp('<', a[0], b[0])
            if c != 0:
                return c
            return -1 if a[1] < b[1] else (1 if a[1] > b[1] else 0)
        ents.sort(key=functools.cmp_to_key(_cmp_ent))
        v[:] = [e[2] for e in ents]
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
    if _too_deep(v, skip_closure=True):
        raise RuntimeError('recursion too deep')
    return _brepr(v)


def _brepr_qstr(v):
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


def _brepr(v):
    out = []
    stk = []
    cur = v
    while True:
        if isinstance(cur, bool):
            out.append('True' if cur else 'False')
        elif cur is None:
            out.append('None')
        elif isinstance(cur, (int, float)):
            out.append(str(cur))
        elif isinstance(cur, str):
            out.append(_brepr_qstr(cur))
        elif isinstance(cur, list):
            if not cur:
                out.append('[]')
            else:
                out.append('[')
                stk.append(('l', cur, 0, None))
                cur = cur[0]
                continue
        elif isinstance(cur, dict):
            has_type = cur.get('$type') is not None
            items = [(k, x) for k, x in cur.items() if k != '$type']
            out.append('{')
            if has_type:
                out.append("'$type': '%s'" % cur['$type'])
            if not items:
                out.append('}')
            else:
                if has_type:
                    out.append(', ')
                stk.append(('d', items, 0, 0))
                cur = _munkey(items[0][0])
                continue
        elif isinstance(cur, tuple):
            if cur and cur[0] == 'closure':
                out.append('fn')
            elif len(cur) <= 1:
                out.append("('%s',)" % (cur[0] if cur else ''))
            else:
                out.append("('%s', " % cur[0])
                stk.append(('t', cur, 1, None))
                cur = cur[1]
                continue
        else:
            out.append(str(cur))
        advanced = False
        while stk:
            knd, seq, idx, ph = stk[-1]
            if knd == 'l':
                nx = idx + 1
                if nx < len(seq):
                    out.append(', ')
                    stk[-1] = ('l', seq, nx, None)
                    cur = seq[nx]
                    advanced = True
                    break
                out.append(']')
                stk.pop()
            elif knd == 't':
                nx = idx + 1
                if nx < len(seq):
                    out.append(', ')
                    stk[-1] = ('t', seq, nx, None)
                    cur = seq[nx]
                    advanced = True
                    break
                out.append(')')
                stk.pop()
            else:
                if ph == 0:
                    out.append(': ')
                    stk[-1] = ('d', seq, idx, 1)
                    cur = seq[idx][1]
                    advanced = True
                    break
                nx = idx + 1
                if nx < len(seq):
                    out.append(', ')
                    stk[-1] = ('d', seq, nx, 0)
                    cur = _munkey(seq[nx][0])
                    advanced = True
                    break
                out.append('}')
                stk.pop()
        if not advanced:
            return ''.join(out)


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


def assoc_fn_name(recv, meth, env, funcs):
    """`Type.f` for an `impl` function without `self`, or None.

    A bare uppercase identifier parses as a zero-argument call because that is
    how enum variants are written (`Red`), so `Point.make(1)` arrives as an
    `mcall` whose receiver is `('call', 'Point', [])`. Resolving it as a path
    is only correct when the `impl` really declares that name and the first
    parameter is not `self`; otherwise `Red.code()` must keep evaluating the
    receiver and dispatching on its variant.
    """
    if not (isinstance(recv, tuple) and len(recv) >= 3 and recv[0] == 'call'):
        return None
    tname = recv[1]
    if recv[2] or not isinstance(tname, str) or '.' in tname:
        return None
    if not tname[:1].isupper() or find(env, tname) is not _MISS:
        return None
    info = funcs.get(tname + '.' + meth)
    if not (isinstance(info, tuple) and len(info) == 2 and isinstance(info[0], list)):
        return None
    if info[0] and info[0][0][0] == 'self':
        # `Color.code()` for a method that wants a receiver. The C++ emitter
        # refuses this with the same sentence; without this the interpreter
        # fell through to the builtin table and raised a CPython `NameError:
        # no fn Color`, which no BRAT handler can catch and which names the
        # type rather than the mistake.
        raise RuntimeError(tname + '.' + meth + ' needs instance, use value.method()')
    return tname + '.' + meth


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
_UNIONS = set()

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


def _run_frame(frame, body, env, funcs, params=(), fallthrough=0):
    """Run a function body in `frame`, which leaves `env` however the body ends.

    A runtime error caught by a `try` further up used to leave the callee's frame on the
    stack, where its parameters and locals shadowed the caller's variables from then on."""
    with _frame():
        _note_ptr_params(frame, params)
        env.append(frame)
        try:
            run_block(body, env, funcs, frame=True)
        except Propagate:
            raise
        except (Done, Ret) as r:
            return r.v
        finally:
            _drop_all(frame)
            _forget_ptr_frame(frame)
            env.pop()
        return fallthrough


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


def _as_index(ix):
    """The position an index names, as `vindex_of` in `crt.h` decides it.

    A float counts when it is a whole number. A bool, a fraction, NaN and infinity are
    refused: the last two used to escape as CPython's own ValueError and OverflowError text."""
    if isinstance(ix, bool) or not isinstance(ix, (int, float)):
        raise RuntimeError('index must be int')
    if isinstance(ix, float) and not (math.isfinite(ix) and ix == math.floor(ix)):
        raise RuntimeError('index must be int')
    return int(ix)


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
            k = None if isinstance(ix, (list, dict)) else _mkey(ix)
            if k == _TYPE_KEY:
                raise RuntimeError('no field ' + str(ix))
            if k is None or k not in base:
                raise RuntimeError(('no field ' if '$type' in base else 'no key ')
                                   + str(ix))
            return base[k]
        ix = _as_index(ix)
        if not isinstance(base, (list, str)):
            raise RuntimeError('index of non-indexable')
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
                ev(src, env, funcs)
                _out_names(env)[src[1][1]] = True
                return ('addr', env, src[1][1], 0, None)
            if src[0] == 'idx' and isinstance(src[1], tuple) and src[1][0] == 'var':
                ev(src, env, funcs)
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
        if name:
            cap.pop(name, None)
        return ('closure', name, params, body, cap)
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
                sa = a if isinstance(a, str) else str(a)
                sb = b if isinstance(b, str) else str(b)
                if len(sa.encode('utf-8')) + len(sb.encode('utf-8')) > MAX_STR:
                    raise RuntimeError('out of memory')
                return sa + sb
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
            if not isinstance(a, (int, float)) or not isinstance(b, (int, float)):
                raise RuntimeError('type error in /')
            if b == 0:
                raise RuntimeError('div by zero')
            if isinstance(a, int) and isinstance(b, int):
                if a == -_I64 and b == -1:
                    return -_I64
                return wrap64(a // b)
            return a / b
        if op == '%':
            if not isinstance(a, (int, float)) or not isinstance(b, (int, float)):
                raise RuntimeError('type error in %')
            if b == 0:
                raise RuntimeError('div by zero')
            if isinstance(a, int) and isinstance(b, int):
                if a == -_I64 and b == -1:
                    return 0
                return wrap64(a % b)
            try:
                r = math.fmod(a, b)
            except ValueError:
                return math.nan
            if r != 0 and ((r < 0) != (b < 0)):
                r += b
            return r
        if op == '==' or op == '!=':
            if (_kind(a) == _kind(b) and isinstance(a, (list, dict, tuple))
                    and _too_deep(a, b)):
                # Deep enough that the native `veq` would stop with its budget:
                # walk it here with the same budget instead of asking Python,
                # whose answer for a 2500-deep pair is a `RecursionError`.
                same = _eq_deep(a, b)
            else:
                try:
                    same = a == b
                except RecursionError:
                    same = _eq_deep(a, b)
            return same if op == '==' else not same
        if op in ('<', '>', '<=', '>='):
            # The budget is `_ncmp`'s now: it counts as it descends, so a deep
            # value compared against `None` answers instead of raising, exactly
            # as `vcmp_` does when the two classes differ.
            return _compare(op, a, b)
        if op == 'in':
            if isinstance(b, dict):
                return not isinstance(a, (list, dict)) and _mkey(a) in b and a != '$type'
            if isinstance(b, list) or (isinstance(b, str) and isinstance(a, str)):
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
        sf = assoc_fn_name(e[1], e[2], env, funcs)
        if sf is not None:
            # `Type.make(x)`: an `impl` function that does not take `self`. A
            # bare uppercase name parses as a zero-argument call, so name_path()
            # above does not see it as a path. Both checkers accept the call, so
            # refusing it here would be a backend disagreement (F-033).
            return call(sf, e[3], env, funcs)
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


_RE_MAX_INST = 65536
_RE_MAX_REP = 1024
_RE_MAX_GROUPS = 64


def _re_set_match(body: bytes, ch: int) -> bool:
    neg = len(body) > 0 and body[0] == ord('^')
    if neg:
        body = body[1:]
    found = False
    i = 0
    n = len(body)
    while i < n:
        if i + 2 < n and body[i + 1] == ord('-'):
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


def _re_compile_pike(pat_bytes: bytes):
    prog = []  # list of [op, q, ch, body, x, y]
    plen = len(pat_bytes)
    st = [0, 0]  # [pos, ngroups]

    def emit(op, x=0, y=0, q='', ch=0, body=b''):
        if len(prog) >= _RE_MAX_INST:
            raise SyntaxError('bad regex pattern')
        idx = len(prog)
        prog.append([op, q, ch, body, x, y])
        return idx

    def parse_bounds():
        st[0] += 1  # skip '{'
        if st[0] >= plen or not (48 <= pat_bytes[st[0]] <= 57):
            raise SyntaxError('bad regex pattern')
        lo = 0
        while st[0] < plen and 48 <= pat_bytes[st[0]] <= 57:
            lo = lo * 10 + (pat_bytes[st[0]] - 48)
            st[0] += 1
            if lo > _RE_MAX_REP:
                raise SyntaxError('bad regex pattern')
        if st[0] >= plen:
            raise SyntaxError('bad regex pattern')
        if pat_bytes[st[0]] == ord('}'):
            st[0] += 1
            return lo, lo
        if pat_bytes[st[0]] != ord(','):
            raise SyntaxError('bad regex pattern')
        st[0] += 1
        if st[0] >= plen:
            raise SyntaxError('bad regex pattern')
        if pat_bytes[st[0]] == ord('}'):
            st[0] += 1
            return lo, -1
        if not (48 <= pat_bytes[st[0]] <= 57):
            raise SyntaxError('bad regex pattern')
        hi = 0
        while st[0] < plen and 48 <= pat_bytes[st[0]] <= 57:
            hi = hi * 10 + (pat_bytes[st[0]] - 48)
            st[0] += 1
            if hi > _RE_MAX_REP:
                raise SyntaxError('bad regex pattern')
        if st[0] >= plen or pat_bytes[st[0]] != ord('}') or hi < lo:
            raise SyntaxError('bad regex pattern')
        st[0] += 1
        return lo, hi

    def emit_tpl(tpl, orig_body, orig_end):
        dst = len(prog)
        delta = dst - orig_body
        for ins in tpl:
            op, q, ch, b, x, y = ins
            nx, ny = x, y
            if op == 'j' and orig_body <= x <= orig_end:
                nx = x + delta
            elif op == 's':
                if orig_body <= x <= orig_end:
                    nx = x + delta
                if orig_body <= y <= orig_end:
                    ny = y + delta
            emit(op, nx, ny, q, ch, b)
        return dst

    def r_atom():
        sp = emit('s', 0, 0)
        if st[0] >= plen:
            raise SyntaxError('bad regex pattern')
        c = pat_bytes[st[0]]
        if c in (ord('*'), ord('+'), ord('?')):
            raise SyntaxError('bad regex pattern')
        if c == ord('('):
            st[0] += 1
            st[1] += 1
            gid = st[1]
            sv0 = emit('v', 2 * gid, 0) if gid <= _RE_MAX_GROUPS else -1
            inner, _ = r_alt()
            if st[0] >= plen or pat_bytes[st[0]] != ord(')'):
                raise SyntaxError('bad regex pattern')
            st[0] += 1
            if gid <= _RE_MAX_GROUPS:
                emit('v', 2 * gid + 1, 0)
            body = sv0 if sv0 >= 0 else inner
        elif c == ord('['):
            j = st[0] + 1
            m = j
            while m < plen and pat_bytes[m] != ord(']'):
                m += 1
            if m >= plen:
                raise SyntaxError('bad regex pattern')
            cls_bytes = pat_bytes[j:m]
            st[0] = m + 1
            body = emit('c', 0, 0, 's', 0, cls_bytes)
        elif c == ord('.'):
            st[0] += 1
            body = emit('c', 0, 0, 'd')
        elif c == ord('^'):
            st[0] += 1
            body = emit('b', 0, 0)
        elif c == ord('$'):
            st[0] += 1
            body = emit('e', 0, 0)
        elif c == ord('\\'):
            eb = pat_bytes[st[0] + 1] if st[0] + 1 < plen else ord('\\')
            st[0] += 2
            if eb == ord('d'):
                body = emit('c', 0, 0, 's', 0, b'0-9')
            elif eb == ord('D'):
                body = emit('c', 0, 0, 's', 0, b'^0-9')
            elif eb == ord('w'):
                body = emit('c', 0, 0, 's', 0, b'a-zA-Z0-9_')
            elif eb == ord('W'):
                body = emit('c', 0, 0, 's', 0, b'^a-zA-Z0-9_')
            elif eb == ord('s'):
                body = emit('c', 0, 0, 's', 0, b' \t\n\r')
            elif eb == ord('S'):
                body = emit('c', 0, 0, 's', 0, b'^ \t\n\r')
            elif eb == ord('n'):
                body = emit('c', 0, 0, 'l', 10)
            elif eb == ord('t'):
                body = emit('c', 0, 0, 'l', 9)
            elif eb == ord('r'):
                body = emit('c', 0, 0, 'l', 13)
            else:
                body = emit('c', 0, 0, 'l', eb)
        else:
            st[0] += 1
            body = emit('c', 0, 0, 'l', c)

        body_end = len(prog)
        if st[0] < plen and pat_bytes[st[0]] == ord('{'):
            lo, hi = parse_bounds()
            if hi == 0:
                del prog[sp:]
                nop = emit('j', 0, 0)
                prog[nop][4] = len(prog)
                return nop
            tpl = [list(ins) for ins in prog[body:body_end]]
            del prog[sp:]
            entry = emit('j', 0, 0)
            prog[entry][4] = len(prog)
            for _ in range(lo):
                emit_tpl(tpl, body, body_end)
            if hi < 0:
                star_sp = emit('s', 0, 0)
                dst = emit_tpl(tpl, body, body_end)
                emit('j', star_sp, 0)
                prog[star_sp][4] = dst
                prog[star_sp][5] = len(prog)
            elif hi > lo:
                for _ in range(hi - lo):
                    opt_sp = emit('s', 0, 0)
                    dst = emit_tpl(tpl, body, body_end)
                    prog[opt_sp][4] = dst
                    prog[opt_sp][5] = len(prog)
            return entry

        q = 0
        if st[0] < plen and pat_bytes[st[0]] in (ord('*'), ord('+'), ord('?')):
            q = pat_bytes[st[0]]
            st[0] += 1
        if q == 0:
            prog[sp][0] = 'j'
            prog[sp][4] = body
            return sp
        if q == ord('*'):
            emit('j', sp, 0)
            prog[sp][4] = body
            prog[sp][5] = len(prog)
            return sp
        if q == ord('+'):
            prog[sp][0] = 'j'
            prog[sp][4] = body
            sp2 = emit('s', body, 0)
            prog[sp2][5] = len(prog)
            return sp
        prog[sp][4] = body
        prog[sp][5] = len(prog)
        return sp

    def r_seq():
        entry = emit('j', 0, 0)
        first_atom = -1
        while st[0] < plen:
            c = pat_bytes[st[0]]
            if c in (ord('|'), ord(')')):
                break
            a = r_atom()
            if first_atom < 0:
                first_atom = a
        prog[entry][4] = entry if first_atom < 0 else first_atom
        return entry, (first_atom < 0)

    def r_alt():
        head = emit('s', 0, 0)
        _, empty0 = r_seq()
        if not (st[0] < plen and pat_bytes[st[0]] == ord('|')):
            prog[head][0] = 'j'
            prog[head][4] = head + 1
            return head, empty0
        j1 = emit('j', 0, 0)
        sps = []
        jmps = []
        while st[0] < plen and pat_bytes[st[0]] == ord('|'):
            st[0] += 1
            sp = emit('s', 0, 0)
            sps.append(sp)
            s0, emp = r_seq()
            j = emit('j', 0, 0)
            jmps.append(j)
            if emp:
                prog[s0][4] = j
        fail = emit('f', 0, 0)
        exit_pc = len(prog)
        prog[head][4] = head + 1
        prog[head][5] = sps[0] if sps else fail
        prog[j1][4] = exit_pc
        for k, sp in enumerate(sps):
            prog[sp][4] = sp + 1
            prog[sp][5] = sps[k + 1] if k + 1 < len(sps) else fail
        for j in jmps:
            prog[j][4] = exit_pc
        return head, False

    sv0 = emit('v', 0, 0)
    r_alt()
    if st[0] != plen:
        raise SyntaxError('bad regex pattern')
    emit('v', 1, 0)
    emit('m', 0, 0)
    target_end = len(prog) - 2
    for k, ins in enumerate(prog):
        if ins[0] == 'j' and ins[4] == k:
            ins[4] = target_end
    ng = min(st[1], _RE_MAX_GROUPS)
    return prog, sv0, ng


def _re_pike_add(prog, pc, sb, slen, si, out_threads, seen, work_slots, nslots):
    stk = [(pc, 0, 0)]
    while stk:
        it_pc, sl, prev = stk.pop()
        if it_pc == -1:
            if 0 <= sl < nslots:
                work_slots[sl] = prev
            continue
        if it_pc < 0 or seen[it_pc]:
            continue
        seen[it_pc] = True
        op, _q, _ch, _body, x, y = prog[it_pc]
        if op == 'j':
            stk.append((x, 0, 0))
        elif op == 's':
            stk.append((y, 0, 0))
            stk.append((x, 0, 0))
        elif op == 'b':
            if si == 0:
                stk.append((it_pc + 1, 0, 0))
        elif op == 'e':
            if si == slen:
                stk.append((it_pc + 1, 0, 0))
        elif op == 'v':
            if 0 <= x < nslots:
                stk.append((-1, x, work_slots[x]))
                work_slots[x] = si
            stk.append((it_pc + 1, 0, 0))
        elif op == 'f':
            pass
        else:
            out_threads.append((it_pc, work_slots[:] if nslots > 0 else None))


def _re_pike_search(prog, start, sb: bytes, min_from: int, nslots: int):
    slen = len(sb)
    nprog = len(prog)
    best_slots = None
    matched = False
    cur = []
    seen = [False] * (nprog + 1)
    for si in range(min_from, slen + 1):
        if not matched:
            work_slots = [-1] * nslots
            _re_pike_add(prog, start, sb, slen, si, cur, seen, work_slots, nslots)
        nxt = []
        next_seen = [False] * (nprog + 1)
        for pc, tslots in cur:
            op, q, ch, body, _x, _y = prog[pc]
            if op == 'm':
                matched = True
                if nslots > 0 and tslots is not None:
                    best_slots = tslots[:]
                break
            if si >= slen or op != 'c':
                continue
            b = sb[si]
            if q == 'l':
                if b != ch:
                    continue
            elif q == 'd':
                pass
            else:
                if not _re_set_match(body, b):
                    continue
            ws = tslots[:] if (nslots > 0 and tslots is not None) else []
            _re_pike_add(prog, pc + 1, sb, slen, si + 1, nxt, next_seen, ws, nslots)
        if si >= slen:
            break
        cur = nxt
        seen = next_seen
        if matched and not cur:
            break
    return matched, best_slots


def re_match(s, pat):
    sb = s.encode('utf-8')
    pb = pat.encode('utf-8')
    try:
        prog, start, _ng = _re_compile_pike(pb)
    except SyntaxError:
        raise RuntimeError('bad regex pattern')
    ok, _ = _re_pike_search(prog, start, sb, 0, 0)
    return ok


def re_find(s, pat):
    sb = s.encode('utf-8')
    pb = pat.encode('utf-8')
    try:
        prog, start, _ng = _re_compile_pike(pb)
    except SyntaxError:
        raise RuntimeError('bad regex pattern')
    ok, slots = _re_pike_search(prog, start, sb, 0, 2)
    if not ok or slots is None or slots[0] < 0 or slots[1] < slots[0]:
        return None
    return ('some', sb[slots[0]:slots[1]].decode('utf-8', errors='replace'))


def re_captures(s, pat):
    sb = s.encode('utf-8')
    pb = pat.encode('utf-8')
    try:
        prog, start, ng = _re_compile_pike(pb)
    except SyntaxError:
        raise RuntimeError('bad regex pattern')
    nslots = 2 * (ng + 1)
    ok, slots = _re_pike_search(prog, start, sb, 0, nslots)
    if not ok or slots is None or slots[0] < 0 or slots[1] < slots[0]:
        return None
    out = []
    for k in range(ng + 1):
        a0, a1 = slots[2 * k], slots[2 * k + 1]
        if a0 >= 0 and a1 >= a0:
            out.append(sb[a0:a1].decode('utf-8', errors='replace'))
        else:
            out.append('')
    return ('some', out)


def re_replace(s, pat, repl):
    sb = s.encode('utf-8')
    pb = pat.encode('utf-8')
    rb = repl.encode('utf-8')
    try:
        prog, start, ng = _re_compile_pike(pb)
    except SyntaxError:
        raise RuntimeError('bad regex pattern')
    slen = len(sb)
    rlen = len(rb)
    nslots = 2 * (ng + 1)
    out = bytearray()
    cur = 0
    search_from = 0
    while search_from <= slen:
        ok, slots = _re_pike_search(prog, start, sb, search_from, nslots)
        if not ok or slots is None:
            break
        m0, m1 = slots[0], slots[1]
        if m0 > cur:
            out.extend(sb[cur:m0])
        ri = 0
        while ri < rlen:
            if rb[ri] == ord('$') and ri + 1 < rlen:
                nxt = rb[ri + 1]
                if nxt == ord('$'):
                    out.append(ord('$'))
                    ri += 2
                    continue
                if 48 <= nxt <= 57:
                    gi = nxt - 48
                    if gi <= ng and slots[2 * gi] >= 0 and slots[2 * gi + 1] >= slots[2 * gi]:
                        out.extend(sb[slots[2 * gi]:slots[2 * gi + 1]])
                    ri += 2
                    continue
            if rb[ri] == ord('\\') and ri + 1 < rlen and 49 <= rb[ri + 1] <= 57:
                gi = rb[ri + 1] - 48
                if gi <= ng and slots[2 * gi] >= 0 and slots[2 * gi + 1] >= slots[2 * gi]:
                    out.extend(sb[slots[2 * gi]:slots[2 * gi + 1]])
                ri += 2
                continue
            out.append(rb[ri])
            ri += 1
        cur = m1
        if m1 == m0:
            if m1 >= slen:
                break
            lead = sb[m1]
            step = 1 if lead < 0x80 else (2 if (lead & 0xE0) == 0xC0 else (3 if (lead & 0xF0) == 0xE0 else 4))
            if m1 + step > slen:
                step = 1
            out.extend(sb[m1:m1 + step])
            cur = m1 + step
            search_from = cur
        else:
            search_from = m1
    if cur < slen:
        out.extend(sb[cur:])
    return bytes(out).decode('utf-8', errors='replace')

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
    # no var base` in the interpreter while the native backend printed it.
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
# The whitespace `trim`, `split()` and the `parse_*` methods agree on, and the
# only line breaks `lines` knows. `crt.h` implements exactly these, byte-wise;
# Python's own `strip`/`split`/`splitlines` also take NBSP, U+2028, U+0085 and
# the information separators, which the native runtime does not.
_WS = ' \t\n\v\f\r'
_WS_RUN = re.compile('[ \t\n\v\f\r]+')
_LINE_BREAK = re.compile('\r\n|\n|\r')
_FLOAT_RE = re.compile(r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z')
_FLOAT_WORD = re.compile(r'[+-]?(?:inf(?:inity)?|nan)\Z', re.IGNORECASE | re.ASCII)
_VEC_METH = {'join', 'push', 'pop', 'first', 'last', 'sort', 'rev', 'ins', 'del', 'slice', 'sum', 'min', 'max', 'enumerate', 'copy', 'zip', 'chunks', 'pos'}
_MAP_METH = {'get', 'has', 'keys', 'del', 'set', 'items', 'vals', 'copy', 'get_or'}

# How many arguments each builtin method takes. The dispatch below reads
# `vals[0]` / `vals[1]`, so a wrong count used to leave the interpreter as a
# CPython `IndexError: list index out of range` -- a host exception printed in a
# BRAT program's place, naming a list nobody wrote. The counts are the checker's
# ("method `set` wants 2 args"), and the native backend refuses the same calls.
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
    except (TypeError, AttributeError, IndexError, KeyError, ValueError, OverflowError):
        # A host exception means the call was wrong for this receiver -- an i64
        # where a separator belongs, a record with no such field, a string where
        # a count belongs. BRAT reports that in its own words; the CPython text
        # names nothing the program has.
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
            if not vals:
                return [w for w in _WS_RUN.split(obj) if w]
            if vals[0] == '':
                raise RuntimeError('empty separator')
            return obj.split(vals[0])
        if meth == 'lines':
            parts = _LINE_BREAK.split(obj)
            if parts[-1] == '':
                parts.pop()
            return parts
        if meth == 'trim':
            return obj.strip(_WS)
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
            t = obj.strip(_WS)
            body = t[1:] if t[:1] in ('+', '-') else t
            if body and all('0' <= ch <= '9' for ch in body):
                n = int(t)
                if not -_I64 <= n < _I64:
                    return ('err', 'number out of range')
                return ('ok', n)
            return ('err', 'not a number')
        if meth == 'replace':
            pat, rep = vals[0], vals[1]
            cnt = (len(obj) + 1) if pat == '' else obj.count(pat)
            if cnt > 0:
                est = len(obj.encode('utf-8')) + cnt * (len(rep.encode('utf-8')) - len(pat.encode('utf-8')))
                if est > MAX_STR:
                    raise RuntimeError('out of memory')
            try:
                out = obj.replace(pat, rep)
            except MemoryError:
                raise RuntimeError('out of memory')
            if len(out.encode('utf-8')) > MAX_STR:
                raise RuntimeError('out of memory')
            return out
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
            if vals[0] == '':
                raise RuntimeError('empty separator')
            h, sep, t = obj.partition(vals[0])
            return [h, t] if sep else None
        if meth == 'copy':
            return obj
        if meth == 'chars':
            return list(obj)
        if meth == 'slice':
            return obj[int(vals[0]):int(vals[1])]
        if meth == 'parse_float':
            t = obj.strip(_WS)
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
            if len(vals) != 1 or not isinstance(vals[0], str):
                raise RuntimeError('no method join')
            sep_s = vals[0]
            sep_b = len(sep_s.encode('utf-8'))
            parts = []
            tot_b = 0
            for idx, x in enumerate(obj):
                sx = _bstr(x)
                tot_b += len(sx.encode('utf-8')) + (sep_b if idx else 0)
                if tot_b > MAX_STR:
                    raise RuntimeError('out of memory')
                parts.append(sx)
            return sep_s.join(parts)
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


def _ffi_decode_str(raw, null_msg, bad_msg):
    if raw is None:
        raise RuntimeError(null_msg)
    if isinstance(raw, bytes):
        if len(raw) > MAX_STR:
            raise RuntimeError('out of memory')
        try:
            return raw.decode('utf-8')
        except UnicodeDecodeError:
            raise RuntimeError(bad_msg) from None
    return raw


def _ffi_returned_str(raw):
    return _ffi_decode_str(raw, 'ffi: C returned NULL where str expected',
                           'ffi: C returned invalid UTF-8 where str expected')


def _ffi_passed_str(raw):
    return _ffi_decode_str(raw, 'ffi: C passed NULL where str expected',
                           'ffi: C passed invalid UTF-8 where str expected')


def _ffi_field_str(raw):
    if raw is None:
        return None
    return _ffi_decode_str(raw, 'ffi: C string field is NULL',
                           'ffi: C string field has invalid UTF-8')


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
            'ptr': ctypes.c_void_p, 'bool': ctypes.c_bool}.get(
                ty, _struct_class(ty) if ty in _STRUCTS else None)

    boxes = []

    def to_c(ty, v, i, variadic_slot=False):
        ptr_name = ty if isinstance(ty, str) and ty.startswith('ptr[') else 'ptr'
        if isinstance(ty, str) and ty.startswith('ptr['):
            ty = 'ptr'      # the element is a promise to BRAT, not to C
        if isinstance(ty, str) and ty.startswith('fn'):
            return _callback_ptr(ty, v, funcs, i, name)
        if ty in _STRUCTS:
            if not isinstance(v, dict):
                raise RuntimeError('ffi: expected struct %s' % ty)
            cls = _struct_class(ty)
            inst = cls()
            is_un = ty in _UNIONS
            flds = _STRUCTS[ty]
            if is_un:
                flds = sorted(flds, key=lambda kv: (0 if kv[1] in ('f32', 'f64') else 1))
            for f, ft in flds:
                if is_un and isinstance(v, dict) and f not in v:
                    continue
                fv = v.get(f) if isinstance(v, dict) else v
                arr = _array_field_spec(ft)
                if arr is not None:
                    elem_t, cnt = arr
                    if not isinstance(fv, list) or len(fv) != cnt:
                        raise RuntimeError('ffi: field %s.%s wants vec of length %d' % (ty, f, cnt))
                    act = _struct_ctype(ft)
                    items = []
                    for xv in fv:
                        cv = to_c(elem_t, xv, i)
                        items.append(cv.value if hasattr(cv, 'value') else cv)
                    setattr(inst, f, act(*items))
                    continue
                # A scalar field assigns through the ctypes descriptor, which
                # refuses a c_float where it declared c_long: unwrap first.
                cv = to_c(ft, fv, i)
                setattr(inst, f, cv.value if hasattr(cv, 'value') else cv)
            return inst
        if ty is None:
            return v   # untyped `...` argument
        if ty == 'bool':
            if not isinstance(v, bool):
                raise RuntimeError('ffi: arg %d of %s wants bool' % (i, name))
            return ctypes.c_bool(v)
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
                  'usize': ctypes.c_size_t, 'bool': ctypes.c_bool,
                  'ptr': ctypes.c_void_p}.get(
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
                raise RuntimeError('ffi: arg %d of %s wants %s' % (i, name, ptr_name))
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
    if ret == 'bool':
        fn.restype = ctypes.c_bool
    elif ret in _FFI_INT:
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
        oenv[oname] = (box.value or 0) if isinstance(box, ctypes.c_void_p) else box.value
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
            arr = _array_field_spec(ft)
            if arr is not None:
                out[f] = [_read_struct_elem(val[k], arr[0]) for k in range(arr[1])]
            elif ft in _STRUCTS:
                out[f] = _struct_val(ft, val)
            elif ft == 'str':
                out[f] = _ffi_field_str(val)
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
    if ret == 'bool':
        return bool(r)
    if ret in _FFI_INT:
        if r > 2**63 - 1:
            raise RuntimeError('ffi: result of %s does not fit i64' % name)
        return r
    if ret == 'str':
        return _ffi_returned_str(r)
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
    ix = _as_index(ix)
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
    ix = _as_index(ix)
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
        return _run_frame(frame, body, env, funcs, fallthrough=None)
    raise NameError(name)


_PLUGINS = {}
_PLUGIN_LIBS = {}
_PLUGIN_SYMBOL_OWNERS = {}
_PLUGIN_SEQ = [0]
_PLUGIN_LOCK = threading.RLock()


def _norm_plugin_slot(raw):
    s = ''.join((raw or '').split())
    if s in ('ownptr', 'moveptr'):
        return 'own ptr'
    if s == 'outptr':
        return 'out ptr'
    if s in ('&ptr', 'refptr'):
        return '&ptr'
    if s.startswith('fn('):
        while True:
            hit = None
            for w, vals in (('thread', ('same', 'any')), ('reenter', ('yes', 'no'))):
                for v in vals:
                    suf = '%s(%s)' % (w, v)
                    if s.endswith(suf):
                        hit = suf
                        break
                if hit:
                    break
            if not hit:
                break
            s = s[:-len(hit)]
    return s


def _sig_parts(text):
    raw = text.strip() if isinstance(text, str) else ''
    open_i = raw.find('(')
    if open_i <= 0 or not raw.endswith(')'):
        raise RuntimeError('call: bad signature `%s`' % (text.replace(' ', '') if isinstance(text, str) else ''))
    depth = 0
    for i in range(open_i, len(raw)):
        if raw[i] == '(':
            depth += 1
        elif raw[i] == ')':
            depth -= 1
            if depth == 0 and i != len(raw) - 1:
                raise RuntimeError('call: bad signature `%s`' % text.replace(' ', ''))
    if depth != 0:
        raise RuntimeError('call: bad signature `%s`' % text.replace(' ', ''))
    ret = _norm_plugin_slot(raw[:open_i])
    rest = raw[open_i + 1:-1]
    params = []
    depth = 0
    start = 0
    for i, ch in enumerate(rest):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        elif ch == ',' and depth == 0:
            piece = _norm_plugin_slot(rest[start:i])
            if piece:
                params.append(piece)
            start = i + 1
    tail = _norm_plugin_slot(rest[start:])
    if tail:
        params.append(tail)
    return ret, params


_CTYPES = {'i64': 'c_longlong', 'f32': 'c_float', 'f64': 'c_double', 'str': 'c_char_p',
           'ptr': 'c_void_p', 'own ptr': 'c_void_p', '&ptr': 'c_void_p', 'out ptr': 'c_void_p'}


def _plugin(name, vals, funcs=None):
    import ctypes
    import _ctypes

    def _close_cdll(cdll):
        if os.name != 'nt':
            return
        h = getattr(cdll, '_handle', None)
        if h and hasattr(_ctypes, 'FreeLibrary'):
            try:
                _ctypes.FreeLibrary(h)
                cdll._handle = 0
            except Exception:
                pass

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
        load_target = vals[0]
        if _MOD_BASE_SET[0] and ('/' in load_target or '\\' in load_target):
            cp = confined_path(load_target)
            if cp is None:
                raise RuntimeError('lib_open: path outside the program directory')
            load_target = cp
        try:
            if os.name == 'nt':
                winmode = 0x00000008 if ('/' in load_target or '\\' in load_target) else 0x00000800
                try:
                    cdll = ctypes.CDLL(load_target, winmode=winmode)
                except TypeError:
                    cdll = ctypes.CDLL(load_target)
            else:
                cdll = ctypes.CDLL(load_target, mode=ctypes.RTLD_LOCAL)
        except OSError as e:
            raise RuntimeError('lib_open: %s' % e)
        abi_map = {}
        try:
            class _CAbiEntry(ctypes.Structure):
                _fields_ = [('name', ctypes.c_char_p), ('sig', ctypes.c_char_p)]
            _abi_head = _CAbiEntry.in_dll(cdll, 'brat_plugin_abi_v1')
            _abi_arr = ctypes.cast(ctypes.addressof(_abi_head), ctypes.POINTER(_CAbiEntry))
            _ai = 0
            while _abi_arr[_ai].name is not None:
                _an = _abi_arr[_ai].name.decode('utf-8', 'replace')
                _as = (_abi_arr[_ai].sig or b'').decode('utf-8', 'replace')
                abi_map[_an] = _as
                _ai += 1
        except (ValueError, AttributeError, OSError):
            abi_map = {}
        with _PLUGIN_LOCK:
            handle = next_id()
            _PLUGIN_LIBS[handle] = {
                'library': cdll,
                'active': 0,
                'closing': False,
                'abi': abi_map,
                'sym_names': {},
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
            entry['sym_names'][symbol] = vals[1]
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
                _close_cdll(entry['library'])
                del _PLUGIN_LIBS[vals[0]]
        return 0
    if len(vals) < 2:
        raise TypeError('call wants a function, a signature and arguments')
    ret, params = _sig_parts(vals[1])
    if (ret not in _CTYPES and ret != 'none') or ret in ('&ptr', 'out ptr'):
        raise RuntimeError('call: unsupported type `%s`' % ret)
    for t in params:
        if t.startswith('fn(') and ')->' in t:
            continue
        if t not in _CTYPES:
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
        sym_name = entry.get('sym_names', {}).get(vals[0])
        abi_sig = entry.get('abi', {}).get(sym_name) if sym_name else None
        if abi_sig is not None:
            def _canon_pslot(s):
                if s in ('own ptr', '&ptr', 'out ptr') or s.startswith('fn('):
                    return 'ptr'
                return s
            call_sig = '%s(%s)' % (_canon_pslot(ret), ','.join(_canon_pslot(t) for t in params))
            if abi_sig != call_sig:
                raise RuntimeError(
                    'call: ABI mismatch on `%s`: plugin exports `%s`, call site wrote `%s`'
                    % (sym_name, abi_sig, call_sig))
        entry['active'] += 1
    try:
        fn.argtypes = [(ctypes.c_void_p if t.startswith('fn(') else getattr(ctypes, _CTYPES[t]))
                       for t in params]
        fn.restype = getattr(ctypes, _CTYPES[ret]) if ret != 'none' else None
        argv = []
        boxes = []
        for idx, (t, v) in enumerate(zip(params, vals[2:]), 1):
            if t.startswith('fn('):
                cb_ptr = _callback_ptr(t, v, funcs if funcs is not None else {}, idx, 'call')
                with _PLUGIN_LOCK:
                    entry.setdefault('callbacks', []).append(cb_ptr)
                argv.append(ctypes.cast(cb_ptr, ctypes.c_void_p))
            elif t == 'out ptr' and isinstance(v, tuple) and v and v[0] == 'addr':
                _tag, oenv, oname, _w, _whole = v
                _scope = next((sc for sc in reversed(oenv) if oname in sc), None)
                cur = _scope.get(oname, 0) if _scope else 0
                if isinstance(cur, float):
                    box = ctypes.c_double(cur)
                else:
                    box = ctypes.c_longlong(int(cur))
                boxes.append((_scope, oname, box))
                argv.append(ctypes.byref(box))
            elif t == 'str' and isinstance(v, str):
                argv.append(v.encode('utf-8'))
            else:
                argv.append(v)
        r = fn(*argv)
        for _scope, oname, box in boxes:
            if _scope is not None:
                _scope[oname] = box.value
        if ret == 'none':
            return 0
        if ret == 'f32':
            return float(r)
        if ret == 'str':
            return _ffi_decode_str(r, 'call: NULL str result',
                                   'call: invalid UTF-8 str result')
        if ret in ('ptr', 'own ptr'):
            return r or 0
        return r
    finally:
        with _PLUGIN_LOCK:
            entry['active'] -= 1
            if entry['closing'] and not entry['active']:
                _close_cdll(entry['library'])
                del _PLUGIN_LIBS[owner]


_AMP = [0]          # >0 while the arguments of an extern call are evaluated


def call(name, args, env, funcs):
    info = funcs.get(name)
    if (isinstance(info, tuple) and info and info[0] == 'extern') or name == 'call':
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
            raise Propagate(('err', vals[1] if len(vals) == 2 else 'assert failed'))
        return True
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
        p0, perr = _fs_path(vals[0], 'read_file')
        if perr is not None:
            return ('err', perr or 'read_file wants a string path')
        path = confined_path(p0)
        if path is None:
            return ('err', 'path outside the program directory')
        fd, ferr = _open_confined_fd(path, False, _MAX_FILE_BYTES, 'file exceeds 64 MiB limit')
        if fd < 0:
            return ('err', _err_text(ferr))
        try:
            chunks = []
            total = 0
            with os.fdopen(fd, 'rb') as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > _MAX_FILE_BYTES:
                        return ('err', 'file exceeds 64 MiB limit')
                    chunks.append(chunk)
            raw = b''.join(chunks)
            if b'\x00' in raw:
                return ('err', 'invalid utf-8')
            return ('ok', raw.decode('utf-8'))
        except UnicodeDecodeError:
            return ('err', 'invalid utf-8')
        except OSError as e:
            return ('err', _os_error_text(e))
    if name == 'read_bytes':
        if len(vals) != 1:
            raise TypeError('read_bytes wants 1 arg')
        p0, perr = _fs_path(vals[0], 'read_bytes')
        if perr is not None:
            return ('err', perr or 'read_bytes wants a string path')
        path = confined_path(p0)
        if path is None:
            return ('err', 'path outside the program directory')
        fd, ferr = _open_confined_fd(path, False, _MAX_READ_BYTES, 'file exceeds 16 MiB byte vector limit')
        if fd < 0:
            return ('err', _err_text(ferr))
        try:
            chunks = []
            total = 0
            with os.fdopen(fd, 'rb') as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > _MAX_READ_BYTES:
                        return ('err', 'file exceeds 16 MiB byte vector limit')
                    chunks.append(chunk)
            return ('ok', list(b''.join(chunks)))
        except OSError as e:
            return ('err', _os_error_text(e))
    if name == 'utf8_bytes':
        if len(vals) != 1:
            raise TypeError('utf8_bytes wants 1 arg')
        if not isinstance(vals[0], str):
            raise RuntimeError('utf8_bytes wants str')
        enc = vals[0].encode('utf-8')
        if len(enc) > _MAX_BYTE_VEC:
            raise RuntimeError('utf8_bytes: string exceeds 16 MiB byte vector limit')
        return list(enc)
    if name == 'utf8_str':
        if len(vals) != 1:
            raise TypeError('utf8_str wants 1 arg')
        if not isinstance(vals[0], list):
            return ('err', 'utf8_str wants a byte vec')
        buf = bytearray()
        for b in vals[0]:
            if isinstance(b, bool) or not isinstance(b, int) or b < 0 or b > 255:
                return ('err', 'utf8_str byte out of range 0..255')
            if b == 0:
                return ('err', 'invalid utf-8')
            buf.append(b)
        try:
            return ('ok', bytes(buf).decode('utf-8'))
        except UnicodeDecodeError:
            return ('err', 'invalid utf-8')
    if name == 'fs_rename':
        if len(vals) != 2:
            raise TypeError('fs_rename wants 2 args')
        p0, perr = _fs_path(vals[0], 'fs_rename')
        if perr is not None:
            return ('err', perr or 'fs_rename wants string paths')
        p1, perr = _fs_path(vals[1], 'fs_rename')
        if perr is not None:
            return ('err', perr or 'fs_rename wants string paths')
        src = confined_path(p0)
        dst = confined_path(p1)
        if src is None or dst is None:
            return ('err', 'path outside the program directory')
        rerr = _confined_rename(src, dst)
        if rerr is not None:
            return ('err', _err_text(rerr))
        return ('ok', 0)
    if name == 'fs_remove':
        if len(vals) != 1:
            raise TypeError('fs_remove wants 1 arg')
        p0, perr = _fs_path(vals[0], 'fs_remove')
        if perr is not None:
            return ('err', perr or 'fs_remove wants a string path')
        path = confined_path(p0)
        if path is None:
            return ('err', 'path outside the program directory')
        rerr = _confined_remove(path)
        if rerr is not None:
            return ('err', _err_text(rerr))
        return ('ok', 0)
    if name == 'sbuf':
        if len(vals) != 0:
            raise TypeError('sbuf wants 0 args')
        return ['sb', bytearray(), 0]
    if name == 'sbuf_add':
        if len(vals) != 2:
            raise TypeError('sbuf_add wants 2 args')
        h, part = vals
        if not (isinstance(h, list) and len(h) == 3 and h[0] == 'sb' and isinstance(h[1], bytearray)):
            raise TypeError('sbuf_add wants a builder')
        if not isinstance(part, str):
            return ('err', 'sbuf_add wants a string')
        pb = part.encode('utf-8')
        if len(h[1]) + len(pb) > _MAX_STR_BYTES:
            return ('err', 'sbuf exceeds maximum string size')
        h[1].extend(pb)
        h[2] += len(part)
        return h[2]
    if name == 'sbuf_len':
        if len(vals) != 1:
            raise TypeError('sbuf_len wants 1 arg')
        h, = vals
        if not (isinstance(h, list) and len(h) == 3 and h[0] == 'sb' and isinstance(h[1], bytearray)):
            raise TypeError('sbuf_len wants a builder')
        return h[2]
    if name == 'sbuf_str':
        if len(vals) != 1:
            raise TypeError('sbuf_str wants 1 arg')
        h, = vals
        if not (isinstance(h, list) and len(h) == 3 and h[0] == 'sb' and isinstance(h[1], bytearray)):
            raise TypeError('sbuf_str wants a builder')
        return ('ok', bytes(h[1]).decode('utf-8'))
    if name == 'tcp_listen':
        if len(vals) != 1:
            raise TypeError('tcp_listen wants 1 arg')
        addr = _parse_addr(vals[0])
        if addr is None:
            return ('err', 'address wants host:port with port 1..65535')
        try:
            s_ = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s_.set_inheritable(False)
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
            conn.set_inheritable(False)
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
            s_.set_inheritable(False)
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
        if type(n) is not int or not 0 <= n <= _RECV_MAX:
            return ('err', 'recv wants 0 <= n <= 1048576')
        fd = vals[0]
        s_ = _sock_get(fd)
        if s_ is None:
            return ('err', 'bad socket slot')
        if not _sock_apply_deadline(fd, s_):
            return ('err', 'socket deadline exceeded')
        try:
            b = s_.recv(n)
        except socket.timeout:
            return ('err', 'socket deadline exceeded')
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
        fd = vals[0]
        s_ = _sock_get(fd)
        if s_ is None:
            return ('err', 'bad socket slot')
        if not _sock_apply_deadline(fd, s_):
            return ('err', 'socket deadline exceeded')
        payload = data.encode('utf-8')
        try:
            s_.sendall(payload)
        except socket.timeout:
            return ('err', 'socket deadline exceeded')
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
        if not isinstance(vals[1], str):
            return ('err', 'write_file wants string path and text')
        p0, perr = _fs_path(vals[0], 'write_file')
        if perr is not None:
            return ('err', perr or 'write_file wants string path and text')
        path = confined_path(p0)
        if path is None:
            return ('err', 'path outside the program directory')
        enc = vals[1].encode('utf-8')
        fd, ferr = _open_confined_fd(path, True, 0, '')
        if fd < 0:
            return ('err', _err_text(ferr))
        try:
            with os.fdopen(fd, 'wb') as f:
                f.write(enc)
            return ('ok', len(enc))
        except OSError as e:
            return ('err', _os_error_text(e))
    if name == 'write_bytes':
        if len(vals) != 2:
            raise TypeError('write_bytes wants 2 args')
        if not isinstance(vals[1], list):
            return ('err', 'write_bytes wants string path and byte vec')
        p0, perr = _fs_path(vals[0], 'write_bytes')
        if perr is not None:
            return ('err', perr or 'write_bytes wants string path and byte vec')
        for b in vals[1]:
            if isinstance(b, bool) or not isinstance(b, int) or b < 0 or b > 255:
                return ('err', 'write_bytes byte out of range 0..255')
        path = confined_path(p0)
        if path is None:
            return ('err', 'path outside the program directory')
        raw = bytes(vals[1])
        fd, ferr = _open_confined_fd(path, True, 0, '')
        if fd < 0:
            return ('err', _err_text(ferr))
        try:
            with os.fdopen(fd, 'wb') as f:
                f.write(raw)
            return ('ok', len(raw))
        except OSError as e:
            return ('err', _os_error_text(e))
    if name == 'proc_run':
        if len(vals) not in (2, 3):
            raise TypeError('proc_run wants 2-3 args')
        cmd, argv = vals[0], vals[1]
        tmo_arg = vals[2] if len(vals) == 3 else 0
        if not isinstance(cmd, str) or not cmd:
            return ('err', 'proc_run wants non-empty command string')
        if not isinstance(argv, list):
            return ('err', 'proc_run wants vec[str] args')
        if type(tmo_arg) is not int:
            return ('err', 'proc_run timeout must be an integer (ms)')
        if '\x00' in cmd:
            return ('err', 'proc_run nul byte in argument')
        argv_exec = []
        for x in argv:
            if isinstance(x, str):
                if '\x00' in x:
                    return ('err', 'proc_run nul byte in argument')
                argv_exec.append(x)
            elif isinstance(x, list):
                raw = bytearray()
                for b in x:
                    if isinstance(b, bool) or not isinstance(b, int) or b < 0 or b > 255:
                        return ('err', 'proc_run byte out of range 0..255')
                    if b == 0:
                        return ('err', 'proc_run nul byte in argument')
                    raw.append(b)
                if os.name == 'nt':
                    try:
                        argv_exec.append(bytes(raw).decode('utf-8'))
                    except UnicodeDecodeError:
                        return ('err', 'proc_run invalid utf-8 in argument')
                else:
                    argv_exec.append(bytes(raw).decode('utf-8', 'surrogateescape'))
            else:
                return ('err', 'proc_run wants vec[str] args')
        if tmo_arg <= 0:
            env_tmo = os.environ.get('BRAT_PROC_TIMEOUT_MS', '').strip()
            try:
                parsed_tmo = int(env_tmo) if env_tmo else 60000
            except ValueError:
                parsed_tmo = 60000
            timeout_ms = parsed_tmo if 0 < parsed_tmo <= 3600000 else 60000
        else:
            timeout_ms = min(tmo_arg, 3600000)
        exec_env = os.environ.get('BRAT_EXEC', '').strip().lower()
        allowed = exec_env in ('1', 'yes', 'true', 'on')
        if not allowed:
            allow_raw = os.environ.get('BRAT_EXEC_ALLOW', '')
            has_sep = ('/' in cmd or '\\' in cmd)
            for item in allow_raw.split(','):
                tok = item.strip()
                if not tok:
                    continue
                if '/' in tok or '\\' in tok:
                    if cmd == tok:
                        allowed = True
                        break
                elif not has_sep and cmd == tok:
                    allowed = True
                    break
        if not allowed:
            return ('err', 'proc_run refused: set BRAT_EXEC=1 or BRAT_EXEC_ALLOW')
        exe_path = cmd
        if os.name != 'nt' and '/' not in cmd and '\\' not in cmd:
            path_env = os.environ.get('PATH') or '/usr/local/bin:/usr/bin:/bin'
            resolved = None
            for d in path_env.split(':'):
                if not d.startswith('/'):
                    continue
                cand = os.path.join(d, cmd)
                if os.path.isfile(cand) and os.access(cand, os.X_OK):
                    resolved = cand
                    break
            if resolved is None:
                return ('err', 'proc_run executable not found: ' + cmd)
            exe_path = resolved
        return _run_proc_bounded([exe_path] + argv_exec, exe_path, timeout_ms)
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
                        return _run_frame(frame, body, env, funcs)
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
        return _run_frame(frame, body, env, funcs, params)
    if name == 'chan':
        if len(vals) != 1 or not isinstance(vals[0], int) or isinstance(vals[0], bool):
            raise TypeError('chan wants 1 int arg')
        if vals[0] < 1:
            return ('err', 'chan: capacity must be at least 1')
        if vals[0] > 1048576:
            return ('err', 'chan: capacity too large')
        with _CHAN_BUDGET_LOCK:
            if _CHAN_TOTAL_SLOTS[0] + vals[0] > _MAX_TOTAL_CHAN_SLOTS:
                return ('err', 'chan: total channel capacity budget exceeded')
            _CHAN_TOTAL_SLOTS[0] += vals[0]
        return _Chan(vals[0])
    if name == 'join_all':
        if vals:
            raise TypeError('join_all wants 0 args')
        return join_all()
    if name == 'args':
        if vals:
            raise TypeError('args wants 0 args')
        out = []
        for i, raw in enumerate(_ARGS_BYTES[0]):
            s = _os_text(raw)
            if s is None:
                raise RuntimeError('invalid utf-8 in argument %d; use args_bytes()' % (i + 1))
            out.append(s)
        return out
    if name == 'args_bytes':
        if vals:
            raise TypeError('args_bytes wants 0 args')
        out = []
        for raw in _ARGS_BYTES[0]:
            if len(raw) > _MAX_BYTE_VEC:
                raise RuntimeError('args_bytes: argument exceeds 16 MiB byte vector limit')
            out.append(list(raw))
        return out
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
        if _too_deep(vals[0], skip_closure=False):
            raise RuntimeError('recursion too deep')
        return copy.deepcopy(vals[0])
    if name == '$arena_enter':
        _DEPTH.arena = getattr(_DEPTH, 'arena', 0) + 1
        return 0
    if name == '$arena_leave':
        cur_a = getattr(_DEPTH, 'arena', 0)
        if cur_a <= 0:
            raise RuntimeError('arena stack underflow')
        _DEPTH.arena = cur_a - 1
        return 0
    if name == 'time_ms':
        return int(time.time() * 1000)
    if name == 'mono_ns':
        if len(vals) != 0:
            raise TypeError('mono_ns wants 0 args')
        return int(time.monotonic_ns())
    if name == 'sha256_bytes':
        if len(vals) != 1:
            raise TypeError('sha256_bytes wants 1 arg')
        d = vals[0]
        if isinstance(d, str):
            dig = hashlib.sha256(d.encode('utf-8')).digest()
            return ('ok', list(dig))
        if isinstance(d, list):
            buf = bytearray()
            for el in d:
                if type(el) is not int or not 0 <= el <= 255:
                    return ('err', 'sha256_bytes byte out of range 0..255')
                buf.append(el)
            return ('ok', list(hashlib.sha256(bytes(buf)).digest()))
        return ('err', 'sha256_bytes wants str or byte vec')
    if name == 'random_bytes':
        if len(vals) != 1:
            raise TypeError('random_bytes wants 1 arg')
        nv = vals[0]
        if type(nv) is not int or not 0 <= nv <= 1048576:
            return ('err', 'random_bytes wants 0 <= n <= 1048576')
        return ('ok', list(os.urandom(nv)))
    if name == 'https_request_raw':
        if len(vals) != 4:
            raise TypeError('https_request_raw wants 4 args')
        addr_s, sname, wire, ca_file = vals
        if not all(isinstance(x, str) for x in (addr_s, sname, wire, ca_file)):
            return ('err', 'https_request_raw wants string arguments')
        if '\x00' in addr_s or '\x00' in sname or '\x00' in ca_file:
            return ('err', 'https_request_raw nul byte in argument')
        host, sep, port_s = addr_s.rpartition(':')
        if host.startswith('[') and host.endswith(']') and len(host) > 2:
            host = host[1:-1]
        if sname.startswith('[') and sname.endswith(']') and len(sname) > 2:
            sname = sname[1:-1]
        if not sep or not host or len(host.encode('utf-8')) > 255:
            return ('err', 'address wants host:port with port 1..65535')
        if not (port_s.isascii() and port_s.isdigit()) or not 1 <= int(port_s) <= 65535:
            return ('err', 'address wants host:port with port 1..65535')
        port = int(port_s)
        ca_conf = None
        if ca_file != '':
            ca_conf = confined_path(ca_file)
            if ca_conf is None:
                return ('err', 'path outside the program directory')
        try:
            ctx = ssl.create_default_context(cafile=ca_conf)
        except Exception:
            return ('err', 'failed to load CA file')
        try:
            raw_sock = socket.create_connection((host, port), timeout=_SOCK_TIMEOUT)
            raw_sock.set_inheritable(False)
        except OSError:
            return ('err', 'TLS TCP connect failed')
        try:
            tsock = ctx.wrap_socket(raw_sock, server_hostname=(sname if sname else host))
        except Exception:
            try:
                raw_sock.close()
            except OSError:
                pass
            return ('err', 'TLS handshake or certificate verification failed')
        try:
            tsock.sendall(wire.encode('utf-8'))
            chunks = []
            total = 0
            while True:
                part = tsock.recv(4096)
                if not part:
                    break
                total += len(part)
                if total > (16 << 20):
                    return ('err', 'HTTPS response exceeds 16 MB')
                chunks.append(part)
        except OSError:
            return ('err', 'TLS read/write failed')
        finally:
            try:
                tsock.close()
            except OSError:
                pass
        raw_bytes = b''.join(chunks)
        if b'\x00' in raw_bytes:
            return ('err', 'invalid utf-8')
        try:
            return ('ok', raw_bytes.decode('utf-8'))
        except UnicodeDecodeError:
            return ('err', 'invalid utf-8')
    if name == 'sql_open':
        if len(vals) != 1:
            raise TypeError('sql_open wants 1 arg')
        pv, perr = _fs_path(vals[0], 'sql_open')
        if perr is not None:
            if perr == 'embedded NUL in file path':
                return ('err', 'path outside the program directory')
            return ('err', perr or 'sql_open wants string path')
        if pv == ':memory:':
            use_path = ':memory:'
        else:
            use_path = confined_path(pv)
            if use_path is None:
                return ('err', 'path outside the program directory')
            serr = _verify_sqlite_confined(use_path)
            if serr is not None:
                return ('err', _err_text(serr))
        try:
            conn = sqlite3.connect(use_path, timeout=5.0, isolation_level=None, check_same_thread=False)
        except Exception as e:
            return ('err', _err_text(str(e)))
        if use_path != ':memory:' and not (_FILES_FREE or _is_files_free()):
            try:
                pst = os.lstat(use_path)
                if not stat.S_ISREG(pst.st_mode):
                    conn.close()
                    return ('err', 'path escaped during open')
            except OSError:
                pass
        with _SQL_LOCK:
            live_n = sum(1 for e in _SQL_DBS.values() if not e.closing)
            if live_n >= 127:
                conn.close()
                return ('err', 'too many open sqlite databases')
            sid = _NEXT_SQL[0]
            _NEXT_SQL[0] += 1
            _SQL_DBS[sid] = _SqlEntry(conn)
        return ('ok', sid)
    if name == 'sql_close':
        if len(vals) != 1:
            raise TypeError('sql_close wants 1 arg')
        sv = vals[0]
        if type(sv) is not int:
            return ('err', 'bad sqlite handle')
        to_close = None
        with _SQL_LOCK:
            ent = _SQL_DBS.get(sv)
            if ent is None or ent.closing or ent.conn is None:
                return ('err', 'bad sqlite handle')
            if ent.active == 0:
                to_close = ent.conn
                ent.conn = None
                ent.closing = True
                _SQL_DBS.pop(sv, None)
            else:
                ent.closing = True
        if to_close is not None:
            try:
                to_close.close()
            except Exception:
                pass
        return ('ok', 0)
    if name == 'sql_exec':
        if len(vals) != 2:
            raise TypeError('sql_exec wants 2 args')
        sv, sql_s = vals
        if type(sv) is not int or not isinstance(sql_s, str):
            return ('err', 'sql_exec wants handle and sql string')
        if '\x00' in sql_s:
            return ('err', 'sql_exec nul byte in sql')
        ent = _sql_acquire(sv)
        if ent is None:
            return ('err', 'bad sqlite handle')
        try:
            conn = ent.conn
            before = conn.total_changes
            cur = conn.cursor()
            buf = ''
            for piece in sql_s.splitlines(True):
                buf += piece
                while ';' in buf:
                    semi = buf.find(';')
                    cand = buf[:semi + 1]
                    if sqlite3.complete_statement(cand):
                        if cand.strip():
                            cur.execute(cand)
                        buf = buf[semi + 1:]
                    else:
                        nxt = buf.find(';', semi + 1)
                        while nxt != -1 and not sqlite3.complete_statement(buf[:nxt + 1]):
                            nxt = buf.find(';', nxt + 1)
                        if nxt == -1:
                            break
                        cand = buf[:nxt + 1]
                        if cand.strip():
                            cur.execute(cand)
                        buf = buf[nxt + 1:]
            if buf.strip():
                cur.execute(buf)
            cur.close()
            return ('ok', conn.total_changes - before)
        except Exception as e:
            return ('err', str(e))
        finally:
            _sql_release(sv, ent)
    if name == 'sql_query':
        if len(vals) != 3:
            raise TypeError('sql_query wants 3 args')
        sv, sql_s, pvec = vals
        if type(sv) is not int or not isinstance(sql_s, str) or not isinstance(pvec, list):
            return ('err', 'sql_query wants (handle, sql, params_vec)')
        if '\x00' in sql_s:
            return ('err', 'sql_query nul byte in sql')
        bound = []
        for el in pvec:
            if el is None:
                bound.append(None)
            elif type(el) is bool:
                bound.append(1 if el else 0)
            elif type(el) is int or type(el) is float or isinstance(el, str):
                bound.append(el)
            else:
                return ('err', 'unsupported sqlite parameter type')
        ent = _sql_acquire(sv)
        if ent is None:
            return ('err', 'bad sqlite handle')
        cur = None
        try:
            conn = ent.conn
            cur = conn.cursor()
            cur.execute(sql_s, bound)
            desc = cur.description or ()
            if len(desc) > _MAX_SQL_COLS:
                return ('err', 'sql_query exceeded maximum column limit')
            rows = []
            total_bytes = 0
            while True:
                batch = cur.fetchmany(512)
                if not batch:
                    break
                for r in batch:
                    if len(rows) + 1 > _MAX_SQL_ROWS:
                        return ('err', 'sql_query exceeded maximum row limit')
                    m = {}
                    for i, col in enumerate(desc):
                        cname = col[0] or ''
                        v = r[i]
                        cell_cost = len(cname) + 32
                        if isinstance(v, (bytes, bytearray, memoryview)):
                            raw_b = bytes(v)
                            if len(raw_b) > _MAX_SQL_BLOB:
                                return ('err', 'sql_query blob exceeds 4 MiB limit')
                            cell_cost += len(raw_b) * 16
                            if total_bytes + cell_cost > _MAX_SQL_RESULT_BYTES:
                                return ('err', 'sql_query exceeded result size limit')
                            v = list(raw_b)
                        elif isinstance(v, str):
                            if '\x00' in v:
                                return ('err', 'invalid utf-8 or embedded NUL in sqlite text result')
                            cell_cost += len(v.encode('utf-8'))
                        if total_bytes + cell_cost > _MAX_SQL_RESULT_BYTES:
                            return ('err', 'sql_query exceeded result size limit')
                        total_bytes += cell_cost
                        m[cname] = v
                    rows.append(m)
            return ('ok', rows)
        except Exception as e:
            msg = str(e)
            if msg == 'You can only execute one statement at a time.':
                msg = 'sql_query wants one statement'
            return ('err', msg)
        finally:
            if cur is not None:
                try:
                    cur.close()
                except Exception:
                    pass
            _sql_release(sv, ent)
    if name == 'sleep_ms':
        if len(vals) != 1 or not isinstance(vals[0], (int, float)):
            raise TypeError('sleep_ms wants 1 int arg')
        time.sleep(max(0, vals[0]) / 1000.0)
        return 0
    if name in ('lib_open', 'lib_sym', 'lib_close', 'call'):
        return _plugin(name, vals, funcs)
    if name == 'slice_of_struct':
        if len(vals) != 3:
            raise TypeError('slice_of_struct wants 3 args')
        p0, n0, sname = vals
        if not isinstance(sname, str) or sname not in _STRUCTS:
            raise RuntimeError('slice_of_struct: %s is not an extern struct' % (sname,))
        if n0 < 0:
            raise RuntimeError('slice_of_struct: n must be >= 0')
        if p0 < 0:
            raise RuntimeError('pointer is negative')
        if p0 == 0 and n0 > 0:
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
        if p < 0:
            raise RuntimeError('pointer is negative')
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
        if p < 0:
            raise RuntimeError('pointer is negative')
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
                is_un = tname_ in _UNIONS
                if is_un:
                    ctypes.memset(ctypes.addressof(inst), 0, ctypes.sizeof(inst))
                flds = _STRUCTS[tname_]
                if is_un:
                    flds = sorted(flds, key=lambda kv: (0 if kv[1] in ('f32', 'f64') else 1))
                for f, ft in flds:
                    if f not in v:
                        if is_un:
                            continue
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
        return _ffi_decode_str(ctypes.cast(vals[0], ctypes.c_char_p).value,
                               'null pointer dereference', 'ptr_read_str: invalid UTF-8')
    if name == 'ptr_of_str':
        if len(vals) != 1:
            raise TypeError('ptr_of_str wants 1 arg')
        import ctypes
        b = vals[0].encode('utf-8') if isinstance(vals[0], str) else vals[0]
        buf = _pin_str_buf(b)
        return ctypes.cast(buf, ctypes.c_void_p).value or 0
    if name == 'ptr_write_str':
        if len(vals) != 2:
            raise TypeError('ptr_write_str wants 2 args')
        import ctypes
        if vals[0] <= 0:
            raise RuntimeError('null pointer dereference' if vals[0] == 0 else 'pointer is negative')
        b = vals[1].encode('utf-8') if isinstance(vals[1], str) else vals[1]
        buf = _pin_str_buf(b)
        ctypes.cast(vals[0], ctypes.POINTER(ctypes.c_void_p)).contents.value = ctypes.cast(buf, ctypes.c_void_p).value
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
        msg = os.strerror(int(vals[0]))
        if not _valid_text(msg):
            raise RuntimeError('strerror: invalid UTF-8')
        return msg
    if name == 'env_get':
        if len(vals) != 1:
            raise TypeError('env_get wants 1 arg')
        return _env_lookup(vals[0], False)
    if name == 'env_get_bytes':
        if len(vals) != 1:
            raise TypeError('env_get_bytes wants 1 arg')
        return _env_lookup(vals[0], True)
    if name == 're_match':
        if len(vals) != 2:
            raise TypeError('re_match wants 2 args')
        if not isinstance(vals[0], str) or not isinstance(vals[1], str):
            raise RuntimeError('re_match wants str')
        return re_match(vals[0], vals[1])
    if name == 're_find':
        if len(vals) != 2:
            raise TypeError('re_find wants 2 args')
        if not isinstance(vals[0], str) or not isinstance(vals[1], str):
            raise RuntimeError('re_find wants str')
        return re_find(vals[0], vals[1])
    if name == 're_captures':
        if len(vals) != 2:
            raise TypeError('re_captures wants 2 args')
        if not isinstance(vals[0], str) or not isinstance(vals[1], str):
            raise RuntimeError('re_captures wants str')
        return re_captures(vals[0], vals[1])
    if name == 're_replace':
        if len(vals) != 3:
            raise TypeError('re_replace wants 3 args')
        if not isinstance(vals[0], str) or not isinstance(vals[1], str) or not isinstance(vals[2], str):
            raise RuntimeError('re_replace wants str')
        return re_replace(vals[0], vals[1], vals[2])
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
_MOD_BASE_SET = [False]

# `BRAT_FILES=1` is the runner's own opt-out, never the program's: `0`, `no` or
# a typo must leave the sandbox on. Mirrors `_vfiles_free` in crt.h.
def _is_files_free():
    return os.environ.get('BRAT_FILES', '').strip().lower() in ('1', 'yes', 'true', 'on')

_FILES_FREE = _is_files_free()


_SECRET_VAL_RE = re.compile(
    r'(?:gh[pousr]_[A-Za-z0-9_]{12,}|github_pat_[A-Za-z0-9_]{12,}'
    r'|sk-(?:proj-)?[A-Za-z0-9_-]{12,}|[sr]k_(?:live|test)_[A-Za-z0-9]{12,}'
    r'|xox[bpars]-[A-Za-z0-9-]{12,}|AIza[A-Za-z0-9_-]{12,}'
    r'|(?<![A-Za-z0-9])A(?:KIA|SIA)[A-Z0-9]{16}(?![A-Za-z0-9])'
    r'|(?<![A-Za-z0-9])eyJ[A-Za-z0-9_-]{4,}\.eyJ[A-Za-z0-9_-]{4,}\.[A-Za-z0-9_-]+)'
)


def _env_val_is_secret(val):
    if not val:
        return False
    if ('://' in val and '@' in val) or '-----BEGIN' in val:
        return True
    return _SECRET_VAL_RE.search(val) is not None


def _env_refused(key):
    return ('err', 'env_get refused: %r' % key)


def _env_raw(key):
    if hasattr(os, 'environb'):
        return os.environb.get(os.fsencode(key))
    val = os.environ.get(key)
    return None if val is None else val.encode('utf-8', 'surrogateescape')


def _env_lookup(key, want_bytes):
    if not isinstance(key, str) or '\x00' in key or '=' in key:
        return None
    if os.environ.get('BRAT_ENV_DENY_ALL', '').strip().lower() in ('1', 'true'):
        return _env_refused(key)
    if 'BRAT_ENV_ALLOW' in os.environ:
        allowed = {x.strip() for x in os.environ['BRAT_ENV_ALLOW'].split(',') if x.strip()}
        if key not in allowed:
            return _env_refused(key)
    upper = key.upper()
    for bad in ("SECRET", "KEY", "TOKEN", "PASS", "PASSWORD", "CREDENTIAL",
                "PRIVATE", "DATABASE", "DB_", "AUTH", "DSN", "CONN", "COOKIE",
                "SESSION", "BEARER", "REDIS", "MONGO", "POSTGRES", "MYSQL",
                "AMQP", "AWS_", "GCP_", "AZURE_", "GITHUB_", "GITLAB_",
                "OPENAI_", "ANTHROPIC_", "STRIPE_", "SLACK_", "CLOUDFLARE_",
                "VAULT_", "KUBECONFIG", "WEBHOOK", "SIGNING", "CERT",
                "SALT", "SEED"):
        if bad in upper:
            return _env_refused(key)
    raw = _env_raw(key)
    if raw is None:
        return None
    if _env_val_is_secret(raw.decode('latin-1')):
        return _env_refused(key)
    if want_bytes:
        if len(raw) > _MAX_BYTE_VEC:
            raise RuntimeError('env_get_bytes: value exceeds 16 MiB byte vector limit')
        return list(raw)
    val = _os_text(raw)
    if val is None:
        return ('err', 'env_get invalid utf-8: %r' % key)
    return val


def _open_parent_dir_fd(path):
    base = os.path.realpath(_MOD_BASE[0])
    if not base or base == os.sep:
        return (-1, '', 'path outside the program directory')
    if path == base or not path.startswith(base + os.sep):
        return (-1, '', 'path outside the program directory' if path != base else 'Is a directory')
    rel = path[len(base) + 1:]
    parts = [seg for seg in rel.split(os.sep) if seg]
    if not parts or any(seg in ('.', '..') for seg in parts):
        return (-1, '', 'Is a directory')
    dir_flags = os.O_RDONLY | getattr(os, 'O_CLOEXEC', 0)
    dir_flags |= getattr(os, 'O_DIRECTORY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    try:
        dfd = os.open(base, dir_flags)
    except OSError as e:
        if e.errno == _errno.ELOOP:
            return (-1, '', 'path escaped during open')
        return (-1, '', str(e))
    for seg in parts[:-1]:
        try:
            nfd = os.open(seg, dir_flags, dir_fd=dfd)
        except OSError as e:
            os.close(dfd)
            if e.errno == _errno.ELOOP:
                return (-1, '', 'path escaped during open')
            return (-1, '', str(e))
        os.close(dfd)
        dfd = nfd
    return (dfd, parts[-1], '')


def _open_confined_fd(path, write_mode, max_bytes, limit_msg):
    free_fs = _FILES_FREE or _is_files_free()
    flags = (os.O_WRONLY | os.O_CREAT if write_mode else os.O_RDONLY) | getattr(os, 'O_CLOEXEC', 0)
    flags |= getattr(os, 'O_BINARY', 0)
    if free_fs or os.name == 'nt':
        if not free_fs:
            try:
                lst = os.lstat(path)
                if stat.S_ISLNK(lst.st_mode):
                    return (-1, 'path escaped during open')
            except FileNotFoundError:
                pass
            except OSError as e:
                return (-1, str(e))
        try:
            fd = os.open(path, flags, 0o666)
        except OSError as e:
            return (-1, str(e))
    else:
        dfd, leaf, perr = _open_parent_dir_fd(path)
        if dfd < 0:
            return (-1, perr)
        flags |= getattr(os, 'O_NOFOLLOW', 0)
        try:
            fd = os.open(leaf, flags, 0o666, dir_fd=dfd)
        except OSError as e:
            os.close(dfd)
            if e.errno == _errno.ELOOP:
                return (-1, 'path escaped during open')
            return (-1, str(e))
        os.close(dfd)
    try:
        os.set_inheritable(fd, False)
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode):
            os.close(fd)
            return (-1, 'Is a directory' if stat.S_ISDIR(st.st_mode) else 'not a regular file')
        if not free_fs and sys.platform.startswith('linux'):
            real_fd_path = os.readlink('/proc/self/fd/%d' % fd)
            base = os.path.realpath(_MOD_BASE[0])
            if not (real_fd_path == base or real_fd_path.startswith(base + os.sep)):
                os.close(fd)
                return (-1, 'path escaped during open')
        if not write_mode and max_bytes > 0 and st.st_size > max_bytes:
            os.close(fd)
            return (-1, limit_msg)
        if write_mode:
            os.ftruncate(fd, 0)
        return (fd, '')
    except OSError as e:
        try:
            os.close(fd)
        except OSError:
            pass
        return (-1, str(e))


def _confined_remove(path):
    if _FILES_FREE or _is_files_free() or os.name == 'nt':
        if not (_FILES_FREE or _is_files_free()):
            try:
                if stat.S_ISLNK(os.lstat(path).st_mode):
                    return 'path escaped during open'
            except FileNotFoundError:
                pass
            except OSError as e:
                return str(e)
        try:
            os.remove(path)
            return None
        except OSError as e:
            return str(e)
    dfd, leaf, perr = _open_parent_dir_fd(path)
    if dfd < 0:
        return perr
    try:
        os.unlink(leaf, dir_fd=dfd)
        return None
    except OSError as e:
        return str(e)
    finally:
        os.close(dfd)


def _confined_rename(src, dst):
    if _FILES_FREE or _is_files_free() or os.name == 'nt':
        try:
            os.replace(src, dst)
            return None
        except OSError as e:
            return str(e)
    sfd, sleaf, serr = _open_parent_dir_fd(src)
    if sfd < 0:
        return serr
    dfd, dleaf, derr = _open_parent_dir_fd(dst)
    if dfd < 0:
        os.close(sfd)
        return derr
    try:
        os.replace(sleaf, dleaf, src_dir_fd=sfd, dst_dir_fd=dfd)
        return None
    except OSError as e:
        return str(e)
    finally:
        os.close(sfd)
        os.close(dfd)


def _verify_sqlite_confined(path):
    if _FILES_FREE or _is_files_free():
        return None
    if os.name == 'nt':
        for sfx in ('', '-wal', '-journal', '-shm'):
            try:
                st = os.lstat(path + sfx)
                if not stat.S_ISREG(st.st_mode):
                    return 'path escaped during open'
            except FileNotFoundError:
                pass
            except OSError as e:
                return str(e)
        return None
    dfd, leaf, perr = _open_parent_dir_fd(path)
    if dfd < 0:
        return perr
    try:
        for sfx in ('', '-wal', '-journal', '-shm'):
            try:
                st = os.stat(leaf + sfx, dir_fd=dfd, follow_symlinks=False)
                if not stat.S_ISREG(st.st_mode):
                    return 'path escaped during open'
            except FileNotFoundError:
                pass
        fl = os.O_RDWR | os.O_CREAT | getattr(os, 'O_CLOEXEC', 0) | getattr(os, 'O_NOFOLLOW', 0)
        tfd = os.open(leaf, fl, 0o666, dir_fd=dfd)
        try:
            tst = os.fstat(tfd)
            if not stat.S_ISREG(tst.st_mode):
                return 'path escaped during open'
        finally:
            os.close(tfd)
        return None
    except OSError as e:
        if e.errno == _errno.ELOOP:
            return 'path escaped during open'
        return str(e)
    finally:
        os.close(dfd)


def _run_proc_bounded(argv_full, exe_path, timeout_ms):
    import subprocess as _subp
    max_total = 16 << 20
    if os.name == 'nt':
        try:
            proc = _subp.Popen(
                argv_full, executable=exe_path, shell=False,
                stdin=_subp.DEVNULL, stdout=_subp.PIPE, stderr=_subp.PIPE,
                creationflags=getattr(_subp, 'CREATE_NEW_PROCESS_GROUP', 0),
            )
            out_b, err_b = proc.communicate(timeout=timeout_ms / 1000.0)
        except _subp.TimeoutExpired:
            proc.kill()
            proc.wait()
            return ('err', 'proc_run timed out')
        except OSError as e:
            return ('err', _os_error_text(e))
        if len(out_b) + len(err_b) > max_total:
            return ('err', 'proc_run output exceeded 16 MiB limit')
        if b'\x00' in out_b or b'\x00' in err_b:
            return ('err', 'invalid utf-8')
        try:
            return ('ok', {'code': int(proc.returncode), 'out': out_b.decode('utf-8'), 'err': err_b.decode('utf-8')})
        except UnicodeDecodeError:
            return ('err', 'invalid utf-8')
    try:
        proc = _subp.Popen(
            argv_full, executable=exe_path, shell=False,
            stdin=_subp.DEVNULL, stdout=_subp.PIPE, stderr=_subp.PIPE,
            start_new_session=True,
        )
    except OSError as e:
        return ('err', _os_error_text(e))
    out_fd = proc.stdout.fileno()
    err_fd = proc.stderr.fileno()
    os.set_blocking(out_fd, False)
    os.set_blocking(err_fd, False)
    obuf = bytearray()
    ebuf = bytearray()
    open_fds = {out_fd: obuf, err_fd: ebuf}
    deadline = time.monotonic() + (timeout_ms / 1000.0)
    post_exit_dl = None
    timed_out = False
    over = False
    while open_fds:
        now = time.monotonic()
        if now >= deadline:
            timed_out = True
            break
        if post_exit_dl is None and proc.poll() is not None:
            post_exit_dl = now + 0.05
        if post_exit_dl is not None and now >= post_exit_dl:
            break
        eff = min(deadline, post_exit_dl) if post_exit_dl is not None else deadline
        wait_s = min(0.05, max(0.0, eff - now))
        try:
            rlist, _, _ = select.select(list(open_fds.keys()), [], [], wait_s)
        except OSError as e:
            if e.errno == _errno.EINTR:
                continue
            break
        for rfd in rlist:
            try:
                chunk = os.read(rfd, 65536)
            except BlockingIOError:
                continue
            except OSError:
                open_fds.pop(rfd, None)
                continue
            if not chunk:
                open_fds.pop(rfd, None)
                continue
            if len(obuf) + len(ebuf) + len(chunk) > max_total:
                over = True
                break
            open_fds[rfd].extend(chunk)
        if over:
            break
    proc.stdout.close()
    proc.stderr.close()
    rc = proc.poll()
    if rc is None and not (over or timed_out):
        try:
            rc = proc.wait(timeout=max(0.0, deadline - time.monotonic()))
        except _subp.TimeoutExpired:
            timed_out = True
    if over or timed_out:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            try:
                proc.kill()
            except OSError:
                pass
    if rc is None:
        rc = proc.wait()
    if timed_out:
        return ('err', 'proc_run timed out')
    if over:
        return ('err', 'proc_run output exceeded 16 MiB limit')
    if b'\x00' in obuf or b'\x00' in ebuf:
        return ('err', 'invalid utf-8')
    try:
        return ('ok', {'code': int(rc), 'out': bytes(obuf).decode('utf-8'), 'err': bytes(ebuf).decode('utf-8')})
    except UnicodeDecodeError:
        return ('err', 'invalid utf-8')


def confined_path(p):
    # Non-str or embedded NUL resolves to None: a raw int would otherwise reach
    # open() as an fd, and a NUL byte raises ValueError in os/open calls.
    if not isinstance(p, str) or '\x00' in p:
        return None
    if _FILES_FREE or _is_files_free():
        return p
    base = os.path.realpath(_MOD_BASE[0])
    if not base or base == os.sep:
        return None  # refuse to run with an empty or filesystem-root base
    full = os.path.realpath(os.path.join(base, p))
    if full == base or full.startswith(base + os.sep):
        return full
    return None

_ARGS = [[]]
_ARGS_BYTES = [[]]

_SOCKETS = {}
_SOCKET_DEADLINES = {}
_NEXT_FD = [1]
_SOCK_LOCK = threading.Lock()
_SOCK_TIMEOUT = 30.0      # maximum lifetime of a connected socket
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
        _SOCKET_DEADLINES[fd] = time.monotonic() + _SOCK_TIMEOUT
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
        _SOCKET_DEADLINES.pop(fd, None)
        return _SOCKETS.pop(fd, None)


def _sock_apply_deadline(fd, sock):
    with _SOCK_LOCK:
        if _SOCKETS.get(fd) is not sock:
            return False
        deadline = _SOCKET_DEADLINES.get(fd)
    if deadline is None:
        return False
    left = deadline - time.monotonic()
    if left <= 0:
        return False
    try:
        sock.settimeout(left)
    except (OSError, ValueError):
        return False
    return True


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
    __slots__ = ('cap', 'buf', 'closed', 'cond', 'released')

    def __init__(self, cap):
        self.cap = cap
        self.buf = []
        self.closed = False
        self.cond = threading.Condition()
        self.released = False

    def _release_budget_locked(self):
        if not self.released and self.closed and not self.buf:
            self.released = True
            with _CHAN_BUDGET_LOCK:
                _CHAN_TOTAL_SLOTS[0] = max(0, _CHAN_TOTAL_SLOTS[0] - self.cap)

    def __del__(self):
        if not getattr(self, 'released', True):
            self.released = True
            with _CHAN_BUDGET_LOCK:
                _CHAN_TOTAL_SLOTS[0] = max(0, _CHAN_TOTAL_SLOTS[0] - self.cap)

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
                self._release_budget_locked()
                return None
            v = self.buf.pop(0)
            if self.closed and not self.buf:
                self._release_budget_locked()
            self.cond.notify_all()
            return v

    def close(self):
        with self.cond:
            self.closed = True
            self._release_budget_locked()
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
            if scope is None:
                _POOL['done'].append(handle)
            _POOL['cond'].notify_all()
        handle.done.set()


def _ensure_workers():
    global _SPAWN_WORKERS
    if _SPAWN_WORKERS is None:
        n = min(2 * (os.cpu_count() or 1), 32)
        workers = []
        for _ in range(n):
            t = threading.Thread(target=_spawn_worker, daemon=True)
            try:
                t.start()
            except RuntimeError:
                break
            workers.append(t)
        if not workers:
            raise RuntimeError('spawn failed: could not start worker thread')
        _SPAWN_WORKERS = workers


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
    err = None
    i = 0
    while i < len(handles):
        h = handles[i]
        i += 1
        h.done.wait()
        if err is None and h.err is not None:
            err = h.err
    return err


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
    'bool': 'c_bool',
    'i8': 'c_int8', 'u8': 'c_ubyte', 'i16': 'c_int16', 'u16': 'c_ushort',
    'i32': 'c_int', 'u32': 'c_uint', 'i64': 'c_longlong', 'u64': 'c_ulonglong',
    'usize': 'c_size_t', 'f32': 'c_float', 'f64': 'c_double',
    'str': 'c_char_p', 'ptr': 'c_void_p',
}


def _array_field_spec(ty):
    if not isinstance(ty, str) or '[' not in ty or not ty.endswith(']') or ty.startswith('ptr['):
        return None
    lb = ty.find('[')
    elem, cnt = ty[:lb].strip(), ty[lb + 1:-1].strip()
    if elem and cnt.isdigit() and int(cnt) >= 1:
        return (elem, int(cnt))
    return None


def _struct_ctype(ty):
    import ctypes
    arr = _array_field_spec(ty)
    if arr is not None:
        return _struct_ctype(arr[0]) * arr[1]
    if isinstance(ty, str) and ty in _STRUCT_CTYPES:
        return getattr(ctypes, _STRUCT_CTYPES[ty])
    if isinstance(ty, str) and ty.startswith('ptr['):
        return ctypes.c_void_p      # a typed pointer is still one word wide
    if ty in _STRUCTS:
        return _struct_class(ty)
    return ctypes.c_longlong


def _read_struct_elem(v, elem):
    if elem in _STRUCTS:
        return _read_struct(v, elem)
    if elem == 'str':
        return _ffi_field_str(v)
    if isinstance(v, bytes):
        return v.decode('utf-8')
    if hasattr(v, 'value'):
        return v.value
    return v


def _read_struct(inst, tname_):
    """One level of a C struct as a BRAT struct value, nested included."""
    got = {'$type': tname_}
    for f, ft in _STRUCTS[tname_]:
        v = getattr(inst, f)
        arr = _array_field_spec(ft)
        if arr is not None:
            got[f] = [_read_struct_elem(v[k], arr[0]) for k in range(arr[1])]
        elif ft in _STRUCTS:
            got[f] = _read_struct(v, ft)
        elif ft == 'str':
            got[f] = _ffi_field_str(v)
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
    if name in _PACKED and name not in _UNIONS:
        # `extern packed struct`: ctypes pads by default, which would put the
        # second field at the wrong byte and read the neighbouring one.
        attrs['_pack_'] = 1
    base = ctypes.Union if name in _UNIONS else ctypes.Structure
    cls = type(name, (base,), attrs)
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


_CB_SCALARS = {'bool': 'c_bool', 'i8': 'c_byte', 'u8': 'c_ubyte', 'i16': 'c_short',
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
        arr = _array_field_spec(ft)
        if arr is not None:
            elem_t, cnt = arr
            if isinstance(fv, list) and len(fv) == cnt:
                act = _struct_ctype(ft)
                fv = act(*[(_filled_struct(elem_t, x) if elem_t in _STRUCTS else x) for x in fv])
        elif ft in _STRUCTS:
            fv = _filled_struct(ft, fv)
        elif ft == 'str' and isinstance(fv, str):
            fv = fv.encode('utf-8')
        elif ft == 'ptr' and isinstance(fv, int):
            fv = ctypes.c_void_p(fv)
        elif hasattr(fv, 'value'):
            fv = fv.value
        setattr(inst, f, fv)
    return inst


def _cb_die(msg):
    sys.stderr.write("unhandled: ('err', %r)\n" % (msg,))
    sys.stderr.flush()
    os._exit(1)


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
        out.append('typedef union {' if name in _UNIONS else ('typedef struct%s {' % pack))
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
    for i, (t, (_off, _size, _big)) in enumerate(zip(ptypes, slots)):
        out.append('    { %s t = a%d; memcpy(in + %d, &t, sizeof t); }'
                   % (_cb_c_slot(t), i, _off))
    out.append('    __brat_fill(out, in);')
    out.append('    __brat_s_%s r; memcpy(&r, out, sizeof r); return r;' % ret)
    out.append('}')
    return '\n'.join(out) + '\n'


def _cb_shim_dir():
    import stat
    import tempfile
    uid_tag = str(os.getuid()) if hasattr(os, 'getuid') else 'win'
    cand = os.path.join(tempfile.gettempdir(), 'brat-cb-shim-%s' % uid_tag)
    try:
        os.makedirs(cand, mode=0o700, exist_ok=True)
        st = os.lstat(cand)
        if not stat.S_ISDIR(st.st_mode) or stat.S_ISLNK(st.st_mode):
            raise OSError('not a private directory')
        if hasattr(os, 'getuid') and st.st_uid != os.getuid():
            raise OSError('foreign directory owner')
        if os.name != 'nt' and (st.st_mode & 0o077) != 0:
            os.chmod(cand, 0o700)
        return cand
    except OSError:
        return tempfile.mkdtemp(prefix='brat-cb-shim-')


def _cb_shim_lib(src):
    import hashlib
    import subprocess
    key = hashlib.sha1(src.encode('utf-8')).hexdigest()[:20]
    if key in _CB_SHIMS:
        return _CB_SHIMS[key]
    d = _cb_shim_dir()
    so = os.path.join(d, 'shim-%s.so' % key)
    if not os.path.exists(so):
        pid = os.getpid()
        cpath = os.path.join(d, 'shim-%s.%d.c' % (key, pid))
        tmp_so = '%s.tmp.%d' % (so, pid)
        with open(cpath, 'w', encoding='utf-8') as f:
            f.write(src)
        cc = os.environ.get('CC') or 'cc'
        try:
            r = subprocess.run([cc, '-shared', '-fPIC', '-O1', '-w', cpath, '-o', tmp_so],
                               capture_output=True, text=True, timeout=120)
        except OSError as e:
            try:
                os.unlink(cpath)
            except OSError:
                pass
            raise RuntimeError("ffi: a callback returning a struct by value needs a C "
                               "compiler to reach C; `%s` could not run (%s): run "
                               "`brat build`" % (cc, e))
        try:
            os.unlink(cpath)
        except OSError:
            pass
        if r.returncode != 0:
            try:
                os.unlink(tmp_so)
            except OSError:
                pass
            raise RuntimeError(
                "ffi: a callback returning a struct by value needs a C trampoline and it "
                "did not compile -- the record `%s` is not laid out the same way twice, so "
                "returning it would be a guess: run `brat build`\n%s"
                % (src.split('__brat_s_')[1].split(';')[0], (r.stderr or '').strip().splitlines()[:3]))
        os.replace(tmp_so, so)
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
        try:
            brat_args = []
            for t, (o, size, _big) in zip(ptypes, slots):
                raw = _cb_ctype(t).from_buffer_copy(ctypes.string_at(inp + o, size))
                if isinstance(t, str) and t in _STRUCTS:
                    brat_args.append(('num', _read_struct(raw, t)))
                elif t == 'str':
                    brat_args.append(('str', _ffi_passed_str(raw.value)))
                elif t == 'ptr':
                    brat_args.append(('num', raw.value if hasattr(raw, 'value') else (raw or 0)))
                else:
                    brat_args.append(('num', raw.value if hasattr(raw, 'value') else raw))
            inst = _filled_struct(ret, _cb_run(fname, brat_args, funcs))
            ctypes.memmove(outp, ctypes.byref(inst), ctypes.sizeof(inst))
            # A `str` field is a pointer into a bytes object: keep the object alive
            # until C has read the record it points at.
            _CALLBACK_REFS.append(inst)
        except Exception as e:   # noqa: B902 - a callback cannot unwind through C
            _cb_die(str(e))

    ptr = FILL(wrapper)
    _CALLBACK_REFS.append(ptr)
    lib.__brat_cb_bind.argtypes = [ctypes.c_void_p]
    lib.__brat_cb_bind(ctypes.cast(ptr, ctypes.c_void_p))
    return ctypes.cast(lib.__brat_cb_call, ctypes.c_void_p)


def _callback_ptr(sig, fname, funcs, pos=None, where=None):
    import ctypes
    if isinstance(fname, int) and not isinstance(fname, bool) and fname in (0, -1):
        return ctypes.c_void_p(fname)
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
            return _ffi_passed_str(cv)
        if ty == 'ptr':
            return cv.value if hasattr(cv, 'value') else (cv or 0)
        return cv

    def wrapper(*cvals):
        try:
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
            if ret == 'bool':
                return bool(r)
            if ret in ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64', 'usize', 'ptr'):
                return int(r) if not isinstance(r, bool) else int(r)
            if ret in ('f32', 'f64'):
                return float(r)
            if ret == 'str':
                return r.encode('utf-8') if isinstance(r, str) else r
            return None
        except Exception as e:   # noqa: B902 - a callback cannot unwind through C
            _cb_die(str(e))

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
        _STRUCT_CACHE.pop(s[1], None)
        _PACKED.discard(s[1])
        _UNIONS.discard(s[1])
        _STRUCTS[s[1]] = s[2]
        if len(s) > 3 and s[3]:
            _PACKED.add(s[1])
        if len(s) > 4 and s[4]:
            _UNIONS.add(s[1])
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
    return _run_frame(frame, body, env, funcs, params)


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
            ix = _as_index(ix)
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
            err = _scope_join(handles)
            active = sys.exception()
            if err is not None and (active is None or isinstance(active, (Ret, LoopBreak, LoopCont))):
                raise err
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
    _MOD_BASE_SET[0] = True
    raw_argv = list(argv) if argv else []
    _ARGS_BYTES[0] = [_os_arg_bytes(v) for v in raw_argv]
    _ARGS[0] = raw_argv
    _TRAITS.clear()
    _VARIANTS.clear()
    prog = load_source(src, base).prog
    env = [{}]
    funcs = {}
    for s in prog[1]:
        hoist(s, funcs)
    for s in prog[1]:
        u = s[1] if s[0] == 'pub' else s
        if u[0] not in ('fn', 'impl', 'trait', 'enum', 'extern', 'externstruct', 'exportfn'):
            run_stmt(u, env, funcs)
    out = call('main', [], env, funcs) if 'main' in funcs else 0
    join_all()
    return out