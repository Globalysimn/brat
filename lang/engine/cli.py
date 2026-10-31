import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile

try:
    from diag import Diagnostic, to_human, classify, _entries_to_diags
except ImportError:  # pragma: no cover
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from diag import Diagnostic, to_human, classify, _entries_to_diags

from toolchain import CcError, resolve_cc, cc_cached

HERE = os.path.dirname(os.path.abspath(__file__))
_BRAT_MOD = None
_OWN_MOD = None
_CBACK2_MOD = None
_CBACK2_FAILED = False
_SCAFFOLD_MOD = None

def _load_mod(tag, filename):
    path = os.path.join(HERE, filename)
    spec = importlib.util.spec_from_file_location(tag, path)
    if spec is None or spec.loader is None:
        raise ImportError("cannot load " + filename)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

def brat_mod():
    global _BRAT_MOD
    if _BRAT_MOD is None:
        _BRAT_MOD = _load_mod("brat_engine_cli", "brat.py")
    return _BRAT_MOD

def own_mod():
    global _OWN_MOD
    if _OWN_MOD is None:
        _OWN_MOD = _load_mod("brat_borrow_cli", "borrow.py")
    return _OWN_MOD

def codegen_c_mod():
    global _CBACK2_MOD, _CBACK2_FAILED
    if _CBACK2_MOD is not None:
        return _CBACK2_MOD
    if _CBACK2_FAILED:
        raise ImportError("codegen_c unavailable")
    try:
        _CBACK2_MOD = _load_mod("brat_codegen_c_cli", "codegen_c.py")
    except Exception as e:
        _CBACK2_FAILED = True
        raise ImportError(str(e))
    return _CBACK2_MOD

def scaffold_mod():
    global _SCAFFOLD_MOD
    if _SCAFFOLD_MOD is None:
        _SCAFFOLD_MOD = _load_mod("brat_scaffold_cli", "scaffold.py")
    return _SCAFFOLD_MOD

def normalize_equals(line):
    out = []
    i = 0
    n = len(line)
    in_str = False
    while i < n:
        c = line[i]
        if in_str:
            out.append(c)
            if c == "\\":
                if i + 1 < n:
                    out.append(line[i + 1])
                    i += 2
                    continue
            elif c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            out.append(line[i:])
            break
        if c == "=":
            prev = line[i - 1] if i - 1 >= 0 else ""
            nxt = line[i + 1] if i + 1 < n else ""
            if nxt == "=" or nxt == ">" or prev in ("=", "!", "<", ">", "+", "-", "*", "/", "%"):
                out.append(c)
                i += 1
                continue
            cur = "".join(out).rstrip(" \t")
            j = i + 1
            while j < n and line[j] in (" ", "\t"):
                j += 1
            if j >= n:
                if cur == "":
                    out = ["="]
                else:
                    out = [cur + " ="]
                i = j
                continue
            if cur == "":
                out = ["= "]
            else:
                out = [cur + " = "]
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)

def count_braces(line, in_str=False):
    """(opens, closes, in_str) for one line, with the literal state threaded in.

    A string literal may span lines: without the incoming state the braces of the
    second half of a literal would count as block structure, and the formatter
    would both misindent the file and rewrite the literal's own bytes.
    """
    opens = 0
    closes = 0
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            i += 1
            continue
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        if c == "{":
            opens += 1
        elif c == "}":
            closes += 1
        i += 1
    return opens, closes, in_str

def fmt_src(src):
    src = src.replace("\r\n", "\n").replace("\r", "\n")
    lines = src.splitlines()
    out = []
    depth = 0
    in_str = False
    for raw in lines:
        if in_str:
            # Inside a literal these bytes are data, not layout: keep them as
            # written and only look for the quote that ends the literal.
            out.append(raw)
            o, c, in_str = count_braces(raw, True)
            depth = depth + (o - c)
            if depth < 0:
                depth = 0
            continue
        stripped = raw.strip()
        if stripped == "":
            out.append("")
            continue
        norm = normalize_equals(stripped).strip()
        if norm == "":
            out.append("")
            continue
        orig = depth
        if norm.startswith("}"):
            depth = depth - 1 if depth > 0 else 0
        out.append("  " * depth + norm)
        o, c, in_str = count_braces(norm)
        depth = orig + (o - c)
        if depth < 0:
            depth = 0
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out) + "\n"

def print_usage():
    sys.stderr.write("usage: brat file.brat [args...]\n")
    sys.stderr.write("       brat run file.brat [-- args...]\n")
    sys.stderr.write("       brat check file.brat [more...] [--json]\n")
    sys.stderr.write("       brat fmt file.brat [more...]\n")
    sys.stderr.write("       brat emit-c file.brat [--bundle] [--mem N] [--header FILE] "
                     "[--target TRIPLET]\n")
    sys.stderr.write("       brat test file.brat [more...]\n")
    sys.stderr.write("       brat build file.brat [-o out.exe] [--target TRIPLET] [--static|--dynamic] [--mem N]\n")
    sys.stderr.write("       brat dist  file.brat [-o out]   [--target TRIPLET] [--static|--dynamic]\n")
    sys.stderr.write("       brat pkg add <url>|owner/name[@constraint]\n")
    sys.stderr.write("       brat pkg publish owner/name@version file.brat [--depends owner/name@constraint]\n")
    sys.stderr.write("       brat pkg update [owner/name] [--dry-run]\n")
    sys.stderr.write("       brat pkg install | verify | list | remove <file>\n")
    sys.stderr.write("       brat new NAME [--kind service|tool]\n")
    sys.stderr.write("       brat bindgen HEADER.h [-o out.brat]\n")
    sys.stderr.write("       brat lsp\n")
    sys.stderr.write("       brat mcp\n")
    sys.stderr.write("       brat deps file.brat [-o graph.json] [--json]\n")
    sys.stderr.write("       brat api file.brat [--json]\n")
    sys.stderr.write("       brat ffi file.brat [--header h.h] [--json] [--target TRIPLET]\n")
    sys.stderr.write("       brat ffi file.brat --header h.h --bin prog.exe   "
                     "re-prove a built binary\n")
    sys.stderr.write("       brat ffi --bin prog.exe [--json]                 "
                     "what a binary claims\n")
    sys.stderr.write("       brat ffi --header h.h --emit [--emit-lib NAME]\n")
    sys.stderr.write("       brat explain [CODE]\n")


def read_source(path):
    """(source, None) or (None, message) for one source file.

    A file that is missing or is not UTF-8 becomes a message rather than a
    UnicodeDecodeError traceback: the decoder failure is about the user's file,
    not about this program. Both front ends agree the file is unreadable, and
    every command reports it through the same words.
    """
    try:
        with open(path, encoding="utf-8") as f:
            return f.read(), None
    except OSError as e:
        return None, "cannot read %s: %s" % (path, e)
    except UnicodeDecodeError as e:
        return None, "%s is not valid UTF-8: byte 0x%02x at offset %d" % (
            path, e.object[e.start], e.start)


def _read_src(path):
    """Source text for a diagnostic snippet, or "" when the file is unreadable.

    Every caller is already on its way to exit 1, so this reports the read
    failure instead of raising in the middle of building a diagnostic -- but it
    does not stay silent: an empty source otherwise prints a parse error for a
    file nobody managed to read.
    """
    src, err = read_source(path)
    if err is not None:
        sys.stderr.write("brat: %s\n" % err)
        return ""
    return src


def parse_error_line(exc):
    """Line number of a BRAT lexer/parser error, or None if `exc` is not one.

    BRAT raises a bare `SyntaxError('... at line N')` with no filename, so a
    Python SyntaxError raised while compiling brat.py itself is told apart by
    its populated `filename`.
    """
    if getattr(exc, "filename", None) is not None:
        return None
    m = re.search(r"\bat line (\d+)\s*\Z", str(exc))
    return int(m.group(1)) if m else None


def syntax_error(path, src, exc):
    """SPEC.md 9: a parse error is always `file:line: error: ...` plus the
    offending source line -- never a Python traceback. Returns 1."""
    ln = parse_error_line(exc)
    diags = [Diagnostic(path, ln, "error", "B0000", str(exc))]
    sys.stderr.write(to_human(path, src or "", diags))
    return 1


def do_test(args):
    files = [a for a in args if a != "--"]
    if not files:
        print_usage()
        return 2
    failed = 0
    for path in files:
        p = subprocess.run([sys.executable, os.path.join(HERE, "brat.py"), path],
                           capture_output=True, text=True, timeout=300)
        if p.returncode == 0:
            sys.stdout.write("ok %s\n" % path)
        else:
            failed += 1
            tail = (p.stdout + p.stderr)[-300:]
            sys.stdout.write("FAIL %s exit=%d %s\n" % (path, p.returncode, tail))
    sys.stdout.write("%d passed, %d failed\n" % (len(files) - failed, failed))
    return 1 if failed else 0

def blame_parse_error(brat, files, exc):
    """Find which file a parser error came from and report it with a snippet.

    Only reached after borrow.py/typer.py already blew up, so the extra parse
    costs nothing on the happy path and its `_LINES` pollution cannot be
    observed: we are on our way to exit 1.
    """
    for path in files:
        src = _read_src(path)
        try:
            brat.parse_prog(brat.Cur(brat.lex(src)))
        except SyntaxError as inner:
            return syntax_error(path, src, inner)
    first = files[0] if files else ""
    return syntax_error(first, _read_src(first), exc)


def _native_runner():
    """The C++ binary when it is built and current, else None.

    `brat run` is native by default: check, emit, link once into a
    content-addressed cache, exec -- 4 ms warm against the interpreter's 91 ms on
    a program that prints one line. `--interp` and `BRAT_INTERP=1` select the
    reference implementation instead, which is what the suite diffs against;
    `brat.py FILE` runs the same interpreter directly.
    """
    if os.environ.get("BRAT_INTERP"):
        return None
    binary = os.path.join(os.path.dirname(HERE), "cxx", "bin", "brat-cxx")
    if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
        return None
    src = os.path.join(os.path.dirname(HERE), "cxx", "src")
    try:
        newest = max(os.path.getmtime(os.path.join(src, f))
                     for f in os.listdir(src) if f.endswith((".cpp", ".h")))
    except OSError:
        return None
    return binary if os.path.getmtime(binary) >= newest else None


def do_run(args):
    # `brat run` is the product path: native by default, because the interpreter
    # costs 91 ms of startup and a 3M-iteration loop costs 20 s against 0.01 s.
    # The interpreter is one flag away and is what the suite compares against.
    want_interp = "--interp" in args
    args = [a for a in args if a not in ("--interp", "--native")]
    header, unsafe, args = _take_ffi_flags(args)
    if "--" in args:
        files = args[:args.index("--")]
        extra = args[args.index("--") + 1:]
    else:
        files = args[:1]
        extra = args[1:]
    if not files:
        print_usage()
        return 2
    path = files[0]
    src, err = read_source(path)
    if err is not None:
        sys.stderr.write("brat run: %s\n" % err)
        return 1
    native = None if want_interp else _native_runner()
    if native is not None:
        # A run compiles a program, so it is a build and takes the same gate: no
        # binary is produced for C it cannot prove. `--interp` is the reference
        # implementation and is not gated -- it is asked for by name.
        gate = ffi_gate(path, header, target=None, who="brat run", unsafe=unsafe)
        if gate:
            return gate
        # No Python in the loop: the loop should not be re-entered to read the
        # gate at all. One exec, and the C++ checker decides -- it is stricter,
        # not looser: it runs the ownership and type passes on every program.
        argv = [native, "run", path]
        if extra:
            argv.append("--")
            argv.extend(extra)
        os.execv(native, argv)
    # Arena blocks rely on the escape rules, which only the checker applies.
    # The interpreter has no arena memory, so the output would be right, but an
    # escaping value would silently survive the (non-existent) reset. Gate arena
    # programs the same way `brat check` does: the escape rules live in the
    # typer, the aliasing rules in the ownership pass, and both must agree.
    gated = (re.search(r"\barena\s*\{", src) is not None,
             re.search(r"\bspawn\b", src) is not None,
             re.search(r"\blib_open\b", src) is not None)
    if any(gated):
        diags = _gate_ownership(path)
        typer_bad = _gate_typer(path)
        # A warning is not a refusal: only an error blocks the run. This used to
        # test "any diagnostic", so a program with an unused binding was refused.
        if _has_errors(diags) or typer_bad:
            why = ("Arena blocks depend on the escape rules." if gated[0] else
                   "The sharing rule is what keeps two threads off one value."
                   if gated[1] else
                   "A plugin value belongs to its library; the lifetime rule "
                   "is what stops a call into a closed one.")
            sys.stderr.write(
                "brat run: refusing to run %s: the checker reported errors "
                "above. %s\n" % (path, why))
            return 1
    return brat_mod().run_file(path, extra)

def do_check(args):
    want_json = "--json" in args
    want_effects = "--effects" in args
    want_proto = "--proto" in args
    want_caps = "--caps" in args
    want_own = "--ownership" in args
    want_py = "--py" in args
    files = [a for a in args
             if a not in ("--", "--json", "--effects", "--proto", "--caps",
                          "--ownership", "--py", "--native")]
    if not files:
        print_usage()
        return 2
    # The plain case is the C++ checker when it is built and current: `run`
    # already decides with it, and two commands in one toolchain refusing
    # different programs is a contract nobody can hold in their head. `--py`
    # and `BRAT_INTERP=1` name the reference checker explicitly, which is what
    # the differential tests call -- they compare the two, so they must not be
    # able to silently compare one with itself.
    plain = not (want_json or want_effects or want_proto or want_caps or want_own)
    native = None if want_py else _native_runner()
    if plain and native is not None:
        os.execv(native, [native, "check"] + files)
    if want_effects:
        return _check_effects(files)
    if want_proto:
        return _check_proto(files)
    if want_caps:
        return _check_view(files, "caps", "caps.py", "format_caps")
    if want_own:
        return _check_view(files, "ownership", "ownership.py", "format_strategies")
    if want_json:
        return _check_json(files)
    brat = brat_mod()
    own = own_mod()
    own.load_brat = lambda path=None: brat
    try:
        rc = own.main(["check"] + files)
    except SyntaxError as e:
        return blame_parse_error(brat, files, e)
    if rc == 2:
        # A load error, already printed once. The type pass would open the same
        # missing file and print the same line a second time, which is where the
        # reference checker and the native one disagreed (one line against two).
        return rc
    try:
        tp = _load_mod("brat_typer_cli", "typer.py")
        tp.load_brat = lambda path=None: brat
        rc2 = tp.main(["check"] + files)
    except SyntaxError as e:
        return blame_parse_error(brat, files, e)
    except Exception as e:
        sys.stderr.write("brat check: typer failed, result is not a clean "
                         "bill: %s: %s\n" % (type(e).__name__, e))
        return rc or 1
    return rc or rc2

def _check_effects(files):
    """`check --effects`: the inferred effect row of every function in each file."""
    eff = _load_mod("brat_effects_cli", "effects.py")
    brat = brat_mod()
    status = 0
    for path in files:
        try:
            loaded = brat.load_file(os.path.abspath(path))
        except (OSError, brat.ModuleError, SyntaxError) as e:
            sys.stderr.write("brat check --effects: %s: %s\n" % (path, e))
            status = 1
            continue
        for f in loaded.files:
            text = eff.format_rows(eff.summarise(f.prog)).strip()
            if text:
                print(text)
    return status


def _check_view(files, flag, module, formatter):
    """`check --caps` / `--ownership`: a table, not a verdict."""
    mod = _load_mod("brat_" + flag + "_cli", module)
    brat = brat_mod()
    status = 0
    for path in files:
        try:
            loaded = brat.load_file(os.path.abspath(path))
        except (OSError, brat.ModuleError, SyntaxError) as e:
            sys.stderr.write("brat check --%s: %s: %s\n" % (flag, path, e))
            status = 1
            continue
        table = getattr(mod, formatter)
        for f in loaded.files:
            rows = table(f.prog).strip()
            if rows:
                print(rows)
    return status


def _check_proto(files):
    """`check --proto`: the inferred protocol of every channel, and its balance."""
    ses = _load_mod("brat_session_cli", "session.py")
    brat = brat_mod()
    status = 0
    for path in files:
        try:
            loaded = brat.load_file(os.path.abspath(path))
        except (OSError, brat.ModuleError, SyntaxError) as e:
            sys.stderr.write("brat check --proto: %s: %s\n" % (path, e))
            status = 1
            continue
        for f in loaded.files:
            text = ses.format_protocols(ses.analyse(f.prog)).strip()
            if text:
                print(text)
    return status


def do_fmt(args):
    files = [a for a in args if a != "--"]
    if not files:
        print_usage()
        return 2
    code = 0
    for path in files:
        src, err = read_source(path)
        if err is not None:
            sys.stderr.write("brat fmt: %s\n" % err)
            code = 1
            continue
        formatted = fmt_src(src)
        # Rewrite through a sibling temporary and rename it into place: a
        # formatter that truncates its input first can leave a user with an
        # empty program if anything goes wrong between the two writes.
        tmp = "%s.brat-fmt-%d" % (path, os.getpid())
        try:
            with open(tmp, "w", encoding="utf-8", newline="\n") as f:
                f.write(formatted)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, path)
        except OSError as e:
            sys.stderr.write("brat fmt: cannot write %s: %s\n" % (path, e))
            try:
                os.unlink(tmp)
            except OSError:
                pass
            code = 1
    return code

def _has_externs(path):
    """Whether a source declares an `extern`, and so needs a contract in its binary.

    The native runner builds without one, so a build that has an extern surface
    to record takes the Python driver instead. Nothing else changes: the C is the
    same text either way.
    """
    try:
        src, err = read_source(path)
        if err is not None or "extern" not in src:
            return False
        brat = codegen_c_mod()._brat
        prog = brat.parse_prog(brat.Cur(brat.lex(src)))
    except Exception:  # noqa: BLE001 - a source that will not parse has no surface
        return False
    return any(x and x[0] in ("extern", "externstruct") for x in prog[1])


def _ffi_contract_block(path, header=None, target=None):
    """The contract block for the emitted C, or "" when the program has no externs.

    Written by the compiler, not by codegen: both emitters produce the same C and
    this text is appended to whichever answered, so the byte-for-byte agreement
    between them is untouched.
    """
    ffi = _load_mod("brat_ffi", "ffi.py")
    brat = codegen_c_mod()._brat
    try:
        src, err = read_source(path)
        if err is not None:
            return ""
        prog = brat.parse_prog(brat.Cur(brat.lex(src)))
    except Exception:  # noqa: BLE001 - a source that will not parse has no contract
        return ""
    if not any(x and x[0] in ("extern", "externstruct") for x in prog[1]):
        return ""
    try:
        obj = ffi.contract(prog[1], header=header, target=target)
    except Exception as e:  # noqa: BLE001 - the build goes on, the claim does not
        sys.stderr.write("brat: cannot record the FFI contract: %s: %s\n"
                         % (type(e).__name__, e))
        return ""
    return ffi.contract_c(obj)


def do_emit_c(args):
    no_check = "--no-check" in args
    bundle = "--bundle" in args
    mem = None
    target = None
    header = None
    unsafe = False
    files = []
    rest = [a for a in args if a != "--"]
    i = 0
    while i < len(rest):
        a = rest[i]
        if a in ("--no-check", "--bundle"):
            i += 1
            continue
        if a == "--target" and i + 1 < len(rest):
            target = rest[i + 1]
            i += 2
            continue
        if a.startswith("--target="):
            target = a.split("=", 1)[1]
            i += 1
            continue
        if a == "--header" and i + 1 < len(rest):
            header = rest[i + 1]
            i += 2
            continue
        if a.startswith("--header="):
            header = a.split("=", 1)[1]
            i += 1
            continue
        if a == "--unsafe-ffi":
            unsafe = True
            i += 1
            continue
        if a == "--mem" or a.startswith("--mem="):
            val = rest[i + 1] if a == "--mem" and i + 1 < len(rest) else a[6:] if a.startswith("--mem=") else ""
            mem = _parse_mem(val)
            if mem is None:
                sys.stderr.write(
                    "brat emit-c: bad --mem %r (want e.g. 16777216, 512K, 16M, 1G)\n"
                    % val)
                return 2
            i += 2 if a == "--mem" else 1
            continue
        files.append(a)
        i += 1
    if not files:
        print_usage()
        return 2
    path = files[0]
    src, err = read_source(path)
    if err is not None:
        sys.stderr.write("brat emit-c: %s\n" % err)
        return 1
    if no_check and re.search(r"\barena\s*\{", src):
        sys.stderr.write(
            "brat emit-c: refusing --no-check for %s: arena blocks depend on the "
            "escape rules, which only the checker applies.\n" % path)
        return 1
    if not no_check:
        diags = _gate_ownership(path)
        if _has_errors(diags):
            sys.stderr.write(
                "brat emit-c: refusing to emit %s: the ownership checker "
                "reported errors above. Emitted C skips every check `brat "
                "build` applies.\nFix it, or pass --no-check to override "
                "deliberately.\n" % path)
            return 1
    try:
        cb = codegen_c_mod()
        cb.set_ffi_target(target)
        out = cb.emit_file_c(path, bundle=bundle)
        if out is None:
            out = cb.emit_file(path, bundle=bundle)
    except Exception as e:
        if parse_error_line(e) is not None:
            return syntax_error(path, src, e)
        sys.stderr.write("brat emit-c: %s: %s\n" % (type(e).__name__, e))
        return 1
    gate = ffi_gate(path, header, quiet=True, target=target, who="brat emit-c",
                    unsafe=unsafe)
    if gate:
        return gate
    block = _ffi_contract_block(path, header=header, target=target)
    if block:
        out = out + ("\n" if not out.endswith("\n") else "") + block
    if no_check:
        out = "/* BRAT: UNVERIFIED -- ownership not checked */\n" + out
    if mem is not None:
        out = "#define BRAT_MEM_BYTES %d\n" % mem + out
    sys.stdout.write(out)
    if not out.endswith("\n"):
        sys.stdout.write("\n")
    return 0

def lock_path():
    return os.path.join(os.getcwd(), "brat.lock")


def _valid_pkg_version(version):
    if not isinstance(version, str) or len(version) > 64:
        return False
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", version):
        return False
    return all(len(part) <= 10 and int(part) <= 2147483647
               for part in version.split("."))


def _valid_package_file(filename):
    if (not isinstance(filename, str) or len(filename) > 128 or
            not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*\.brat", filename) or
            os.path.basename(filename) != filename or
            pathlib.PureWindowsPath(filename).name != filename):
        return False
    stem = filename[:-5].split(".", 1)[0].casefold()
    reserved = {"con", "prn", "aux", "nul"}
    return stem not in reserved and not re.fullmatch(r"(?:com|lpt)[1-9]", stem)


def _validate_lock_items(items):
    import urllib.parse as _up
    if not isinstance(items, list):
        raise ValueError("lockfile wants a JSON list")
    files = set()
    names = set()
    for item in items:
        if not isinstance(item, dict):
            raise ValueError("lockfile entry must be an object")
        filename = item.get("file")
        if not _valid_package_file(filename):
            raise ValueError("lockfile entry has an unsafe package filename")
        file_key = filename.casefold()
        if file_key in files:
            raise ValueError("lockfile has duplicate package filename %s" % filename)
        files.add(file_key)
        url = item.get("url")
        if not isinstance(url, str) or not url or any(ord(c) < 32 for c in url):
            raise ValueError("%s has no valid URL" % filename)
        parsed = _up.urlsplit(url)
        if parsed.scheme not in ("https", "http", "file"):
            raise ValueError("%s has an unsupported URL scheme" % filename)
        if parsed.scheme in ("https", "http"):
            if not parsed.hostname or parsed.username or parsed.password:
                raise ValueError("%s has an invalid network URL" % filename)
        elif parsed.netloc not in ("", "localhost"):
            raise ValueError("%s has an invalid file URL" % filename)
        expected = item.get("sha256")
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError("%s has no valid SHA-256" % filename)
        name = item.get("name")
        version = item.get("version")
        if name is not None or version is not None:
            if not _valid_pkg_name(name) or not _valid_pkg_version(version):
                raise ValueError("%s has an invalid package identity" % filename)
            name_key = name.casefold()
            if name_key in names:
                raise ValueError("lockfile has duplicate package %s" % name)
            names.add(name_key)
    return items


def read_lock():
    """Contents of brat.lock, or [] when there is no lockfile yet.

    A lockfile that exists but cannot be read or parsed is an error, not an
    empty lock. `brat pkg install` would otherwise proceed as if nothing were
    pinned and stop honouring the hashes it exists to enforce.
    """
    try:
        with open(lock_path(), encoding="utf-8") as f:
            data = f.read()
    except FileNotFoundError:
        return []
    except OSError as e:
        raise SystemExit("brat pkg: cannot read %s: %s" % (lock_path(), e))
    except UnicodeError as e:
        raise SystemExit("brat pkg: %s is not valid UTF-8: %s" % (lock_path(), e))
    import json as _json
    try:
        items = _json.loads(data)
    except (ValueError, RecursionError) as e:
        raise SystemExit("brat pkg: %s is not valid JSON: %s" % (lock_path(), e))
    try:
        return _validate_lock_items(items)
    except ValueError as e:
        raise SystemExit("brat pkg: %s: %s" % (lock_path(), e))


def atomic_write(path, data):
    parent = os.path.dirname(os.path.abspath(path))
    fd, temp = tempfile.mkstemp(prefix=".brat-tmp-", dir=parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.remove(temp)


def write_lock(items):
    import json as _json
    data = (_json.dumps(items, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    atomic_write(lock_path(), data)


def fetch_url(url, allow_file=False):
    import hashlib as _hl
    import urllib.parse as _up
    import urllib.request as _ur
    parsed = _up.urlsplit(url)
    if parsed.scheme not in ("https", "http", "file"):
        raise ValueError("unsupported URL scheme")
    # A `file://` dependency makes sense for a local index and is a way to read
    # a local file for a remote one, so the caller says which it is.
    if parsed.scheme == "file" and not allow_file:
        raise ValueError("a remote index may not name a file:// dependency")
    if parsed.scheme in ("https", "http"):
        if not parsed.hostname or parsed.username or parsed.password:
            raise ValueError("invalid network URL")
    elif parsed.netloc not in ("", "localhost"):
        raise ValueError("invalid file URL")
    with _ur.urlopen(url, timeout=30) as r:
        body = r.read(16 * 1024 * 1024 + 1)
    if len(body) > 16 * 1024 * 1024:
        raise ValueError("download exceeds 16 MiB")
    try:
        text = body.decode("utf-8")
    except ValueError:
        raise RuntimeError("non-utf8 dep " + url)
    digest = _hl.sha256(body).hexdigest()
    return text, digest


def index_path():
    """Where the registry index lives: a path or a URL, else ./brat-index.json."""
    return os.environ.get("BRAT_INDEX") or os.path.join(os.getcwd(), "brat-index.json")


# Set by `read_index`: a remote index may only name http(s) dependencies, so
# callers fetching an entry need to know which kind of index named it.
_INDEX_REMOTE = False


def read_index():
    """The registry index: {owner/name: {version: {url, sha256, dependencies}}}.

    A registry here is an index of what `brat pkg` already does -- URL, sha256,
    vendor -- so nothing about fetching or verifying changes; only the lookup
    does. A missing index is an empty index: direct URLs still work without one.
    """
    import json as _json
    path = index_path()
    remote = "://" in path
    global _INDEX_REMOTE
    _INDEX_REMOTE = remote
    if remote:
        try:
            text, _ = fetch_url(path, allow_file=True)
        except Exception as e:
            raise SystemExit("brat pkg: index %s: %s" % (path, e))
    else:
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except FileNotFoundError:
            return {}
        except OSError as e:
            raise SystemExit("brat pkg: index %s: %s" % (path, e))
    try:
        data = _json.loads(text)
    except (ValueError, RecursionError) as e:
        raise SystemExit("brat pkg: index %s is not valid JSON: %s" % (path, e))
    pkgs = data.get("packages") if isinstance(data, dict) else None
    if not isinstance(pkgs, dict):
        raise SystemExit("brat pkg: index %s wants {\"packages\": {...}}" % path)
    if remote:
        # Anyone may publish an index. `file://` in one would turn a dependency
        # into a read of the local disk, so a remote index names http(s) only.
        for name, versions in pkgs.items():
            for version, item in (versions.items() if isinstance(versions, dict) else ()):
                url = item.get("url") if isinstance(item, dict) else None
                if not isinstance(url, str) or url.split(":", 1)[0].lower() not in ("http", "https"):
                    raise SystemExit("brat pkg: index %s: %s@%s must name an "
                                     "http(s) URL" % (path, name, version))
    return pkgs


def _ver_key(v):
    parts = [int(piece) for piece in v.split(".")]
    while len(parts) > 1 and parts[-1] == 0:
        parts.pop()
    return tuple(parts)


def _parse_pkg_constraint(constraint):
    if constraint is None or constraint == "*":
        return []
    if not isinstance(constraint, str) or not constraint:
        raise ValueError("invalid package version constraint %s" % constraint)
    parsed = []
    for part in constraint.split(","):
        match = re.fullmatch(r"(>=|<=|>|<|=)?([0-9]+(?:\.[0-9]+)*)", part)
        if not match or not _valid_pkg_version(match.group(2)):
            raise ValueError("invalid package version constraint %s" % constraint)
        parsed.append((match.group(1) or "=", match.group(2)))
    return parsed


def _valid_pkg_constraint(constraint):
    try:
        _parse_pkg_constraint(constraint)
        return True
    except ValueError:
        return False


def _constraint_matches(version, constraint):
    version_key = _ver_key(version)
    for operator, bound in _parse_pkg_constraint(constraint):
        bound_key = _ver_key(bound)
        if operator == "=" and version_key != bound_key:
            return False
        if operator == ">=" and version_key < bound_key:
            return False
        if operator == "<=" and version_key > bound_key:
            return False
        if operator == ">" and version_key <= bound_key:
            return False
        if operator == "<" and version_key >= bound_key:
            return False
    return True


def _package_versions(name, index):
    versions = index.get(name)
    if not isinstance(versions, dict) or not versions:
        raise ValueError("%s is not in %s" % (name, index_path()))
    if any(not _valid_pkg_version(version) for version in versions):
        raise ValueError("%s has an invalid version in %s" % (name, index_path()))
    return sorted(versions, key=_ver_key, reverse=True)


def _valid_pkg_name(name):
    if not isinstance(name, str):
        return False
    parts = name.split("/")
    return (len(parts) == 2 and
            all(len(part) <= 64 and
                re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", part) and
                part.casefold() not in {"con", "prn", "aux", "nul"} and
                not re.fullmatch(r"(?:com|lpt)[1-9]", part.casefold())
                for part in parts))


def resolve_pkg(spec, index):
    """Resolve an exact version or comma-separated numeric version constraint."""
    if "@" in spec:
        name, constraint = spec.rsplit("@", 1)
    else:
        name, constraint = spec, None
    if not _valid_pkg_name(name):
        raise SystemExit("brat pkg: invalid package name %s" % name)
    try:
        _parse_pkg_constraint(constraint)
        versions = _package_versions(name, index)
    except ValueError as e:
        raise SystemExit("brat pkg: %s" % e)
    matching = [version for version in versions
                if _constraint_matches(version, constraint)]
    if not matching:
        available = ", ".join(sorted(versions, key=_ver_key))
        if constraint is not None and _valid_pkg_version(constraint):
            raise SystemExit("brat pkg: %s has no version %s (index has %s)"
                             % (name, constraint, available))
        raise SystemExit("brat pkg: no version of %s satisfies %s (index has %s)"
                         % (name, constraint or "*", available))
    version = matching[0]
    entry = index[name][version]
    if not isinstance(entry, dict) or "url" not in entry:
        raise SystemExit("brat pkg: %s has no version %s (index has %s)"
                         % (name, version, ", ".join(sorted(index[name], key=_ver_key))))
    return name, version, entry


def resolve_pkg_tree(spec, index, locked_items=(), unpinned_names=None):
    if isinstance(spec, (list, tuple)):
        root_specs = [(name, None) for name in spec]
    else:
        if "@" in spec:
            root, root_constraint = spec.rsplit("@", 1)
        else:
            root, root_constraint = spec, None
        root_specs = [(root, root_constraint)]
    root_names = set()
    for root, root_constraint in root_specs:
        if not _valid_pkg_name(root):
            raise ValueError("invalid package name %s" % root)
        _parse_pkg_constraint(root_constraint)
        root_names.add(root.casefold())
    if unpinned_names is None:
        unpinned_names = root_names
    else:
        unpinned_names = {name.casefold() for name in unpinned_names}

    constraints = {}
    for item in locked_items:
        name = item.get("name")
        if name and name.casefold() not in unpinned_names:
            constraints.setdefault(name, []).append("=" + item["version"])
    latest_error = [None]

    def search(pending, chosen, current_constraints):
        if not pending:
            return chosen
        name, constraint, ancestry = pending[0]
        rest = pending[1:]
        if name in ancestry:
            cycle = ancestry[ancestry.index(name):] + (name,)
            latest_error[0] = "dependency cycle: " + " -> ".join(cycle)
            return None
        try:
            _parse_pkg_constraint(constraint)
            versions = _package_versions(name, index)
        except ValueError as e:
            latest_error[0] = str(e)
            return None
        combined = {key: list(value) for key, value in current_constraints.items()}
        combined.setdefault(name, []).append(constraint or "*")
        assigned = chosen.get(name)
        if assigned is not None:
            if all(_constraint_matches(assigned["version"], requirement)
                   for requirement in combined[name]):
                return search(rest, chosen, combined)
            latest_error[0] = "no version of %s satisfies %s" % (
                name, " and ".join(combined[name]))
            return None

        candidates = [version for version in versions
                      if all(_constraint_matches(version, requirement)
                             for requirement in combined[name])]
        if not candidates:
            if (len(combined[name]) == 1 and
                    _valid_pkg_version(combined[name][0])):
                latest_error[0] = "%s has no version %s (index has %s)" % (
                    name, combined[name][0], ", ".join(sorted(versions, key=_ver_key)))
                return None
            latest_error[0] = "no version of %s satisfies %s (index has %s)" % (
                name, " and ".join(combined[name]),
                ", ".join(sorted(versions, key=_ver_key)))
            return None
        for version in candidates:
            entry = index[name][version]
            if not isinstance(entry, dict) or "url" not in entry:
                latest_error[0] = "%s has no valid entry for version %s" % (
                    name, version)
                continue
            dependencies = entry.get("dependencies", {})
            if not isinstance(dependencies, dict):
                latest_error[0] = "%s@%s dependencies must be an object" % (
                    name, version)
                continue
            next_pending = []
            invalid = False
            for dep, dep_constraint in sorted(dependencies.items()):
                if not _valid_pkg_name(dep):
                    latest_error[0] = "%s@%s has an invalid dependency %s" % (
                        name, version, dep)
                    invalid = True
                    break
                try:
                    _parse_pkg_constraint(dep_constraint)
                except ValueError:
                    latest_error[0] = "%s@%s has an invalid dependency %s@%s" % (
                        name, version, dep, dep_constraint)
                    invalid = True
                    break
                next_pending.append((dep, dep_constraint, ancestry + (name,)))
            if invalid:
                continue
            next_chosen = dict(chosen)
            next_chosen[name] = {"name": name, "version": version,
                                 "entry": entry}
            result = search(next_pending + rest, next_chosen, combined)
            if result is not None:
                return result
        return None

    pending_roots = [(name, constraint, ()) for name, constraint in root_specs]
    selected = search(pending_roots, {}, constraints)
    if selected is None:
        raise ValueError(latest_error[0] or "could not resolve %s" % spec)

    plan = []
    filenames = {}
    completed = set()

    def add_resolved(name):
        if name in completed:
            return
        item = selected[name]
        entry = item["entry"]
        for dependency in sorted(entry.get("dependencies", {})):
            add_resolved(dependency)
        url = entry.get("url")
        if not isinstance(url, str) or not url:
            raise ValueError("%s@%s has no valid URL" % (name, item["version"]))
        expected = entry.get("sha256")
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError("%s@%s has no valid SHA-256" % (name, item["version"]))
        try:
            text, digest = fetch_url(url, allow_file=not _INDEX_REMOTE)
        except Exception as e:
            raise ValueError("fetch %s: %s" % (url, e))
        if digest != expected:
            raise ValueError("HASH MISMATCH %s@%s (index %s, got %s)"
                             % (name, item["version"], expected[:12], digest[:12]))
        filename = name.rsplit("/", 1)[-1] + ".brat"
        filename_key = filename.casefold()
        other = filenames.get(filename_key)
        if other and other != name:
            raise ValueError("%s and %s both install as %s" % (other, name, filename))
        filenames[filename_key] = name
        plan.append({"name": name, "version": item["version"], "url": url,
                     "sha256": digest, "file": filename, "text": text})
        completed.add(name)

    for root, _ in root_specs:
        add_resolved(root)
    return plan


def install_pkg_plan(plan):
    lock = read_lock()
    names = {item["name"] for item in plan if "name" in item}
    files = {item["file"] for item in plan}
    items = [it for it in lock
             if it.get("name") not in names and it.get("file") not in files]
    for item in plan:
        if "name" in item:
            items.append({k: item[k]
                          for k in ("file", "url", "sha256", "name", "version")})
        else:
            items.append({k: item[k] for k in ("file", "url", "sha256")})

    _install_vendor_files(plan, items)


def _locked_dependency_names(name, locked, index):
    by_name = {item["name"]: item for item in locked if item.get("name")}
    found = set()

    def visit(current):
        if current in found:
            return
        found.add(current)
        item = by_name.get(current)
        if item is None:
            return
        versions = index.get(current)
        entry = versions.get(item["version"]) if isinstance(versions, dict) else None
        dependencies = entry.get("dependencies", {}) if isinstance(entry, dict) else {}
        if isinstance(dependencies, dict):
            for dependency in dependencies:
                visit(dependency)

    visit(name)
    return found


def _package_update_changes(plan, locked):
    old = {item["name"]: item for item in locked if item.get("name")}
    changes = []
    for item in plan:
        previous = old.get(item["name"])
        if previous is None or previous["version"] != item["version"] or \
                previous["sha256"] != item["sha256"]:
            changes.append((previous, item))
    return changes


def _vendor_dir():
    vendor = os.path.join(os.getcwd(), "vendor")
    try:
        mode = os.lstat(vendor).st_mode
    except FileNotFoundError:
        os.mkdir(vendor)
        mode = os.lstat(vendor).st_mode
    if not stat.S_ISDIR(mode) or stat.S_ISLNK(mode):
        raise OSError("vendor path is not a real directory")
    return vendor


def _install_vendor_files(plan, lock_items=None):
    _validate_lock_items([{k: item[k] for k in ("file", "url", "sha256")}
                          | ({k: item[k] for k in ("name", "version")
                              if k in item})
                          for item in plan])
    vendor = _vendor_dir()
    with tempfile.TemporaryDirectory(prefix=".brat-pkg-", dir=vendor) as stage:
        backup = os.path.join(stage, "backup")
        staged = os.path.join(stage, "staged")
        os.mkdir(backup)
        os.mkdir(staged)
        for item in plan:
            with open(os.path.join(staged, item["file"]), "w",
                      encoding="utf-8", newline="\n") as f:
                f.write(item["text"])
        backed_up = []
        installed = []
        try:
            for item in plan:
                filename = item["file"]
                target = os.path.join(vendor, filename)
                old = os.path.join(backup, filename)
                if os.path.lexists(target):
                    os.replace(target, old)
                    backed_up.append(filename)
                os.replace(os.path.join(staged, filename), target)
                installed.append(filename)
            if lock_items is not None:
                write_lock(lock_items)
        except OSError:
            for filename in reversed(installed):
                os.remove(os.path.join(vendor, filename))
            for filename in reversed(backed_up):
                os.replace(os.path.join(backup, filename),
                           os.path.join(vendor, filename))
            raise


def do_pkg(args):
    if not args:
        print_usage()
        return 2
    sub = args[0]
    if sub == "add" and len(args) == 2 and "://" not in args[1]:
        spec = args[1]
        if "/" not in spec:
            sys.stderr.write("brat pkg: %s is neither a URL nor owner/name\n" % spec)
            return 1
        try:
            plan = resolve_pkg_tree(spec, read_index(), read_lock())
        except SystemExit as e:
            sys.stderr.write(str(e) + "\n")
            return 1
        except ValueError as e:
            sys.stderr.write("brat pkg: %s\n" % str(e))
            return 1
        try:
            install_pkg_plan(plan)
        except OSError as e:
            sys.stderr.write("brat pkg: write %s\n" % e)
            return 1
        for item in plan:
            sys.stdout.write("add %s %s %s\n"
                             % (item["name"], item["version"], item["sha256"][:12]))
        return 0
    if sub == "update":
        options = [arg for arg in args[1:] if arg.startswith("--")]
        targets = [arg for arg in args[1:] if not arg.startswith("--")]
        if len(options) > 1 or any(option != "--dry-run" for option in options) or \
                len(targets) > 1:
            print_usage()
            return 2
        target = targets[0] if targets else None
        if target is not None and not _valid_pkg_name(target):
            sys.stderr.write("brat pkg: invalid package name %s\n" % target)
            return 1
        locked = read_lock()
        package_items = [item for item in locked if item.get("name")]
        if not package_items:
            sys.stderr.write("brat pkg: no indexed packages in lock\n")
            return 1
        names = {item["name"] for item in package_items}
        try:
            index = read_index()
            if target is not None:
                if target not in names:
                    sys.stderr.write("brat pkg: not in lock: %s\n" % target)
                    return 1
                unpinned = _locked_dependency_names(target, locked, index)
            else:
                unpinned = names
            plan = resolve_pkg_tree(sorted(names), index, locked, unpinned)
        except SystemExit as e:
            sys.stderr.write(str(e) + "\n")
            return 1
        except ValueError as e:
            sys.stderr.write("brat pkg: %s\n" % e)
            return 1
        changes = _package_update_changes(plan, locked)
        if "--dry-run" in options:
            if not changes:
                sys.stdout.write("all packages are up to date\n")
            for previous, item in changes:
                if previous is None:
                    old = "not locked"
                else:
                    old = "%s %s" % (previous["version"], previous["sha256"][:12])
                sys.stdout.write("would update %s %s -> %s %s\n"
                                 % (item["name"], old, item["version"],
                                    item["sha256"][:12]))
            return 0
        try:
            install_pkg_plan(plan)
        except OSError as e:
            sys.stderr.write("brat pkg: update failed: %s\n" % e)
            return 1
        if not changes:
            sys.stdout.write("all packages are up to date\n")
        for previous, item in changes:
            old = "not locked" if previous is None else "%s %s" % (
                previous["version"], previous["sha256"][:12])
            sys.stdout.write("updated %s %s -> %s %s\n"
                             % (item["name"], old, item["version"],
                                item["sha256"][:12]))
        return 0
    if sub == "publish" and len(args) >= 3:
        spec, source = args[1:3]
        if "@" not in spec:
            sys.stderr.write("brat pkg: publish requires owner/name@version\n")
            return 1
        package, version = spec.rsplit("@", 1)
        if not _valid_pkg_name(package) or not _valid_pkg_version(version):
            sys.stderr.write("brat pkg: invalid package name or version %s\n" % spec)
            return 1
        parts = package.split("/")
        if "://" in index_path():
            sys.stderr.write("brat pkg: publish requires a local BRAT_INDEX file\n")
            return 1
        dependencies = {}
        i = 3
        while i < len(args):
            if args[i] != "--depends" or i + 1 >= len(args):
                sys.stderr.write("brat pkg: expected --depends owner/name@constraint\n")
                return 1
            dep = args[i + 1]
            if "@" not in dep:
                sys.stderr.write("brat pkg: dependency needs owner/name@constraint: %s\n" % dep)
                return 1
            dep_name, dep_version = dep.rsplit("@", 1)
            if (not _valid_pkg_name(dep_name) or
                    not _valid_pkg_constraint(dep_version) or
                    dep_name == package):
                sys.stderr.write("brat pkg: invalid dependency %s\n" % dep)
                return 1
            previous = dependencies.get(dep_name)
            if previous and previous != dep_version:
                sys.stderr.write("brat pkg: conflicting versions for %s\n" % dep_name)
                return 1
            dependencies[dep_name] = dep_version
            i += 2
        try:
            with open(source, "rb") as f:
                body = f.read()
            text = body.decode("utf-8")
            brat = brat_mod()
            brat.parse_prog(brat.Cur(brat.lex(text)))
        except (OSError, UnicodeError, SyntaxError, ImportError) as e:
            sys.stderr.write("brat pkg: cannot publish %s: %s\n" % (source, e))
            return 1
        digest = hashlib.sha256(body).hexdigest()
        root = os.path.dirname(os.path.abspath(index_path()))
        artifact = os.path.join(root, ".brat-packages", parts[0],
                                "%s-%s-%s.brat" % (parts[1], version, digest[:16]))
        artifact_existed = os.path.exists(artifact)
        try:
            pkgs = read_index()
            versions = pkgs.setdefault(package, {})
            if not isinstance(versions, dict):
                raise ValueError("package versions entry is not an object")
            versions[version] = {"url": pathlib.Path(artifact).as_uri(),
                                 "sha256": digest,
                                 "dependencies": dependencies}
            os.makedirs(os.path.dirname(artifact), exist_ok=True)
            atomic_write(artifact, body)
            index_data = (json.dumps({"packages": pkgs}, indent=2, ensure_ascii=False)
                          + "\n").encode("utf-8")
            atomic_write(index_path(), index_data)
        except (OSError, ValueError, SystemExit) as e:
            if not artifact_existed and os.path.exists(artifact):
                os.remove(artifact)
            sys.stderr.write("brat pkg: publish failed: %s\n" % e)
            return 1
        sys.stdout.write("publish %s %s %s\n" % (package, version, digest[:12]))
        return 0
    if sub == "add" and len(args) == 2:
        url = args[1]
        import urllib.parse as _up
        name = _up.unquote(_up.urlsplit(url).path.rsplit("/", 1)[-1])
        if not _valid_package_file(name):
            sys.stderr.write("brat pkg: URL must name a safe .brat file\n")
            return 1
        try:
            # A URL typed on the command line is the user's own choice.
            text, digest = fetch_url(url, allow_file=True)
        except Exception as e:
            sys.stderr.write("brat pkg: fetch %s: %s\n" % (url, e))
            return 1
        try:
            install_pkg_plan([{"file": name, "url": url, "sha256": digest,
                               "text": text}])
        except OSError as e:
            sys.stderr.write("brat pkg: write %s\n" % e)
            return 1
        sys.stdout.write("add %s %s\n" % (name, digest[:12]))
        return 0
    if sub == "install" and len(args) == 1:
        items = read_lock()
        if not items:
            sys.stderr.write("brat pkg: empty lock\n")
            return 1
        plan = []
        for it in items:
            try:
                # The lock file is a local artifact, so its URLs are the
                # runner's own choice.
                text, digest = fetch_url(it["url"], allow_file=True)
            except Exception as e:
                sys.stderr.write("brat pkg: fetch %s: %s\n" % (it.get("url"), e))
                return 1
            if digest != it.get("sha256"):
                sys.stderr.write("brat pkg: HASH MISMATCH %s\n" % it.get("file"))
                return 1
            plan.append({**it, "text": text})
        try:
            _install_vendor_files(plan)
        except OSError as e:
            sys.stderr.write("brat pkg: install failed: %s\n" % e)
            return 1
        for it in items:
            sys.stdout.write("ok %s\n" % it["file"])
        return 0
    if sub == "verify" and len(args) == 1:
        items = read_lock()
        if not items:
            sys.stderr.write("brat pkg: empty lock\n")
            return 1
        try:
            vendor = _vendor_dir()
        except OSError as e:
            sys.stderr.write("brat pkg: verify failed: %s\n" % e)
            return 1
        for item in items:
            path = os.path.join(vendor, item["file"])
            try:
                if not stat.S_ISREG(os.lstat(path).st_mode):
                    raise OSError("not a regular file")
                with open(path, "rb") as f:
                    body = f.read(16 * 1024 * 1024 + 1)
                if len(body) > 16 * 1024 * 1024:
                    raise OSError("file exceeds 16 MiB")
                digest = hashlib.sha256(body).hexdigest()
            except OSError as e:
                sys.stderr.write("brat pkg: verify %s: %s\n" % (item["file"], e))
                return 1
            if digest != item["sha256"]:
                sys.stderr.write("brat pkg: HASH MISMATCH %s\n" % item["file"])
                return 1
        sys.stdout.write("verified %d package(s)\n" % len(items))
        return 0
    if sub == "list":
        items = read_lock()
        if not items:
            sys.stdout.write("empty lock\n")
            return 0
        for it in items:
            who = it.get("name") or it.get("file")
            if it.get("version"):
                who += "@" + it["version"]
            sys.stdout.write("%s  %s  %s\n" % (who, it.get("sha256", "")[:12], it.get("url")))
        return 0
    if sub == "remove" and len(args) == 2:
        name = args[1]
        items = read_lock()
        new = [it for it in items if it.get("file") != name]
        if len(new) == len(items):
            sys.stderr.write("brat pkg: not in lock: %s\n" % name)
            return 1
        try:
            vendor = os.path.join(os.getcwd(), "vendor")
            try:
                vendor_mode = os.lstat(vendor).st_mode
            except FileNotFoundError:
                vendor_mode = None
            if vendor_mode is not None and (
                    not stat.S_ISDIR(vendor_mode) or stat.S_ISLNK(vendor_mode)):
                raise OSError("vendor path is not a real directory")
            target = os.path.join(vendor, name)
            try:
                target_mode = os.lstat(target).st_mode if vendor_mode is not None else None
            except FileNotFoundError:
                target_mode = None
            if target_mode is not None and not stat.S_ISREG(target_mode):
                raise OSError("vendor package is not a regular file")
            if target_mode is None:
                write_lock(new)
            else:
                with tempfile.TemporaryDirectory(prefix=".brat-remove-", dir=vendor) as stage:
                    backup = os.path.join(stage, name)
                    os.replace(target, backup)
                    try:
                        write_lock(new)
                    except OSError as error:
                        try:
                            os.replace(backup, target)
                        except OSError as rollback:
                            raise OSError("%s; rollback failed: %s" % (error, rollback)) from error
                        raise
        except OSError as e:
            sys.stderr.write("brat pkg: remove failed: %s\n" % e)
            return 1
        sys.stdout.write("removed %s\n" % name)
        return 0
    print_usage()
    return 2


def do_new(args):
    """`brat new NAME [--kind service|tool]`: a project that runs on the first
    try. Exit 2 on bad usage, 1 when the target directory is refused."""
    sc = scaffold_mod()
    rest = [a for a in args if a != "--"]
    name = None
    kind = sc.DEFAULT_KIND
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == "--kind" and i + 1 < len(rest):
            kind = rest[i + 1]
            i += 2
            continue
        if a.startswith("--kind="):
            kind = a.split("=", 1)[1]
            i += 1
            continue
        if a.startswith("-"):
            sys.stderr.write("brat new: unknown option %r\n" % a)
            print_usage()
            return 2
        if name is not None:
            sys.stderr.write("brat new: unexpected argument %r\n" % a)
            print_usage()
            return 2
        name = a
        i += 1
    if name is None:
        sys.stderr.write("brat new: NAME is required\n")
        print_usage()
        return 2
    if kind not in sc.KINDS:
        sys.stderr.write("brat new: unknown kind %r (want %s)\n"
                         % (kind, "|".join(sc.KINDS)))
        print_usage()
        return 2
    try:
        written = sc.write_project(name, kind, os.getcwd())
    except sc.ScaffoldError as e:
        sys.stderr.write("brat new: %s\n" % e)
        return 1
    sys.stdout.write("new %s (%s)\n" % (name, kind))
    for p in written:
        sys.stdout.write("  %s\n" % p)
    sys.stdout.write("\nnext:\n")
    for line in sc.run_hint(name, os.path.join(HERE, "cli.py"), os.getcwd()):
        sys.stdout.write("  %s\n" % line)
    return 0


def _gate_types(path):
    """True when the type checker rejects `path` (or fails): native output is refused.

    Fail closed: a crashing typer must not let a program through to C. Type
    errors used to be advisory for `build`, so an FFI argument of the wrong
    type reached C and segfaulted. The check runs through the same front end
    `brat check` uses, so one program is refused by one message whichever
    command asked.
    """
    native = _native_runner()
    if native is not None:
        r = subprocess.run([native, "check", os.path.abspath(path)],
                           capture_output=True, text=True, timeout=300)
        sys.stdout.write(r.stdout)
        sys.stdout.write(r.stderr)
        return r.returncode != 0
    brat = brat_mod()
    try:
        tp = _load_mod("brat_typer_build", "typer.py")
        tp.load_brat = lambda path=None: brat
        return tp.main(["check", path]) != 0
    except Exception as e:
        sys.stderr.write("type checker failed: %s: %s\n" % (type(e).__name__, e))
        return True

def _gate_typer(path):
    """Run the type checker in `check` mode; True when it reported errors.

    Its report is held back and only shown when it found something: a gate is
    not a `check`, and a clean run must print nothing at all.
    """
    import contextlib
    import io
    try:
        tp = _load_mod("brat_typer_cli", "typer.py")
    except Exception as e:  # noqa: BLE001 - a missing typer must not pass code
        sys.stderr.write("brat run: cannot load the type checker: %s\n" % e)
        return True
    tp.load_brat = lambda path=None: brat_mod()
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = tp.main(["check", path])
    except SyntaxError:
        return True
    if rc != 0 and out.getvalue():
        sys.stderr.write(out.getvalue())
    return rc != 0


def _gate_ownership(path, stream=sys.stderr):
    """Run the ownership checker before producing a native binary.

    Returns the list of diagnostics (empty == code is clean). Diagnostics are
    printed to `stream` so callers can say "errors above".

    `move` carries no runtime effect in any backend (SPEC.md 3), so the static
    checker is the only thing standing between a mistake and a silent alias.
    `brat run` does not gate ownership errors. `brat build` is the production
    path and does.
    """
    brat = brat_mod()
    own = own_mod()
    own.load_brat = lambda path=None: brat
    path = os.path.abspath(path)
    all_diags = []
    try:
        loaded = brat.load_file(path)
    except OSError as e:
        stream.write("brat build: cannot read %s: %s\n" % (path, e))
        return all_diags
    except brat.ModuleError as e:
        stream.write("brat: %s\n" % e)
        return [Diagnostic(path, None, "error", "B0000", str(e))]
    except SyntaxError as e:
        f = getattr(e, "brat_path", path)
        fsrc = getattr(e, "brat_src", _read_src(f))
        diags = [Diagnostic(f, parse_error_line(e), "error", "B0000", str(e))]
        stream.write(to_human(f, fsrc, diags))
        return diags

    progs = [(f.path, f.prog) for f in loaded.files]
    srcs = loaded.srcs

    def pub_sigs(fprog):
        out = {}
        for s in fprog[1]:
            node = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
            if isinstance(node, tuple) and node and node[0] == "fn":
                out[node[1]] = own.fn_modes(node[2])
            elif isinstance(node, tuple) and node and node[0] == "enum":
                for vname, _f in node[2]:
                    out[vname] = []
        return out

    per_file = [pub_sigs(fp) for _, fp in progs]
    for idx, (f, fprog) in enumerate(progs):
        merged = {}
        for j, ps in enumerate(per_file):
            if j != idx:
                merged.update(ps)
        errs, warns = own.check(fprog, merged)
        file_diags = _entries_to_diags(f, errs, "error")
        for wln, wmsg in warns:
            file_diags.append(Diagnostic(f, wln, "warning", classify(wmsg),
                                         wmsg))
        all_diags.extend(file_diags)
        stream.write(to_human(f, srcs[f], file_diags))
    return all_diags


def _parse_mem(s):
    """`--mem` value in bytes: a positive integer with optional K/M/G suffix
    (1024-based). None when the value is malformed or zero."""
    m = re.match(r"^(\d+)([KMG])?$", s.strip().upper())
    if not m:
        return None
    n = int(m.group(1)) << {"K": 10, "M": 20, "G": 30}.get(m.group(2) or "", 0)
    return n or None


def _has_errors(diags):
    """Only errors block a native build; warnings are printed and the build goes on."""
    return any(d.severity == "error" for d in diags)


def _check_json(files):
    """`brat check --json`: one object per module file, no prose, stable codes.

    The payload is `{"version": 1, "file": ..., "diagnostics": [...]}` per file,
    the same shape `diag.to_json` uses, so a caller parses JSON and never scrapes
    text. Exit status is 1 when any diagnostic is an error.
    """
    import io
    brat = brat_mod()
    own = own_mod()
    dg = _load_mod("brat_diag_cli", "diag.py")
    dg._load_brat = lambda: brat
    rc = 0
    for path in files:
        sink = io.StringIO()
        _gate_ownership(path, sink)          # human text is discarded here
        try:
            loaded = brat.load_file(os.path.abspath(path))
        except OSError as e:
            sys.stdout.write(dg.to_json(os.path.abspath(path), [
                dg.Diagnostic(os.path.abspath(path), None, "error", "P0001",
                              "cannot read: %s" % (e,))]))
            rc = 1
            continue
        except SyntaxError as e:
            f = getattr(e, "brat_path", os.path.abspath(path))
            sys.stdout.write(dg.to_json(f, [
                dg.Diagnostic(f, dg._parse_error_line_from(e), "error", "P0001",
                              str(e))]))
            rc = 1
            continue
        except Exception as e:
            sys.stdout.write(dg.to_json(os.path.abspath(path), [
                dg.Diagnostic(os.path.abspath(path), None, "error", "B0000",
                              str(e))]))
            rc = 1
            continue
        sigs = []
        for src_file in loaded.files:
            pub = {}
            for st in src_file.prog[1]:
                node = st[1] if (isinstance(st, tuple) and st and st[0] == "pub") else st
                if isinstance(node, tuple) and node and node[0] == "fn":
                    pub[node[1]] = own.fn_modes(node[2])
                elif isinstance(node, tuple) and node and node[0] == "enum":
                    for vname, _f in node[2]:
                        pub[vname] = []
            sigs.append(pub)
        for idx, src_file in enumerate(loaded.files):
            merged = {}
            for j, other in enumerate(sigs):
                if j != idx:
                    merged.update(other)
            diags, _prog = dg.check_program(src_file.path, loaded.srcs[src_file.path],
                                            merged)
            if any(d.severity == "error" for d in diags):
                rc = 1
            if idx == 0 or diags:
                sys.stdout.write(dg.to_json(src_file.path, diags))
    return rc


def do_build_ffi(args):
    """`brat build prog.brat --header lib.h`: prove the bindings, then build.

    The declaration in the file is the one the generated C uses, so a mismatch
    is not a warning to read later -- it is a program that would return garbage.
    The proof runs first, and only a proven surface reaches the compiler; a
    program that calls C and names no header is refused unless it says
    `--unsafe-ffi`.
    """
    header, unsafe, rest = _take_ffi_flags(args)
    files = [a for a in rest if not a.startswith("-") and a.endswith(".brat")]
    # The gate must judge the widths of the target being built for, not of the
    # machine running the build: the triple is parsed here, before do_build.
    target = None
    for i, a in enumerate(rest):
        if a == "--target" and i + 1 < len(rest):
            target = rest[i + 1]
        elif a.startswith("--target="):
            target = a.split("=", 1)[1]
    for f in files:
        rc = ffi_gate(f, header, target=target, unsafe=unsafe)
        if rc != 0:
            return rc
    return do_build(rest, header=header)


def do_build(args, header=None):
    cxx_dir = os.path.join(os.path.dirname(HERE), "cxx")
    native = os.path.join(cxx_dir, "bin", "brat-cxx")
    try:
        binary_mtime = os.path.getmtime(native)
        sources = [os.path.join(cxx_dir, "src", name)
                   for name in os.listdir(os.path.join(cxx_dir, "src"))
                   if name.endswith((".cpp", ".h"))]
        extern_src = [a for a in args if a.endswith(".brat") and not a.startswith("-")]
        if (sources and all(os.path.getmtime(source) <= binary_mtime for source in sources)
                and not (extern_src and _has_externs(extern_src[0]))):
            native_args = [arg for arg in args if arg != "--"]
            return subprocess.call([native, "build"] + native_args)
    except FileNotFoundError:
        pass

    rest = [a for a in args if a != "--"]
    rest = [a for a in rest if a != "--no-check"]
    out = None
    files = []
    target = None
    static = None          # None -> static by default
    want_static = False    # --static asked for explicitly: only that may pick zig
    shared = False
    mem = None
    i = 0
    while i < len(rest):
        if rest[i] == "-o" and i + 1 < len(rest):
            out = rest[i + 1]
            i += 2
            continue
        if rest[i] == "--mem" and i + 1 < len(rest):
            mem = _parse_mem(rest[i + 1])
            if mem is None:
                sys.stderr.write(
                    "brat build: bad --mem %r (want e.g. 16777216, 512K, 16M, 1G)\n"
                    % rest[i + 1])
                return 2
            i += 2
            continue
        if rest[i].startswith("--mem="):
            mem = _parse_mem(rest[i].split("=", 1)[1])
            if mem is None:
                sys.stderr.write(
                    "brat build: bad --mem %r (want e.g. 16777216, 512K, 16M, 1G)\n"
                    % rest[i])
                return 2
            i += 1
            continue
        if rest[i] == "--target" and i + 1 < len(rest):
            target = rest[i + 1]
            i += 2
            continue
        if rest[i].startswith("--target="):
            target = rest[i].split("=", 1)[1]
            i += 1
            continue
        if rest[i] == "--static":
            static = True
            want_static = True
            i += 1
            continue
        if rest[i] == "--dynamic":
            static = False
            i += 1
            continue
        if rest[i] == "--shared":
            shared = True
            static = False
            i += 1
            continue
        files.append(rest[i])
        i += 1
    if len(files) != 1:
        print_usage()
        return 2
    src = files[0]
    if out is None:
        if shared:
            out = os.path.splitext(src)[0] + ".so"
        else:
            out = os.path.splitext(src)[0] + ".exe"
    if _gate_types(src):
        sys.stderr.write(
            "brat build: refusing to build %s: the type checker reported errors "
            "above. Native code would trust those types.\n" % src)
        return 1
    diags = _gate_ownership(src, sys.stderr)
    if _has_errors(diags):
        sys.stderr.write(
            "brat build: refusing to build %s: the ownership checker reported "
            "errors above. `move` has no runtime effect (SPEC.md 3), so this is\n"
            "the only gate before a native binary. Fix it.\n" % src)
        return 1
    try:
        codegen = codegen_c_mod()
    except ImportError as e:
        sys.stderr.write("brat build: backend unavailable: %s\n" % e)
        return 1
    env = os.environ
    codegen.set_ffi_target(target)
    fd, cfile = tempfile.mkstemp(prefix="brat-build-", suffix=".c")
    os.close(fd)
    try:
        with open(cfile, "w", encoding="utf-8") as f:
            f.write((codegen.emit_file_c(os.path.abspath(src))
                     or codegen.emit_file(os.path.abspath(src)))
                    + _ffi_contract_block(os.path.abspath(src), header=header,
                                          target=target))
    except Exception as e:
        if parse_error_line(e) is not None:
            return syntax_error(src, _read_src(src), e)
        sys.stderr.write("brat build: emit %s: %s\n" % (type(e).__name__, e))
        return 1
    if static is None:
        static = sys.platform.startswith("linux")
    try:
        spec = resolve_cc(target=target, static=want_static)
    except CcError as e:
        try:
            os.unlink(cfile)
        except OSError:
            pass
        sys.stderr.write("brat build: %s\n" % e)
        return 1

    if shared:
        static = False
    argv = spec.argv(cfile, out, target=target, static=static)
    if mem is not None:
        argv.append("-DBRAT_MEM_BYTES=%d" % mem)
    try:
        _ffi_libs = codegen.extern_libs(os.path.abspath(src))
    except Exception:
        _ffi_libs = []
    _link_args = []
    _seen_libs = set()
    for _lib in _ffi_libs:
        if not _lib or _lib in _seen_libs:
            continue
        _seen_libs.add(_lib)
        if os.path.isabs(_lib) or os.path.exists(_lib):
            _link_args.append(_lib)
        else:
            _link_args.append("-l:" + _lib)
    if _ffi_libs and static and "--static" not in args:
        static = False
        argv = spec.argv(cfile, out, target=target, static=static)
    # `-fwrapv`: signed overflow is undefined in C, and the interpreter wraps.
    # Without it the native build's answer on an overflowing i64 is the
    # optimizer's opinion, which is not the contract the other backend ships.
    _cc_extra = ["-O2", "-fwrapv", "-I", HERE, "-lm"] + _link_args
    if shared:
        _cc_extra = ["-shared", "-fPIC"] + _cc_extra
    cc_argv = argv + _cc_extra
    r = cc_cached(cc_argv, cfile, out, timeout=300, env=env)
    try:
        os.unlink(cfile)
    except OSError:
        pass
    if r.returncode != 0:
        sys.stderr.write("brat build: cc failed:\n%s\n" % ((r.stdout + r.stderr)[-2000:]))
        return 1
    sys.stdout.write("build ok %s\n" % out)
    return 0


def _dist_artifact_name(stem, target):
    name = stem if target is None else "%s-%s" % (stem, target)
    if target and "windows" in target.lower():
        name += ".exe"
    return name


def do_dist(args):
    """`brat dist file.brat [-o dir] [--target t1,t2,...] [--static|--dynamic]`

    One C emission, one compilation per target; writes manifest.json with the
    sha256 and size of every artifact.
    """
    rest = [a for a in args if a != "--"]
    rest = [a for a in rest if a != "--no-check"]
    header, unsafe, rest = _take_ffi_flags(rest)
    out = None
    files = []
    target = None
    static = None
    want_static = False
    i = 0
    while i < len(rest):
        if rest[i] == "-o" and i + 1 < len(rest):
            out = rest[i + 1]
            i += 2
            continue
        if rest[i] == "--target" and i + 1 < len(rest):
            target = rest[i + 1]
            i += 2
            continue
        if rest[i].startswith("--target="):
            target = rest[i].split("=", 1)[1]
            i += 1
            continue
        if rest[i] == "--static":
            static = True
            want_static = True
            i += 1
            continue
        if rest[i] == "--dynamic":
            static = False
            i += 1
            continue
        if rest[i] == "--shared":
            shared = True
            static = False
            i += 1
            continue
        files.append(rest[i])
        i += 1
    if len(files) != 1:
        print_usage()
        return 2
    src = files[0]
    if out is None:
        out = "dist"
    if _gate_types(src):
        # The type gate reads the file first, so an unreadable path is named as
        # the load error it is rather than as a missing FFI proof.
        sys.stderr.write(
            "brat dist: refusing to dist %s: the check above failed.\n" % src)
        return 1
    gate = ffi_gate(src, header, target=target, who="brat dist", unsafe=unsafe)
    if gate:
        return gate
    diags = _gate_ownership(src, sys.stderr)
    if _has_errors(diags):
        sys.stderr.write(
            "brat dist: refusing to dist %s: the ownership check above "
            "failed.\n" % src)
        return 1
    try:
        codegen = codegen_c_mod()
    except ImportError as e:
        sys.stderr.write("brat dist: backend unavailable: %s\n" % e)
        return 1
    if static is None:
        static = True
    try:
        spec = resolve_cc(target=target, static=want_static)
    except CcError as e:
        sys.stderr.write("brat dist: %s\n" % e)
        return 1
    targets = [t.strip() for t in target.split(",")] if target else [None]
    targets = [t for t in targets if t != ""]
    try:
        os.makedirs(out, exist_ok=True)
    except OSError as e:
        sys.stderr.write("brat dist: cannot create %s: %s\n" % (out, e))
        return 1
    fd, cfile = tempfile.mkstemp(prefix="brat-dist-", suffix=".c")
    os.close(fd)
    # An `extern struct` is laid out by the target's rules and the emitted asserts
    # say which, so targets that disagree about a pointer's width get one
    # emission each. Everything else in the C is model-independent.
    model_of = _load_mod("brat_ffi", "ffi.py").data_model
    cfiles = {}
    try:
        for t in targets:
            model = model_of(t)
            if model in cfiles:
                continue
            codegen.set_ffi_target(t)
            text = ((codegen.emit_file_c(os.path.abspath(src))
                     or codegen.emit_file(os.path.abspath(src)))
                    + _ffi_contract_block(os.path.abspath(src), target=t))
            cpath = cfile if not cfiles else "%s.%d" % (cfile, len(cfiles))
            with open(cpath, "w", encoding="utf-8") as f:
                f.write(text)
            cfiles[model] = cpath
    except Exception as e:
        for _p in cfiles.values():
            try:
                os.unlink(_p)
            except OSError:
                pass
        try:
            os.unlink(cfile)
        except OSError:
            pass
        if parse_error_line(e) is not None:
            return syntax_error(src, _read_src(src), e)
        sys.stderr.write("brat dist: emit %s: %s\n" % (type(e).__name__, e))
        return 1
    try:
        _pre_libs = codegen.extern_libs(os.path.abspath(src))
    except Exception:
        _pre_libs = []
    if _pre_libs and static and "--static" not in args:
        static = False
    stem = os.path.splitext(os.path.basename(src))[0]
    artifacts = []
    failed = False
    for t in targets:
        out_name = _dist_artifact_name(stem, t)
        out_path = os.path.join(out, out_name)
        try:
            argv = spec.argv(cfiles[model_of(t)], out_path, target=t, static=static)
        except CcError as e:
            sys.stderr.write("brat dist: %s\n" % e)
            failed = True
            continue
        try:
            _ffi_libs = codegen.extern_libs(os.path.abspath(src))
        except Exception:
            _ffi_libs = []
        _link_args = []
        _seen_libs = set()
        for _lib in _ffi_libs:
            if not _lib or _lib in _seen_libs:
                continue
            _seen_libs.add(_lib)
            if os.path.isabs(_lib) or os.path.exists(_lib):
                _link_args.append(_lib)
            else:
                _link_args.append("-l:" + _lib)
        r = subprocess.run(argv + ["-O2", "-fwrapv", "-I", HERE, "-lm"] + _link_args,
                           capture_output=True, text=True, timeout=300, env=os.environ)
        if r.returncode != 0:
            sys.stderr.write("brat dist: cc %s failed:\n%s\n"
                             % (t or "host", (r.stdout + r.stderr)[-2000:]))
            failed = True
            continue
        try:
            with open(out_path, "rb") as f:
                data = f.read()
        except OSError as e:
            sys.stderr.write("brat dist: hash %s: %s\n" % (out_path, e))
            failed = True
            continue
        artifacts.append({
            "target": t if t is not None else "host",
            "artifact": out_name,
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
        })
    for _p in cfiles.values():
        try:
            os.unlink(_p)
        except OSError:
            pass
    manifest = {"source": os.path.abspath(src), "artifacts": artifacts}
    try:
        with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8",
                  newline="\n") as f:
            f.write(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    except OSError as e:
        sys.stderr.write("brat dist: write manifest: %s\n" % e)
        return 1
    if failed:
        sys.stderr.write("brat dist: some targets failed\n")
        return 1
    sys.stdout.write("dist ok %s (%d artifact(s))\n" % (out, len(artifacts)))
    return 0


def _load_modules(src):
    """(LoadedProgram, error message). One place for the loader import dance."""
    try:
        codegen = codegen_c_mod()
    except ImportError as e:
        return None, "backend unavailable: %s" % e
    try:
        return codegen._brat.load_file(os.path.abspath(src)), None
    except OSError as e:
        return None, str(e)
    except Exception as e:
        if parse_error_line(e) is not None:
            return None, "%s:%s" % (src, e)
        return None, "%s: %s" % (type(e).__name__, e)


def do_deps(args):
    """`brat deps file.brat [-o graph.json] [--json]`: which module imports which."""
    want_json = "--json" in args
    rest = [a for a in args if a not in ("--", "--json")]
    out = None
    files = []
    i = 0
    while i < len(rest):
        if rest[i] == "-o" and i + 1 < len(rest):
            out = rest[i + 1]
            i += 2
            continue
        if rest[i] == "--target" and i + 1 < len(rest):
            _bindgen_target = rest[i + 1]
            i += 2
            continue
        if rest[i].startswith("--target="):
            _bindgen_target = rest[i].split("=", 1)[1]
            i += 1
            continue
        files.append(rest[i])
        i += 1
    if len(files) != 1:
        print_usage()
        return 2
    loaded, err = _load_modules(files[0])
    if loaded is None:
        sys.stderr.write("brat deps: %s\n" % err)
        return 1
    edges = []
    for f in loaded.files:
        name = "main" if f.namespace is None else f.namespace.replace("__", ".")
        for imported, target in f.imports:
            to = "main" if target.namespace is None else target.namespace.replace("__", ".")
            edges.append({"from": name, "import": imported, "to": to})
    edges.sort(key=lambda e: (e["from"], e["import"]))
    graph = {"version": 1, "root": os.path.abspath(files[0]),
             "modules": len(loaded.files), "edges": edges}
    if want_json:
        sys.stdout.write(json.dumps(graph, indent=2, ensure_ascii=False) + "\n")
        return 0
    if out:
        with open(out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(json.dumps(graph, indent=2, ensure_ascii=False) + "\n")
        sys.stdout.write("deps ok %s (%d module(s), %d edge(s))\n"
                         % (out, len(loaded.files), len(edges)))
        return 0
    for e in edges:
        sys.stdout.write("%s -> %s (%s)\n" % (e["from"], e["to"], e["import"]))
    sys.stdout.write("%d module(s), %d import(s)\n" % (len(loaded.files), len(edges)))
    return 0


def _line_of_node(node):
    """Line the parser recorded for a statement node, or None."""
    import sys as _sys
    parser = _sys.modules.get("brat.parser")
    lines = getattr(parser, "_LINES", None) if parser is not None else None
    if not lines:
        return None
    return lines.get(id(node))


def _take_ffi_flags(args):
    """(header, unsafe, rest): the FFI flags, taken out of an argument list.

    Only the words before a `--` belong to `brat`: everything after it goes to the
    program being run, and a program is allowed an argument called `--header`.
    """
    header = None
    unsafe = False
    rest = []
    stop = args.index("--") if "--" in args else len(args)
    i = 0
    while i < len(args):
        a = args[i]
        if i < stop and a == "--header" and i + 1 < len(args):
            header = args[i + 1]
            i += 2
            continue
        if i < stop and a.startswith("--header="):
            header = a.split("=", 1)[1]
            i += 1
            continue
        if i < stop and a == "--unsafe-ffi":
            unsafe = True
            i += 1
            continue
        rest.append(a)
        i += 1
    return header, unsafe, rest


def ffi_unproven(path, who):
    """The refusal every native path shares: C without a proof is not built.

    A prototype that disagrees with the library is not a warning to read later --
    it is a call through the wrong ABI, which returns garbage or corrupts memory
    somewhere else. The two ways out are named, because a refusal without a way
    forward is a wall.
    """
    sys.stderr.write(
        "%s: refusing %s: it calls C and no header was given.\n"
        "  A prototype that disagrees with the library is undefined behaviour, not a\n"
        "  warning. Prove it, or say you are not going to:\n"
        "      %s %s --header FILE     the declarations compared to C\n"
        "      %s %s --unsafe-ffi      build it anyway, on your head\n"
        % (who, path, who, path, who, path))
    return 1


def ffi_gate(path, header, quiet=False, target=None, who="brat build", unsafe=False):
    """The proof every command that produces or runs a binary shares.

    `build`, `run`, `dist` and `emit-c` call this before anything is compiled, so
    one program gets one verdict. The same three witnesses `brat ffi` runs, in
    one call. Returns 0 when the surface is proven (or the program calls no C),
    1 when it is neither proven nor declared `--unsafe-ffi`.
    """
    ffi = _load_mod("brat_ffi", "ffi.py")
    brat = codegen_c_mod()._brat
    src, err = read_source(path)
    if err is not None:
        # Not "nothing to compare": a build that cannot read the source has not
        # checked it, and a silent 0 here would let it through.
        sys.stderr.write("%s: %s\n" % (who, err))
        return 2
    try:
        prog = brat.parse_prog(brat.Cur(brat.lex(src)))
    except Exception as e:  # noqa: BLE001 - the checker reports syntax already
        return 0
    body = prog[1]
    if not any(x and x[0] in ("extern", "externstruct") for x in body):
        return 0
    if not header:
        if unsafe:
            if not quiet:
                sys.stderr.write("%s: %s calls C without a proof (--unsafe-ffi)\n"
                                 % (who, path))
            return 0
        return ffi_unproven(path, who)
    ffi.set_target(target)
    cdecl = ffi.parse_header(ffi.read_header(header))
    rep = ffi.check(body, cdecl, header)
    ok, out = True, ""
    try:
        ok, out = ffi.compile_probe(body, os.path.abspath(header), cdecl=cdecl,
                                    target=target)
    except Exception as e:  # noqa: BLE001 - no compiler is not a binding error
        sys.stderr.write("%s: layout stage unavailable: %s\n" % (who, e))
    for ln in rep.lines:
        sys.stderr.write(ln + "\n")
    if out:
        sys.stderr.write(out + "\n")
    if rep.errors or not ok:
        sys.stderr.write("%s: refusing to build %s: %d binding error(s) against %s\n"
                         % (who, path, rep.errors, header))
        return 1
    if not quiet:
        sys.stderr.write("%s: %d binding(s) proven against %s\n"
                         % (who, rep.proven, header))
    return 0


def _emit_library(args):
    """`--emit-lib NAME` names the library for the generated `extern "..."`."""
    for i, a in enumerate(args):
        if a == "--emit-lib" and i + 1 < len(args):
            return args[i + 1]
        if a.startswith("--emit-lib="):
            return a.split("=", 1)[1]
    return None


def _ffi_bin_report(ffi, binpath, now, want_json):
    """What a built binary claims about its extern surface, and whether it still holds.

    `now` is the contract of the declarations in front of us, or None when there
    is no source to compare against -- then the answer is only what the binary
    itself says, which is still more than any other toolchain can tell you about
    a finished executable.
    """
    if not os.path.exists(binpath):
        sys.stderr.write("brat ffi: no such file: %s\n" % binpath)
        return 1
    rec = ffi.read_binary_contract(binpath)
    if rec is None:
        sys.stderr.write(
            "brat ffi: %s carries no FFI contract: built without one, or the block was\n"
            "          stripped. Nothing can be re-proved about it.\n" % binpath)
        return 1
    if now is None:
        if want_json:
            sys.stdout.write(json.dumps({"version": 1, "contract": rec},
                                        ensure_ascii=False, indent=2) + "\n")
            return 0
        tgt = rec.get("target") or {}
        proved = rec.get("proved") or {}
        sys.stdout.write("binary: %d extern fn(s), %d extern struct(s)\n"
                         % (len(rec.get("fns") or []), len(rec.get("structs") or [])))
        sys.stdout.write("target: %s (ptr %s, long %s, wide align %s)\n"
                         % (tgt.get("triple") or "host", tgt.get("ptr"), tgt.get("long"),
                            tgt.get("wide_align")))
        if proved:
            sys.stdout.write("proved: against %s (sha256 %s)\n"
                             % (proved.get("header"), (proved.get("sha256") or "")[:16]))
        else:
            sys.stdout.write("proved: against no header -- the binary carries an unproven "
                             "surface\n")
        return 0
    drift = ffi.contract_drift(rec, now)
    if want_json:
        sys.stdout.write(json.dumps({"version": 1, "bin": os.path.abspath(binpath),
                                     "contract": rec, "drift": drift},
                                    ensure_ascii=False, indent=2) + "\n")
        return 1 if drift else 0
    if drift:
        for line in drift:
            sys.stdout.write("  drift: %s\n" % line)
        sys.stdout.write("%s: the recorded proof no longer describes this source\n" % binpath)
        return 1
    sys.stdout.write("%s: contract matches (%d extern fn(s), %d extern struct(s), target %s)\n"
                     % (binpath, len(now.get("fns") or []), len(now.get("structs") or []),
                        (now.get("target") or {}).get("model")))
    return 0


def do_ffi(args):
    """`brat ffi file.brat --header h.h [--json]`: prove the bindings against C.

    The declarations in the file are compared to the declarations in the header:
    widths, arity, field order, variadic tails. A mismatch is an error here and
    would be undefined behavior at run time, which is the whole point -- this is
    the only FFI that can tell you the prototype is wrong before it runs.
    """
    want_json = "--json" in args
    header = None
    target = None
    binpath = None
    rest = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--bin" and i + 1 < len(args):
            binpath = args[i + 1]
            i += 2
            continue
        if a.startswith("--bin="):
            binpath = a.split("=", 1)[1]
            i += 1
            continue
        if a == "--header" and i + 1 < len(args):
            header = args[i + 1]
            i += 2
            continue
        if a.startswith("--header="):
            header = a.split("=", 1)[1]
            i += 1
            continue
        if a == "--emit-lib":
            i += 2          # takes a value: the library name, not a file to read
            continue
        if a == "--emit":
            i += 1          # a bare mode flag; `--emit=LIB` carries its own value
            continue
        if a.startswith("--emit=") or a.startswith("--emit-lib="):
            i += 1
            continue
        if a == "--target" and i + 1 < len(args):
            target = args[i + 1]
            i += 2
            continue
        if a.startswith("--target="):
            target = a.split("=", 1)[1]
            i += 1
            continue
        if a != "--json":
            rest.append(a)
        i += 1
    # `--emit` reads a header only: there is no BRAT file to compare yet.
    if len(rest) != 0 and len(rest) != 1:
        sys.stderr.write("brat ffi: wants one file\n")
        return 2
    if not rest and "--emit" not in args and not any(a.startswith("--emit=") for a in args) \
            and binpath is None:
        sys.stderr.write("brat ffi: wants one file\n")
        return 2
    ffi = _load_mod("brat_ffi", "ffi.py")
    # Widths are the target's: `long` is 4 bytes on i686 and on Windows, and the
    # comparison must say so before it compares anything.
    ffi.set_target(target)
    if binpath is not None and not rest:
        return _ffi_bin_report(ffi, binpath, None, want_json)
    try:
        brat = codegen_c_mod()._brat
        src = "fn main() -> i64 {\n  return 0\n}\n"
        if rest:
            src, err = read_source(rest[0])
            if err is not None:
                sys.stderr.write("brat ffi: %s\n" % err)
                return 1
        prog = brat.parse_prog(brat.Cur(brat.lex(src)))
    except OSError as e:
        sys.stderr.write("brat ffi: cannot read %s: %s\n" % (rest[0], e))
        return 1
    except Exception as e:
        sys.stderr.write("brat ffi: %s: %s\n" % (type(e).__name__, e))
        return 1
    body = prog[1]

    # `--emit` writes the bindings this header implies, from the same C view the
    # proof compares against: one mechanism, so what it generates is what it
    # can prove.
    emit_lib = None
    for i, a in enumerate(args):
        if a == "--emit" and i + 1 < len(args):
            emit_lib = args[i + 1]
        elif a.startswith("--emit="):
            emit_lib = a.split("=", 1)[1]
        elif a == "--emit-lib" and i + 1 < len(args):
            emit_lib = args[i + 1]
    cdecl = None
    header_name = header
    if header:
        try:
            cdecl = ffi.parse_header(ffi.read_header(header))
        except OSError as e:
            sys.stderr.write("brat ffi: cannot read %s: %s\n" % (header, e))
            return 1
    else:
        header_name = "<no header given>"

    if "--emit" in args or any(a.startswith("--emit=") for a in args) or "--emit-lib" in args:
        if cdecl is None:
            sys.stderr.write("brat ffi --emit needs --header: the bindings come from C\n")
            return 2
        text, notes = ffi.emit_bindings(body, cdecl, header_name, _emit_library(args))
        sys.stdout.write(text)
        for note in notes:
            sys.stderr.write("  note: %s\n" % note)
        return 0

    rep = ffi.check(body, cdecl, header_name) if cdecl is not None else None
    # The second opinion, when a header is there to compile against: the C
    # compiler re-declares every prototype and asserts every struct offset.
    cc_out, cc_ok = "", True
    if header and any(x and x[0] == "extern" for x in body):
        try:
            cc_ok, cc_out = ffi.compile_probe(body, os.path.abspath(header), cdecl=cdecl)
        except Exception as e:  # noqa: BLE001 - a missing cc must not hide the report
            cc_out, cc_ok = "compiler stage unavailable: %s: %s" % (type(e).__name__, e), True
    if binpath is not None:
        try:
            now = ffi.contract(body, header=header, target=target)
        except Exception as e:  # noqa: BLE001 - a report, not a crash
            sys.stderr.write("brat ffi: cannot describe this surface: %s\n" % e)
            return 1
        return _ffi_bin_report(ffi, binpath, now, want_json)

    missing = []
    try:
        missing = ffi.missing_symbols(body)
    except Exception as e:  # noqa: BLE001 - a symbol probe must not hide the report
        missing = [(None, "symbol check unavailable: %s" % e)]

    if want_json:
        out = {
            "version": 1,
            "file": os.path.abspath(rest[0]),
            "header": os.path.abspath(header) if header else None,
            "proven": rep.proven if rep else 0,
            "errors": rep.errors if rep else 0,
            "warnings": rep.warnings if rep else 0,
            "findings": [ln.strip() for ln in (rep.lines if rep else [])],
            "compiler": {"checked": bool(header), "ok": cc_ok,
                         "output": cc_out.splitlines() if cc_out else []},
            "missing": [{"symbol": n, "reason": r} for n, r in missing],
            "libraries": [os.path.abspath(l) if os.path.sep in l else l
                          for l in ffi.headers_for(body)],
        }
        sys.stdout.write(json.dumps(out, ensure_ascii=False, indent=2) + "\n")
    else:
        for ln in (rep.lines if rep else []):
            sys.stdout.write(ln + "\n")
        if cc_out:
            for ln in cc_out.splitlines():
                sys.stdout.write(("  error: " if not cc_ok else "  note: ") + ln + "\n")
        for n, r in missing:
            sys.stdout.write("  %s: %s\n" % ("error" if "linker resolves" not in r else "note", r))
        if rep is not None:
            sys.stdout.write("%s: %d binding(s) proven, %d error(s), %d warning(s)\n"
                             % (rest[0], rep.proven, rep.errors, rep.warnings))
        else:
            sys.stdout.write("%s: %d binding(s) checked against the library only\n"
                             % (rest[0], len([1 for x in body if x and x[0] == "extern"])))
    bad = ((rep is not None and rep.errors) or not cc_ok
           or any("linker resolves" not in r for _n, r in missing))
    return 1 if bad else 0


def do_api(args):
    """`brat api file.brat [--json]`: the exported surface of every module."""
    want_json = "--json" in args
    rest = [a for a in args if a not in ("--", "--json")]
    if len(rest) != 1:
        print_usage()
        return 2
    loaded, err = _load_modules(rest[0])
    if loaded is None:
        if want_json:
            sys.stdout.write(json.dumps({"version": 1, "root": os.path.abspath(rest[0]),
                                         "modules": [], "error": err},
                                        ensure_ascii=False, indent=2) + "\n")
            return 1
        sys.stderr.write("brat api: %s\n" % err)
        return 1
    if want_json:
        mods = []
        for f in loaded.files:
            if f.namespace is None:
                continue
            exports = []
            prefix = f.namespace + "__"
            for st in f.prog[1]:
                node = st[1] if st[0] == "pub" else st
                if node[0] != "fn":
                    continue
                original = node[1]
                if original.startswith(prefix):
                    original = original[len(prefix):]
                if original not in f.public:
                    continue
                exports.append({
                    "name": original,
                    "params": [{"name": p[0], "type": p[2] or "?"} for p in node[2]],
                    "ret": node[5] if len(node) > 5 and node[5] else "none",
                    "line": _line_of_node(node),
                })
            exports.sort(key=lambda e: e["name"])
            mods.append({"module": f.namespace.replace("__", "."),
                         "file": f.path, "exports": exports})
        mods.sort(key=lambda m: m["module"])
        sys.stdout.write(json.dumps({"version": 1, "root": os.path.abspath(rest[0]),
                                     "modules": mods},
                                    ensure_ascii=False, indent=2) + "\n")
        return 0
    sys.stdout.write("# %s\n\n" % os.path.basename(rest[0]))
    for f in loaded.files:
        if f.namespace is None:
            continue
        name = f.namespace.replace("__", ".")
        sigs = []
        for st in f.prog[1]:
            node = st[1] if st[0] == "pub" else st
            if node[0] != "fn":
                continue
            original = node[1]
            prefix = f.namespace + "__"
            if original.startswith(prefix):
                original = original[len(prefix):]
            if original not in f.public:
                continue
            params = ", ".join("%s: %s" % (p[0], p[2] or "?") for p in node[2])
            ret = node[5] if len(node) > 5 and node[5] else "none"
            sigs.append("`%s(%s) -> %s`" % (original, params, ret))
        sys.stdout.write("## %s\n\n" % name)
        if sigs:
            for sig in sorted(sigs):
                sys.stdout.write("- %s\n" % sig)
        else:
            sys.stdout.write("- (no exported functions)\n")
        sys.stdout.write("\n")
    return 0


# The data model `brat bindgen` maps widths for. `--target` sets it; the default
# is the host's LP64, which is what a plain build compiles for.
_BINDGEN_MODEL = {"model": "lp64"}


def _bindgen_model(triple):
    """Which data model a triple names, without importing ffi at module load."""
    if not triple:
        return "lp64"
    t = triple.lower()
    if "windows" in t or "win32" in t or "msvc" in t or "mingw" in t:
        return "llp64"
    if any(t.startswith(p) for p in ("i386", "i486", "i586", "i686", "arm", "armv",
                                     "thumb", "wasm32", "mips", "riscv32")):
        return "ilp32"
    return "lp64"


def _ptr_bits():
    return 64 if _BINDGEN_MODEL["model"] != "ilp32" else 32


def _long_bits():
    return 32 if _BINDGEN_MODEL["model"] in ("ilp32", "llp64") else 64


_RUST_PRIM = {
    "i8": "i32", "u8": "i32", "i16": "i32", "u16": "i32",
    "i32": "i32", "u32": "i32", "isize": "i64", "usize": "i64",
    "i64": "i64", "u64": "i64", "i128": "i64", "u128": "i64",
    "f32": "f64", "f64": "f64", "bool": "i32", "char": "i32",
}


def _map_rust_type(t, structs=(), enums=()):
    """Rust type as written -> (BRAT type, ffi-safe?).

    `#[repr(C)]` makes the layout C's; this is about the scalar mapping and
    about refusing what has no stable C layout instead of guessing. `String`,
    `&str`, `Vec`, references to non-`repr(C)` types: skipped with a comment.
    """
    t = " ".join(t.split())
    base = t.replace("const ", "").strip()
    if base.startswith("&"):
        return "ptr", True
    if base.startswith("*const ") or base.startswith("*mut "):
        return "ptr", True
    if base.startswith("[") or base.startswith("*"):
        return "ptr", True
    if base in _RUST_PRIM:
        return _RUST_PRIM[base], True
    if base in ("String", "str", "Vec", "Option", "Result", "Box", "Rc", "Arc"):
        return "ptr", False
    if base in enums:
        return "i32", True          # a fieldless repr(U) enum is its integer
    if base in structs:
        return base, True           # an extern struct, by value
    return "ptr", True              # unknown named type: opaque pointer


def _rust_items(src):
    """(structs, enums, fns) from a Rust source, `#[repr(C)]` only for layouts."""
    src = re.sub(r"//[^\n]*", " ", src)
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    structs, enums, fns, skipped = [], [], [], []

    def attrs_before(idx):
        """Attribute text above a declaration, ignoring `pub` and friends."""
        lines = src[:idx].rstrip().split("\n")
        out = []
        for line in reversed(lines):
            st = line.strip()
            if st.startswith("#["):
                out.append(st)
            elif st in ("pub", "unsafe", "pub(crate)", "pub(super)", "") and not out:
                continue
            elif st.startswith("pub(") and not out:
                continue
            else:
                break
        return out

    def balanced(text, start, open_ch, close_ch):
        depth = 0
        for i in range(start, len(text)):
            if text[i] == open_ch:
                depth += 1
            elif text[i] == close_ch:
                depth -= 1
                if depth == 0:
                    return i
        return -1

    for m in re.finditer(r"\bstruct\s+([A-Za-z_]\w*)\s*\{", src):
        name = m.group(1)
        attrs = attrs_before(m.start())
        if not any("repr(C" in a for a in attrs):
            skipped.append("%s: not #[repr(C)]" % name)
            continue
        end = balanced(src, src.index("{", m.end() - 1), "{", "}")
        if end < 0:
            continue
        body = src[src.index("{", m.end() - 1) + 1:end]
        raw_fields = []
        for part in body.split(","):
            part = part.strip()
            if not part or "fn " in part:
                continue
            fm = re.match(r"(?:pub\s+)?([A-Za-z_]\w*)\s*:\s*(.+)$", part, flags=re.S)
            if not fm:
                continue
            fname, fty = fm.group(1), fm.group(2).strip()
            raw_fields.append((fname, fty))
        structs.append((name, raw_fields))

    for m in re.finditer(r"\benum\s+([A-Za-z_]\w*)\s*\{", src):
        name = m.group(1)
        attrs = attrs_before(m.start())
        if not any("repr(C" in a or "repr(u" in a or "repr(i" in a for a in attrs):
            skipped.append("%s: not #[repr(C)] or #[repr(uN)]" % name)
            continue
        open_at = src.index("{", m.end() - 1)
        end = balanced(src, open_at, "{", "}")
        if end < 0:
            continue
        body = src[open_at + 1:end]
        if "(" in body or "{" in body:
            skipped.append("%s: enum with payload" % name)
            continue
        variants, nxt = [], 0
        for part in body.split(","):
            part = part.strip()
            if not part:
                continue
            vm = re.match(r"([A-Za-z_]\w*)\s*(?:=\s*(\d+))?$", part)
            if not vm:
                skipped.append("%s: cannot read variant `%s`" % (name, part))
                continue
            if vm.group(2) is not None:
                nxt = int(vm.group(2))
            variants.append((vm.group(1), nxt))
            nxt += 1
        enums.append((name, variants))


    known_structs = {n for n, _f in structs}
    known_enums = {n for n, _v in enums}
    for m in re.finditer(r"\b(?:(?:pub\s+)?unsafe\s+)?extern\s+\"C\"\s+fn\s+"
                         r"([A-Za-z_]\w*)\s*\(", src):
        name = m.group(1)
        open_at = src.index("(", m.end() - 1)
        close_at = balanced(src, open_at, "(", ")")
        if close_at < 0:
            continue
        params_txt = src[open_at + 1:close_at].strip()
        rest = src[close_at + 1:].lstrip()
        ret = "none"
        rm = re.match(r"->\s*([^{;]+)", rest)
        if rm:
            bty, safe = _map_rust_type(rm.group(1).strip(), known_structs, known_enums)
            if not safe:
                skipped.append("%s: return type `%s` has no C layout"
                               % (name, rm.group(1).strip()))
                continue
            ret = bty
        params = []
        ok = True
        if params_txt and params_txt != "":
            for idx, part in enumerate(params_txt.split(",")):
                part = part.strip()
                if not part:
                    continue
                pm = re.match(r"(?:mut\s+)?([A-Za-z_]\w*)\s*:\s*(.+)$", part, flags=re.S)
                if not pm:
                    if part in ("self", "&self"):
                        skipped.append("%s: method, not an FFI function" % name)
                        ok = False
                        break
                    pname, pty = "a%d" % idx, part
                else:
                    pname, pty = pm.group(1), pm.group(2).strip()
                bty, safe = _map_rust_type(pty, known_structs, known_enums)
                if not safe:
                    skipped.append("%s: parameter `%s` has no C layout" % (name, pty))
                    ok = False
                    break
                params.append((pname, bty))
        if ok:
            fns.append((name, params, ret))
    return structs, enums, fns, skipped


# A name in `extern fn`, `extern struct` or one of its fields is the C symbol or
# the C member, written into the generated C as is, so it cannot be renamed: a
# name the BRAT parser will not read back (a keyword) makes the whole binding
# file invalid, and the decl is skipped with the reason instead.
_BRAT_KW = {
    "let", "mut", "fn", "if", "else", "while", "return", "true", "false", "or",
    "try", "catch", "spawn", "match", "for", "in", "import", "defer", "move",
    "break", "continue", "impl", "self", "trait", "enum", "const", "pub",
    "extern", "unsafe", "export", "use",
}


_C_KW = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if", "int",
    "long", "register", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "typedef", "union", "unsigned", "void", "volatile",
    "while",
}


def _bindgen_rust(src, path):
    structs, enums, fns, skipped = _rust_items(src)
    known_structs = {n for n, _f in structs}
    known_enums = {n for n, _v in enums}
    lines = ["// generated by brat bindgen from %s" % os.path.basename(path)]
    for name, raw_fields in structs:
        fields = []
        for fname, fty in raw_fields:
            bty, safe = _map_rust_type(fty, known_structs, known_enums)
            if not safe:
                skipped.append("%s.%s: `%s` has no C layout" % (name, fname, fty))
                continue
            fields.append((fname, bty))
        if fields:
            lines.append("extern struct %s { %s }"
                         % (name, ", ".join("%s: %s" % f for f in fields)))
    for name, variants in enums:
        lines.append("")
        for vname, value in variants:
            lines.append("const %s: i32 = %d" % (vname, value))
    lines.append("")
    lines.append("extern {")
    for name, params, ret in fns:
        lines.append("    extern fn %s(%s) -> %s;" %
                     (name, ", ".join("%s: %s" % p for p in params), ret))
    lines.append("}")
    for note in skipped:
        lines.append("// skipped %s" % note)
    return "\n".join(lines) + "\n"


def do_bindgen(args):
    rest = [a for a in args if a != "--"]
    out = None
    _bindgen_target = None
    _bindgen_lang = "brat"
    files = []
    i = 0
    while i < len(rest):
        if rest[i] == "-o" and i + 1 < len(rest):
            out = rest[i + 1]
            i += 2
            continue
        if rest[i] == "--target" and i + 1 < len(rest):
            _bindgen_target = rest[i + 1]
            i += 2
            continue
        if rest[i].startswith("--target="):
            _bindgen_target = rest[i].split("=", 1)[1]
            i += 1
            continue
        if rest[i] == "--lang" and i + 1 < len(rest):
            _bindgen_lang = rest[i + 1]
            i += 2
            continue
        if rest[i].startswith("--lang="):
            _bindgen_lang = rest[i].split("=", 1)[1]
            i += 1
            continue
        files.append(rest[i])
        i += 1
    if len(files) != 1:
        print_usage()
        return 2
    # Widths in the generated file belong to the target, not to this machine.
    _BINDGEN_MODEL["model"] = _bindgen_model(_bindgen_target)
    path = files[0]
    src, err = read_source(path)
    if err is not None:
        sys.stderr.write("brat bindgen: %s\n" % err)
        return 1
    # `--lang python|go|rust` writes the same C view for another language. One
    # source, four outputs: a header that means one thing in BRAT and another
    # in ctypes would be the four-generators bug again.
    lang = _bindgen_lang
    if lang not in ("brat", "python", "go", "rust"):
        sys.stderr.write("brat bindgen: --lang is brat, python, go or rust\n")
        return 2
    if path.endswith(".rs"):
        out_text = _bindgen_rust(src, path)
        if out:
            with open(out, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(out_text)
            sys.stdout.write("bindgen ok %s\n" % out)
        else:
            sys.stdout.write(out_text)
        return 0
    # One bindgen: the C view `check` verifies is the same view that is
    # generated from, so a generated type and a proven type cannot drift. This
    # used to be a second, regex-based scanner here, and the two had already
    # diverged (`const char *` became `i32`, `long double` became `ptr`).
    ffi = _load_mod("brat_ffi", "ffi.py")
    ffi.set_target(_bindgen_target)
    cdecl = ffi.parse_header(ffi.read_header(path) if os.path.exists(path) else src)
    if lang != "brat":
        text, _notes = ffi.LANG_EMITTERS[lang](cdecl, path, None)
    else:
        text, _notes = ffi.emit_bindings([], cdecl, os.path.basename(path))
    n = len(cdecl.fns)
    if out:
        try:
            with open(out, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        except OSError as e:
            sys.stderr.write("brat bindgen: cannot write %s: %s\n" % (out, e))
            return 1
        sys.stdout.write("bindgen ok %s (%d decl(s))\n" % (out, n))
    else:
        sys.stdout.write(text)
    return 0
def do_explain(args):
    """`brat explain [CODE]`: the diagnostics, as a manual page.

    Every error carries a stable code; this turns the code back into a
    sentence a newcomer can act on, which is cheaper than a web search and
    works offline. No code: list them all, sorted, so the list is a browsable
    index rather than a wall of text.
    """
    import json as _json
    dg = _load_mod("brat_diag", "diag.py")
    want_json = "--json" in args
    codes = [a for a in args if a != "--json"]
    table = dg._EXPLAIN
    if not codes:
        if want_json:
            sys.stdout.write(_json.dumps(
                [{"code": c, "title": v[0]} for c, v in sorted(table.items())],
                indent=2) + "\n")
            return 0
        sys.stdout.write("%d diagnostic codes (brat explain CODE for one)\n" % len(table))
        for code, (title, _what, _fix) in sorted(table.items()):
            sys.stdout.write("  %-6s %s\n" % (code, title))
        sys.stdout.write("\nA code is stable: it means the same thing in every release.\n")
        return 0
    rc = 0
    for i, code in enumerate(codes):
        key = code.strip().upper()
        if key not in table:
            sys.stderr.write("brat explain: no code `%s` (brat explain lists them)\n" % code)
            rc = 1
            continue
        title, what, fix = table[key]
        if want_json:
            sys.stdout.write(_json.dumps(
                {"code": key, "title": title, "what": what, "fix": fix},
                indent=2) + "\n")
            continue
        if i:
            sys.stdout.write("\n")
        sys.stdout.write("%s: %s\n" % (key, title))
        sys.stdout.write("\n  what: %s\n" % what)
        sys.stdout.write("  fix:  %s\n" % fix)
    return rc


def main(argv=None):
    argv = sys.argv if argv is None else argv
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(encoding='utf-8', errors='replace')
        except (AttributeError, ValueError):
            pass
    if len(argv) < 2:
        print_usage()
        return 2
    cmd = argv[1]
    rest = argv[2:]
    # `--help` anywhere before `--` asks this tool for usage, which is what every
    # other command line does; `brat run x.brat -- --help` still reaches the
    # program's own argv.
    head = rest[:rest.index("--")] if "--" in rest else rest
    if cmd in ("-h", "--help") or "-h" in head or "--help" in head:
        print_usage()
        return 0
    if cmd == "run":
        return do_run(rest)
    if cmd == "check":
        return do_check(rest)
    if cmd == "fmt":
        return do_fmt(rest)
    if cmd == "emit-c":
        return do_emit_c(rest)
    if cmd == "test":
        return do_test(rest)
    if cmd == "build":
        # Every build reads the FFI gate, header or not: a build that calls C
        # without a proof is the thing this command refuses.
        return do_build_ffi(rest)
    if cmd == "dist":
        return do_dist(rest)
    if cmd == "pkg":
        return do_pkg(rest)
    if cmd == "new":
        return do_new(rest)
    if cmd == "bindgen":
        return do_bindgen(rest)
    if cmd == "deps":
        return do_deps(rest)
    if cmd == "api":
        return do_api(rest)
    if cmd == "ffi":
        return do_ffi(rest)
    if cmd == "lsp":
        binary = os.path.join(os.path.dirname(HERE), "cxx", "bin", "brat-cxx")
        if not os.path.isfile(binary):
            sys.stderr.write("brat: C++ language server is not built; compile lang/cxx/src/*.cpp\n")
            return 2
        return subprocess.call([binary, "lsp"])
    if cmd == "explain":
        return do_explain(rest)
    if cmd == "mcp":
        mcp = _load_mod("brat_mcp", "mcp.py")
        return mcp.main()
    sys.stderr.write("brat: unknown command %r\n" % cmd)
    print_usage()
    return 2

if __name__ == "__main__":
    sys.exit(main())