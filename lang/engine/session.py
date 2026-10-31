"""Inferred channel protocols: what shape a pipe has, and whether it can drain.

A channel in BRAT is a bounded queue with two ends in the same scope: someone
sends, someone receives, and nothing in the syntax says how many values cross.
This pass reads the shape off the code — `!i64` for a send, `?str` for a
receive, `end` after `close` — and checks the one thing that is cheap and exact:

  * a straight-line pipe whose sends and receives do not match cannot finish.
    Two values sent and one received leaves `send` blocked once the queue is
    full; two received and one sent leaves `recv` blocked forever.

Anything reached through a loop or a branch is printed but not counted: the
iteration count is not known statically, and a rule that guesses would refuse
programs that run fine. `send` on a closed channel is not an error (it returns
an `err` value) and `close` twice is not an error (it is idempotent), so neither
is reported.

    $ brat check --proto examples/spawn.brat
    ch   !i64 x3  ?i64 x3   balanced
"""

try:
    from brat.parser import _LINES
except ImportError:  # pragma: no cover - the module is imported from two roots
    try:
        from parser import _LINES
    except ImportError:
        _LINES = {}

CHAN = "chan"
OPS = {"send", "recv", "close"}
SIDES = ("main", "child")

NEED_MORE_SEND = ("`%s`: %d receive(s) but only %d send(s) — `recv` waits forever "
                  "(hint: send as many values as you receive, or `close` the channel)")
NEED_MORE_RECV = ("`%s`: %d value(s) sent into a queue of %d and only %d received — "
                  "`send` blocks once the queue is full (hint: receive more, or make "
                  "the channel larger)")


def line_of(stmt):
    return _LINES.get(id(stmt)) or None


def kind_of(e):
    """A best-effort payload name for the printed protocol. Never used to reject."""
    if not isinstance(e, tuple) or not e:
        return "any"
    head = e[0]
    if head == "num":
        return "f64" if isinstance(e[1], float) else "i64"
    if head in ("str", "interp"):
        return "str"
    if head == "bool":
        return "bool"
    if head == "vec":
        return "vec"
    if head == "maplit":
        return "map"
    if head == "none":
        return "none"
    if head == "var":
        return "any"
    if head == "call" and e[1] == "chan":
        return "chan"
    return "any"


class Chan:
    """One `chan(...)` binding and everything that happens to it."""

    def __init__(self, name, cap):
        self.name = name
        self.cap = cap          # a literal capacity, or None when it is computed
        self.events = []          # (side, op, kind, quant) in program order
        self.event_lines = []     # line per event, same order, None when unknown
        self.lines = []
        self.escaped = False      # the value left the scope: a helper may use it

    def add(self, side, op, kind, quant, line=None):
        self.events.append((side, op, kind, quant))
        self.event_lines.append(line)
        if line:
            self.lines.append(line)

    def first_blocking_send(self):
        """Line of the send that finds the queue full, or None.

        A straight-line pipe in one scope runs its operations in source order,
        so a prefix is decidable: a `send` that arrives with `cap` values
        already waiting has nobody to hand its value to and blocks. Counting a
        later `recv` as credit -- the old rule was `sent <= cap + recv` -- is
        what let `chan(1)` with two sends and one receive pass the checker and
        then hang, in both backends, forever. Two sides means a `spawn`, and a
        spawn orders nothing: there is no prefix to prove, so nothing is said.
        """
        if len(set(side for side, _o, _k, _q in self.events)) > 1:
            return None
        waiting = 0
        for (side, op, _kind, _quant), line in zip(self.events, self.event_lines):
            if op == "send":
                if waiting >= self.cap:
                    return line if line is not None else (self.lines[-1] if self.lines else None)
                waiting += 1
            elif op == "recv":
                waiting -= 1
        return None

    def counted(self, op):
        """(count, known) where `known` is false if a loop or branch can hide the count."""
        n = 0
        known = True
        for _side, ev, _kind, quant in self.events:
            if ev != op:
                continue
            if quant == "1":
                n += 1
            else:
                known = False
        return n, known

    def closed(self):
        return any(ev == "close" for _s, ev, _k, _q in self.events)

    def known(self):
        """True when every access is exact: no loop, no branch, and no escape.

        A channel handed to a helper (`fill(ch)`) can be sent on where this pass
        cannot see it, so counting would be a guess. Guess-free or silent.
        """
        return not self.escaped and all(quant == "1" for _s, _o, _k, quant in self.events)

    def balanced(self):
        s, sk = self.counted("send")
        r, rk = self.counted("recv")
        return sk and rk and s == r, s, r

    def _payload(self):
        """The one kind that crosses this channel, when the sends agree on it."""
        kinds = set(kind for _s, op, kind, _q in self.events if op == "send" and kind != "any")
        return kinds.pop() if len(kinds) == 1 else "any"

    def protocol(self):
        """`!i64 ?i64` — sends first, then receives, per side, in source order."""
        payload = self._payload()
        out = []
        for side in SIDES:
            parts = []
            prev = None
            for ev_side, op, kind, quant in self.events:
                if ev_side != side:
                    continue
                if op == "recv" and kind == "any":
                    kind = payload     # a receive has no expression of its own
                token = ("!" if op == "send" else "?" if op == "recv" else ".") + kind
                if quant != "1":
                    token += "*"
                if prev == token:
                    continue
                parts.append(token)
                prev = token
            if parts:
                out.append("  ".join(parts))
        text = " ; ".join(out)
        if any(ev == "close" for _s, ev, _k, _q in self.events):
            text += " ; end"
        return text


def _collect(stmts, chans, side, quant, closed):
    """Walk one statement list in program order, recording channel events."""
    for stmt in stmts:
        if not isinstance(stmt, tuple) or not stmt:
            continue
        kind = stmt[0]
        if kind == "let":
            val = stmt[2]
            if isinstance(val, tuple) and val and val[0] == "call" and val[1] == CHAN:
                if stmt[1] not in chans:
                    chans[stmt[1]] = Chan(stmt[1], capacity(val[2][0]) if val[2] else None)
            _collect_expr(val, chans, side, quant)
            continue
        if kind in ("expr", "return", "raise", "defer", "letidx", "set"):
            for sub in stmt[1:]:
                _collect_expr(sub, chans, side, quant)
            continue
        if kind in ("fn", "impl", "trait", "enum", "extern", "externstruct", "pub"):
            continue          # every body is analysed on its own by _bodies()
        if kind == "spawn":
            _collect(stmt[1], chans, "child", quant, set(closed))
            continue
        if kind == "if":
            _collect(stmt[2], chans, side, "|", set(closed))
            if len(stmt) > 3 and stmt[3]:
                _collect(stmt[3], chans, side, "|", set(closed))
            _collect_expr(stmt[1], chans, side, quant)
            continue
        if kind in ("while", "forrange", "foriter"):
            bodies = [b for b in stmt[1:] if isinstance(b, list)]
            for b in bodies:
                _collect(b, chans, side, "n", set(closed))
            for sub in stmt[1:]:
                if not isinstance(sub, list):
                    _collect_expr(sub, chans, side, quant)
            continue
        if kind == "try":
            _collect(stmt[1], chans, side, "|", set(closed))
            if len(stmt) > 2 and stmt[2]:
                _collect(stmt[2], chans, side, "|", set(closed))
            continue
        if kind in ("block", "scope", "unsafe"):
            _collect(stmt[1], chans, side, quant, set(closed))
            continue
        for sub in stmt[1:]:
            if isinstance(sub, list):
                _collect(sub, chans, side, "|", set(closed))
            else:
                _collect_expr(sub, chans, side, quant)


def _collect_expr(e, chans, side, quant):
    if not isinstance(e, tuple) or not e:
        return
    if (e[0] == "mcall" and len(e) > 2 and e[2] in OPS
            and isinstance(e[1], tuple) and e[1] and e[1][0] == "var"):
        ch = chans.get(e[1][1])
        if ch is not None:
            payload = e[3][0] if e[2] == "send" and len(e) > 3 and e[3] else None
            ch.add(side, e[2], kind_of(payload), quant, line_of(e))
            for arg in (e[3] if len(e) > 3 and isinstance(e[3], list) else []):
                _collect_expr(arg, chans, side, quant)
            return
    if e[0] == "var":
        ch = chans.get(e[1])
        if ch is not None:
            ch.escaped = True     # `fill(ch)`, `v.push(ch)`, `return ch`
        return
    for sub in e[1:]:
        if isinstance(sub, tuple):
            _collect_expr(sub, chans, side, quant)
        elif isinstance(sub, list):
            for x in sub:
                _collect_expr(x, chans, side, quant)


def capacity(arg):
    """The capacity of `chan(n)` when it is a literal, else None."""
    if isinstance(arg, tuple) and arg and arg[0] == "num" and isinstance(arg[1], int):
        return arg[1]
    return None


def _bodies(prog):
    """The script body and every function body, each analysed on its own."""
    yield prog[1]
    for stmt in prog[1]:
        node = stmt[1] if stmt[0] == "pub" else stmt
        if isinstance(node, tuple) and node and node[0] == "fn":
            yield node[3]
        elif isinstance(node, tuple) and node and node[0] == "impl":
            for fn in node[3]:
                yield fn[3]


def analyse(prog):
    """[(Chan, [(message, line)])] for every channel in every body of `prog`."""
    found = []
    for body in _bodies(prog):
        chans = {}
        _collect(body, chans, "main", "1", set())
        for ch in chans.values():
            errors = []
            if ch.known():
                sent, _ = ch.counted("send")
                recv, _ = ch.counted("recv")
                line = ch.lines[-1] if ch.lines else None
                # A send blocks only once the queue is full. In a straight-line
                # pipe the events happen in the order they are written, so the
                # question is a prefix one, not a total one: every prefix needs
                # `sends_so_far - receives_so_far <= cap`. Without a capacity
                # literal there is nothing to prove and nothing is said.
                if recv > sent and not ch.closed():
                    errors.append((line, NEED_MORE_SEND % (ch.name, recv, sent)))
                if isinstance(ch.cap, int):
                    blocked = ch.first_blocking_send()
                    if blocked is not None:
                        errors.append((blocked, NEED_MORE_RECV % (ch.name, sent, ch.cap, recv)))
            found.append((ch, errors))
    return found


def check(prog):
    """Errors only, as (line, message) pairs — the shape `borrow.py` expects."""
    out = []
    for _ch, errors in analyse(prog):
        out.extend(errors)
    return out


def format_protocols(results):
    """One line per channel: name, shape, balance."""
    lines = []
    for ch, errors in results:
        ok, sent, recv = ch.balanced()
        if errors:
            state = "DEADLOCK"
        elif not ch.known():
            state = "dynamic"
        elif ok:
            state = "balanced"
        else:
            state = "unbalanced"
        lines.append("%-14s %-40s %s" % (ch.name, ch.protocol() or "(unused)", state))
    return "\n".join(lines)
