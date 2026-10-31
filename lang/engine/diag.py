import importlib.util
import json
import os
from dataclasses import asdict, dataclass, field


@dataclass
class Diagnostic:
    file: str
    line: int | None
    severity: str          # "error" | "warning" | "note"
    code: str
    message: str
    note: dict | None = None      # {"line": int, "message": str}
    helps: list = field(default_factory=list)
    fix: dict | None = None


_EXPLAIN = {
    "E0382": ("use of moved value",
              "Value was moved (`move`); the source location can no longer be used. "
              "Move is linear: one ownership.",
              "Use the variable the value was moved into, or copy it beforehand "
              "(Copy types only)."),
    "E0505": ("cannot move while borrowed",
              "Location is held by a live borrow; cannot move until the borrow ends.",
              "Wait for the last use of the borrower, then move."),
    "E0507": ("cannot move out of borrowed parameter",
              "A non-Copy parameter is a view of the caller's storage; moving it "
              "would transfer data the function does not own.",
              "Declare the parameter `move` to take ownership before consuming it."),
    "E0502": ("cannot mutate while iterating",
              "A `for` holds a view of the container it walks; writing into that "
              "container inside the loop changes the thing being read.",
              "Collect the writes in a second container and apply them after the "
              "loop, or iterate a copy (`for x in v.copy()`)."),
    "E0499": ("double mut borrow",
              "One location cannot have two mutable borrows at once "
              "(law of exclusivity).",
              "Keep one writer; write through the owner, not an alias copy."),
    "E0515": ("ref escapes via return",
              "A borrow of local storage escapes the function via `return`; "
              "the storage dies with the frame.",
              "Return a copy/value, not a borrow; borrowing a parameter is fine — "
              "its storage belongs to the caller."),
    "E0596": ("assign to immutable",
              "Assignment to an immutable binding or through an immutable receiver.",
              "Declare `let mut ...` (or a `mut ...` parameter) to write."),
    "B0521": ("ref escapes via spawn",
              "A borrow captured by `spawn`; the frame is copied, the borrow "
              "hangs on dead storage — data race.",
              "Pass only Copy values or explicitly `move` into `spawn`; "
              "do not capture a shared non-Copy binding."),
    "B0530": ("plugin value used after close",
              "A symbol or a pointer made through a library handle belongs to "
              "that library; `lib_close` ends it, and the memory or the code "
              "behind it may already be gone.",
              "Move the `lib_close` after the last use, or open the library "
              "again before using the value."),
    "B0531": ("plugin signature mismatch",
              "The literal signature in `call` disagrees with the `extern fn` "
              "declaration of the same name, so the call would read its "
              "arguments through the wrong prototype.",
              "Make the string match the declaration, or let `brat bindgen` "
              "write it from the header."),
    "B0522": ("data race between threads",
              "One value is shared with a running `spawn` and both sides touch "
              "it while both run; exactly one side may write until they join.",
              "Call `join_all()` (or `join(handle)`) before the second access, "
              "or pass the value through a channel with `send`/`recv`."),
    "F0001": ("unsupported FFI type",
              "A slot of an `extern` declaration names a type the C emitter "
              "cannot spell, so a call would read or write memory through a "
              "prototype that does not exist.",
              "Use i8..u64, usize, f32, f64, str, ptr, ptr[T], fn(...) or an "
              "`extern struct`; `brat bindgen` writes the BRAT name for a header."),
    "B0101": ("use of undeclared var",
              "Name not declared in this scope (no `let`, parameter, "
              "`for`/`match` binding).",
              "Add `let <name> = ...` or import the name from another file."),
    "P0001": ("syntax error",
              "The file does not parse; nothing after this point was checked.",
              "Fix the syntax first: everything else is reported after it parses."),
    "T0001": ("type error",
              "A value's type does not match what the position requires.",
              "Check the annotation and the expression: an i64 position does not "
              "take a str, and a `?` needs a Result."),
    "W0001": ("unused variable",
              "The binding is never read. Not an error; the build goes on.",
              "Remove the binding, or prefix work with `_` if it is intentional."),
    "W0002": ("unused mut",
              "`mut` is declared and never written.",
              "Drop `mut`: an immutable binding is enough here."),
    "W0003": ("unused import",
              "The module is imported and nothing from it is used.",
              "Remove the `import`, or use what it exports."),
    "W0004": ("string placeholder",
              "A `{...}` placeholder looks cut short by a bare quote.",
              "Bind the value first, then interpolate the binding."),
    "B0000": ("other error",
              "Unclassified diagnostic.",
              "See the message text above."),
}


def classify(message):
    """Message -> stable code, deterministic by text."""
    m = message
    if "use of moved" in m:
        return "E0382"
    if "cannot move borrowed parameter" in m:
        return "E0507"
    if "while borrowed" in m or "cannot pass" in m or "cannot use" in m \
            or "cannot assign" in m:
        return "E0505"
    if "while iterating it" in m:
        return "E0502"
    if "double mut borrow" in m or "law of exclusivity" in m:
        return "E0499"
    if "escapes via return" in m:
        return "E0515"
    if "escapes via spawn" in m or "return escapes spawn" in m:
        return "B0521"
    if "inside `spawn`" in m or "spawn` launched in a loop" in m:
        return "B0522"
    if "is from library" in m or "is closed twice" in m or "is closed on line" in m:
        return "B0530"
    if "the signature says" in m or "the signature has" in m:
        return "B0531"
    if ("assign to immutable" in m or "cannot mutate immutable" in m
            or "pass immutable" in m or "assign to const" in m
            or "write through" in m):
        return "E0596"
    if "unsupported FFI type" in m:
        return "F0001"
    if "use of undeclared var" in m:
        return "B0101"
    if "unused var" in m:
        return "W0001"
    if "unused mut" in m:
        return "W0002"
    if "unused import" in m:
        return "W0003"
    if "looks cut short" in m:
        return "W0004"
    return "B0000"


def explain(code):
    return _EXPLAIN.get(code, ("other error",
                               "See the diagnostic text above.",
                               "Fix per the message."))


# A snippet is an aid, not a dump. Generated programs carry lines with tens of
# thousands of terms, and echoing one verbatim buries the message it belongs to
# (and can be megabytes). Both front ends cut at the same width so the rendered
# diagnostic stays byte-identical.
SNIPPET_MAX = 200
SNIPPET_TAIL = "..."


def _clip(text):
    if len(text) <= SNIPPET_MAX:
        return text
    return text[:SNIPPET_MAX - len(SNIPPET_TAIL)] + SNIPPET_TAIL


def _source_line(lines, ln):
    if ln is not None and 1 <= ln <= len(lines):
        # rstrip, not strip: keep the line's own indentation so the snippet
        # lines up with the file a reader has open beside the error.
        return _clip(lines[ln - 1].rstrip())
    return None


def to_human(path, src, diags):
    """Byte-stable output shared by every emitter:

        file:line: severity: message
          | source line
        file:line: note: note message
          | note source line
    """
    lines = src.splitlines()
    out = []
    for d in diags:
        ln = d.line
        sl = _source_line(lines, ln)
        if sl is not None:
            out.append("%s:%d: %s: %s" % (d.file, ln, d.severity, d.message))
            out.append("  | " + sl)
        else:
            out.append("%s: %s: %s" % (d.file, d.severity, d.message))
        if d.note:
            nln = d.note.get("line")
            nmsg = d.note.get("message", "")
            nsl = _source_line(lines, nln)
            if nsl is not None:
                out.append("%s:%d: note: %s" % (d.file, nln, nmsg))
                out.append("  | " + nsl)
            else:
                out.append("%s: note: %s" % (d.file, nmsg))
    return ("\n".join(out) + "\n") if out else ""


def _entries_to_diags(file, entries, kind="error"):
    diags = []
    for entry in entries:
        ln = entry[0]
        msg = entry[1]
        rel = entry[2] if len(entry) > 2 else None
        note = None
        if rel is not None:
            note = {"line": rel[0], "message": rel[1]}
        diags.append(Diagnostic(file, ln, kind, classify(msg), msg, note=note))
    return diags


def _load_sibling(name):
    try:
        return __import__(name)
    except ImportError:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name + ".py")
        spec = importlib.util.spec_from_file_location("diag_" + name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod


def _load_brat():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "brat.py")
    spec = importlib.util.spec_from_file_location("diag_brat_oracle", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _parse_error_line_from(exc):
    """Line number of a BRAT lexer/parser error, or None."""
    if getattr(exc, "filename", None) is not None:
        return None
    import re
    m = re.search(r"\bat line (\d+)\s*\Z", str(exc))
    return int(m.group(1)) if m else None


def check_program(path, src, extra=None):
    """Check one file: (diagnostics, AST program or None)."""
    diags = []
    try:
        brat = _load_brat()
        prog = brat.parse_prog(brat.Cur(brat.lex(src)))
    except SyntaxError as e:
        diags.append(Diagnostic(path, _parse_error_line_from(e), "error",
                                "P0001", str(e)))
        return diags, None
    except Exception as e:
        diags.append(Diagnostic(path, None, "error", "P0001",
                                "parse failed: %s" % (e,)))
        return diags, None

    borrow = _load_sibling("borrow")
    typer = _load_sibling("typer")
    borrow_errs, borrow_warns = borrow.check(prog, extra)
    typer_errs = typer.check(prog)

    diags.extend(_entries_to_diags(path, borrow_errs, "error"))
    for wln, wmsg in borrow_warns:
        diags.append(Diagnostic(path, wln, "warning", classify(wmsg), wmsg))
    for d in _entries_to_diags(path, typer_errs, "error"):
        # A type error has no ownership code; give it its own class so a caller
        # can tell "the program does not typecheck" from "the program is unsafe".
        if d.code == "B0000":
            d.code = "T0001"
        diags.append(d)
    return diags, prog


def to_json(path, diags):
    """Protocol v1: one object per file, sorted by line, remedy included.

    A caller that never reads prose still gets `code` (stable across releases)
    and `helps` (what to do), so an agent can branch and retry without parsing
    English.
    """
    rows = []
    for d in sorted(diags, key=lambda d: (d.line or 0, d.severity)):
        row = asdict(d)
        if not row["helps"]:
            help_text = _EXPLAIN.get(row["code"], (None, None, ""))[2]
            row["helps"] = [help_text] if help_text else []
        rows.append(row)
    payload = {"version": 1, "file": path, "diagnostics": rows}
    return json.dumps(payload, ensure_ascii=False, indent=2) + "\n"


_SEVERITY_LSP = {"error": 1, "warning": 2, "note": 3, "info": 3}


def to_lsp(path, diags):
    out = []
    for d in diags:
        line = max(0, (d.line or 1) - 1)
        out.append({
            "range": {
                "start": {"line": line, "character": 0},
                "end": {"line": line, "character": 0},
            },
            "severity": _SEVERITY_LSP.get(d.severity, 3),
            "code": d.code,
            "source": "brat",
            "message": d.message,
        })
    return out