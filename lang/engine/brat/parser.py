import re as _re

_seq = 0
_LINES = {}
_KEEP = []
_ENUMS = {}
_STRAY_BRACES = []
_CURLINE = [1]
_ENUMS_REV = {}

_ESC = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'", '0': '\0',
        '{': '{', '}': '}'}

# Names the C emitter spells into the generated source exactly as written
# (an `extern` symbol is the library's symbol; nothing may rename it), so they
# have to be identifiers C can spell too. Anything else would reach `cc` as
# text, not as a name.
_C_IDENT_RE = _re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')
_C_RESERVED = {
    'auto', 'break', 'case', 'char', 'const', 'continue', 'default', 'do',
    'double', 'else', 'enum', 'extern', 'float', 'for', 'goto', 'if', 'int',
    'long', 'register', 'return', 'short', 'signed', 'sizeof', 'static',
    'struct', 'switch', 'typedef', 'union', 'unsigned', 'void', 'volatile',
    'while',
}


def c_ident(c, what, c_kw=False):
    """One token read as a name that reaches generated C.

    A struct name and a field name are written as `bf_<name>`, so only a name the
    BRAT lexer refuses matters there. A function name is the C symbol itself, so
    a C keyword cannot be spelled and is refused as well.
    """
    t = c.peek()
    if t.k != 'ID' or not _C_IDENT_RE.match(t.v) or (c_kw and t.v in _C_RESERVED):
        raise SyntaxError("%s wants a plain name, got '%s' at line %d"
                          % (what, t.v, eline(c)))
    return c.step().v


def unesc(s):
    out = []
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s) and s[i + 1] in _ESC:
            out.append(_ESC[s[i + 1]])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)

def no_nul(node, line):
    """A literal string may not contain NUL: native strings end at it, the interpreter's do not."""
    if isinstance(node, tuple) and node and node[0] in ("str", "rstr") and chr(0) in node[1]:
        raise SyntaxError("string literal contains NUL at line %d" % line)
    if isinstance(node, tuple) and node and node[0] == "bin":
        for part in node[1:]:
            no_nul(part, line)
    return node


def reset_fresh():
    """Start temporary naming over for a new program.

    `fresh` numbers compiler-made temporaries with one counter for the process.
    Without a reset, a tool that processes two programs in one process gives
    the second program different temporary names than a separate run would,
    because the counter kept going. That is a determinism defect, not a
    cosmetic one: the same source must produce the same parsed program.
    """
    global _seq
    _seq = 0


def fresh(tag):
    global _seq
    _seq += 1
    return '@%s%d' % (tag, _seq)


def _mark(n, ln):
    # Iterative: a flat chain `1+1+...+1` is a left-deep tree 9000 levels tall,
    # so the recursive form of this walk was itself a stack overflow -- it ran
    # before any depth bound could look at the tree.
    stack = [n]
    while stack:
        cur = stack.pop()
        if isinstance(cur, tuple) and cur:
            if id(cur) not in _LINES:
                _LINES[id(cur)] = ln
                _KEEP.append(cur)
            kids = cur[1:]
        elif isinstance(cur, list):
            kids = cur
        else:
            continue
        for sub in kids:
            if isinstance(sub, (tuple, list)):
                stack.append(sub)

try:
    from .lexer import Cur, eline, lex
except ImportError:
    from lexer import Cur, eline, lex


# The grammar is recursive, and the text comes from a file or an editor: without
# a bound, `((((1))))` a few thousand deep is a RecursionError at best and a
# crash at worst, and both are worse than a sentence that says what happened.
# Each level costs about ten frames, so the bound is kept well inside the
# interpreter's budget (see `brat/__init__.py`) and well above anything written
# by hand.
MAX_NEST = 200
_nest = {"expression": 0, "block": 0}

# Depth of the *tree*, not of the nesting in the text. `MAX_NEST` bounds how
# deep the parser's own stack may go; it says nothing about `1+1+...+1`, which
# is one nesting level and ten thousand nodes. Every pass after the parser walks
# the tree recursively, so an unbounded depth is a stack overflow in whichever
# pass is deepest -- a SIGSEGV, which cannot be caught and so has to be refused
# here. The bound is far above hand-written code and far below the stack.
MAX_TREE_DEPTH = 1000


def tree_depth(node):
    """Longest root-to-leaf chain in an AST node, computed without recursion."""
    deepest = 0
    stack = [(node, 1)]
    while stack:
        cur, d = stack.pop()
        if isinstance(cur, list):
            # A list is only the container for a node's children (call
            # arguments, a block body), not another tree level. Counting it
            # made this bound one level stricter than the C++ node depth
            # on every call, so the front ends disagreed at the boundary.
            for sub in cur:
                if isinstance(sub, (tuple, list)):
                    stack.append((sub, d))
            continue
        if d > deepest:
            deepest = d
        if isinstance(cur, tuple) and cur:
            kids = cur[1:]
        else:
            continue
        for sub in kids:
            if isinstance(sub, (tuple, list)):
                stack.append((sub, d + 1))
    return deepest


class _Depth:
    """Count nesting for expressions and blocks; refuse before the stack does.

    Carries the cursor so the refusal names a line like every other parse error;
    `at line N` goes into the message, which is where the CLI and `--json` read
    the position from (`parse_error_line`, `_parse_error_line_from`).
    """

    def __init__(self, what="expression", c=None):
        self.what = what
        self.c = c

    def __enter__(self):
        _nest[self.what] += 1
        if _nest[self.what] > MAX_NEST:
            _nest[self.what] -= 1
            raise SyntaxError("%s nested too deeply at line %d"
                              % (self.what, eline(self.c) if self.c is not None else 1))
        return self

    def __exit__(self, *exc):
        _nest[self.what] -= 1
        return False


def parse_prog(c):
    _nest["expression"] = _nest["block"] = 0
    _ENUMS.clear()
    _ENUMS_REV.clear()
    body = []
    while c.peek().k != 'EOF':
        r = parse_stmt(c)
        if isinstance(r, list):
            body.extend(r)
        else:
            body.append(r)
    _refuse_deep_tree(body)
    return ('prog', body)


def _refuse_deep_tree(body):
    """Refuse a tree the later passes cannot walk, naming the line it starts on.

    One statement is measured at a time so the diagnostic can point at it: the
    deepest chain in the program is what every pass after this one recurses
    down, and saying which line began it is the difference between a sentence
    and a post-mortem.
    """
    worst = 0
    worst_line = 1
    for st in body:
        d = tree_depth(st)
        if d > worst:
            worst, worst_line = d, _LINES.get(id(st)) or 1
    if worst > MAX_TREE_DEPTH:
        raise SyntaxError("expression tree too deep (%d levels, limit %d) at line %d"
                          % (worst, MAX_TREE_DEPTH, worst_line))

def parse_stmt(c):
    ln = c.peek().line
    _CURLINE[0] = ln
    r = _parse_stmt(c)
    _mark(r, ln)
    return r


def _parse_stmt(c):
    t = c.peek()
    if t.k == 'ID' and t.v == 'arena' and c.i + 1 < len(c.t) and c.t[c.i + 1].v == '{':
        return parse_arena(c)
    if t.k == 'KW' and t.v == 'let':
        return parse_let(c)
    if t.k == 'KW' and t.v == 'const':
        c.step()
        return parse_let(c, True)
    if t.k == 'KW' and t.v == 'extern':
        c.step()
        block_lib = None
        if c.peek().k == 'STR':
            block_lib = c.step().v.strip('"')
        packed = False
        is_union = False
        if c.peek().k == 'ID' and c.peek().v == 'packed':
            if c.peek(1).v == 'union':
                raise SyntaxError('`packed` wants `struct`, not `union` at line %d' % eline(c))
            if c.peek(1).v == 'struct':
                c.step()          # `extern packed struct`: no padding, 1-byte alignment
                packed = True
        if c.peek().k == 'ID' and c.peek().v in ('struct', 'union'):
            is_union = (c.step().v == 'union')
            kind_lbl = 'extern union' if is_union else 'extern struct'
            name = c_ident(c, '%s name' % kind_lbl)
            c.want('{')
            fields = []
            while c.peek().v != '}':
                fname = c_ident(c, '%s field' % kind_lbl)
                c.want(':')
                toks = []
                depth = 0
                while True:
                    x = c.peek()
                    if x.k == 'EOF' or (x.v in (',', '}') and depth == 0):
                        break
                    if x.v in ('[', '('):
                        depth += 1
                    if x.v in (']', ')'):
                        depth -= 1
                    toks.append(x.v)
                    c.step()
                ann = canon_ann(ann_text(toks))
                fields.append((fname, ann))
                if c.peek().v == ',':
                    c.step()
            c.want('}')
            if is_union:
                return ('externstruct', name, fields, False, True)
            return ('externstruct', name, fields, packed)
        if c.peek().v == '{':
            c.step()
            out = []
            while c.peek().v != '}':
                r = parse_stmt(c)
                if isinstance(r, list):
                    rr = []
                    for node in r:
                        if (isinstance(node, tuple) and node and node[0] == 'extern'
                                and node[1] is None and block_lib is not None):
                            node = (node[0], block_lib) + node[2:]
                        rr.append(node)
                    out.extend(rr)
                else:
                    if (isinstance(r, tuple) and r and r[0] == 'extern'
                            and r[1] is None and block_lib is not None):
                        r = (r[0], block_lib) + r[2:]
                    out.append(r)
            c.want('}')
            return out
        if not (c.peek().k == 'STR' or (c.peek().k == 'KW' and c.peek().v == 'fn')):
            raise SyntaxError('extern wants fn at line %d' % eline(c))
        node = parse_extern(c)
        if (block_lib is not None and isinstance(node, tuple) and node and node[0] == 'extern'
                and node[1] is None):
            node = (node[0], block_lib) + node[2:]
        return node
    if t.k == 'KW' and t.v == 'export':
        c.step()
        if not (c.peek().k == 'KW' and c.peek().v == 'fn'):
            raise SyntaxError('export wants fn at line %d' % eline(c))
        node = parse_fn(c)
        if not (_C_IDENT_RE.match(node[1] or '') and node[1] not in _C_RESERVED):
            raise SyntaxError("export fn wants a plain name, got '%s' at line %d"
                              % (node[1], eline(c)))
        return ('exportfn',) + node[1:]
    if t.k == 'KW' and t.v == 'fn':
        return parse_fn(c)
    if t.k == 'KW' and t.v == 'unsafe':
        c.step()
        c.want('{')
        b = parse_block(c)
        c.want('}')
        return ('unsafe', b)
    if t.k == 'KW' and t.v == 'if':
        return parse_if(c)
    if t.k == 'KW' and t.v == 'while':
        return parse_while(c)
    if t.k == 'KW' and t.v == 'for':
        return parse_for(c)
    if t.k == 'KW' and t.v == 'impl':
        return parse_impl(c)
    if t.k == 'KW' and t.v == 'trait':
        return parse_trait(c)
    if t.k == 'KW' and t.v == 'enum':
        return parse_enum(c)
    if t.k == 'KW' and t.v == 'pub':
        c.step()
        u = c.peek()
        if u.k == 'KW' and u.v == 'fn':
            return ('pub', parse_fn(c))
        if u.k == 'KW' and u.v == 'enum':
            return ('pub', parse_enum(c))
        if u.k == 'KW' and u.v == 'trait':
            return ('pub', parse_trait(c))
        if u.k == 'KW' and u.v == 'impl':
            return ('pub', parse_impl(c))
        if u.k == 'KW' and u.v == 'const':
            c.step()
            return ('pub', parse_let(c, True))
        if u.k == 'KW' and u.v == 'use':
            c.step()
            parts = [c.step().v]
            while c.peek().v == '.':
                c.step()
                parts.append(c.step().v)
            if c.peek().v == ';':
                c.step()
            return ('pubuse', '.'.join(parts))
        raise SyntaxError('pub wants fn/enum/trait/impl/use at line %d' % eline(c))
    if t.k == 'KW' and t.v == 'use':
        raise SyntaxError('use wants pub at line %d' % eline(c))
    if t.k == 'KW' and t.v in ('break', 'continue'):
        c.step()
        if c.peek().v == ';':
            c.step()
        return (t.v,)
    if t.k == 'KW' and t.v == 'match':
        return parse_match(c)
    if t.k == 'KW' and t.v == 'return':
        c.step()
        if c.peek().v in ('}', ';'):
            if c.peek().v == ';':
                c.step()
            return ('return', ('none',))
        e = parse_expr(c)
        if c.peek().v == ';':
            c.step()
        return ('return', e)
    if t.k == 'KW' and t.v == 'try':
        return parse_try(c)
    if t.k == 'KW' and t.v == 'spawn':
        c.step()
        c.want('{')
        b = parse_block(c)
        c.want('}')
        return ('spawn', b)
    if t.k == 'ID' and t.v == 'scope' and c.i + 1 < len(c.t) and c.t[c.i + 1].v == '{':
        # `scope { }` is a block with a join at the end, not a keyword: the
        # name stays usable as an ordinary binding.
        c.step()
        c.step()
        b = parse_block(c)
        c.want('}')
        return ('scope', b)
    if t.k == 'KW' and t.v == 'defer':
        c.step()
        e = parse_expr(c)
        if c.peek().v == ';':
            c.step()
        return ('defer', e)
    if t.k == 'KW' and t.v == 'import':
        c.step()
        parts = [c.step().v]
        while c.peek().v == '.':
            c.step()
            parts.append(c.step().v)
        if c.peek().v == ';':
            c.step()
        return ('import', '.'.join(parts))
    if t.k == 'ID' and c.i + 1 < len(c.t) and c.t[c.i + 1].v == '=':
        name = c.step().v
        c.step()
        e = parse_expr(c)
        if c.peek().v == ';':
            c.step()
        return ('set', name, e)
    if t.k == 'ID' and c.i + 1 < len(c.t) and c.t[c.i + 1].v in ('+=', '-=', '*=', '/=', '%='):
        name = c.step().v
        op = c.step().v[0]
        e = parse_expr(c)
        if c.peek().v == ';':
            c.step()
        return ('set', name, ('bin', op, ('var', name), e))
    e = parse_expr(c)
    if c.peek().v == '=' or c.peek().v in ('+=', '-=', '*=', '/=', '%='):
        op = c.step().v
        val = parse_expr(c)
        if c.peek().v == ';':
            c.step()
        if e[0] == 'idx':
            if op != '=':
                val = ('bin', op[0], ('idx', e[1], e[2]), val)
            return ('setidx', e[1], e[2], val)
        if e[0] == 'field':
            if op != '=':
                val = ('bin', op[0], e, val)
            return ('setfield', e[1], e[2], val)
        raise SyntaxError('bad assign target at line %d' % eline(c))
    if c.peek().v == ';':
        c.step()
    return ('expr', e)

def parse_block(c):
    with _Depth("block", c):
        return _parse_block(c)


def _parse_block(c):
    stmts = []
    while c.peek().v != '}':
        r = parse_stmt(c)
        if isinstance(r, list):
            stmts.extend(r)
        else:
            stmts.append(r)
    return stmts

def parse_let(c, const=False):
    if not const:
        c.want('let')
    mut = False
    if c.peek().v == 'mut':
        c.step()
        mut = True
    if const and mut:
        raise SyntaxError('const cannot be mut at line %d' % eline(c))
    if c.peek().v == '[':
        if const:
            raise SyntaxError('const destructure unsupported at line %d' % eline(c))
        c.step()
        names = []
        while c.peek().v != ']' and c.peek().k != 'EOF':
            names.append(c.step().v)
            if c.peek().v == ',':
                c.step()
        c.want(']')
        c.want('=')
        e = parse_expr(c)
        if c.peek().v == ';':
            c.step()
        tm = fresh('d')
        out = [('let', tm, e, False)]
        for i, name in enumerate(names):
            if name == '_':
                continue
            out.append(('let', name, ('idx', ('var', tm), ('num', i)), mut))
        return out
    name = c.step().v
    ann = None
    if c.peek().v == ':':
        c.step()
        toks = []
        depth = 0
        while True:
            x = c.peek()
            if x.k == 'EOF' or (x.v == '=' and depth == 0):
                break
            if x.v == '[':
                depth += 1
            if x.v == ']':
                depth -= 1
            toks.append(x.v)
            c.step()
        ann = ann_text(toks)
    c.want('=')
    e = parse_expr(c)
    if c.peek().v == ';':
        c.step()
    if not const:
        return ('let', name, e, mut, False, ann)
    return ('let', name, e, False, True, ann)

def ann_text(toks):
    if toks and toks[0] in ('move', 'mut', 'ref'):
        toks = toks[1:]
    return ''.join(toks) if toks else None


# C spellings for a slot whose width BRAT already names, so a signature copied
# out of a header is accepted as written instead of being refused for its
# spelling. Only names where both tables agree on the width are here: a
# `int16_t` is deliberately absent, because `brat bindgen` widens it to `i32`
# and a hand-written `i16` would then disagree with a generated one.
C_FFI_ALIAS = {
    "size_t": "usize",
    "ssize_t": "i64", "ptrdiff_t": "i64", "intptr_t": "i64", "uintptr_t": "u64",
    "int64_t": "i64", "uint64_t": "u64", "int32_t": "i32", "uint32_t": "u32",
}


def canon_ann(ann):
    """C spelling -> the BRAT name for the same width (`size_t` -> `usize`)."""
    return C_FFI_ALIAS.get(ann, ann) if isinstance(ann, str) else ann


def parse_params(c, allow_varargs=False):
    params = []
    varargs = None
    while c.peek().v != ')' and c.peek().k != 'EOF':
        if allow_varargs and c.peek().v == '..':
            c.step()
            toks = []
            depth = 0
            while c.peek().k != 'EOF' and not (c.peek().v in (',', ')') and depth == 0):
                x = c.peek()
                if x.v in ('[', '('):
                    depth += 1
                if x.v in (']', ')'):
                    depth -= 1
                toks.append(x.v)
                c.step()
            varargs = ann_text(toks)
            if c.peek().v == ',':
                c.step()
            continue
        pn = c.step().v
        mode = 'ref'
        if pn in ('move', 'mut', 'ref') and c.peek().v != ':':
            mode = pn
            pn = c.step().v
        ann = None
        if c.peek().v == ':':
            c.step()
            depth = 0
            first = None
            toks = []
            while True:
                x = c.peek()
                if x.k == 'EOF' or (x.v in (',', ')', '=') and depth == 0):
                    break
                if x.v in ('[', '('):
                    depth += 1
                if x.v in (']', ')'):
                    depth -= 1
                if allow_varargs and depth == 0 and x.v in EFFECTS:
                    raise SyntaxError('`%s` is an effect of %s, not a parameter type at line %d'
                                      % (x.v, pn, eline(c)))
                if first is None and depth == 0 and x.k in ('ID', 'KW'):
                    first = x.v
                toks.append(x.v)
                c.step()
            if first in ('move', 'mut', 'ref'):
                mode = first
            ann = ann_text(toks)
        default = None
        if c.peek().v == '=':
            c.step()
            default = parse_expr(c)
        params.append((pn, mode, ann, default))
        if c.peek().v == ',':
            c.step()
    seen_default = False
    for p in params:
        if p[3] is not None:
            seen_default = True
        elif seen_default:
            raise SyntaxError('param without default after default at line %d' % eline(c))
    if allow_varargs:
        return params, varargs
    return params


def parse_fn(c):
    c.want('fn')
    name = c.step().v
    tparams = []
    if c.peek().v == '<':
        c.step()
        while c.peek().v != '>':
            t = c.step()
            if t.k not in ('ID', 'KW'):
                raise SyntaxError("want type param have '%s' at line %d" % (t.v, t.line))
            tparams.append(t.v)
            if c.peek().v == ',':
                c.step()
        c.want('>')
    if len(set(tparams)) != len(tparams):
        raise SyntaxError('duplicate type param at line %d' % eline(c))
    c.want('(')
    params = parse_params(c)
    c.want(')')
    ret = None
    if c.peek().v == '->':
        c.step()
        if c.peek().v in ('move', 'ref'):
            if c.step().v == 'ref':
                raise SyntaxError('ref returns dangle; return owned at line %d' % eline(c))
        toks = []
        while c.peek().v != '{' and c.peek().k != 'EOF':
            toks.append(c.step().v)
        ret = ann_text(toks)
    c.want('{')
    b = parse_block(c)
    c.want('}')
    return ('fn', name, params, b, tparams, ret)


# Effects an `extern fn` may declare. They are the only promise the language can
# make about a C function, and every one of them is something a checker or the
# emitter can use: `pure` (no writes through arguments, no state) keeps a call out
# of the sharing rule and out of an optimizer's way, `nothrow` says the callee
# never parks a failure in the error channel, `reads(x)`/`writes(x)` name the
# parameter whose memory the callee touches. Without them the checker treats every
# foreign call as "touches nothing it can see", which is how FFI races used to be
# nobody's problem (docs/project/LEDGER.md, 2026-10-30).
EFFECTS = ("pure", "nothrow", "reads", "writes", "borrows")
EFFECT_ADDRESSED = ("reads", "writes", "borrows")


# What a C function may do with the callback it is handed. Both are properties of
# the callee, not of the slot, which is why they are written on the callback
# parameter and stored on the declaration: libuv calls every callback of a
# handle on the loop thread, and `sqlite3_exec` may re-enter a callback from
# inside `sqlite3_step`. Defaults are the conservative pair -- the callee gets the
# caller's thread and may re-enter -- because a promise not made is a fact not
# known (docs/guide/ffi.md, "Callback contracts").
CONTRACTS = {"thread": ("same", "any"), "reenter": ("yes", "no")}
_CONTRACT_TAIL = _re.compile(r'([a-z_]+)\(([a-z_]+)\)$')


def effect_text(eff):
    """`writes:buf` -> `writes(buf)`, `pure` -> `pure`, for a diagnostic."""
    return eff.replace(":", "(", 1) + ")" if ":" in eff else eff


_C_TYPE_WORDS = {
    'const', 'volatile', 'restrict', 'signed', 'unsigned', 'short', 'long',
    'int', 'char', 'float', 'double', 'void', 'bool', '_Bool', 'struct',
    'union', 'enum',
}


def _take_balanced_type(c, toks):
    stack = []
    while c.peek().k != 'EOF':
        x = c.step().v
        toks.append(x)
        if x == '[':
            stack.append(']')
        elif x == '(':
            stack.append(')')
        elif x == '{':
            stack.append('}')
        elif x in (']', ')', '}'):
            if stack and stack[-1] == x:
                stack.pop()
            else:
                break
        if not stack:
            break


def _take_type_postfix(c, toks, head):
    map_value = False
    while True:
        if c.peek().v == '[':
            _take_balanced_type(c, toks)
            if head == 'map' and not map_value:
                _take_type_toks(c, toks)
                map_value = True
            continue
        if c.peek().v == '*':
            toks.append(c.step().v)
            continue
        break


def _take_type_toks(c, toks=None):
    """Take exactly one type spelling after an `extern` result arrow.

    The old collector read until a keyword it happened to know, so a
    following top-level statement became part of the annotation:
    `-> bool` followed by `print(x)` was the type `boolprint(x)`.
    Types are still carried as glued token text; this only decides
    where one type ends.
    """
    if toks is None:
        toks = []
    if c.peek().k == 'ID' and (c.peek().v in EFFECTS or c.peek().v in CONTRACTS):
        return toks
    while c.peek().v in ('move', 'ref', 'mut'):
        toks.append(c.step().v)
    x = c.peek()
    if x.k == 'EOF' or x.v in ('{', '}', ';', ',', ')', ']'):
        return toks
    if c.peek().k == 'ID' and (x.v in EFFECTS or x.v in CONTRACTS):
        return toks
    if x.v == 'fn':
        toks.append(c.step().v)
        if c.peek().v == '(':
            _take_balanced_type(c, toks)
        if c.peek().v == '->':
            toks.append(c.step().v)
            _take_type_toks(c, toks)
        _take_type_postfix(c, toks, '')
        return toks
    if x.v == '(':
        _take_balanced_type(c, toks)
        _take_type_postfix(c, toks, '')
        return toks
    if x.v in _C_TYPE_WORDS:
        if x.v in ('struct', 'union', 'enum'):
            toks.append(c.step().v)
            if c.peek().k in ('ID', 'KW'):
                toks.append(c.step().v)
        else:
            while c.peek().v in _C_TYPE_WORDS and c.peek().v not in ('struct', 'union', 'enum'):
                toks.append(c.step().v)
        _take_type_postfix(c, toks, '')
        return toks
    if x.k in ('ID', 'KW'):
        head = c.step().v
        toks.append(head)
        while c.peek().v == '.' and c.peek(1).k in ('ID', 'KW'):
            toks.append(c.step().v)
            toks.append(c.step().v)
            head = ''
        _take_type_postfix(c, toks, head)
        return toks
    toks.append(c.step().v)
    return toks


def parse_extern(c):

    lib = None
    if c.peek().k == 'STR':
        lib = c.step().v.strip('"')
    if not (c.peek().k == 'KW' and c.peek().v == 'fn'):
        raise SyntaxError('extern wants fn at line %d' % eline(c))
    c.step()  # fn
    name = c_ident(c, 'extern fn name', c_kw=True)
    c.want('(')
    params, varargs = parse_params(c, allow_varargs=True)
    c.want(')')
    ret = None
    owned = False
    checked = False
    if c.peek().v == '->':
        c.step()
        toks = _take_type_toks(c)
        owned = bool(toks) and toks[0] == 'move'
        if owned:
            toks = toks[1:]
        checked = (c.peek().k == 'ID' and c.peek().v == 'errno'
                   and c.peek(1).v != '(')
        if checked:
            c.step()
            if ann_text(toks) not in ('i32', 'i64', 'ptr'):
                raise SyntaxError('errno wants an i32, i64 or ptr result at line %d' % eline(c))
        if toks:
            ret = canon_ann(ann_text(toks))
    if c.peek().v == ';':
        c.step()
    params = [(n, m, canon_ann(a), d) for (n, m, a, d) in params]
    varargs = canon_ann(varargs)
    contracts = take_contracts(name, params, eline(c))
    effects = parse_effects(c, name, params, owned, checked, ret)
    if c.peek().v == ';':
        c.step()
    return ('extern', lib, name, params, ret, varargs, owned, checked, effects, contracts)


def _points_at_memory(ann):
    """Whether an annotation is something a foreign call can reach memory through."""
    if not isinstance(ann, str):
        return False
    return ann.startswith("ptr") or ann.startswith("str") or ann.endswith("]")


def take_contracts(name, params, lineno):
    """Strip `thread(...)`/`reenter(...)` off a parameter and return them.

    Written where the reader looks for it -- on the callback parameter -- but
    stored on the declaration, because the threading of a callback is the
    callee's behaviour, not the slot's: libuv calls every callback of a handle on
    the loop thread, and one slot is no more "any thread" than another.

    A contract left inside the type would travel into the C prototype as part of
    a type name, which is the kind of thing that compiles on one ABI and lies on
    another. The types here are already canonical, and `canon_ann` glues tokens
    with nothing between them (`fn()->none` + `thread(same)` is
    `fn()->nonethread(same)`), so the words are matched as literal suffixes
    rather than by splitting on whitespace.
    """
    out = []
    for idx, prm in enumerate(params):
        ann = prm[2]
        if not isinstance(ann, str):
            continue
        found = []
        while True:
            hit = None
            for word, values in CONTRACTS.items():
                for value in values:
                    suffix = '%s(%s)' % (word, value)
                    if ann.endswith(suffix):
                        hit = (word, value, suffix)
                        break
                if hit:
                    break
            if not hit:
                break
            word, value, suffix = hit
            rest = ann[:-len(suffix)]
            if not rest.startswith('fn('):
                raise SyntaxError('`%s(%s)` wants a callback parameter, `%s` is %s at line %d'
                                  % (word, value, prm[0], rest, lineno))
            found.append('%s:%s' % (word, value))
            ann = rest
        leftover = _CONTRACT_TAIL.search(ann)
        if leftover and ann.startswith('fn(') and '->' in ann and not ann.endswith('>'):
            raise SyntaxError('`%s(%s)` is not a callback contract (`thread`, `reenter`) '
                              'on %s at line %d'
                              % (leftover.group(1), leftover.group(2), name, lineno))
        for contract in reversed(found):
            if any(c.split(':', 1)[0] == contract.split(':', 1)[0] for c in out):
                raise SyntaxError('`%s` declared twice on %s at line %d'
                                  % (contract.split(':', 1)[0], name, lineno))
            out.append(contract)
        params[idx] = (prm[0], prm[1], ann, prm[3])
    return tuple(out)


def parse_effects(c, name, params, owned, checked, ret=None):
    """`pure nothrow writes(buf) borrows(p)` -- the promise an `extern fn` makes about C.

    Parsed and validated here rather than in a checker, so both front ends refuse
    a contradictory declaration with one message: the parser is the only stage
    whose output is compared byte for byte by construction.
    """
    out = []
    pnames = [p[0] for p in params]
    while c.peek().k == 'ID' and c.peek().v in EFFECTS:
        word = c.step().v
        if word in EFFECT_ADDRESSED:
            if c.peek().v != '(':
                raise SyntaxError('%s wants a parameter name, `%s(p)` at line %d'
                                  % (word, word, eline(c)))
            c.step()
            arg = c.peek().v
            if c.peek().k not in ('ID',) or arg not in pnames:
                raise SyntaxError('`%s(%s)` names no parameter of %s (params: %s) at line %d'
                                  % (word, arg, name, ', '.join(pnames) or 'none', eline(c)))
            ann = params[pnames.index(arg)][2]
            if not _points_at_memory(ann):
                # An effect on an `i64` is a promise about nothing: the callee has
                # no address to reach through it.
                raise SyntaxError('`%s(%s)` wants a pointer parameter, `%s` is %s at line %d'
                                  % (word, arg, arg, ann or 'untyped', eline(c)))
            c.step()
            c.want(')')
            out.append('%s:%s' % (word, arg))
        else:
            out.append(word)
    if 'pure' in out:
        # A promise that cannot hold is a refusal, not a warning: `pure` says the
        # callee leaves no trace, and each of these says it does.
        for eff in out:
            if eff.startswith('writes:'):
                raise SyntaxError('`pure` contradicts `%s` on %s at line %d'
                                  % (effect_text(eff), name, eline(c)))
        if owned or any(p[1] == 'move' for p in params):
            raise SyntaxError('`pure` contradicts a `move` parameter of %s at line %d'
                              % (name, eline(c)))
        if checked:
            raise SyntaxError('`pure` contradicts `errno` on %s at line %d'
                              % (name, eline(c)))
    for eff in out:
        if eff.startswith('borrows:'):
            bp = eff.split(':', 1)[1]
            if owned:
                raise SyntaxError('`%s` contradicts `-> move` on %s at line %d'
                                  % (effect_text(eff), name, eline(c)))
            if not _points_at_memory(ret):
                raise SyntaxError('`%s` wants a pointer result on %s, got %s at line %d'
                                  % (effect_text(eff), name, ret or 'none', eline(c)))
            pmod = params[pnames.index(bp)][1]
            if pmod == 'move':
                raise SyntaxError('`%s` contradicts `move` on parameter `%s` of %s at line %d'
                                  % (effect_text(eff), bp, name, eline(c)))
    for eff in out:
        if eff.startswith('reads:'):
            other = 'writes:' + eff.split(':', 1)[1]
            if other in out:
                raise SyntaxError('`%s` both reads and writes `%s` at line %d'
                                  % (name, eff.split(':', 1)[1], eline(c)))
    for eff in out:
        if out.count(eff) > 1:
            raise SyntaxError('`%s` declared twice on %s at line %d'
                              % (effect_text(eff), name, eline(c)))
    if c.peek().k == 'ID' and c.peek().v in CONTRACTS:
        # A contract describes the callee's use of *its callback*, so it belongs
        # on the callback parameter. Written at the end it would be read as the
        # start of the next statement, which is a diagnostic about a name that
        # does not exist rather than about the declaration.
        raise SyntaxError('`%s(...)` is written on the callback parameter of %s, '
                          'not at the end of the declaration at line %d'
                          % (c.peek().v, name, eline(c)))
    return tuple(out)

def parse_arena(c):
    """`arena { body }` becomes an if-block that enters the arena, defers the
    leave, and runs the body. Reusing the if and defer nodes gives every backend
    the same behavior, including on error unwinding. The escape rules that keep
    arena values inside the block live in typer.py."""
    c.step()
    c.want('{')
    body = parse_block(c)
    c.want('}')
    enter = ('expr', ('call', '$arena_enter', []))
    leave = ('defer', ('call', '$arena_leave', []))
    return ('if', ('bool', True), [enter, leave] + body, [])


def parse_if(c):
    c.want('if')
    if c.peek().k == 'KW' and c.peek().v == 'let':
        c.step()
        pat = parse_pattern(c)
        c.want('=')
        scrut = parse_expr(c)
        c.want('{')
        then = parse_block(c)
        c.want('}')
        els = None
        if c.peek().v == 'else':
            c.step()
            if c.peek().v == 'if':
                eb = parse_if(c)
                els = eb if isinstance(eb, list) else [eb]
            else:
                c.want('{')
                els = parse_block(c)
                c.want('}')
        tm = fresh('m')
        if pat is None:
            node = ('if', ('bool', True), then, els if els is not None else [])
        else:
            node = build_if(('var', tm), pat, then, els if els is not None else [])
        return [('let', tm, scrut, False), node]
    cond = parse_expr(c)
    c.want('{')
    then = parse_block(c)
    c.want('}')
    els = None
    if c.peek().v == 'else':
        c.step()
        if c.peek().v == 'if':
            eb = parse_if(c)
            els = eb if isinstance(eb, list) else [eb]
        else:
            c.want('{')
            els = parse_block(c)
            c.want('}')
    return ('if', cond, then, els)

def parse_while(c):
    c.want('while')
    if c.peek().k == 'KW' and c.peek().v == 'let':
        c.step()
        pat = parse_pattern(c)
        c.want('=')
        scrut = parse_expr(c)
        c.want('{')
        body = parse_block(c)
        c.want('}')
        tm = fresh('m')
        if pat is None:
            inner = ('if', ('bool', True), body, [('break',)])
        else:
            inner = build_if(('var', tm), pat, body, [('break',)])
        return [('while', ('bool', True), [('let', tm, scrut, False), inner])]
    cond = parse_expr(c)
    c.want('{')
    b = parse_block(c)
    c.want('}')
    return ('while', cond, b)

def parse_try(c):
    c.want('try')
    c.want('{')
    b = parse_block(c)
    c.want('}')
    c.want('catch')
    ename = None
    if c.peek().v != '{':
        ename = c.step().v
    c.want('{')
    h = parse_block(c)
    c.want('}')
    return ('try', b, h, ename)

def parse_pat_arg(c):
    t = c.peek()
    if t.k == 'ID' and t.v == '_':
        c.step()
        return ('wild',)
    if t.k == 'ID' and t.v == 'None':
        c.step()
        return ('none',)
    if t.k == 'KW' and t.v in ('true', 'false'):
        c.step()
        return ('lit', ('bool', t.v == 'true'))
    if t.k == 'ID' and c.i + 1 < len(c.t) and c.t[c.i + 1].v == '(':
        c.step()
        vname = t.v
        c.want('(')
        subs = []
        while c.peek().v != ')':
            subs.append(parse_pat_arg(c))
            if c.peek().v == ',':
                c.step()
        c.want(')')
        return ('variant', vname.lower(), subs)
    if t.k == 'NUM':
        c.step()
        return ('lit', ('num', _num_val(t.v)))
    if t.k == 'STR':
        c.step()
        return ('lit', no_nul(_expand_str(t.v[1:-1], _parse_sub), t.line))
    if t.k == 'ID':
        c.step()
        return ('bind', t.v)
    raise SyntaxError("bad pattern arg '%s' at line %d" % (t.v, eline(c)))


def build_if(tm, pat, body, rest, guard=None):
    if pat is None or pat[0] == 'wild':
        if guard is not None:
            return ('if', guard, body, rest)
        return body
    if guard is not None:
        body = [('if', guard, body, rest)]
    if pat[0] == 'lit':
        return ('if', ('bin', '==', tm, pat[1]), body, rest)
    if pat[0] == 'none':
        return ('if', ('call', 'is_none', [tm]), body, rest)
    if pat[0] == 'variant':
        _, tag, subs = pat
        cond = ('bin', '==', ('call', 'tagof', [tm]), ('str', tag))
        pre = []
        inner = body
        for j, sp in reversed(list(enumerate(subs))):
            acc = ('call', '__at', [tm, ('num', j + 1)])
            if sp[0] == 'bind':
                pre.append(('let', sp[1], acc, False))
            elif sp[0] == 'wild':
                pass
            else:
                t = fresh('p')
                pre.append(('let', t, acc, False))
                inner = [build_if(('var', t), sp, inner, rest)]
        return ('if', cond, pre + inner, rest)
    if pat[0] == 'bind':
        return [('let', pat[1], tm, False)] + body
    raise SyntaxError('bad match pattern')

def space_total(pat):
    if pat is None or pat[0] in ('wild', 'bind'):
        return True
    if pat[0] == 'variant':
        o = _ENUMS_REV.get(pat[1])
        if o and len(_ENUMS[o]) == 1:
            return all(space_total(sp) for sp in pat[2])
        return False
    return False


def parse_match(c):
    ml = c.peek().line
    c.want('match')
    scrut = parse_expr(c)
    c.want('{')
    arms = []
    seen = []
    while c.peek().v != '}':
        pat = parse_pattern(c)
        guard = None
        if c.peek().k == 'KW' and c.peek().v == 'if':
            c.step()
            guard = parse_expr(c)
        c.want('=>')
        if pat is not None:
            key = repr(pat) + repr(guard)
            if key in seen:
                raise SyntaxError('duplicate match pattern at line %d' % eline(c))
            seen.append(key)
        if c.peek().v == '{':
            c.step()
            body = parse_block(c)
            c.want('}')
        else:
            body = [parse_stmt(c)]
        arms.append((pat, guard, body))
        if c.peek().v == ',':
            c.step()
    c.want('}')
    if not arms:
        raise SyntaxError('match needs arms at line %d' % eline(c))
    owners = {}
    has_wild = False
    for pat, guard, _ in arms:
        if pat is None or pat[0] in ('wild', 'bind'):
            if guard is None:
                has_wild = True
            continue
        if pat[0] == 'variant':
            o = _ENUMS_REV.get(pat[1])
            if o and (pat[1] not in owners.setdefault(o, {}) or guard is None):
                owners[o][pat[1]] = (pat[2], guard)
        elif pat[0] == 'none':
            o = _ENUMS_REV.get('none')
            if o and ('none' not in owners.setdefault(o, {}) or guard is None):
                owners[o]['none'] = ([], guard)
    if len(owners) == 1 and not has_wild:
        owner = next(iter(owners))
        total = set()
        for tag, (subs, guard) in owners[owner].items():
            if guard is None and all(space_total(sp) for sp in subs):
                total.add(tag)
        missing = [t for t in _ENUMS[owner] if t not in total]
        if missing:
            raise SyntaxError('non-exhaustive match: missing %s in enum %s at line %d' % (', '.join(missing), owner, ml))
    if not owners and not has_wild:
        bools = set()
        onlybool = True
        for pat, guard, _ in arms:
            if guard is None and pat and pat[0] == 'lit' and pat[1][0] == 'bool':
                bools.add(pat[1][1])
            else:
                onlybool = False
        if onlybool and bools != {True, False}:
            raise SyntaxError('non-exhaustive match: missing %s at line %d' % ('false' if True in bools else 'true', ml))
    tm = fresh('m')
    node = None
    for pat, guard, body in reversed(arms):
        if pat is None:
            if guard is None:
                if node is not None:
                    raise SyntaxError('_ must be last at line %d' % eline(c))
                node = body
                continue
            rest = node if node is not None else []
            node = build_if(('var', tm), pat, body, rest, guard)
            continue
        if pat[0] == 'bind' and guard is None:
            if node is not None:
                raise SyntaxError('_ must be last at line %d' % eline(c))
            node = [('let', pat[1], ('var', tm), False)] + body
            continue
        rest = node if node is not None else []
        node = build_if(('var', tm), pat, body, rest, guard)
    if node == []:
        return [('expr', scrut)]
    out = [('let', tm, scrut, False)]
    if isinstance(node, list):
        out.extend(node)
    else:
        out.append(node)
    return out


def parse_pattern(c):
    t = c.peek()
    if t.k == 'ID' and t.v == '_':
        c.step()
        return None
    p = parse_pat_arg(c)
    if p[0] == 'bind':
        if p[1][:1].isupper():
            return ('variant', p[1].lower(), [])
        return ('bind', p[1])
    return p


def parse_enum(c):
    c.want('enum')
    tname = c.want_id('enum name').v
    c.want('{')
    variants = []
    while c.peek().v != '}':
        v = c.step()
        if v.k not in ('ID', 'KW'):
                raise SyntaxError("want variant have '%s' at line %d" % (v.v, v.line))
        fields = []
        if c.peek().v == '(':
            c.step()
            while c.peek().v != ')':
                f = c.step()
                if f.k not in ('ID', 'KW'):
                    raise SyntaxError("want field have '%s' at line %d" % (f.v, f.line))
                fields.append(f.v)
                if c.peek().v == ',':
                    c.step()
            c.want(')')
        variants.append((v.v, fields))
        if c.peek().v == ',':
            c.step()
    c.want('}')
    tags = []
    for vname, fields in variants:
        tag = 'none' if vname == 'None' and not fields else vname.lower()
        tags.append(tag)
        if tag in _ENUMS_REV and _ENUMS_REV[tag] != tname:
            _ENUMS_REV[tag] = None
        elif tag not in _ENUMS_REV:
            _ENUMS_REV[tag] = tname
    _ENUMS[tname] = tags
    return ('enum', tname, variants)

def parse_for(c):
    c.want('for')
    v = c.step()
    if v.k != 'ID':
        raise SyntaxError("want loop var have '%s' at line %d" % (v.v, v.line))
    c.want('in')
    start = parse_expr(c)
    if c.peek().v == '..':
        c.step()
        end = parse_expr(c)
        c.want('{')
        body = parse_block(c)
        c.want('}')
        return [('forrange', v.v, start, end, body)]
    c.want('{')
    body = parse_block(c)
    c.want('}')
    return [('foriter', v.v, start, body)]

def parse_sig(c):
    c.want('fn')
    name = c.step().v
    c.want('(')
    params = []
    while c.peek().v != ')' and c.peek().k != 'EOF':
        pn = c.step().v
        mode = 'ref'
        ann = None
        if c.peek().v == ':':
            c.step()
            depth = 0
            first = None
            toks = []
            while True:
                x = c.peek()
                if x.k == 'EOF' or (x.v in (',', ')') and depth == 0):
                    break
                if x.v in ('[', '('):
                    depth += 1
                if x.v in (']', ')'):
                    depth -= 1
                if first is None and depth == 0 and x.k in ('ID', 'KW'):
                    first = x.v
                toks.append(x.v)
                c.step()
            if first in ('move', 'mut', 'ref'):
                mode = first
            ann = ann_text(toks)
        params.append((pn, mode, ann))
        if c.peek().v == ',':
            c.step()
    c.want(')')
    ret = None
    if c.peek().v == '->':
        c.step()
        if c.peek().v in ('move', 'ref'):
            if c.step().v == 'ref':
                raise SyntaxError('ref returns dangle; return owned at line %d' % eline(c))
        toks = []
        while c.peek().k != 'EOF' and c.peek().v != '}' and not (c.peek().k == 'KW' and c.peek().v == 'fn'):
            toks.append(c.step().v)
        ret = ann_text(toks)
    return ('sig', name, params, ret)


def parse_trait(c):
    c.want('trait')
    name = c.step().v
    c.want('{')
    sigs = []
    while c.peek().v != '}':
        sigs.append(parse_sig(c))
    c.want('}')
    return ('trait', name, sigs)


def parse_impl(c):
    c.want('impl')
    first = c.want_id('impl name').v
    trait = None
    tname = first
    if c.peek().v == 'for':
        c.step()
        trait = first
        tname = c.want_id('impl type name').v
    c.want('{')
    fns = []
    while c.peek().v != '}':
        fns.append(parse_fn(c))
    c.want('}')
    return ('impl', trait, tname, fns)


def parse_expr(c):
    with _Depth("expression", c):
        return parse_or(c)

def parse_or(c):
    e = parse_lor(c)
    while c.peek().v == 'or':
        c.step()
        e = ('or', e, parse_lor(c))
    return e

def parse_lor(c):
    e = parse_land(c)
    while c.peek().v == '||':
        c.step()
        e = ('bin', '||', e, parse_land(c))
    return e

def parse_land(c):
    e = parse_cmp(c)
    while c.peek().v == '&&':
        c.step()
        e = ('bin', '&&', e, parse_cmp(c))
    return e

def parse_cmp(c):
    e = parse_add(c)
    while c.peek().v in ('==', '!=', '<', '>', '<=', '>=', 'in'):
        op = c.step().v
        e = ('bin', op, e, parse_add(c))
    return e

def parse_add(c):
    e = parse_mul(c)
    while c.peek().v in ('+', '-'):
        op = c.step().v
        e = ('bin', op, e, parse_mul(c))
    return e

def parse_mul(c):
    e = parse_un(c)
    while c.peek().v in ('*', '/', '%'):
        op = c.step().v
        e = ('bin', op, e, parse_un(c))
    return e

def parse_un(c):
    if c.peek().v in ('!', '-', '&'):
        op = c.step().v
        # `&` is an address, and it binds as tightly as `-`: `&s.f` is the
        # address of the field, never `(&s).f`.
        return ('un', op, parse_un(c))
    if c.peek().k == 'KW' and c.peek().v == 'move':
        c.step()
        return ('move', parse_un(c))
    return parse_post(c)

def parse_post(c):
    e = parse_prim(c)
    while True:
        if c.peek().v == '?':
            c.step()
            e = ('quest', e)
        elif c.peek().v == '[':
            c.step()
            ix = parse_expr(c)
            c.want(']')
            e = ('idx', e, ix)
        elif c.peek().v == '(':
            # `v[0](10)` is not a call: a call needs a name. Left alone it was
            # misread as two arguments of the enclosing call -- `print(v[0](10))`
            # printed the closure and then 10 -- and printing a closure that
            # captures the vector holding it recursed until the frame budget
            # ran out. A function value is not first class (SPEC 11), so this
            # is a syntax error rather than a silently different program.
            raise SyntaxError('a call needs a name, a function value is not first '
                              'class at line %d' % c.peek().line)
        elif c.peek().v == '.':
            c.step()
            meth = c.want_id('method or field name').v
            if c.peek().v == '(':
                c.step()
                args = []
                while c.peek().v != ')':
                    args.append(parse_expr(c))
                    if c.peek().v == ',':
                        c.step()
                c.want(')')
                if e == ('var', 'trace') and c.peek().v == '{':
                    c.step()
                    blk = parse_block(c)
                    c.want('}')
                    args = args + [('closure', None, [], blk)]
                e = ('mcall', e, meth, args)
            else:
                e = ('field', e, meth)
        else:
            return e

def _parse_sub(text):
    """Parse an interpolation placeholder as an expression.

    A placeholder that does not parse stays literal text: `"{"a": 1}"` is a
    JSON blob, not an interpolation. That is the same rule Python uses -- an
    f-string is opt-in, and here only a placeholder that is a real expression
    counts as one.
    """
    try:
        c = Cur(lex(text))
        node = parse_expr(c)
        if c.peek().k != 'EOF':
            return None
        return node
    except SyntaxError:
        return None


def _expand_str(raw, src_expr):
    """`"a{x}b"` -> `("bin","+",("rstr","a"),("bin","+",x,("rstr","b")))`.

    The placeholder is parsed by the real parser, so anything that works outside
    a string works inside it: `{len(v)}`, `{p.x}`, `{m.get(k)}`, `{a + b}`.
    Braces that do not hold exactly one expression stay literal, which is what
    keeps JSON templates working: `"{\"a\": 1}"` is text, `"{len(v)}"` is not.
    A literal chunk is `rstr`, never `str`, so `str`'s grow() cannot touch it.
    """
    if '{' not in raw:
        return ('str', unesc(raw))
    # Match braces once. Re-scanning from every unmatched `{` to the end
    # made a 15 KiB string of openings take seconds: O(n^2) on text whose
    # contents the program, not the author, may choose.
    matches = {}
    stack = []          # [opening index, deepest nested opening below it]
    k = 0
    while k < len(raw):
        if raw[k] == '\\' and k + 1 < len(raw):
            k += 2
            continue
        if raw[k] == '{':
            stack.append([k, 0])
        elif raw[k] == '}' and stack:
            opening, inner_depth = stack.pop()
            matches[opening] = (k + 1, inner_depth)
            if stack:
                stack[-1][1] = max(stack[-1][1], inner_depth + 1)
        k += 1
    parts = []
    buf = ''
    i = 0
    while i < len(raw):
        c = raw[i]
        if c == '\\' and i + 1 < len(raw):
            buf += raw[i:i + 2]
            i += 2
            continue
        if c == '{':
            found = matches.get(i)
            # A candidate nested past the parser's own bound cannot become
            # an expression. Trying each enclosing level re-lexed almost
            # the whole string every time and repeated its warning.
            if found is not None and found[1] <= MAX_NEST:
                j = found[0]
                sub = src_expr(unesc(raw[i + 1:j - 1]))
                if sub is not None:
                    if buf:
                        parts.append(('rstr', unesc(buf)))
                        buf = ''
                    parts.append(('expr', sub))
                    i = j
                    continue
                _note_stray(raw[i + 1:j - 1])
        buf += c
        i += 1
    if buf:
        parts.append(('rstr', unesc(buf)))
    if len(parts) == 1:
        return parts[0][1] if parts[0][0] == 'expr' else ('rstr', parts[0][1])
    node = parts[0][1] if parts[0][0] == 'expr' else ('rstr', parts[0][1])
    for kind, val in parts[1:]:
        node = ('bin', '+', node, val if kind == 'expr' else ('rstr', val))
    return node


def _note_stray(text):
    """Record a `{...}` that stayed literal but looks like it wanted to be one.

    JSON-shaped content (`{"a": 1}`) has a top-level colon and is left alone;
    so is anything without identifier characters (`{}`, `{ }`). What remains
    is code the parser could not read, which is what a typo looks like.
    """
    body = text.strip()
    if not body:
        return
    depth = 0
    for ch in body:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        elif ch == ':' and depth == 0:
            return
    if _re.fullmatch(r"\d+,\d*", body):
        return
    if not any(ch.isalnum() or ch == '_' for ch in body):
        return
    _STRAY_BRACES.append((_CURLINE[0], body))


def take_stray_braces():
    """Drain the stray-placeholder findings (the checker prints them)."""
    out = list(_STRAY_BRACES)
    del _STRAY_BRACES[:]
    return out


_I64 = 1 << 63


def _wrap64(n):
    """Fold an integer literal into `i64`, the way the C emitter and the C++
    front end do (`parser.cpp` accumulates modulo 2^64 for the same reason).

    Without it the interpreter kept the arbitrary-precision value: `print(
    9223372036854775808)` printed the literal itself here and the wrapped
    value from every other backend, and `x > 0` answered True here and False
    there. The literal is the one place an i64 can appear without an
    operation to wrap it, which is why §10's "the interpreter holds no
    bignums" needed this line rather than a later one.
    """
    if -_I64 <= n < _I64:
        return n
    n &= (1 << 64) - 1
    return n - (1 << 64) if n >= _I64 else n


def _num_val(tok):
    """`1` is an int, `1.0` and `1e3` are floats; `_` separates digits."""
    tok = tok.replace('_', '')
    if '.' in tok or 'e' in tok or 'E' in tok:
        return float(tok)
    return _wrap64(int(tok))


def parse_parfor(c):
    """`par for x in v reduce(op, unit) { return cost(x) }` — see docs/design/PARFOR.md."""
    c.want('for')
    v = c.step()
    if v.k != 'ID':
        raise SyntaxError("want loop var have '%s' at line %d" % (v.v, v.line))
    c.want('in')
    it = parse_expr(c)
    c.want('reduce')
    c.want('(')
    op = c.step()
    if op.k not in ('ID', 'KW'):
        raise SyntaxError("want the folding function have '%s' at line %d" % (op.v, op.line))
    c.want(',')
    unit = parse_expr(c)
    c.want(')')
    c.want('{')
    body = parse_block(c)
    c.want('}')
    if not body or not (isinstance(body[-1], tuple) and body[-1] and body[-1][0] == 'return'):
        raise SyntaxError('a `par for` body ends with `return <value>` at line %d' % eline(c))
    return ('parfor', v.v, it, op.v, unit, body)


def parse_prim(c):
    t = c.step()
    if t.k == 'ID' and t.v == 'par' and c.peek().k == 'KW' and c.peek().v == 'for':
        return parse_parfor(c)
    if t.k == 'NUM':
        return ('num', _num_val(t.v))
    if t.k == 'STR':
        return no_nul(_expand_str(t.v[1:-1], _parse_sub), t.line)
    if t.k == 'KW' and t.v in ('true', 'false'):
        return ('bool', t.v == 'true')
    if t.k == 'ID' and t.v == 'None':
        return ('none',)
    if t.v == '[':
        items = []
        while c.peek().v != ']':
            items.append(parse_expr(c))
            if c.peek().v == ',':
                c.step()
        c.want(']')
        return ('vec', items)
    if t.v == '{':
        pairs = []
        while c.peek().v != '}':
            k = parse_expr(c)
            c.want(':')
            pairs.append((k, parse_expr(c)))
            if c.peek().v == ',':
                c.step()
        c.want('}')
        return ('maplit', pairs)
    if t.k == 'KW' and t.v == 'fn':
        name = None
        if c.peek().k == 'ID' and c.i + 1 < len(c.t) and c.t[c.i + 1].v == '(':
            name = c.step().v
        c.want('(')
        params = parse_params(c)
        c.want(')')
        c.want('{')
        body = parse_block(c)
        c.want('}')
        return ('closure', name, params, body)
    if t.k in ('ID', 'KW'):
        if c.peek().v == '(':
            c.step()
            args = []
            while c.peek().v != ')':
                args.append(parse_expr(c))
                if c.peek().v == ',':
                    c.step()
            c.want(')')
            return ('call', t.v, args)
        if c.peek().v == '{' and t.k == 'ID' and t.v[:1].isupper():
            j = c.i + 1
            is_struct = False
            if j < len(c.t):
                if c.t[j].v == '}':
                    is_struct = True
                elif c.t[j].k == 'ID' and j + 1 < len(c.t) and c.t[j + 1].v == ':':
                    is_struct = True
            if is_struct:
                c.step()
                fields = []
                while c.peek().v != '}':
                    k = c.want_id('field name').v
                    c.want(':')
                    fields.append((k, parse_expr(c)))
                    if c.peek().v == ',':
                        c.step()
                c.want('}')
                return ('struct', t.v, fields)
        if t.k == 'ID' and t.v[:1].isupper():
            return ('call', t.v, [])
        return ('var', t.v)
    if t.v == '(':
        e = parse_expr(c)
        c.want(')')
        return e
    # `t.line`, not `eline(c)`: by now a `Name {` lookahead may have
    # consumed tokens, and the reader needs the line of the token that
    # actually stopped the parser, which is the one `t` holds.
    raise SyntaxError("stuck at '%s' at line %d" % (t.v, t.line))