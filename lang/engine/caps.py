r"""The capability lattice: one answer per place, computed over the control flow.

`borrow.py` decides a write by keeping a list of live views per root and asking
"is any of them live here". That works, but it is a lexical approximation: the
list does not say whether a view is live on *this* path or on *some* path, and
every rule that wants a different question grows its own bookkeeping.

This module is the same decision as one lattice per place:

```
        top            (dyn, FFI, pointer: nothing is provable)
         |
         RW            (written, exclusive)
        /  \
      RO    MV         (read by someone / moved away)
        \  /
       none
```

`join` is the weaker capability, so a branch that only reads cannot talk a place
out of a write it never saw. A use *strengthens* the requirement: a read needs at
least `RO`, a write needs `RW`, and anything after `MV` is `use of moved`.

Two things the list could not express, and this can:

  * a view whose last use is before the write stops holding the place — the
    lattice moves it back to `none` when the block walks past its last use;
  * a branch that moves or aliases on one path only leaves the place at the join
    as `MV`-or-`RO`, which is exactly "may be gone", so the write after the join
    is refused with the same words as the move before it.

Nothing here prints. `verdicts()` returns `(line, message, note)` in the same
shape `borrow.py` produces, and `test_caps_diff.py` is the gate: on 207 programs
the two models must read the same, through both backends, and the lattice may add
only the findings reviewed in that file.
"""

try:
    from brat.parser import _LINES
except ImportError:  # pragma: no cover - the module is imported from two roots
    try:
        from parser import _LINES
    except ImportError:
        _LINES = {}

NONE, RO, MV, RW, TOP = 0, 1, 2, 3, 4

CAP_ORDER = (NONE, RO, MV, RW, TOP)
CAP_NAME = {NONE: "none", RO: "read", MV: "moved", RW: "write", TOP: "top"}

MOVED_USE = "use of moved `%s`"
MOVED_HINT = "use of %s `%s` (hint: %s)"
ADVICE = {
    "moved": "the value was moved; use a copy or re-bind",
    "sent": "a channel send hands the value to the receiver; send a copy if you need it back",
}
CONSUMERS = {"send": 0}          # method -> index of the argument it consumes
ESCAPES_RETURN = "ref `%s` escapes via return"
ESCAPES_SPAWN = "ref `%s` escapes via spawn"
TWO_WRITERS = "double mut borrow of `%s`: `%s` clashes with `%s`"
WRITE_WHILE_READ = "cannot mutate immutable `%s` while borrowed by `%s`"
ASSIGN_WHILE_READ = "cannot assign `%s` while borrowed by `%s`"
MOVE_WHILE_READ = "cannot move `%s` while borrowed by `%s`"

COPY_ANNS = {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "isize",
             "f32", "f64", "bool"}
MUTATORS = {"push", "pop", "sort", "sort_by", "ins", "del", "set", "clear"}


def join(a, b):
    """The weaker of two capabilities: the one a branch may have left behind."""
    if a == b:
        return a
    if a == TOP or b == TOP:
        return TOP
    if a == NONE:
        return b
    if b == NONE:
        return a
    # RO with RW is RO (a reader still holds the place); RO with MV is "read or
    # gone", which refuses a write and refuses a use: RW is the safe summary of
    # "this place is no longer exclusively ours".
    if {a, b} == {RO, RW}:
        return RO
    if {a, b} == {MV, RW}:
        return RW
    return RW


def exits(body):
    """True when control cannot fall out of `body` at the bottom.

    The same rule as `_always_exits` in the ownership pass: a branch that ends in
    `return` does not join into the state after the `if`, because on that path
    there is no after. Joining it anyway made the cleanup idiom -- free on the
    error branch, return, free on the normal path -- a second free.
    """
    if not isinstance(body, list) or not body:
        return False
    last = body[-1]
    if not isinstance(last, tuple) or not last:
        return False
    # Only `return` leaves the function. `break`/`continue` leave the *body*,
    # and a place moved on a path that a `break` cuts short is still gone after
    # the loop: joining nothing there would accept a double free. The ownership
    # pass keeps its own rule, and the two verdicts are unioned.
    if last[0] == "return":
        return True
    if last[0] == "if":
        els = last[3]
        if not els:
            return False
        return exits(last[2]) and exits(els if isinstance(els, list) else [els])
    return False


def body_of(stmt):
    """A branch body as a list: the parser writes one statement or a block."""
    if not stmt:
        return []
    return stmt if isinstance(stmt, list) else [stmt]


def line_of(node):
    return _LINES.get(id(node)) or None


def place_of(e):
    """`v`, `v.*`, `p.x` — the storage an expression denotes, or None."""
    if not isinstance(e, tuple) or not e:
        return None
    if e[0] == "var":
        return e[1]
    if e[0] == "move":
        return place_of(e[1])
    if e[0] == "field":
        base = place_of(e[1])
        return base + "." + e[2] if base else None
    if e[0] == "idx":
        base = place_of(e[1])
        return base + ".*" if base else None
    return None


class State:
    """Capability per place, the name that put it there, and the alias table."""

    def __init__(self, caps=None, who=None, alias=None):
        self.caps = dict(caps or {})      # place -> capability
        self.who = dict(who or {})        # place -> (name, line) that claims it
        self.alias = dict(alias or {})    # binding -> the place it denotes
        self.second = {}                  # place -> (name, line) of a second writer
        self.copies = set()               # bindings that hold a Copy value
        self.anns = {}                    # binding -> its declared type, when it has one
        self.params = set()               # parameters: their storage is the caller's
        self.muts = set()                 # bindings declared `let mut ...`
        self.verb = {}                    # place -> ("moved"|"sent", is a root, line)
        self.consts = set()               # `const` names: a write to them is not ours

    def copy(self):
        st = State(self.caps, self.who, self.alias)
        st.second = dict(self.second)
        st.copies = set(self.copies)
        st.anns = dict(self.anns)
        st.params = set(self.params)
        st.muts = set(self.muts)
        st.verb = dict(self.verb)
        st.consts = set(self.consts)
        return st

    def place(self, name):
        """`y` that denotes `v` answers `v`: a write through a view writes the owner."""
        seen = 0
        while name in self.alias and seen < 32:
            name = self.alias[name]
            seen += 1
        return name

    def cap(self, place):
        return self.caps.get(place, NONE)

    def claim(self, place, cap, name, line):
        """Take the place at `cap` for `name`; earlier names keep their own claim."""
        place = self.place(place)
        self.caps[place] = cap
        held = [c for c in self.who.get(place, []) if c[0] != name]
        held.append((name, line, cap))
        self.who[place] = held

    def forget(self, place):
        """Drop what was learned about a place and everything under it.

        A loop body declares its own bindings, and those are fresh at the top of
        every iteration: carrying `s: moved` out of pass one into pass two is what
        made `let s = ...; let t = move s` inside a loop read as a use-after-move.
        """
        for p in [p for p in self.caps if p == place or p.startswith(place + ".")]:
            self.caps.pop(p, None)
            self.who.pop(p, None)
            self.verb.pop(p, None)
            self.second.pop(p, None)
        for name in [n for n, target in self.alias.items()
                     if target == place or target.startswith(place + ".")]:
            self.alias.pop(name, None)

    def join_from(self, other):
        for place, how in other.verb.items():
            self.verb.setdefault(place, how)
        places = set(self.caps) | set(other.caps)
        for place in places:
            a, b = self.cap(place), other.cap(place)
            merged = join(a, b)
            if merged == NONE:
                self.caps.pop(place, None)
                self.who.pop(place, None)
            else:
                self.caps[place] = merged
                holder = self.who.get(place) or other.who.get(place)
                if holder:
                    self.who[place] = holder


def _root_places(state, root):
    """Every place under `root`, including `root` itself."""
    return [p for p in state.caps if p == root or p.startswith(root + ".")]


def prefixes(place):
    """`a.b.c` -> [`a.b.c`, `a.b`, `a`]: a use needs all of them alive."""
    out = [place]
    while "." in place:
        place = place.rsplit(".", 1)[0]
        out.append(place)
    return out


def gone_message(state, place, under):
    """The verdict for a use of something gone: text, and where it went.

    A move of the whole place carries the advice and points at the line that
    took it; a move of one path inside it carries neither, because the reader
    still has most of the value.
    """
    verb, root, line = state.verb.get(under, ("moved", False, None))
    if root and verb in ADVICE:
        note = (line, "`%s` %s here" % (under, verb)) if line else None
        return MOVED_HINT % (verb, under, ADVICE[verb]), note
    return MOVED_USE % under, None


def _moved_under(state, place):
    """The moved prefix that makes `place` gone, or None.

    Moving `p.x` leaves `p.y` alone, so only a *prefix* matters: moving `p` takes
    everything inside it, moving one field takes that field.
    """
    for p in prefixes(place):
        if state.cap(p) == MV:
            return p
    return None


def _elem_copy(state, base):
    """`v[0]` is a value when `v` is declared `vec[i64]`: SPEC 10, Copy elements."""
    while isinstance(base, tuple) and base and base[0] == "quest":
        base = base[1]
    if not isinstance(base, tuple) or not base:
        return False
    if base[0] in ("move", "field", "idx"):
        return _elem_copy(state, base[1])
    if base[0] != "var":
        return False
    ann = state.anns.get(base[1])
    if not isinstance(ann, str):
        return False
    a = ann.strip()
    if a.startswith("vec[") and a.endswith("]"):
        a = a[4:-1].strip()
    elif a.startswith("map[") and a.endswith("]"):
        a = a[4:-1].split(",")[-1].strip()
    else:
        return False
    return a in COPY_ANNS


def _moved_below(state, place):
    """A move of `p` cannot happen while something inside `p` has been moved out."""
    for p in list(state.caps):
        if p.startswith(place + ".") and state.cap(p) == MV:
            return p
    return None


def _relatives(state, place):
    """Everything that shares storage with `place`: its prefixes and what it holds."""
    out = list(prefixes(place))
    for p in state.caps:
        if p.startswith(place + "."):
            out.append(p)
    return out


def _live_reader(state, place, writer):
    for p in _relatives(state, place):
        for name, line, cap in state.who.get(p, []):
            if cap == RO and name != writer:
                return name, line
    return None


def _live_writer(state, place, writer):
    for p in _relatives(state, place):
        for name, line, cap in state.who.get(p, []):
            if cap == RW and name != writer:
                return name, line
    return None


class Walk:
    """The analysis proper: one pass over a body, a fixpoint only inside loops."""

    def __init__(self, funcs=None):
        self.funcs = funcs or {}
        self.out = []                     # (line, message, note)
        self.index = 0                    # statement position, for last-use
        self.cur = 0                      # line of the statement being walked
        self.last = {}                    # name -> index of its last use

    def note(self, line, msg, note=None):
        self.out.append((line, msg, note))

    def stmt(self, s, state):
        if not isinstance(s, tuple) or not s:
            return state
        self.index += 1
        self.cur = line_of(s) or self.cur
        for name in _used_in(s):
            self.last[name] = self.index
        kind = s[0]
        if kind in ("fn", "exportfn", "impl", "trait", "enum", "extern", "externstruct", "pub", "pubuse"):
            return state          # every body is walked on its own
        if kind == "let":
            return self.let_stmt(s, state)
        if kind == "set":
            return self.assign(s[1], s[2], state, line_of(s))
        if kind == "setidx":
            return self.assign(place_of(s[1]), s[3], state, line_of(s))
        if kind == "setfield":
            base = place_of(s[1])
            return self.assign(base + "." + s[2] if base else None, s[3], state, line_of(s))
        if kind == "expr":
            self.expr(s[1], state, mutating=True)
            return state
        if kind == "return":
            place = place_of(s[1])
            if place and state.place(place).split(".", 1)[0] in state.params:
                self.expr(s[1], state, mutating=False)
                return state          # borrowing a parameter is not an escape
            if place:
                holder = _live_reader(state, state.place(place), None)
                if holder and holder[0] and holder[0] != "?":
                    self.note(line_of(s), ESCAPES_RETURN % holder[0])
            self.expr(s[1], state, mutating=False)
            return state
        if kind == "if":
            self.expr(s[1], state, mutating=False)
            then_body = body_of(s[2])
            else_body = body_of(s[3])
            a = self.block(then_body, state.copy())
            b = self.block(else_body, state.copy())
            if not exits(then_body):
                state.join_from(a)
            if not exits(else_body):
                state.join_from(b)
            return state
        if kind in ("while", "foriter", "forrange"):
            body = s[2] if kind == "while" else (s[3] if kind == "foriter" else s[4])
            if kind == "forrange":
                self.expr(s[2], state, mutating=False)
                self.expr(s[3], state, mutating=False)
                state.copies.add(s[1])       # a range variable is an i64
            elif kind == "foriter":
                self.expr(s[2], state, mutating=False)
            else:
                self.expr(s[1], state, mutating=False)
            # A loop body runs zero or more times, so its effect joins with "not
            # taken": one pass to a fixpoint, which the lattice reaches in two
            # because its height is five.
            declared = _bound_in(body)
            for _ in range(2):
                seed = state.copy()
                for name in declared:
                    seed.forget(name)
                once = self.block(body, seed)
                state.join_from(once)
            return state
        if kind == "try":
            try_body = body_of(s[1])
            catch_body = body_of(s[2])
            a = self.block(try_body, state.copy())
            b = self.block(catch_body, state.copy())
            if not exits(try_body):
                state.join_from(a)
            if not exits(catch_body):
                state.join_from(b)
            return state
        if kind in ("spawn", "scope"):
            # A child copies the frame: its writes are the sharing pass's business,
            # but a view read inside it would outlive the parent frame.
            for name in _used_in(s[1]):
                holder = _live_reader(state, state.place(name), None)
                if holder and holder[0] == name:
                    self.note(line_of(s), ESCAPES_SPAWN % name)
            self.block(s[1], state.copy())
            return state
        if kind == "unsafe":
            # `unsafe` is the program saying it knows better than the checker, so
            # nothing inside it may decide a verdict outside it.
            self.block(s[1], state.copy())
            return state
        if kind == "defer":
            # Deferred code runs when the scope ends, after every statement that
            # follows it: falling into the generic walk would make `defer move v`
            # claim `v` at once and refuse the ordinary uses before it.
            return state
        for sub in s[1:]:
            if isinstance(sub, list):
                self.block(sub, state)
            elif isinstance(sub, tuple):
                self.expr(sub, state, mutating=False)
        return state

    def block(self, body, state):
        for s in body if isinstance(body, list) else []:
            state = self.stmt(s, state)
            self.expire(state)
        return state

    def expire(self, state):
        """Drop a view whose last use is behind us, so the place is free again."""
        for place, held in list(state.who.items()):
            kept = [c for c in held if c[2] != RO or self.last.get(c[0], 0) > self.index]
            if kept:
                state.who[place] = kept
                if not any(c[2] == RO for c in kept):
                    state.caps[place] = max((c[2] for c in kept), default=NONE)
                continue
            state.who.pop(place, None)
            if state.cap(place) == RO:
                state.caps[place] = NONE

    # the two places a capability changes
    def let_stmt(self, s, state):
        name, e = s[1], s[2]
        mut = s[3] if len(s) > 3 else False
        ann = s[5] if len(s) > 5 else None
        if ann:
            state.anns[name] = ann
        if mut:
            state.muts.add(name)          # a mutable binding is a writer of its place
        self.expr(e, state, mutating=False)
        # `i64`, `f64`, `bool` and a binding that holds one are copies: no view,
        # no borrow, no second place.
        copylike = (ann in COPY_ANNS or _copy_expr(e)
                    or (isinstance(e, tuple) and e and e[0] == "var" and e[1] in state.copies)
                    or (isinstance(e, tuple) and e and e[0] == "idx"
                        and _elem_copy(state, e[1])))
        if copylike:
            state.copies.add(name)
            return state
        place = place_of(e)
        if place is None or not _is_alias(e):
            if s[4] if len(s) > 4 else False:
                state.consts.add(name)
            return state              # a fresh value: `name` owns it outright
        line = line_of(s)
        if isinstance(e, tuple) and e[0] == "move":
            inside = _moved_below(state, state.place(place))
            if inside:
                self.note(line, MOVED_USE % state.place(place))
            return state              # the move claimed the place; binding it is fine
        owner = state.place(place)
        if owner in state.consts:
            return state              # a const alias: the walk explains this one
        if state.cap(owner) == MV or state.cap(place) == MV:
            self.note(line, MOVED_USE % name)
            return state
        if mut:
            holder = _live_reader(state, place, name)
            if holder:
                # Two names over one buffer, one of them exclusive: a mutable
                # target is a second writer, an immutable one is a write refused.
                if owner in state.muts:
                    self.note(line, TWO_WRITERS % (owner, name, holder[0]),
                              (holder[1], "first borrow `%s` here" % holder[0]))
                else:
                    self.note(line, WRITE_WHILE_READ % (owner, holder[0]),
                              (holder[1], "borrowed by `%s` here" % holder[0]))
            other = _live_writer(state, place, name)
            if other and other[0] != owner and other[0] != name:
                self.note(line, TWO_WRITERS % (owner, name, other[0]),
                          (other[1], "first borrow `%s` here" % other[0]))
            state.muts.add(name)
            state.alias[name] = owner
            state.claim(owner, RW, name, line)
            # The owner is still a mutable place of its own: two names, one buffer.
            state.second.setdefault(owner, (name, line))
        else:
            state.alias[name] = owner
            state.claim(owner, RO, name, line)
        return state

    def assign(self, place, value, state, line):
        self.expr(value, state, mutating=False)
        if place is None:
            return state
        place = state.place(place)
        if "." in place:
            # Writing *into* a value is a use of it, unless the write lands on
            # exactly what was moved out: that is how a moved field comes back
            # (`let a = move p.x` then `p.x = 9`).
            gone = _moved_under(state, place)
            if gone and gone != place:
                msg, note = gone_message(state, place.split(".", 1)[0], gone)
                self.note(line, msg, note)
                return state
        elif state.cap(place) == MV:
            # `v = ...` on a moved binding rebinds it: the move is over.
            state.caps[place] = NONE
            state.who.pop(place, None)
        holder = _live_reader(state, place, place.split(".", 1)[0])
        if holder:
            self.note(line, ASSIGN_WHILE_READ % (place, holder[0]),
                      (holder[1], "borrowed by `%s` here" % holder[0]))
        state.claim(place, RW, place.split(".", 1)[0], line)
        return state

    def expr(self, e, state, mutating):
        if not isinstance(e, tuple) or not e:
            return
        kind = e[0]
        if kind == "var":
            place = state.place(e[1])
            gone = _moved_under(state, place)
            if gone:
                msg, note = gone_message(state, place, gone)
                self.note(self.cur, msg, note)
            return
        if kind in ("field", "idx", "quest"):
            place = place_of(e)
            if place:
                gone = _moved_under(state, place)
                if gone:
                    msg, note = gone_message(state, place, gone)
                    self.note(self.cur, msg, note)
                for sub in ([e[1]] if kind != "idx" else [e[1], e[2]]):
                    if kind == "idx" and sub is e[2]:
                        self.expr(sub, state, mutating)
            else:
                for sub in e[1:]:
                    if isinstance(sub, tuple):
                        self.expr(sub, state, mutating)
            return
        if kind == "move":
            place = place_of(e[1])
            place = state.place(place) if place else None
            if place:
                if state.cap(place) == MV:
                    self.note(self.cur, gone_message(state, place, place)[0])
                holder = _live_reader(state, place, None)
                if holder:
                    # A move out of a borrowed place does not happen: the borrow
                    # checker refuses it, and the alias stays usable.
                    self.note(self.cur, MOVE_WHILE_READ % (place, holder[0]),
                              (holder[1], "borrowed by `%s` here" % holder[0]))
                    return
                state.claim(place, MV, place, self.cur)
                state.verb[place] = ("moved", "." not in place, self.cur)
            return
        if kind == "closure":
            cparams = e[2] if len(e) > 2 else []
            body = e[3] if len(e) > 3 else []
            if isinstance(body, list):
                inner = state.copy()
                for cp in cparams:
                    pn = cp[0] if isinstance(cp, tuple) else cp
                    if isinstance(pn, str):
                        inner.forget(pn)
                self.block(body, inner)
            return
        if kind == "parfor":
            self.expr(e[2], state, mutating)
            self.expr(e[4], state, mutating)
            body = e[5] if len(e) > 5 else []
            if isinstance(body, list):
                inner = state.copy()
                if isinstance(e[1], str):
                    inner.forget(e[1])
                self.block(body, inner)
            return
        if kind == "mcall" and e[2] in CONSUMERS:
            args = e[3] if len(e) > 3 else []
            for i, a in enumerate(args):
                place = place_of(a)
                copied = _copy_expr(a) or (
                    isinstance(a, tuple) and a and a[0] == "var" and a[1] in state.copies)
                if i == CONSUMERS[e[2]] and place and not copied:
                    place = state.place(place)
                    if _moved_under(state, place):
                        self.note(self.cur, gone_message(state, place, place)[0])
                    state.claim(place, MV, place, self.cur)
                    state.verb[place] = ("sent", "." not in place, self.cur)
                    continue
                self.expr(a, state, mutating)
            return
        if kind == "mcall" and e[2] in MUTATORS:
            place = place_of(e[1])
            place = state.place(place) if place else None
            if place:
                if state.cap(place) == MV:
                    self.note(self.cur, MOVED_USE % place)
                if place in state.second:
                    other, oln = state.second[place]
                    named = e[1][1] if e[1][0] == "var" else place
                    if other != named:
                        self.note(self.cur, TWO_WRITERS % (place, named, other),
                                  (oln, "first borrow `%s` here" % other))
                holder = _live_reader(state, place, place.split(".", 1)[0])
                if holder:
                    self.note(self.cur, WRITE_WHILE_READ % (place, holder[0]),
                              (holder[1], "borrowed by `%s` here" % holder[0]))
                state.claim(place, RW, place.split(".", 1)[0], self.cur)
            for a in e[3]:
                self.expr(a, state, mutating)
            return
        if kind == "call" and isinstance(e[1], str):
            modes = self.funcs.get(e[1]) or ()
            for i, arg in enumerate(e[2] if len(e) > 2 else []):
                place = place_of(arg)
                if i < len(modes) and modes[i] == "move" and place:
                    place = state.place(place)
                    below = _moved_below(state, place)
                    if below:
                        self.note(self.cur, MOVED_USE % place)
                        continue
                    if _moved_under(state, place):
                        self.note(self.cur, gone_message(state, place, place)[0])
                    state.claim(place, MV, place, self.cur)
                    state.verb[place] = ("moved", "." not in place, self.cur)
                    continue
                self.expr(arg, state, mutating)
            return
        for sub in e[1:]:
            if isinstance(sub, tuple):
                self.expr(sub, state, mutating)
            elif isinstance(sub, list):
                for x in sub:
                    self.expr(x, state, mutating)


def _is_alias(e):
    """`let y = x` where `x` is a binding: y denotes the same storage."""
    return isinstance(e, tuple) and e and e[0] in ("var", "move", "field", "idx")


def _copy_expr(e):
    return isinstance(e, tuple) and e and e[0] in ("num", "bool", "none", "rstr")


def _used_in(s, acc=None):
    acc = [] if acc is None else acc
    if isinstance(s, tuple):
        if s and s[0] == "var":
            acc.append(s[1])
            return acc
        for sub in s[1:]:
            if isinstance(sub, (tuple, list)):
                _used_in(sub, acc)
    elif isinstance(s, list):
        for sub in s:
            _used_in(sub, acc)
    return acc


def _index_uses(body, start=0):
    """name -> index of its last use, in statement order, before anything is judged.

    A view holds its place until its last use, and that use may be later in the
    body than the walk has got to, so the table has to be complete up front.
    """
    last = {}
    index = start
    order = []

    def flatten(stmts):
        for s in stmts if isinstance(stmts, list) else []:
            order.append(s)
            if isinstance(s, tuple) and s:
                for sub in s[1:]:
                    if isinstance(sub, list):
                        flatten(sub)
                    elif isinstance(sub, tuple):
                        for x in sub[1:]:
                            if isinstance(x, list):
                                flatten(x)

    flatten(body)
    for s in order:
        index += 1
        for name in _used_in(s):
            last[name] = index
    return last


def param_modes(prog):
    """name -> parameter modes, so `fn eat(s: move str)` moves its argument."""
    out = {}
    for stmt in prog[1]:
        node = stmt[1] if (isinstance(stmt, tuple) and stmt and stmt[0] == "pub") else stmt
        if not isinstance(node, tuple) or not node:
            continue
        if node[0] in ("fn", "exportfn"):
            out[node[1]] = [p[1] for p in node[2]]
        elif node[0] == "extern":
            out[node[2]] = [p[1] for p in node[3]]
        elif node[0] == "impl":
            for fn in node[3]:
                out.setdefault(fn[1], [p[1] for p in fn[2]])
    return out


def analyse(body, funcs=None, params=None):
    """(line, message, note) for one body: the script or a function."""
    walk = Walk(funcs)
    walk.last = _index_uses(body)
    state = State()
    state.params = set(p[0] for p in params or [] if isinstance(p, tuple))
    for p in params or []:
        if isinstance(p, tuple) and len(p) > 2:
            if p[2]:
                state.anns[p[0]] = p[2]
            if p[2] in COPY_ANNS:
                state.copies.add(p[0])
    walk.block(body, state)
    return walk.out


def _bound_in(body, out=None):
    """Names a block binds at its own level: `let` bindings and loop variables.

    Nested blocks are their own scopes and hand back nothing, so the walk does not
    descend into `if`/`while`/`spawn`: a loop-carried value can only be one this
    level declares.
    """
    if out is None:
        out = set()
    for s in body or ():
        if not isinstance(s, tuple) or not s:
            continue
        kind = s[0]
        if kind == "let" and isinstance(s[1], str):
            out.add(s[1])
        elif kind == "forrange" and isinstance(s[1], str):
            out.add(s[1])
        elif kind == "foriter" and isinstance(s[1], str):
            out.add(s[1])
        elif kind == "destruct":
            for name in _destruct_names(s[1]):
                out.add(name)
    return out


def _destruct_names(pattern):
    """`let [a, b] = pair` binds `a` and `b`; `_` binds nothing."""
    names = []
    if isinstance(pattern, str):
        return [] if pattern == "_" else [pattern]
    if isinstance(pattern, (list, tuple)):
        for part in pattern:
            names.extend(_destruct_names(part))
    return names


def verdicts(prog, funcs=None):
    """Every verdict in a program, functions first and the script body last.

    `funcs` maps a function name to its parameter modes, so a call that moves
    its argument moves the place here too.
    """
    modes = param_modes(prog)
    if funcs:
        modes.update(funcs)
    out = []
    seen = set()
    for body, params in _bodies(prog):
        for item in analyse(body, modes, params):
            key = (item[0], item[1])
            if key in seen:
                continue
            seen.add(key)
            out.append(item)
    return out


def _bodies(prog):
    """(body, params) for every function and then the script, matching caps.cpp."""
    for stmt in prog[1]:
        node = stmt[1] if stmt[0] == "pub" else stmt
        if isinstance(node, tuple) and node:
            if node[0] in ("fn", "exportfn"):
                yield node[3], node[2]
            elif node[0] == "impl":
                for fn in node[3]:
                    yield fn[3], fn[2]
    yield prog[1], []
