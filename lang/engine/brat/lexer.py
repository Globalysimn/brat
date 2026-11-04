import re


# A number may separate its digits with `_` (`1_000`, `1_0.5`, `1_0e1_0`), the
# way Rust and Python read them. `_` is only ever *between* two digits, so `1_`
# and `1__0` still lex as a number followed by a name -- a parse error that
# names the token, which is what `parser.cpp` reports for the same text.
_NUM = r'\d(?:_?\d)*(?:\.\d(?:_?\d)*)?(?:[eE][+-]?\d(?:_?\d)*)?'
# ASCII only, like `lexer.cpp`: `\s` and `\d` would otherwise accept a no-break space and
# Arabic-Indic digits that the native front end refuses. Anything that matches no token is
# BAD, and `lex` refuses it: the scanner used to skip it, so `6 | 1` and `~x` were read as
# `6 1` and `x`.
TOKEN_RE = re.compile(r'(?P<WS>\s+)|(?P<COMMENT>//[^\n]*)|(?P<STR>"(?:\\[\s\S]|[^"\\])*")|(?P<NUM>' + _NUM + r')|(?P<DOT>\.(?!\.))|(?P<ID>[A-Za-z_][A-Za-z0-9_]*)|(?P<OP>\.\.|=>|->|==|!=|<=|>=|&&|\|\||\+=|-=|\*=|/=|%=|[+\-*/%<>=!&{}()\[\],:;?])|(?P<BAD>.)', re.ASCII)
KEYWORDS = {'let', 'mut', 'fn', 'if', 'else', 'while', 'return', 'true', 'false', 'or', 'try', 'catch', 'spawn', 'match', 'for', 'in', 'import', 'defer', 'move', 'break', 'continue', 'impl', 'self', 'trait', 'enum', 'const', 'pub', 'extern', 'unsafe', 'export', 'use'}

class Tok:
    __slots__ = ('k', 'v', 'line')
    def __init__(self, k, v, line=1):
        self.k = k
        self.v = v
        self.line = line
    def __repr__(self):
        return '%s:%s@%d' % (self.k, self.v, self.line)


def tok_line(nls, pos):
    import bisect
    return bisect.bisect_right(nls, pos) + 1


def _str_end(src, i):
    """Index of the closing quote of the string literal starting at `i`.

    A `{...}` placeholder may contain quotes -- `"{v.join("|")}"` is one string,
    not three tokens -- so the scan tracks how deep it is inside placeholders
    and inside the parentheses of the expression it is reading. A quote only
    ends the literal at placeholder depth 0, or at placeholder depth > 0 with no
    unclosed `(`, because that is what a lone `"{"` looks like: a JSON template
    whose braces are meant literally.
    """
    n = len(src)
    j = i + 1
    depth = 0
    paren = 0
    while j < n:
        c = src[j]
        if c == '\\' and j + 1 < n:
            j += 2
            continue
        if c == '{':
            depth += 1
        elif c == '}':
            if depth > 0:
                depth -= 1
        elif c == '(':
            paren += 1
        elif c == ')':
            if paren > 0:
                paren -= 1
        elif c == '"':
            if depth == 0 or paren == 0:
                return j
        j += 1
    return None


def _stray(ch, line):
    """The sentence for a character no token starts with."""
    if ch == '"':
        return 'unterminated string literal at line %d' % line
    shown = "'%s'" % ch if ch.isascii() and ch.isprintable() else 'U+%04X' % ord(ch)
    return 'unexpected character %s at line %d' % (shown, line)


def lex(src):
    """One scan over the source, one regex pass, line numbers counted forward.

    The scanner is `finditer`, not `match` at every position: the regex engine
    walks the file itself instead of being re-entered per token, which is most of
    the cost on a large file. Line numbers come from a cursor that only moves
    forward, because tokens arrive in order; `bisect` per token was the other
    measurable slice.
    """
    if src.startswith('\ufeff'):
        src = src[1:]
    newlines = [m.start() for m in re.finditer('\n', src)]
    nls_len = len(newlines)
    out = []
    append = out.append
    li = 0
    total = len(src)
    resume = 0
    while resume < total:
        again = None
        for m in TOKEN_RE.finditer(src, resume):
            start = m.start()
            k = m.lastgroup
            while li < nls_len and newlines[li] < start:
                li += 1
            if k == 'STR':
                v = m.group()
                if '{' in v:
                    # A placeholder may contain quotes, so the regex cannot see the
                    # end of the literal. Rescan, and start the regex again after it:
                    # a match that began inside the literal may run past its end, and
                    # carrying on from there would swallow what follows.
                    end = _str_end(src, start)
                    if end is None:
                        raise SyntaxError(_stray('"', li + 1))
                    append(Tok('STR', src[start:end + 1], li + 1))
                    again = end + 1
                    break
                append(Tok('STR', v, li + 1))
                continue
            if k == 'WS' or k == 'COMMENT':
                continue
            v = m.group()
            if k == 'BAD':
                raise SyntaxError(_stray(v, li + 1))
            if k == 'ID' and v in KEYWORDS:
                k = 'KW'
            append(Tok(k, v, li + 1))
        if again is None:
            break
        resume = again
    return out


def eline(c):
    t = c.peek()
    if t.k != 'EOF':
        return t.line
    if c.t:
        return c.t[-1].line
    return 1


class Cur:
    def __init__(self, t):
        self.t = t
        self.i = 0
    def peek(self, k=0):
        """The token `k` ahead; past the end it is EOF, as it always was."""
        j = self.i + k
        return self.t[j] if j < len(self.t) else Tok('EOF', '')
    def step(self):
        if self.i >= len(self.t):
            ln = self.t[-1].line if self.t else 1
            raise SyntaxError('unexpected end of file at line %d' % ln)
        x = self.t[self.i]
        self.i += 1
        return x
    def want(self, v):
        x = self.step()
        if x.v != v:
            raise SyntaxError("want '%s' have '%s' at line %d" % (v, x.v, x.line))
        return x
    def want_id(self, what='identifier'):
        """Step, but only accept a name-shaped token (ID or keyword)."""
        x = self.step()
        if x.k not in ('ID', 'KW'):
            raise SyntaxError("want %s (identifier) have '%s' at line %d"
                              % (what, x.v, x.line))
        return x