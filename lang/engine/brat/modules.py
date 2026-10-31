import os

try:
    from .lexer import Cur, lex
    from .parser import _KEEP, _LINES, parse_prog, reset_fresh
except ImportError:
    from lexer import Cur, lex
    from parser import _KEEP, _LINES, parse_prog, reset_fresh


class ModuleError(Exception):
    pass


class SourceFile:
    def __init__(self, path, src, prog, namespace):
        self.path = path
        self.src = src
        self.prog = prog
        self.namespace = namespace
        self.imports = []
        self.functions = {}
        self.callback_params = {}
        self.public = set()
        self.reexports = {}   # name -> (module it came from, its name there)
        self.iface = None   # {name: (params, ret)} from a sibling .bri, when present


class LoadedProgram:
    def __init__(self, prog, files):
        self.prog = prog
        self.files = files
        self.srcs = {f.path: f.src for f in files}


def _param_ann(p):
    return (p[2] if isinstance(p, tuple) and len(p) > 2 else None) or "none"


def parse_interface(path):
    """Map each `fn name(params) -> type` line of a .bri file to its full signature.

    The line is parsed with an empty body, so the signature syntax matches the implementation.
    """
    sigs = {}
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    for number, line in enumerate(lines, 1):
        text = line.strip()
        if not text or text.startswith("//"):
            continue
        try:
            prog = parse_prog(Cur(lex(text + " { return 0 }")))
        except SyntaxError as e:
            raise ModuleError("%s:%d: bad interface line: %s" % (path, number, e))
        node = prog[1][0] if prog[1] else None
        if node and node[0] == "pub":
            node = node[1]
        if not (isinstance(node, tuple) and node and node[0] == "fn") or len(prog[1]) != 1:
            raise ModuleError("%s:%d: bad interface line: expected `fn name(params) -> type`" % (path, number))
        sigs[node[1]] = ([_param_ann(p) for p in node[2]],
                         (node[5] if len(node) > 5 else None) or "none")
    return sigs


def load_file(path):
    reset_fresh()            # one temporary counter per program, not per process
    path = os.path.realpath(os.path.abspath(path))
    loader = _Loader(os.path.dirname(path))
    loader.visit(path, root=True)
    return loader.finish()


def load_source(src, base="."):
    reset_fresh()
    loader = _Loader(base)
    prog = parse_prog(Cur(lex(src)))
    root = SourceFile(None, src, prog, None)
    loader.files.append(root)
    loader.scan_imports(root)
    return loader.finish()


class _Loader:
    def __init__(self, base):
        self.base = os.path.realpath(os.path.abspath(base))
        self.files = []
        self._resolved = set()
        self.by_path = {}
        self.public_names = {}
        self.private_names = {}
        self.callback_params = {}

    def visit(self, path, root=False, import_name=None):
        path = os.path.realpath(os.path.abspath(path))
        if path in self.by_path:
            return self.by_path[path]
        if not self.confined(path) or not os.path.isfile(path):
            if root:
                raise OSError("cannot read " + path)
            raise ModuleError("no module " + import_name)
        try:
            with open(path, encoding="utf-8") as f:
                src = f.read()
        except UnicodeDecodeError as e:
            # The module is a file the user has to fix; the decoder's message
            # names the byte, which is what a diagnostic needs.
            raise ModuleError("module %s is not valid UTF-8: byte 0x%02x at offset %d"
                              % (import_name, e.object[e.start], e.start))
        try:
            prog = parse_prog(Cur(lex(src)))
        except SyntaxError as e:
            error = e if root else SyntaxError("%s (module %s)" % (e, import_name))
            error.brat_path = path
            error.brat_src = src
            raise error
        namespace = None if root else self.namespace(path)
        current = SourceFile(path, src, prog, namespace)
        self.files.append(current)
        self.by_path[path] = current
        self.scan_imports(current)
        return current

    def scan_imports(self, current):
        base = self.base if current.path is None else os.path.dirname(current.path)
        for st in current.prog[1]:
            if not (isinstance(st, tuple) and st and st[0] == "import"):
                continue
            name = st[1]
            target = self.visit(self.resolve(base, name), import_name=name)
            current.imports.append((name, target))

    def resolve(self, base, name):
        parts = name.split(".")
        if any(not p or p in (".", "..") or "/" in p or "\\" in p for p in parts):
            raise ModuleError("invalid module name " + name)
        tried = []
        for candidate in (os.path.join(base, *parts) + ".brat",
                          os.path.join(base, "vendor", *parts) + ".brat"):
            path = os.path.realpath(candidate)
            tried.append(os.path.relpath(path, self.base))
            if self.confined(path) and os.path.isfile(path):
                return path
        where = os.path.relpath(base, self.base) if base != self.base else "the program root"
        hint = (" (tried `%s` from %s; imports are relative to the importing file, "
                "so a module next to it is imported by its short name)"
                % ("`, `".join(tried), where))
        raise ModuleError("no module %s" % name + hint)

    def confined(self, path):
        try:
            return os.path.commonpath((self.base, path)) == self.base
        except ValueError:
            return False

    def namespace(self, path):
        rel = os.path.relpath(path, self.base)
        return "__".join(rel[:-5].split(os.sep))

    def finish(self):
        for current in self.files:
            if current.namespace is not None and os.path.isfile(current.path[:-5] + ".bri"):
                current.iface = parse_interface(current.path[:-5] + ".bri")
        for current in self.files:
            arity = {}
            for st in current.prog[1]:
                node = st[1] if st[0] == "pub" else st
                if node[0] == "extern":
                    params = node[3]
                    raw = [p[2] if isinstance(p, tuple) and len(p) > 2 else None
                           for p in params]
                    current.callback_params[node[2]] = raw
                    self.callback_params[node[2]] = raw
                    continue
                if node[0] != "fn":
                    continue
                original = node[1]
                full = self.mangle(current, original)
                current.functions[original] = full
                params = node[2]
                raw = [p[2] if isinstance(p, tuple) and len(p) > 2 else None
                       for p in params]
                current.callback_params[original] = raw
                current.callback_params[full] = raw
                self.callback_params[full] = raw
                arity[original] = ([_param_ann(p) for p in node[2]],
                                   (node[5] if len(node) > 5 else None) or "none")
                exported = original in current.iface if current.iface is not None else st[0] == "pub"
                if exported:
                    current.public.add(original)
                    self.public_names.setdefault(original, []).append((current, full))
                elif current.namespace is not None:
                    self.private_names[full] = (current, original)
            self.check_interface(current, arity)
        # A re-export may point at another module's re-export, so a module is
        # resolved only after the modules it imports are: `facade -> mid -> base`
        # is three levels and one pass in load order saw `mid` too late.
        for current in self.files:
            self.resolve_module(current, ())
        for current in self.files:
            current.prog = ("prog", [self.transform_top(st, current) for st in current.prog[1]])
        body = []
        expanded = set()

        def expand(current):
            if current.path in expanded:
                return
            expanded.add(current.path)
            imports = dict(current.imports)
            for st in current.prog[1]:
                if st[0] == "import":
                    expand(imports[st[1]])
                elif st[0] != "pubuse":
                    body.append(st)

        expand(self.files[0])
        return LoadedProgram(("prog", body), self.files)

    def resolve_module(self, current, stack):
        if current in self._resolved:
            return
        self._resolved.add(current)
        if current in stack:
            return   # an import cycle: the strict pass below reports what is left
        for _, target in current.imports:
            self.resolve_module(target, stack + (current,))
        self.resolve_uses(current)

    def resolve_uses(self, current):
        """`pub use a.f` puts `f` on this module's public surface.

        Runs after every module's own functions are known, because a re-export
        can point at a module that is loaded later than the one using it.
        """
        for st in current.prog[1]:
            if not (isinstance(st, tuple) and st and st[0] == "pubuse"):
                continue
            qualifier, fn = st[1].rsplit(".", 1) if "." in st[1] else ("", st[1])
            targets = [t for name, t in current.imports
                       if qualifier and (name == qualifier or name.rsplit(".", 1)[-1] == qualifier)]
            if not targets:
                raise ModuleError("`pub use %s`: module `%s` is not imported" % (st[1], qualifier))
            target = targets[0]
            if fn not in target.functions:
                raise ModuleError("module `%s` has no function `%s`" % (qualifier, fn))
            if target is not current and fn not in target.public:
                raise ModuleError("function `%s` is private in module `%s`" %
                                  (fn, self.module_name(target)))
            source = (self.module_name(target), fn)
            if fn in current.reexports and current.reexports[fn] != source:
                raise ModuleError("`%s` is re-exported from two modules: `%s` and `%s`" %
                                  (fn, current.reexports[fn][0], source[0]))
            current.reexports[fn] = source
            current.functions[fn] = target.functions[fn]
            current.public.add(fn)

    def check_interface(self, current, arity):
        """The interface is a promise: names, parameter count, parameter types, return type."""
        if current.iface is None:
            return
        where = self.module_name(current)
        for name, (want_params, want_ret) in current.iface.items():
            if name not in arity:
                raise ModuleError("The implementation of module `%s` does not match its interface: "
                                  "`%s` is declared in the interface but not defined" % (where, name))
            params, ret = arity[name]
            if len(params) != len(want_params):
                raise ModuleError("The implementation of module `%s` does not match its interface: "
                                  "`%s` takes %d parameter(s) in the implementation, %d in the interface"
                                  % (where, name, len(params), len(want_params)))
            for i, (got, want) in enumerate(zip(params, want_params), 1):
                if got != want:
                    raise ModuleError("The implementation of module `%s` does not match its interface: "
                                      "`%s` parameter %d is `%s` in the interface, `%s` in the implementation"
                                      % (where, name, i, want, got))
            if ret != want_ret:
                raise ModuleError("The implementation of module `%s` does not match its interface: "
                                  "`%s` returns `%s` in the interface, `%s` in the implementation"
                                  % (where, name, want_ret, ret))

    def mangle(self, current, name):
        return name if current.namespace is None else current.namespace + "__" + name

    def remap(self, old, new):
        if id(old) in _LINES:
            _LINES[id(new)] = _LINES[id(old)]
            _KEEP.append(new)
        return new

    def walk(self, node, current):
        if isinstance(node, list):
            return [self.walk(x, current) for x in node]
        if not isinstance(node, tuple) or not node:
            return node
        kind = node[0]
        if kind == "call":
            args = [self.walk(a, current) for a in node[2]]
            name = self.resolve_call(node[1], current)
            self.resolve_callback_args(args, node[1], name, current)
            return self.remap(node, ("call", name, args))
        if kind == "mcall":
            path = name_path(node[1])
            if path is not None:
                name = self.resolve_qualified(path + "." + node[2], current)
                if name is not None:
                    args = [self.walk(a, current) for a in node[3]]
                    self.resolve_callback_args(args, path + "." + node[2], name, current)
                    return self.remap(node, ("call", name, args))
        new = (kind,) + tuple(self.walk(x, current) for x in node[1:])
        return self.remap(node, new)

    def resolve_callback_args(self, args, original, resolved, current):
        params = current.callback_params.get(original)
        if params is None:
            params = self.callback_params.get(resolved)
        if params is None:
            return
        for i, ann in enumerate(params):
            if (i < len(args) and isinstance(ann, str) and ann.startswith("fn")
                    and isinstance(args[i], tuple) and args[i] and args[i][0] == "str"):
                value = self.resolve_call(args[i][1], current)
                args[i] = self.remap(args[i], ("str", value))

    def transform_top(self, st, current):
        new = self.walk(st, current)
        if new[0] == "pub" and new[1][0] == "fn":
            inner = new[1]
            inner = self.remap(inner, ("fn", current.functions[inner[1]]) + inner[2:])
            return self.remap(new, ("pub", inner))
        if new[0] == "fn":
            return self.remap(new, ("fn", current.functions[new[1]]) + new[2:])
        return new

    def resolve_call(self, name, current):
        if not isinstance(name, str):
            return name
        if "." in name:
            return self.resolve_qualified(name, current) or name
        if name in current.functions:
            return current.functions[name]
        private = self.private_names.get(name)
        if private is not None and private[0] is not current:
            raise ModuleError("function `%s` is private in module `%s`" %
                              (private[1], self.module_name(private[0])))
        candidates = self.public_names.get(name, [])
        unique = list(dict.fromkeys(full for _, full in candidates))
        if len(unique) == 1:
            return unique[0]
        if len(unique) > 1:
            owners = [f.path for f, _ in candidates]
            raise ModuleError("ambiguous unqualified name `%s`: defined in both %s and %s; qualify the call" %
                              (name, owners[0], owners[1]))
        return name

    def resolve_qualified(self, name, current):
        qualifier, fn = name.rsplit(".", 1)
        targets = []
        for imported, target in current.imports:
            if qualifier == imported or qualifier == imported.rsplit(".", 1)[-1]:
                if target not in targets:
                    targets.append(target)
        if not targets:
            return None
        if len(targets) != 1:
            raise ModuleError("ambiguous module qualifier `%s`" % qualifier)
        target = targets[0]
        if fn not in target.functions:
            raise ModuleError("module `%s` has no function `%s`" % (qualifier, fn))
        if target is not current and fn not in target.public:
            raise ModuleError("function `%s` is private in module `%s`" %
                              (fn, self.module_name(target)))
        return target.functions[fn]

    def module_name(self, current):
        return current.namespace.replace("__", ".")


def name_path(node):
    if not isinstance(node, tuple) or not node:
        return None
    if node[0] == "var":
        return node[1]
    if node[0] == "field":
        base = name_path(node[1])
        return base + "." + node[2] if base else None
    return None
