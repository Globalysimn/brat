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

from toolchain import CcError, resolve_cc

HERE = os.path.dirname(os.path.abspath(__file__))
_BRAT_MOD = None
_OWN_MOD = None
_SCAFFOLD_MOD = None
_FFI_MOD = None
_VERSION_MOD = None

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

def version_mod():
    global _VERSION_MOD
    if _VERSION_MOD is None:
        _VERSION_MOD = _load_mod("brat_version_cli", os.path.join("brat", "version.py"))
    return _VERSION_MOD

def own_mod():
    global _OWN_MOD
    if _OWN_MOD is None:
        _OWN_MOD = _load_mod("brat_borrow_cli", "borrow.py")
    return _OWN_MOD

def ffi_mod():
    global _FFI_MOD
    if _FFI_MOD is None:
        _FFI_MOD = _load_mod("brat_ffi", "ffi.py")
    return _FFI_MOD

def scaffold_mod():
    global _SCAFFOLD_MOD
    if _SCAFFOLD_MOD is None:
        _SCAFFOLD_MOD = _load_mod("brat_scaffold_cli", "scaffold.py")
    return _SCAFFOLD_MOD

def _fmt_is_raw_open(line, quote_idx):
    if quote_idx <= 0 or line[quote_idx - 1] != "r":
        return False
    if quote_idx >= 2 and (line[quote_idx - 2].isalnum() or line[quote_idx - 2] == "_"):
        return False
    return True


def normalize_equals(line, in_blk=False):
    out = []
    i = 0
    n = len(line)
    in_str = False
    raw_str = False
    while i < n:
        c = line[i]
        if in_blk:
            out.append(c)
            if c == "*" and i + 1 < n and line[i + 1] == "/":
                out.append("/")
                in_blk = False
                i += 2
                continue
            i += 1
            continue
        if in_str:
            out.append(c)
            if not raw_str and c == "\\":
                if i + 1 < n:
                    out.append(line[i + 1])
                    i += 2
                    continue
            elif c == '"':
                in_str = False
                raw_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            raw_str = _fmt_is_raw_open(line, i)
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            out.append(line[i:])
            break
        if c == "/" and i + 1 < n and line[i + 1] == "*":
            in_blk = True
            out.append("/*")
            i += 2
            continue
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

def count_braces(line, in_str=False, raw_str=False, in_blk=False):
    """(opens, closes, in_str, raw_str, in_blk) for one line."""
    opens = 0
    closes = 0
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if in_blk:
            if c == "*" and i + 1 < n and line[i + 1] == "/":
                in_blk = False
                i += 2
                continue
            i += 1
            continue
        if in_str:
            if not raw_str and c == "\\":
                i += 2 if i + 1 < n else 1
                continue
            if c == '"':
                in_str = False
                raw_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            raw_str = _fmt_is_raw_open(line, i)
            i += 1
            continue
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        if c == "/" and i + 1 < n and line[i + 1] == "*":
            in_blk = True
            i += 2
            continue
        if c == "{":
            opens += 1
        elif c == "}":
            closes += 1
        i += 1
    return opens, closes, in_str, raw_str, in_blk

def fmt_src(src):
    src = src.replace("\r\n", "\n").replace("\r", "\n")
    lines = src.splitlines()
    out = []
    depth = 0
    in_str = False
    raw_str = False
    in_blk = False
    for raw in lines:
        if in_str:
            # Inside a literal these bytes are data, not layout: keep them as
            # written and only look for the quote that ends the literal.
            out.append(raw)
            o, c, in_str, raw_str, in_blk = count_braces(raw, True, raw_str, in_blk)
            depth = depth + (o - c)
            if depth < 0:
                depth = 0
            continue
        stripped = raw.strip()
        if stripped == "":
            out.append("")
            continue
        norm = normalize_equals(stripped, in_blk).strip()
        if norm == "":
            out.append("")
            continue
        orig = depth
        if not in_blk and norm.startswith("}"):
            depth = depth - 1 if depth > 0 else 0
        out.append("  " * depth + norm)
        o, c, in_str, raw_str, in_blk = count_braces(norm, False, False, in_blk)
        depth = orig + (o - c)
        if depth < 0:
            depth = 0
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out) + "\n"

def print_usage():
    sys.stderr.write("usage: brat --version\n")
    sys.stderr.write("       brat file.brat [args...]\n")
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
    sys.stderr.write("       brat deps file.brat [--json|--dot] [-o graph]\n")
    sys.stderr.write("       brat api file.brat [--json]\n")
    sys.stderr.write("       brat api --diff OLD.bri NEW.bri [--json]\n")
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


def _interpreter_chosen():
    return bool(os.environ.get("BRAT_INTERP") or os.environ.get("BRAT_NO_NATIVE_CLI"))


def _native_binary():
    override = os.environ.get("BRAT_CXX")
    if override:
        return os.path.abspath(override)
    return os.path.join(os.path.dirname(HERE), "cxx", "bin", "brat-cxx")


def _native_tool_binary():
    """The built C++ tool, regardless of interpreter-selection switches."""
    binary = _native_binary()
    if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
        return None
    if os.environ.get("BRAT_CXX"):
        return binary
    src = os.path.join(os.path.dirname(HERE), "cxx", "src")
    try:
        newest = max(os.path.getmtime(os.path.join(src, f))
                     for f in os.listdir(src) if f.endswith((".cpp", ".h")))
    except OSError:
        return None
    return binary if os.path.getmtime(binary) >= newest else None


def _native_required(who):
    binary = _native_tool_binary()
    if binary is not None:
        return binary
    path = _native_binary()
    if os.path.isfile(path):
        why = "it is older than the C++ sources"
    elif os.environ.get("BRAT_CXX"):
        why = "BRAT_CXX does not point to an executable"
    else:
        why = "it has not been built"
    sys.stderr.write(
        "%s: native compiler unavailable: %s (%s). Run `make` first.\n"
        % (who, path, why))
    return None


def _native_runner():
    """The C++ binary when it is built and current, else None.

    `brat run` is native by default: check, emit, link once into a
    content-addressed cache, exec -- 4 ms warm against the interpreter's 91 ms on
    a program that prints one line. `--interp` and `BRAT_INTERP=1` select the
    reference implementation instead, which is what the suite diffs against;
    `brat.py FILE` runs the same interpreter directly.
    """
    if _interpreter_chosen():
        return None
    return _native_tool_binary()


def _native_file_package_url(url):
    try:
        import urllib.parse as _up
        parsed = _up.urlsplit(url)
        filename = _up.unquote(parsed.path).rsplit("/", 1)[-1]
        return parsed.scheme == "file" and _valid_package_file(filename)
    except (TypeError, ValueError):
        return False


def _native_file_registry():
    index_path = os.environ.get("BRAT_INDEX") or "brat-index.json"
    if "://" in index_path:
        return False
    try:
        with open(index_path, encoding="utf-8") as index_file:
            index = json.load(index_file)
        packages = index.get("packages") if isinstance(index, dict) else None
        if not isinstance(packages, dict) or not packages:
            return False
        import urllib.parse as _up
        for versions in packages.values():
            if not isinstance(versions, dict) or not versions:
                return False
            for entry in versions.values():
                if not isinstance(entry, dict) or \
                        _up.urlsplit(str(entry.get("url", ""))).scheme != "file":
                    return False
        return True
    except (OSError, UnicodeError, ValueError):
        return False


def _has_arena_tokens(src):
    try:
        toks = brat_mod().lex(src)
        return any(toks[i][1] == "arena" and toks[i + 1][1] == "{"
                   for i in range(len(toks) - 1))
    except Exception:
        return bool(re.search(r"\barena\s*\{", src))


def do_run(args):
    # `brat run` is the product path: native by default, because the interpreter
    # costs 91 ms of startup and a 3M-iteration loop costs 20 s against 0.01 s.
    # The interpreter is one flag away and is what the suite compares against.
    if "--" in args:
        pre_dd = args[:args.index("--")]
        post_dd = args[args.index("--"):]
    else:
        pre_dd = list(args)
        post_dd = []
    want_interp = "--interp" in pre_dd
    no_check = "--no-check" in pre_dd
    trace_lines = ("--trace-lines" in pre_dd or
                   os.environ.get("BRAT_TRACE", "") not in ("", "0"))
    pre_dd = [a for a in pre_dd
              if a not in ("--interp", "--native", "--no-check", "--trace-lines")]
    args = pre_dd + post_dd
    header, objects, unsafe, args = _take_ffi_flags_ex(args)
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
    if native is None and not want_interp and not _interpreter_chosen():
        sys.stderr.write(
            "brat run: %s is missing, not executable or older than cxx/src, so this runs in the "
            "interpreter: slower, and not always the answer compiled code gives. Build it, or "
            "pass --interp to choose the interpreter.\n" % _native_binary())
    if native is not None:
        argv = [native, "run"]
        if unsafe:
            argv.append("--unsafe-ffi")
        if header is not None:
            argv.extend(["--header", header])
        for obj in objects:
            argv.extend(["--object", obj])
        if trace_lines:
            argv.append("--trace-lines")
        argv.append(path)
        if extra:
            argv.append("--")
            argv.extend(extra)
        return subprocess.call(argv)
    # The interpreter still crosses the C ABI through ctypes, so it needs the
    # same header/.bratbind proof as native execution, or --unsafe-ffi. This
    # conservative scan does not gate a program containing declarations alone.
    if _program_calls_c(path):
        gate = ffi_gate(path, header, target=None, who="brat run", unsafe=unsafe,
                        objects=objects)
        if gate:
            return gate
    # Default checker gate on `--interp`: ownership, capability and type rules
    # run before interpreting so `brat run --interp` enforces the same static
    # contract as `brat run` and `brat check`. `--no-check` opts out for
    # deliberate low-level runtime-guard tests, except where arena/spawn/plugin
    # memory safety strictly requires the checker.
    gated = (_has_arena_tokens(src),
             re.search(r"\bspawn\b", src) is not None,
             re.search(r"\blib_open\b", src) is not None)
    if no_check and any(gated):
        why = ("arena blocks depend on the escape rules" if gated[0] else
               "spawn blocks depend on the sharing rule" if gated[1] else
               "plugin handles depend on the lifetime rule")
        sys.stderr.write(
            "brat run: refusing --no-check for %s: %s, which only the checker "
            "applies.\n" % (path, why))
        return 1
    if not no_check:
        diags = _gate_ownership(path)
        typer_bad = _gate_typer(path)
        # A warning is not a refusal: only an error blocks the run.
        if _has_errors(diags) or typer_bad:
            why = ("Arena blocks depend on the escape rules." if gated[0] else
                   "The sharing rule is what keeps two threads off one value."
                   if gated[1] else
                   "A plugin value belongs to its library; the lifetime rule "
                   "is what stops a call into a closed one." if gated[2] else
                   "Fix it, or pass --no-check to override deliberately.")
            sys.stderr.write(
                "brat run: refusing to run %s: the checker reported errors "
                "above. %s\n" % (path, why))
            return 1
    return brat_mod().run_file(path, extra)

def do_check(args):
    want_json = "--json" in args
    want_py = "--py" in args
    files = [a for a in args
             if a not in ("--", "--json", "--py", "--native")]
    if not files:
        print_usage()
        return 2
    # The plain case is the C++ checker when it is built and current: `run`
    # already decides with it, and two commands in one toolchain refusing
    # different programs is a contract nobody can hold in their head. `--py`
    # and `BRAT_INTERP=1` name the reference checker explicitly, which is what
    # the differential tests call -- they compare the two, so they must not be
    # able to silently compare one with itself.
    native = None if want_py else _native_runner()
    if native is not None:
        os.execv(native, [native, "check"] + (["--json"] if want_json else []) + files)
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


def do_fmt(args):
    native = _native_runner()
    if native is not None:
        os.execv(native, [native, "fmt"] + list(args))
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
        tmp_dir = os.path.dirname(os.path.abspath(path)) or "."
        tmp = None
        try:
            st_mode = None
            try:
                st_mode = stat.S_IMODE(os.stat(path).st_mode)
            except OSError:
                pass
            fd, tmp = tempfile.mkstemp(dir=tmp_dir, prefix=".brat-fmt-")
            with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
                f.write(formatted)
                f.flush()
                os.fsync(f.fileno())
            if st_mode is not None:
                os.chmod(tmp, st_mode)
            os.replace(tmp, path)
        except OSError as e:
            sys.stderr.write("brat fmt: cannot write %s: %s\n" % (path, e))
            if tmp is not None:
                try:
                    os.unlink(tmp)
                except OSError:
                    pass
            code = 1
    return code

def _stmt_externs(stmts):
    out = []
    for x in stmts or ():
        if not x:
            continue
        if x[0] in ("extern", "externstruct"):
            out.append(x)
        elif x[0] == "pub" and len(x) > 1:
            inner = x[1] if isinstance(x[1], list) else [x[1]]
            out.extend(_stmt_externs(inner))
    ffi = ffi_mod()
    out.extend(ffi.plugin_externs(stmts))
    return out


def _stmt_calls_c(stmts):
    if _stmt_externs(stmts):
        return True
    ffi = ffi_mod()
    return ffi.has_plugin_calls(stmts)




def _program_calls_c(path):
    """Find foreign-call sites in loaded code without control-flow reachability analysis."""
    try:
        loaded, _err = _load_modules(path)
        if loaded is None:
            return True
        ffi = ffi_mod()
        extern_names = set()
        for source_file in loaded.files:
            for node in _stmt_externs(source_file.prog[1]):
                if isinstance(node, tuple) and len(node) > 2 and node[0] == "extern":
                    extern_names.add(node[2])
        stack = [loaded.prog]
        while stack:
            node = stack.pop()
            if isinstance(node, list):
                stack.extend(node)
                continue
            if not isinstance(node, tuple) or not node:
                continue
            if (node[0] == "call" and len(node) > 1 and isinstance(node[1], str)
                    and node[1] in extern_names):
                return True
            stack.extend(node[1:])
        return ffi.has_plugin_calls(loaded.prog[1])
    except Exception:  # noqa: BLE001 - uncertainty takes the proof-gated path
        return True




def do_emit_c(args):
    """Emit C through the native compiler, the only C emitter.

    The Python front end remains the reference interpreter and checker;
    generating C is a native-tool operation, like linking the result.
    """
    native = _native_required("brat emit-c")
    if native is None:
        return 1
    return subprocess.call([native, "emit-c"] + list(args))

def lock_path():
    return os.path.join(os.getcwd(), "brat.lock")


MAX_PACKAGE_BYTES = 16 * 1024 * 1024


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


def _valid_archive_name(name):
    if not isinstance(name, str) or not (1 <= len(name) <= 64):
        return False
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name):
        return False
    low = name.casefold()
    return low not in {"con", "prn", "aux", "nul", "std"} and not re.fullmatch(r"(?:com|lpt)[1-9]", low)


_ARCHIVE_TEXT_EXTS = (".brat", ".bri", ".bratbind", ".c", ".h")
_ARCHIVE_BIN_EXTS = (".o", ".obj", ".a", ".lib", ".so", ".dylib", ".dll")
_ARCHIVE_ALL_EXTS = _ARCHIVE_TEXT_EXTS + _ARCHIVE_BIN_EXTS


def _archive_file_ext(filename):
    for ext in _ARCHIVE_ALL_EXTS:
        if filename.endswith(ext):
            return ext
    return None


def _valid_archive_relpath(rel, archive_name):
    if not isinstance(rel, str) or not (1 <= len(rel) <= 256):
        return False
    if "\\" in rel or rel.startswith("/") or rel.endswith("/"):
        return False
    parts = rel.split("/")
    if len(parts) < 2 or parts[0] != archive_name:
        return False
    if len(parts) == 2 and parts[1] == "brat.pkg.json":
        return True
    ext = _archive_file_ext(parts[-1])
    if ext is None:
        return False
    reserved = {"con", "prn", "aux", "nul"}
    for i, part in enumerate(parts):
        if i < len(parts) - 1:
            if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", part):
                return False
            stem = part.casefold()
        else:
            base = part[:-len(ext)]
            if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", base):
                return False
            stem = base.casefold()
        if stem in reserved or re.fullmatch(r"(?:com|lpt)[1-9]", stem):
            return False
    return True


def _validate_lock_items(items):
    import urllib.parse as _up
    if not isinstance(items, list):
        raise ValueError("lockfile wants a JSON list")
    files = set()
    archives = set()
    names = set()
    for item in items:
        if not isinstance(item, dict):
            raise ValueError("lockfile entry must be an object")
        archive = item.get("archive")
        if archive is not None:
            if "file" in item or not _valid_archive_name(archive):
                raise ValueError("lockfile entry has an unsafe archive package name")
            arc_key = archive.casefold()
            if arc_key in archives or (arc_key + ".brat") in files:
                raise ValueError("lockfile has duplicate package %s" % archive)
            archives.add(arc_key)
            arc_files = item.get("files")
            if not isinstance(arc_files, dict) or not arc_files:
                raise ValueError("%s has no valid file manifest" % archive)
            for rel, fhash in arc_files.items():
                if not _valid_archive_relpath(rel, archive):
                    raise ValueError("%s has an unsafe archive file path" % archive)
                if not isinstance(fhash, str) or not re.fullmatch(r"[0-9a-f]{64}", fhash):
                    raise ValueError("%s has an invalid SHA-256 for %s" % (archive, rel))
                rkey = rel.casefold()
                if rkey in files:
                    raise ValueError("lockfile has duplicate package filename %s" % rel)
                files.add(rkey)
            c_sources = item.get("c_sources")
            if c_sources is not None:
                if not isinstance(c_sources, list) or not c_sources:
                    raise ValueError("%s has an invalid c_sources list" % archive)
                for cs in c_sources:
                    if not isinstance(cs, str) or not cs.endswith(".c") or cs not in arc_files:
                        raise ValueError("%s lists untracked or non-.c source %r" % (archive, cs))
            label = archive
        else:
            filename = item.get("file")
            if not _valid_package_file(filename):
                raise ValueError("lockfile entry has an unsafe package filename")
            file_key = filename.casefold()
            if file_key in files or filename[:-5].casefold() in archives:
                raise ValueError("lockfile has duplicate package filename %s" % filename)
            files.add(file_key)
            label = filename
        url = item.get("url")
        if not isinstance(url, str) or not url or any(ord(c) < 32 for c in url):
            raise ValueError("%s has no valid URL" % label)
        parsed = _up.urlsplit(url)
        if parsed.scheme not in ("https", "http", "file"):
            raise ValueError("%s has an unsupported URL scheme" % label)
        if parsed.scheme in ("https", "http"):
            if not parsed.hostname or parsed.username or parsed.password:
                raise ValueError("%s has an invalid network URL" % label)
        elif parsed.netloc not in ("", "localhost"):
            raise ValueError("%s has an invalid file URL" % label)
        expected = item.get("sha256")
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError("%s has no valid SHA-256" % label)
        name = item.get("name")
        version = item.get("version")
        if archive is None and (name is not None or version is not None):
            if not _valid_pkg_name(name) or not _valid_pkg_version(version):
                raise ValueError("%s has an invalid package identity" % label)
            name_key = name.casefold()
            if name_key in names:
                raise ValueError("lockfile has duplicate package %s" % name)
            names.add(name_key)
        elif archive is not None and version is not None:
            if not isinstance(version, str) or not (1 <= len(version) <= 64) or any(ord(c) < 33 for c in version):
                raise ValueError("%s has an invalid archive version" % label)
        if archive is not None and "dependencies" in item:
            deps = item["dependencies"]
            if (not isinstance(deps, list) or
                    any(not _valid_archive_name(d) or d == archive for d in deps)):
                raise ValueError("%s has invalid archive dependencies" % label)
    return items


def read_lock():
    """Contents of brat.lock, or [] when there is no lockfile yet.

    A lockfile that exists but cannot be read or parsed is an error, not an
    empty lock. `brat pkg install` would otherwise proceed as if nothing were
    pinned and stop honouring the hashes it exists to enforce.
    """
    try:
        with open(lock_path(), "rb") as f:
            raw = f.read(MAX_PACKAGE_BYTES + 1)
    except FileNotFoundError:
        return []
    except OSError as e:
        raise SystemExit("brat pkg: cannot read %s: %s" % (lock_path(), e))
    except UnicodeError as e:
        raise SystemExit("brat pkg: %s is not valid UTF-8: %s" % (lock_path(), e))
    if len(raw) > MAX_PACKAGE_BYTES:
        raise SystemExit("brat pkg: %s exceeds 16 MiB" % lock_path())
    try:
        data = raw.decode("utf-8")
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


def _validate_fetch_url(url, allow_file):
    import urllib.parse as _up
    if any(ord(ch) <= 0x20 or ord(ch) >= 0x7F for ch in url):
        raise ValueError("invalid space or control character in URL")
    parsed = _up.urlsplit(url)
    if parsed.scheme not in ("https", "http", "file"):
        raise ValueError("unsupported URL scheme")
    if parsed.scheme == "file" and not allow_file:
        raise ValueError("a remote index may not name a file:// dependency")
    if parsed.scheme in ("https", "http"):
        if not parsed.hostname or parsed.username or parsed.password:
            raise ValueError("invalid network URL")
    elif parsed.netloc not in ("", "localhost"):
        raise ValueError("invalid file URL")
    return parsed


def _check_resolved_host(hostname, port, block_internal):
    import ipaddress as _ip
    import socket as _sock
    if os.environ.get("BRAT_PKG_ALLOW_INTERNAL") == "1":
        return False
    infos = _sock.getaddrinfo(hostname, port or 80, type=_sock.SOCK_STREAM)
    any_internal = False
    for _fam, _st, _pr, _cn, sa in infos:
        addr = _ip.ip_address(sa[0])
        if isinstance(addr, _ip.IPv6Address) and addr.ipv4_mapped is not None:
            addr = addr.ipv4_mapped
        if addr.is_unspecified or addr.is_link_local or str(addr) == "255.255.255.255":
            raise ValueError("refusing connection to link-local/unspecified address for %s" % hostname)
        is_int = (addr.is_private or addr.is_loopback or addr.is_link_local or
                  addr.is_multicast or addr.is_reserved)
        if is_int:
            any_internal = True
            if block_internal:
                raise ValueError("refusing redirect to private/loopback/link-local address for %s" % hostname)
    return any_internal


def fetch_bytes(url, allow_file=False, block_internal_root=False, internal_out=None):
    import hashlib as _hl
    import urllib.parse as _up
    import urllib.request as _ur
    parsed = _validate_fetch_url(url, allow_file)
    if parsed.scheme == "file":
        if isinstance(internal_out, list):
            internal_out[:] = [True]
        opener = _ur.build_opener(_ur.FileHandler())
    else:
        init_host = (parsed.hostname or "").lower()
        init_port = parsed.port or (443 if parsed.scheme == "https" else 80)
        init_internal = _check_resolved_host(parsed.hostname, init_port,
                                             block_internal=block_internal_root)
        state = {"https": parsed.scheme == "https", "internal": init_internal}

        class _SafeRedirect(_ur.HTTPRedirectHandler):
            def redirect_request(self, req, fp, code, msg, headers, newurl):
                if any(ord(ch) <= 0x20 or ord(ch) >= 0x7F for ch in newurl):
                    raise ValueError("invalid character in redirect Location")
                target = _up.urljoin(req.full_url, newurl)
                tp = _validate_fetch_url(target, allow_file=False)
                if state["https"] and tp.scheme == "http":
                    raise ValueError("refusing HTTPS downgrade redirect to %s" % target)
                if tp.scheme == "https":
                    state["https"] = True
                thost = (tp.hostname or "").lower()
                tport = tp.port or (443 if tp.scheme == "https" else 80)
                hop_int = _check_resolved_host(
                    tp.hostname, tport,
                    block_internal=(block_internal_root or thost != init_host or not init_internal),
                )
                state["internal"] = hop_int
                return super().redirect_request(req, fp, code, msg, headers, target)

        opener = _ur.build_opener(_ur.HTTPHandler(), _ur.HTTPSHandler(), _SafeRedirect())
    with opener.open(url, timeout=30) as r:
        body = r.read(MAX_PACKAGE_BYTES + 1)
    if len(body) > MAX_PACKAGE_BYTES:
        raise ValueError("download exceeds 16 MiB")
    if parsed.scheme != "file" and isinstance(internal_out, list):
        internal_out[:] = [state["internal"]]
    return body, _hl.sha256(body).hexdigest()


def fetch_url(url, allow_file=False, block_internal_root=False, internal_out=None):
    body, digest = fetch_bytes(url, allow_file=allow_file,
                               block_internal_root=block_internal_root,
                               internal_out=internal_out)
    try:
        text = body.decode("utf-8")
    except ValueError:
        raise RuntimeError("non-utf8 dep " + url)
    return text, digest


def _expand_github_spec(spec):
    """Expand `github.com/<owner>/<repo>@<ref>` to `(pkg_name, tarball_url, ref)`."""
    if not spec.startswith("github.com/"):
        return None
    rest = spec[len("github.com/"):]
    ref = "main"
    if "@" in rest:
        rest, ref = rest.rsplit("@", 1)
    parts = rest.split("/")
    if len(parts) != 2:
        raise ValueError("GitHub spec wants github.com/owner/repo[@ref]")
    owner, repo = parts
    if repo.endswith(".git"):
        repo = repo[:-4]
    if (not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", owner) or
            not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", repo) or
            not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_./-]{0,63}", ref) or
            ".." in ref):
        raise ValueError("invalid GitHub package spec %s" % spec)
    pkg = re.sub(r"[^A-Za-z0-9_]", "_", repo)
    if pkg and pkg[0].isdigit():
        pkg = "pkg_" + pkg
    if not _valid_archive_name(pkg):
        raise ValueError("repository name %s cannot form a valid BRAT module name" % repo)
    url = "https://codeload.github.com/%s/%s/tar.gz/%s" % (owner, repo, ref)
    return pkg, url, ref


def _archive_pkg_name_from_url(url):
    import urllib.parse as _up
    tail = _up.unquote(_up.urlsplit(url).path.rsplit("/", 1)[-1])
    low = tail.lower()
    if low.endswith(".tar.gz"):
        stem = tail[:-7]
    elif low.endswith(".tgz"):
        stem = tail[:-4]
    else:
        return None
    # Strip trailing `-v0.1.0` / `-0.1.0` version suffix if present.
    stem = re.sub(r"-[vV]?[0-9]+(?:\.[0-9A-Za-z_.-]+)*$", "", stem)
    pkg = re.sub(r"[^A-Za-z0-9_]", "_", stem)
    if pkg and pkg[0].isdigit():
        pkg = "pkg_" + pkg
    if not _valid_archive_name(pkg):
        raise ValueError("cannot derive safe package name from archive URL %s" % url)
    return pkg


def _parse_archive_manifest(raw_bytes, pkg_name):
    import json as _json
    try:
        obj = _json.loads(raw_bytes.decode("utf-8"))
    except (UnicodeDecodeError, ValueError, RecursionError) as e:
        raise ValueError("%s/brat.pkg.json is invalid JSON: %s" % (pkg_name, e))
    if not isinstance(obj, dict):
        raise ValueError("%s/brat.pkg.json must be a JSON object" % pkg_name)
    mname = obj.get("name")
    if mname is not None and mname != pkg_name:
        raise ValueError("%s/brat.pkg.json declares mismatched name %r" % (pkg_name, mname))
    ver = obj.get("version")
    if ver is not None:
        if not isinstance(ver, str) or not (1 <= len(ver) <= 64) or any(ord(c) < 33 for c in ver):
            raise ValueError("%s/brat.pkg.json has invalid version" % pkg_name)
    raw_deps = obj.get("dependencies", [])
    deps = []
    if isinstance(raw_deps, list):
        for entry in raw_deps:
            if isinstance(entry, str) and entry:
                deps.append((None, entry, None))
            elif isinstance(entry, dict) and isinstance(entry.get("url"), str):
                k = entry.get("name")
                if k is not None and not _valid_archive_name(k):
                    raise ValueError("%s/brat.pkg.json has invalid dependency name %r" % (pkg_name, k))
                sha = entry.get("sha256")
                if sha is not None and (not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha)):
                    raise ValueError("%s/brat.pkg.json dependency %s has invalid sha256" % (pkg_name, k or entry["url"]))
                deps.append((k, entry["url"], sha))
            else:
                raise ValueError("%s/brat.pkg.json dependency list entries must be spec strings or {name, url, sha256}" % pkg_name)
    elif isinstance(raw_deps, dict):
        for k, entry in raw_deps.items():
            if not _valid_archive_name(k):
                raise ValueError("%s/brat.pkg.json has invalid dependency name %r" % (pkg_name, k))
            if isinstance(entry, str) and entry:
                deps.append((k, entry, None))
            elif isinstance(entry, dict) and isinstance(entry.get("url"), str):
                sha = entry.get("sha256")
                if sha is not None and (not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha)):
                    raise ValueError("%s/brat.pkg.json dependency %s has invalid sha256" % (pkg_name, k))
                deps.append((k, entry["url"], sha))
            else:
                raise ValueError("%s/brat.pkg.json dependency %s must be spec string or {url, sha256}" % (pkg_name, k))
    else:
        raise ValueError("%s/brat.pkg.json `dependencies` must be a list or object" % pkg_name)
    raw_csrc = obj.get("c_sources", [])
    if not isinstance(raw_csrc, list):
        raise ValueError("%s/brat.pkg.json `c_sources` must be a list of .c paths" % pkg_name)
    c_sources = []
    seen_csrc = set()
    for cs in raw_csrc:
        if not isinstance(cs, str) or not cs:
            raise ValueError("%s/brat.pkg.json `c_sources` entries must be non-empty strings" % pkg_name)
        norm_cs = cs[len(pkg_name) + 1:] if cs.startswith(pkg_name + "/") else cs
        full_rel = "%s/%s" % (pkg_name, norm_cs)
        if not norm_cs.endswith(".c") or not _valid_archive_relpath(full_rel, pkg_name):
            raise ValueError("%s/brat.pkg.json has invalid c_sources path %r" % (pkg_name, cs))
        if norm_cs not in seen_csrc:
            seen_csrc.add(norm_cs)
            c_sources.append(norm_cs)
    return {"version": ver, "dependencies": deps, "c_sources": c_sources}


def _verify_archive_bratbinds(extracted, target=None, pkg_name=None, c_sources=()):
    """Verify every `.bratbind` in an extracted archive (and refuse unproven archive externs)."""
    brat_mod_inst = brat_mod()
    binds = {rel: data for rel, data in extracted.items() if rel.endswith(".bratbind")}
    brats = {rel: data for rel, data in extracted.items() if rel.endswith(".brat")}
    bins = {rel: data for rel, data in extracted.items() if rel.endswith(_ARCHIVE_BIN_EXTS)}
    cfiles = {rel: data for rel, data in extracted.items() if rel.endswith((".c", ".h"))}

    for brel in sorted(binds):
        mrel = brel[:-len(".bratbind")] + ".brat"
        if mrel not in brats:
            raise ValueError("%s has no matching %s in archive" % (brel, mrel))

    extern_mods = []
    for mrel, mbytes in sorted(brats.items()):
        prog = brat_mod_inst.parse_prog(brat_mod_inst.Cur(brat_mod_inst.lex(mbytes.decode("utf-8"))))
        if _stmt_externs(prog[1]):
            extern_mods.append(mrel)

    if not binds and not extern_mods and not c_sources:
        return

    for mrel in extern_mods:
        brel = mrel[:-len(".brat")] + ".bratbind"
        if brel not in binds:
            raise ValueError("%s declares C externs without a %s proof" % (mrel, brel))

    if c_sources and not binds:
        raise ValueError("%s declares c_sources without a .bratbind proof" % (pkg_name or "archive"))

    ffi = ffi_mod()
    eff_target = target if target is not None else os.environ.get("BRAT_TARGET")
    with tempfile.TemporaryDirectory(prefix="brat-bindchk-") as td:
        bin_paths_by_dir = {}
        for crel, cbytes in cfiles.items():
            cp = os.path.join(td, *crel.split("/"))
            os.makedirs(os.path.dirname(cp), exist_ok=True)
            with open(cp, "wb") as f:
                f.write(cbytes)
        for orel, obytes in bins.items():
            op = os.path.join(td, *orel.split("/"))
            os.makedirs(os.path.dirname(op), exist_ok=True)
            with open(op, "wb") as f:
                f.write(obytes)
            dkey = "/".join(orel.split("/")[:-1])
            bin_paths_by_dir.setdefault(dkey, {})[orel.split("/")[-1]] = op

        cs_bundle_path = None
        if c_sources and pkg_name:
            spec = None
            try:
                spec = resolve_cc(target=eff_target, static=False)
            except CcError as e:
                if eff_target is None:
                    raise ValueError("cannot verify c_sources for %s: %s" % (pkg_name, e))
            if spec is not None and (eff_target is None or spec.zig):
                pkg_td = os.path.join(td, pkg_name)
                for brel, bbytes in sorted(binds.items()):
                    try:
                        bdoc = json.loads(bbytes.decode("utf-8"))
                    except Exception as e:
                        raise ValueError("invalid JSON in %s: %s" % (brel, e))
                    hdr = (bdoc.get("header") if isinstance(bdoc, dict) else None) or {}
                    raw_hname = hdr.get("name") or ""
                    hname = os.path.basename(raw_hname) if isinstance(raw_hname, str) else ""
                    htext = hdr.get("text")
                    if hname and isinstance(htext, str):
                        if not _is_safe_header_filename(hname):
                            raise ValueError("unsafe or reserved header name %r in %s" % (hname, brel))
                        hp = os.path.join(pkg_td, hname)
                        if not os.path.exists(hp):
                            os.makedirs(pkg_td, exist_ok=True)
                            with open(hp, "w", encoding="utf-8") as hf:
                                hf.write(htext)
                built_objs = []
                for idx, cs in enumerate(c_sources):
                    cpath = _confined_pkg_subpath(pkg_td, cs)
                    if cpath is None:
                        raise ValueError("unsafe c_source path %r in %s" % (cs, pkg_name))
                    rel_cs = "%s/%s" % (pkg_name, cs)
                    opath = os.path.join(td, "__cs_%d.o" % idx)
                    cc_head = [spec.exe] + (["cc"] if spec.zig else [])
                    if spec.zig and eff_target:
                        cc_head.append("--target=" + eff_target)
                    cc_cmd = cc_head + ["-c", "-g", cpath, "-o", opath, "-std=gnu11", "-O2", "-fwrapv", "-I", pkg_td]
                    r = subprocess.run(cc_cmd, capture_output=True, text=True, timeout=60)
                    if r.returncode != 0:
                        raise ValueError("c_sources compile failed for %s: %s"
                                         % (rel_cs, (r.stdout + r.stderr).strip()[:400]))
                    built_objs.append(opath)
                if built_objs:
                    cs_bundle_path = os.path.join(td, "__c_sources_bundle__.a")
                    with open(cs_bundle_path, "wb") as af:
                        af.write(b"!<arch>\n")
                        for idx, op in enumerate(built_objs):
                            with open(op, "rb") as of:
                                odata = of.read()
                            mname = ("m%d.o/" % idx)[:16]
                            hdr_b = ("%-16s%-12s%-6s%-6s%-8s%-10d`\n"
                                     % (mname, "0", "0", "0", "100644", len(odata))).encode("ascii")
                            af.write(hdr_b)
                            af.write(odata)
                            if len(odata) & 1:
                                af.write(b"\n")

        for brel, bbytes in sorted(binds.items()):
            mrel = brel[:-len(".bratbind")] + ".brat"
            dkey = "/".join(mrel.split("/")[:-1])
            objs = dict(bin_paths_by_dir.get(dkey, {}))
            if cs_bundle_path is not None:
                objs["__c_sources_bundle__.a"] = cs_bundle_path
            ffi.verify_bratbind_bytes(
                brats[mrel], bbytes, target=eff_target,
                object_files=objs,
                label=brel,
            )


_RESERVED_C_HEADERS = frozenset({
    "assert.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h",
    "iso646.h", "limits.h", "locale.h", "math.h", "pthread.h", "sched.h",
    "setjmp.h", "signal.h", "stdalign.h", "stdarg.h", "stdatomic.h",
    "stdbool.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h",
    "stdnoreturn.h", "string.h", "strings.h", "tgmath.h", "threads.h",
    "time.h", "uchar.h", "unistd.h", "wchar.h", "wctype.h", "crt.h",
})


def _is_safe_header_filename(name):
    if not isinstance(name, str) or not name or "\x00" in name:
        return False
    if "/" in name or "\\" in name or ":" in name or name.startswith("."):
        return False
    if not (name.endswith(".h") or name.endswith(".hpp")) or len(name) <= 2:
        return False
    if name.lower() in _RESERVED_C_HEADERS:
        return False
    return all(ch.isalnum() or ch in ("_", "-", ".") for ch in name)


def _confined_pkg_subpath(root_dir, rel):
    if not isinstance(rel, str) or not rel or "\x00" in rel:
        return None
    if os.path.isabs(rel) or "\\" in rel or ":" in rel:
        return None
    parts = [p for p in rel.split("/") if p]
    if not parts or any(p in (".", "..") for p in parts):
        return None
    cur = os.path.realpath(root_dir)
    if os.path.islink(root_dir):
        return None
    for part in parts:
        cur = os.path.join(cur, part)
        if os.path.islink(cur):
            return None
    real_root = os.path.realpath(root_dir)
    real_cur = os.path.realpath(cur)
    if real_cur == real_root or not real_cur.startswith(real_root + os.sep):
        return None
    return cur


def _gunzip_bounded(raw_bytes, max_stream_bytes):
    import zlib as _zlib
    if len(raw_bytes) < 18 or raw_bytes[:2] != b"\x1f\x8b":
        raise ValueError("corrupt tar.gz archive: bad gzip header")
    out = bytearray()
    rem = memoryview(raw_bytes)
    while rem:
        if all(b == 0 for b in rem):
            break
        dec = _zlib.decompressobj(16 + _zlib.MAX_WBITS)
        try:
            chunk = dec.decompress(rem, max_stream_bytes + 1 - len(out))
        except _zlib.error as e:
            raise ValueError("corrupt tar.gz archive: %s" % e)
        out.extend(chunk)
        if len(out) > max_stream_bytes or dec.unconsumed_tail:
            raise ValueError("uncompressed archive exceeds 16 MiB")
        if not dec.eof:
            raise ValueError("corrupt tar.gz archive: truncated gzip stream")
        rem = memoryview(dec.unused_data)
    return bytes(out)


def _extract_tar_package(raw_bytes, pkg_name, with_manifest=False, target=None):
    """Safely extract `.brat`, `.bri`, `.bratbind` and binary objects from a `.tar.gz`."""
    import hashlib as _hl
    import io as _io
    import tarfile as _tf

    if not _valid_archive_name(pkg_name):
        raise ValueError("invalid archive package name %s" % pkg_name)
    tar_bytes = _gunzip_bounded(raw_bytes, 64 * 1024 * 1024)
    if len(tar_bytes) % 512 != 0:
        raise ValueError("corrupt tar.gz archive: trailing partial block")
    try:
        tf = _tf.open(fileobj=_io.BytesIO(tar_bytes), mode="r:")
    except (_tf.TarError, OSError, EOFError) as e:
        raise ValueError("corrupt tar.gz archive: %s" % e)

    all_files = []
    seen_raw = set()
    total_bytes = 0
    with tf:
        members = tf.getmembers()
        if len(members) > 512:
            raise ValueError("archive has too many entries (>512)")
        for m in members:
            if m.issym() or m.islnk():
                raise ValueError("archive may not contain symbolic or hard links (%s)" % m.name)
            if m.isdir():
                continue
            if not m.isfile():
                raise ValueError("archive contains unsupported entry type (%s)" % m.name)
            norm = m.name.replace("\\", "/")
            while norm.startswith("./"):
                norm = norm[2:]
            if not norm or norm.startswith("/") or "\x00" in norm:
                raise ValueError("unsafe path in archive: %s" % m.name)
            segs = norm.split("/")
            if any(not s or s in (".", "..") for s in segs):
                raise ValueError("path traversal in archive: %s" % m.name)
            if m.size < 0 or total_bytes + m.size > MAX_PACKAGE_BYTES:
                raise ValueError("uncompressed archive exceeds 16 MiB")
            total_bytes += m.size
            if not (_archive_file_ext(segs[-1]) is not None or segs[-1] == "brat.pkg.json"):
                continue
            fobj = tf.extractfile(m)
            if fobj is None:
                continue
            data = fobj.read(MAX_PACKAGE_BYTES + 1)
            if len(data) > MAX_PACKAGE_BYTES:
                raise ValueError("archive file exceeds 16 MiB")
            all_files.append((segs, data))

    if not all_files:
        raise ValueError("archive contains no .brat files")

    # Strip a single shared top-level wrapper directory (e.g. `repo-0.1.0/...`).
    if all(len(segs) >= 2 for segs, _ in all_files):
        top_heads = {segs[0] for segs, _ in all_files}
        if len(top_heads) == 1:
            all_files = [(segs[1:], data) for segs, data in all_files]

    manifest = {"version": None, "dependencies": [], "c_sources": []}
    manifest_raw = None
    raw_entries = []
    for segs, data in all_files:
        canon = "/".join(segs)
        if canon in seen_raw:
            raise ValueError("duplicate file in archive: %s" % canon)
        seen_raw.add(canon)
        if segs == ["brat.pkg.json"] or segs == [pkg_name, "brat.pkg.json"]:
            if manifest_raw is not None:
                raise ValueError("duplicate file in archive: %s/brat.pkg.json" % pkg_name)
            manifest = _parse_archive_manifest(data, pkg_name)
            manifest_raw = data
        elif _archive_file_ext(segs[-1]) is not None:
            raw_entries.append((segs, data))

    if not raw_entries:
        raise ValueError("archive contains no .brat files")

    brat = brat_mod()
    extracted = {}
    file_hashes = {}
    has_brat = False
    for segs, data in raw_entries:
        if segs[0] == pkg_name and len(segs) >= 2:
            rel_segs = segs
        else:
            rel_segs = [pkg_name] + segs
        rel = "/".join(rel_segs)
        if not _valid_archive_relpath(rel, pkg_name):
            raise ValueError("unsafe module path in archive: %s" % rel)
        if rel in extracted:
            raise ValueError("duplicate file in archive: %s" % rel)
        if rel.endswith(_ARCHIVE_TEXT_EXTS):
            try:
                text = data.decode("utf-8")
            except UnicodeDecodeError as e:
                raise ValueError("%s is not valid UTF-8: %s" % (rel, e))
            norm_bytes = text.replace("\r\n", "\n").encode("utf-8")
            if rel.endswith(".brat"):
                has_brat = True
                brat.parse_prog(brat.Cur(brat.lex(text)))
            extracted[rel] = norm_bytes
            file_hashes[rel] = _hl.sha256(norm_bytes).hexdigest()
        else:
            extracted[rel] = data
            file_hashes[rel] = _hl.sha256(data).hexdigest()

    if not has_brat:
        raise ValueError("archive contains no .brat files")
    if manifest["c_sources"]:
        for cs in manifest["c_sources"]:
            rel_cs = "%s/%s" % (pkg_name, cs)
            if rel_cs not in extracted:
                raise ValueError("%s/brat.pkg.json names missing c_source %s" % (pkg_name, cs))
        if manifest_raw is not None:
            mrel = "%s/brat.pkg.json" % pkg_name
            norm_m = manifest_raw.decode("utf-8").replace("\r\n", "\n").encode("utf-8")
            extracted[mrel] = norm_m
            file_hashes[mrel] = _hl.sha256(norm_m).hexdigest()
    _verify_archive_bratbinds(extracted, target=target, pkg_name=pkg_name,
                              c_sources=manifest["c_sources"])
    if with_manifest:
        return extracted, dict(sorted(file_hashes.items())), manifest
    return extracted, dict(sorted(file_hashes.items()))


def index_path():
    """Where the registry index lives: a path or a URL, else ./brat-index.json."""
    return os.environ.get("BRAT_INDEX") or os.path.join(os.getcwd(), "brat-index.json")


# Set by `read_index`: a remote index may only name http(s) dependencies, so
# callers fetching an entry need to know which kind of index named it.
_INDEX_REMOTE = False
_INDEX_BLOCK_INTERNAL = False


def read_index():
    """The registry index: {owner/name: {version: {url, sha256, dependencies}}}.

    A registry here is an index of what `brat pkg` already does -- URL, sha256,
    vendor -- so nothing about fetching or verifying changes; only the lookup
    does. A missing index is an empty index: direct URLs still work without one.
    """
    import json as _json
    path = index_path()
    remote = "://" in path
    is_local_file_url = path.lower().startswith("file:")
    global _INDEX_REMOTE, _INDEX_BLOCK_INTERNAL
    _INDEX_REMOTE = remote and not is_local_file_url
    _INDEX_BLOCK_INTERNAL = False
    if remote:
        try:
            conn_int = [False]
            text, _ = fetch_url(path, allow_file=is_local_file_url, internal_out=conn_int)
            _INDEX_BLOCK_INTERNAL = _INDEX_REMOTE and not conn_int[0]
        except Exception as e:
            raise SystemExit("brat pkg: index %s: %s" % (path, e))
    else:
        try:
            with open(path, "rb") as f:
                body = f.read(MAX_PACKAGE_BYTES + 1)
            if len(body) > MAX_PACKAGE_BYTES:
                raise ValueError("index exceeds 16 MiB")
            text = body.decode("utf-8")
        except FileNotFoundError:
            return {}
        except (OSError, UnicodeError, ValueError) as e:
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

    def search(pending, chosen, current_constraints, depth=0):
        if not pending:
            return chosen
        if depth > 256 or len(chosen) > 64:
            latest_error[0] = "package dependency graph exceeds resolver limits"
            return None
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
                return search(rest, chosen, combined, depth + 1)
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
            result = search(next_pending + rest, next_chosen, combined, depth + 1)
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
    graph_bytes = [0]

    def add_resolved(name):
        if name in completed:
            return
        if len(plan) >= 256:
            raise ValueError("package dependency graph exceeds 256 packages")
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
            text, digest = fetch_url(url, allow_file=not _INDEX_REMOTE,
                                     block_internal_root=_INDEX_BLOCK_INTERNAL)
        except Exception as e:
            raise ValueError("fetch %s: %s" % (url, e))
        graph_bytes[0] += len(text.encode("utf-8"))
        if graph_bytes[0] > (64 << 20):
            raise ValueError("package dependency graph exceeds 64 MiB total download budget")
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


def _stage_vendor_tree(single_plan, archive_plan, lock_items=None):
    vendor = _vendor_dir()
    with tempfile.TemporaryDirectory(prefix=".brat-pkg-", dir=vendor) as stage:
        backup = os.path.join(stage, "backup")
        staged = os.path.join(stage, "staged")
        os.mkdir(backup)
        os.mkdir(staged)
        for item in single_plan:
            with open(os.path.join(staged, item["file"]), "w",
                      encoding="utf-8", newline="\n") as f:
                f.write(item["text"])
        for arc in archive_plan:
            pkg_dir = os.path.join(staged, arc["archive"])
            os.makedirs(pkg_dir, exist_ok=True)
            for rel, content in arc["extracted"].items():
                dest = os.path.join(staged, *rel.split("/"))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as f:
                    f.write(content)
        targets = ([item["file"] for item in single_plan] +
                   [arc["archive"] for arc in archive_plan])
        backed_up = []
        installed = []
        try:
            for entry_name in targets:
                target = os.path.join(vendor, entry_name)
                old = os.path.join(backup, entry_name)
                if os.path.lexists(target):
                    if os.path.islink(target):
                        raise OSError("refusing symlink in vendor: %s" % entry_name)
                    os.replace(target, old)
                    backed_up.append(entry_name)
                os.replace(os.path.join(staged, entry_name), target)
                installed.append(entry_name)
            if lock_items is not None:
                write_lock(lock_items)
        except OSError:
            for entry_name in reversed(installed):
                p = os.path.join(vendor, entry_name)
                if os.path.isdir(p) and not os.path.islink(p):
                    shutil.rmtree(p, ignore_errors=True)
                elif os.path.lexists(p):
                    os.remove(p)
            for entry_name in reversed(backed_up):
                os.replace(os.path.join(backup, entry_name),
                           os.path.join(vendor, entry_name))
            raise


def _install_vendor_files(plan, lock_items=None):
    _validate_lock_items([{k: item[k] for k in ("file", "url", "sha256")}
                          | ({k: item[k] for k in ("name", "version")
                              if k in item})
                          for item in plan])
    _stage_vendor_tree(plan, [], lock_items=lock_items)


def _archive_spec_info(spec):
    """Map `github.com/owner/repo[@ref]` or a `.tar.gz`/`.tgz` URL to `(pkg, url, ref, is_local)`."""
    if spec.startswith("github.com/"):
        pkg, url, ref = _expand_github_spec(spec)
        return pkg, url, ref, False
    pkg = _archive_pkg_name_from_url(spec)
    if pkg is None:
        raise ValueError("archive dependency must be github.com/owner/repo[@ref] or a .tar.gz/.tgz URL: %s" % spec)
    is_local = spec.lower().startswith("file:")
    return pkg, spec, None, is_local


def _resolve_and_install_archive(root_spec, allow_file_root, target=None):
    lock = read_lock()
    locked_by_name = {it["archive"]: it for it in lock if "archive" in it}
    resolved = {}
    order = []
    stack = []
    graph_archive_bytes = [0]
    graph_extracted_bytes = [0]
    graph_files = [0]

    def visit(spec, allow_file, block_internal_root=False, expected_name=None, pinned_sha=None):
        pkg_name, url, ref, is_local = _archive_spec_info(spec)
        if expected_name is not None and expected_name != pkg_name:
            raise ValueError("dependency %s resolved to mismatched archive name %s"
                             % (expected_name, pkg_name))
        if is_local and not allow_file:
            raise ValueError("a remote archive may not name a file:// dependency (%s)" % spec)
        if pkg_name in stack:
            cycle = " -> ".join(stack[stack.index(pkg_name):] + [pkg_name])
            raise ValueError("archive dependency cycle: %s" % cycle)
        prev = resolved.get(pkg_name)
        if prev is not None:
            if prev["url"] != url:
                raise ValueError("conflicting URLs for archive %s: %s vs %s"
                                 % (pkg_name, prev["url"], url))
            if pinned_sha is not None and prev["sha256"] != pinned_sha:
                raise ValueError("HASH MISMATCH %s" % pkg_name)
            return pkg_name
        if stack:
            lprev = locked_by_name.get(pkg_name)
            if lprev is not None:
                if lprev["url"] != url:
                    raise ValueError("conflicting URLs for locked archive %s: %s vs %s"
                                     % (pkg_name, lprev["url"], url))
                if pinned_sha is not None and lprev["sha256"] != pinned_sha:
                    raise ValueError("HASH MISMATCH %s" % pkg_name)
        if len(resolved) >= 64:
            raise ValueError("archive dependency graph exceeds 64 packages")

        conn_int = [False]
        raw, digest = fetch_bytes(url, allow_file=allow_file and is_local,
                                  block_internal_root=block_internal_root,
                                  internal_out=conn_int)
        graph_archive_bytes[0] += len(raw)
        if graph_archive_bytes[0] > (64 << 20):
            raise ValueError("archive dependency graph exceeds 64 MiB total download budget")
        if pinned_sha is not None and digest != pinned_sha:
            raise ValueError("HASH MISMATCH %s" % pkg_name)
        extracted, file_hashes, manifest = _extract_tar_package(
            raw, pkg_name, with_manifest=True, target=target)
        graph_files[0] += len(extracted)
        if graph_files[0] > 4096:
            raise ValueError("archive dependency graph exceeds 4096 total extracted files")
        for fbytes in extracted.values():
            graph_extracted_bytes[0] += len(fbytes)
            if graph_extracted_bytes[0] > (128 << 20):
                raise ValueError("archive dependency graph exceeds 128 MiB total extracted size")
        ver = ref if ref is not None else manifest["version"]

        stack.append(pkg_name)
        dep_names = []
        child_allow_file = allow_file and is_local
        child_block_int = block_internal_root or (not is_local and not conn_int[0])
        for dep_key, dep_spec, dep_sha in manifest["dependencies"]:
            dname = visit(dep_spec, child_allow_file, block_internal_root=child_block_int,
                          expected_name=dep_key, pinned_sha=dep_sha)
            if dname not in dep_names:
                dep_names.append(dname)
        stack.pop()

        item = {
            "archive": pkg_name,
            "url": url,
            "sha256": digest,
            "extracted": extracted,
            "files": file_hashes,
            "version": ver,
            "dependencies": dep_names,
            "c_sources": ["%s/%s" % (pkg_name, cs) for cs in manifest["c_sources"]],
        }
        resolved[pkg_name] = item
        order.append(pkg_name)
        return pkg_name

    visit(root_spec, allow_file_root)
    replacing = {n.casefold() for n in order}
    kept = [it for it in lock if (it.get("archive") or "").casefold() not in replacing]
    new_entries = []
    stage_plan = []
    for n in order:
        it = resolved[n]
        ent = {"archive": it["archive"], "url": it["url"], "sha256": it["sha256"], "files": it["files"]}
        if it["version"] is not None:
            ent["version"] = it["version"]
        if it["dependencies"]:
            ent["dependencies"] = it["dependencies"]
        if it["c_sources"]:
            ent["c_sources"] = it["c_sources"]
        new_entries.append(ent)
        stage_plan.append({"archive": it["archive"], "extracted": it["extracted"]})
    new_lock = kept + new_entries
    _validate_lock_items(new_lock)
    _stage_vendor_tree([], stage_plan, lock_items=new_lock)
    return [resolved[n] for n in order]


def _take_pkg_target(args):
    target = None
    rest = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--target" and i + 1 < len(args):
            target = args[i + 1]
            i += 2
            continue
        if a.startswith("--target="):
            target = a.split("=", 1)[1]
            i += 1
            continue
        rest.append(a)
        i += 1
    return target, rest


def do_pkg(args):
    native = _native_runner()
    if native is not None:
        return subprocess.call([native, "pkg"] + list(args))
    if not args:
        print_usage()
        return 2
    sub = args[0]
    pkg_target = None
    if sub in ("add", "install", "verify"):
        pkg_target, args = _take_pkg_target(args)
        if not args:
            print_usage()
            return 2
        sub = args[0]
    if sub == "add" and len(args) == 2 and args[1].startswith("github.com/"):
        try:
            installed = _resolve_and_install_archive(args[1], allow_file_root=False, target=pkg_target)
        except Exception as e:
            sys.stderr.write("brat pkg: %s\n" % e)
            return 1
        for it in installed:
            ver = "@%s" % it["version"] if it["version"] else ""
            sys.stdout.write("add %s%s %s (%d file(s))\n"
                             % (it["archive"], ver, it["sha256"][:12], len(it["files"])))
        return 0
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
                body = f.read(MAX_PACKAGE_BYTES + 1)
            if len(body) > MAX_PACKAGE_BYTES:
                raise ValueError("package exceeds 16 MiB")
            text = body.decode("utf-8")
            brat = brat_mod()
            brat.parse_prog(brat.Cur(brat.lex(text)))
        except (OSError, UnicodeError, SyntaxError, ImportError, ValueError) as e:
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
        url_path = _up.unquote(_up.urlsplit(url).path)
        if url_path.lower().endswith((".tar.gz", ".tgz")):
            try:
                installed = _resolve_and_install_archive(url, allow_file_root=True, target=pkg_target)
            except Exception as e:
                sys.stderr.write("brat pkg: %s\n" % e)
                return 1
            for it in installed:
                ver = "@%s" % it["version"] if it["version"] else ""
                sys.stdout.write("add %s%s %s (%d file(s))\n"
                                 % (it["archive"], ver, it["sha256"][:12], len(it["files"])))
            return 0
        name = url_path.rsplit("/", 1)[-1]
        if not _valid_package_file(name):
            sys.stderr.write("brat pkg: URL must name a safe .brat file or .tar.gz archive\n")
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
        single_plan = []
        archive_plan = []
        for it in items:
            if "archive" in it:
                pkg_name = it["archive"]
                try:
                    raw, digest = fetch_bytes(it["url"], allow_file=True)
                except Exception as e:
                    sys.stderr.write("brat pkg: fetch %s: %s\n" % (it.get("url"), e))
                    return 1
                if digest != it.get("sha256"):
                    sys.stderr.write("brat pkg: HASH MISMATCH %s\n" % pkg_name)
                    return 1
                try:
                    extracted, file_hashes = _extract_tar_package(raw, pkg_name, target=pkg_target)
                except Exception as e:
                    sys.stderr.write("brat pkg: extract %s: %s\n" % (pkg_name, e))
                    return 1
                if file_hashes != it.get("files"):
                    sys.stderr.write("brat pkg: HASH MISMATCH %s\n" % pkg_name)
                    return 1
                archive_plan.append({"archive": pkg_name, "extracted": extracted})
            else:
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
                single_plan.append({**it, "text": text})
        try:
            _stage_vendor_tree(single_plan, archive_plan)
        except OSError as e:
            sys.stderr.write("brat pkg: install failed: %s\n" % e)
            return 1
        for it in items:
            sys.stdout.write("ok %s\n" % (it.get("archive") or it["file"]))
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
            if "archive" in item:
                pkg_name = item["archive"]
                arc_extracted = {}
                for rel, expected_hash in item["files"].items():
                    parts = rel.split("/")
                    cur = vendor
                    try:
                        for part in parts[:-1]:
                            cur = os.path.join(cur, part)
                            st_dir = os.lstat(cur).st_mode
                            if not stat.S_ISDIR(st_dir) or stat.S_ISLNK(st_dir):
                                raise OSError("not a real directory")
                        fpath = os.path.join(cur, parts[-1])
                        if not stat.S_ISREG(os.lstat(fpath).st_mode):
                            raise OSError("not a regular file")
                        with open(fpath, "rb") as f:
                            body = f.read(MAX_PACKAGE_BYTES + 1)
                        if len(body) > MAX_PACKAGE_BYTES:
                            raise OSError("file exceeds 16 MiB")
                        digest = hashlib.sha256(body).hexdigest()
                    except OSError as e:
                        sys.stderr.write("brat pkg: verify %s: %s\n" % (rel, e))
                        return 1
                    if digest != expected_hash:
                        sys.stderr.write("brat pkg: HASH MISMATCH %s\n" % rel)
                        return 1
                    arc_extracted[rel] = body
                try:
                    raw_cs = item.get("c_sources") or []
                    rel_cs = [cs[len(pkg_name) + 1:] if cs.startswith(pkg_name + "/") else cs
                              for cs in raw_cs]
                    _verify_archive_bratbinds(arc_extracted, target=pkg_target,
                                              pkg_name=pkg_name, c_sources=rel_cs)
                except Exception as e:
                    sys.stderr.write("brat pkg: verify %s: %s\n" % (pkg_name, e))
                    return 1
            else:
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
            who = it.get("name") or it.get("archive") or it.get("file")
            if it.get("version"):
                who += "@" + it["version"]
            sys.stdout.write("%s  %s  %s\n" % (who, it.get("sha256", "")[:12], it.get("url")))
        return 0
    if sub == "remove" and len(args) == 2:
        name = args[1]
        items = read_lock()
        new = [it for it in items if it.get("file") != name and it.get("archive") != name]
        if len(new) == len(items):
            sys.stderr.write("brat pkg: not in lock: %s\n" % name)
            return 1
        is_arc = any(it.get("archive") == name for it in items)
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
            if target_mode is not None:
                if is_arc:
                    if not stat.S_ISDIR(target_mode) or stat.S_ISLNK(target_mode):
                        raise OSError("vendor archive path is not a real directory")
                elif not stat.S_ISREG(target_mode):
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
    native = _native_runner()
    if native is not None:
        os.execv(native, [native, "new"] + list(args))
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

def _gate_typer(path, arena_only=False):
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
    args = ["check", "--arena-only", path] if arena_only else ["check", path]
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = tp.main(args)
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

    progs = [(f.path, f.prog, {f.functions[n] for n in f.public if n in f.functions})
             for f in loaded.files]
    srcs = loaded.srcs

    def pub_sigs(fprog, exp_fns):
        out = {}
        consts = set()
        copy_rets = set()
        borrows_rets = {}
        owned_rets = set()
        for s in fprog[1]:
            if isinstance(s, tuple) and s and s[0] == "extern":
                out[s[2]] = own.fn_modes(s[3])
                if len(s) > 4 and s[4] in own.FFI_COPY_RETS:
                    copy_rets.add(s[2])
                elif len(s) > 6 and s[6]:
                    owned_rets.add(s[2])
                _bix = own._extern_borrows_indices(s)
                if _bix:
                    borrows_rets[s[2]] = _bix
            elif isinstance(s, tuple) and s and s[0] == "fn" and s[1] in exp_fns:
                out[s[1]] = own.fn_modes(s[2])
            elif isinstance(s, tuple) and s and s[0] == "pub":
                node = s[1]
                if isinstance(node, tuple) and node and node[0] == "fn":
                    out[node[1]] = own.fn_modes(node[2])
                elif isinstance(node, tuple) and node and node[0] == "extern":
                    out[node[2]] = own.fn_modes(node[3])
                    if len(node) > 4 and node[4] in own.FFI_COPY_RETS:
                        copy_rets.add(node[2])
                    elif len(node) > 6 and node[6]:
                        owned_rets.add(node[2])
                    _bix = own._extern_borrows_indices(node)
                    if _bix:
                        borrows_rets[node[2]] = _bix
                elif isinstance(node, tuple) and node and node[0] == "enum":
                    for vname, _f in node[2]:
                        out[vname] = []
                elif isinstance(node, tuple) and node and node[0] == "let" and len(node) > 4 and node[4]:
                    consts.add(node[1])
        return out, consts, copy_rets, borrows_rets, owned_rets

    per_file = [pub_sigs(fp, ef) for _, fp, ef in progs]
    for idx, (f, fprog, _) in enumerate(progs):
        merged = {}
        merged_consts = set()
        merged_copy = set()
        merged_borrows = {}
        merged_owned = set()
        for j, (ps, cset, crset, brset, orset) in enumerate(per_file):
            if j != idx:
                merged.update(ps)
                merged_consts.update(cset)
                merged_copy.update(crset)
                merged_borrows.update(brset)
                merged_owned.update(orset)
        errs, warns = own.check(fprog, merged, merged_consts, merged_copy, merged_borrows, merged_owned)
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
                elif isinstance(node, tuple) and node and node[0] == "extern":
                    pub[node[2]] = own.fn_modes(node[3])
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
    """`brat build prog.brat --header lib.h [--object lib.o]`: prove, then build."""
    return do_build(args)


def do_build(args, header=None, unsafe=False, objects=()):
    """Build through the native compiler, the only C emitter and linker driver."""
    native = _native_required("brat build")
    if native is None:
        return 1
    native_args = [arg for arg in args if arg != "--"]
    if unsafe and "--unsafe-ffi" not in native_args:
        native_args = ["--unsafe-ffi"] + native_args
    if header is not None and "--header" not in native_args and not any(
            a.startswith("--header=") for a in native_args):
        native_args = ["--header", header] + native_args
    for obj in objects:
        native_args = ["--object", obj] + native_args
    return subprocess.call([native, "build"] + native_args)

def do_dist(args):
    """`brat dist file.brat [-o dir] [--target t1,t2,...] [--static|--dynamic]`."""
    native = _native_required("brat dist")
    if native is None:
        return 1
    return subprocess.call([native, "dist"] + list(args))

def _load_modules(src):
    """(LoadedProgram, error message). One place for the loader import dance."""
    try:
        return brat_mod().load_file(os.path.abspath(src)), None
    except OSError as e:
        return None, str(e)
    except Exception as e:
        if parse_error_line(e) is not None:
            return None, "%s:%s" % (src, e)
        return None, "%s: %s" % (type(e).__name__, e)


def do_deps(args):
    """`brat deps file.brat [--json|--dot] [-o graph]`: module dependencies."""
    native = _native_runner()
    if native is not None:
        os.execv(native, [native, "deps"] + list(args))
    want_json = False
    want_dot = False
    out = None
    files = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--":
            i += 1
            continue
        if arg == "--json":
            want_json = True
        elif arg == "--dot":
            want_dot = True
        elif arg == "-o":
            if i + 1 >= len(args):
                sys.stderr.write("brat deps: -o requires a path\n")
                return 2
            out = args[i + 1]
            i += 2
            continue
        elif arg == "--target":
            if i + 1 >= len(args):
                sys.stderr.write("brat deps: --target requires a value\n")
                return 2
            i += 2
            continue
        elif arg.startswith("--target="):
            pass
        elif arg.startswith("-"):
            sys.stderr.write("brat deps: unknown flag %s\n" % arg)
            return 2
        else:
            files.append(arg)
        i += 1
    if want_json and want_dot:
        sys.stderr.write("brat deps: --json and --dot cannot be combined\n")
        return 2
    if len(files) != 1:
        sys.stderr.write("usage: brat deps FILE [--json|--dot] [-o OUT]\n")
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
    dot = None
    if want_dot:
        def quote(value):
            return '"' + value.replace("\\", "\\\\").replace('"', '\\"') \
                .replace("\n", "\\n").replace("\r", "\\r") + '"'
        nodes = sorted({"main" if f.namespace is None else f.namespace.replace("__", ".")
                        for f in loaded.files})
        lines = ["digraph dependencies {"]
        lines.extend("  %s;" % quote(name) for name in nodes)
        lines.extend("  %s -> %s [label=%s];" % (quote(e["from"]), quote(e["to"]),
                                                   quote(e["import"])) for e in edges)
        lines.append("}")
        dot = "\n".join(lines) + "\n"
    if want_json:
        sys.stdout.write(json.dumps(graph, indent=2, ensure_ascii=False) + "\n")
        return 0
    if out:
        text = dot if want_dot else json.dumps(graph, indent=2, ensure_ascii=False) + "\n"
        with open(out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        sys.stdout.write("deps ok %s (%d module(s), %d edge(s))\n"
                         % (out, len(loaded.files), len(edges)))
        return 0
    if want_dot:
        sys.stdout.write(dot)
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


def _take_ffi_flags_ex(args):
    """(header, objects, unsafe, rest): the FFI flags before `--`."""
    header = None
    objects = []
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
        if i < stop and a == "--object" and i + 1 < len(args):
            objects.append(args[i + 1])
            i += 2
            continue
        if i < stop and a.startswith("--object="):
            objects.append(a.split("=", 1)[1])
            i += 1
            continue
        if i < stop and a == "--unsafe-ffi":
            unsafe = True
            i += 1
            continue
        rest.append(a)
        i += 1
    return header, objects, unsafe, rest


def _take_ffi_flags(args):
    header, _objects, unsafe, rest = _take_ffi_flags_ex(args)
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


def ffi_gate(path, header, quiet=False, target=None, who="brat build", unsafe=False,
             objects=()):
    """Apply the FFI proof/unsafe gate to foreign declarations in loaded source."""
    ffi = ffi_mod()
    brat = brat_mod()
    src, err = read_source(path)
    if err is not None:
        sys.stderr.write("%s: %s\n" % (who, err))
        return 2
    try:
        prog = brat.parse_prog(brat.Cur(brat.lex(src)))
    except Exception:  # noqa: BLE001 - the checker reports syntax already
        return 0

    abs_root = os.path.abspath(path)
    loaded, _lerr = _load_modules(path)
    mod_entries = []
    unresolvable_plugin = False
    if loaded is not None:
        for sf in loaded.files:
            exts = _stmt_externs(sf.prog[1])
            if exts:
                mod_entries.append((os.path.abspath(sf.path), sf.src, exts))
            elif ffi.has_plugin_calls(sf.prog[1]):
                unresolvable_plugin = True
    else:
        exts = _stmt_externs(prog[1])
        if exts:
            mod_entries.append((abs_root, src, exts))
        elif ffi.has_plugin_calls(prog[1]):
            unresolvable_plugin = True

    if not mod_entries and not unresolvable_plugin:
        return 0

    if unresolvable_plugin and not mod_entries:
        if unsafe:
            if not quiet:
                sys.stderr.write("%s: %s calls C without a proof (--unsafe-ffi)\n"
                                 % (who, path))
            return 0
        if header:
            sys.stderr.write(
                "%s: refusing to build %s: dynamic `lib_sym` symbol cannot be proven "
                "statically against %s (pass a literal symbol name or --unsafe-ffi)\n"
                % (who, path, header))
            return 1
        return ffi_unproven(path, who)

    body = []
    for mpath, msrc, mexts in mod_entries:
        # When `--header` or `--object` was explicitly passed for the root file,
        # prove the root file against those flags rather than an older `.bratbind`.
        if mpath == abs_root and (header or objects):
            body.extend(mexts)
            continue
        ok_bind, err_bind = ffi.verify_module_bratbind(mpath, msrc, target=target)
        if ok_bind:
            continue
        if err_bind is not None and not unsafe:
            sys.stderr.write("%s: refusing %s: %s\n" % (who, path, err_bind))
            return 1
        body.extend(mexts)

    if not body:
        return 0

    if not header:
        if unsafe:
            if not quiet:
                sys.stderr.write("%s: %s calls C without a proof (--unsafe-ffi)\n"
                                 % (who, path))
            return 0
        return ffi_unproven(path, who)
    ffi.set_target(target)
    try:
        cdecl = ffi.parse_header(ffi.read_header(header))
    except OSError as e:
        sys.stderr.write("%s: cannot read %s: %s\n" % (who, header, e))
        return 1
    rep = ffi.check(body, cdecl, header)
    ok, out = True, ""
    try:
        ok, out = ffi.compile_probe(body, os.path.abspath(header), cdecl=cdecl,
                                    target=target)
    except Exception as e:  # noqa: BLE001 - no compiler is not a binding error
        sys.stderr.write("%s: layout stage unavailable: %s\n" % (who, e))
    obj_errors = 0
    for obj_p in objects:
        orep = ffi.verify_object(body, obj_p, cdecl=cdecl, target=target)
        rep.lines.extend(orep.lines)
        rep.proven += orep.proven
        obj_errors += orep.errors
    for ln in rep.lines:
        sys.stderr.write(ln + "\n")
    if out:
        sys.stderr.write(out + "\n")
    total_errors = rep.errors + obj_errors
    if total_errors or not ok:
        target_desc = header + ("" if not objects else " + %s" % ", ".join(objects))
        sys.stderr.write("%s: refusing to build %s: %d binding error(s) against %s\n"
                         % (who, path, total_errors, target_desc))
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
    native = _native_runner()
    if native is not None:
        return subprocess.call([native, "ffi"] + list(args))
    want_json = "--json" in args
    abi_matrix = "--abi-matrix" in args
    header = None
    objects = []
    emit_bind = False
    emit_bind_out = None
    target = None
    binpath = None
    preprocess = False
    inc_dirs = []
    defs = []
    rest = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--preprocess":
            preprocess = True
            i += 1
            continue
        if a == "-I" and i + 1 < len(args):
            inc_dirs.append(args[i + 1])
            i += 2
            continue
        if a.startswith("-I") and len(a) > 2:
            inc_dirs.append(a[2:])
            i += 1
            continue
        if a == "-D" and i + 1 < len(args):
            defs.append(args[i + 1])
            i += 2
            continue
        if a.startswith("-D") and len(a) > 2:
            defs.append(a[2:])
            i += 1
            continue
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
        if a == "--object" and i + 1 < len(args):
            objects.append(args[i + 1])
            i += 2
            continue
        if a.startswith("--object="):
            objects.append(a.split("=", 1)[1])
            i += 1
            continue
        if a == "--emit-bind":
            emit_bind = True
            if i + 1 < len(args) and not args[i + 1].startswith("-") and args[i + 1].endswith(".bratbind"):
                emit_bind_out = args[i + 1]
                i += 2
            else:
                i += 1
            continue
        if a.startswith("--emit-bind="):
            emit_bind = True
            emit_bind_out = a.split("=", 1)[1]
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
        if a not in ("--json", "--abi-matrix"):
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
    ffi = ffi_mod()
    # Widths are the target's: `long` is 4 bytes on i686 and on Windows, and the
    # comparison must say so before it compares anything.
    ffi.set_target(target)
    if binpath is not None and not rest:
        return _ffi_bin_report(ffi, binpath, None, want_json)
    try:
        brat = brat_mod()
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
    body = prog[1] + ffi.plugin_externs(prog[1])

    # `--emit` writes the bindings this header implies, from the same C view the
    # proof compares against: one mechanism, so what it generates is what it
    # can prove.
    cdecl = None
    header_name = header
    if header:
        try:
            htext = (ffi.preprocess_header(header, include_dirs=inc_dirs, defines=defs, target=target)
                     if (preprocess or defs or inc_dirs) else ffi.read_header(header))
            cdecl = ffi.parse_header(htext)
        except (OSError, RuntimeError) as e:
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

    if emit_bind:
        if not rest or not header:
            sys.stderr.write("brat ffi --emit-bind needs <file.brat> and --header <file.h>\n")
            return 2
        try:
            bind_obj = ffi.make_bratbind(rest[0], header, objects=objects, target=target,
                                         preprocess=bool(preprocess or defs or inc_dirs),
                                         include_dirs=inc_dirs, defines=defs)
        except Exception as e:
            sys.stderr.write("brat ffi: %s\n" % e)
            return 1
        payload = json.dumps(bind_obj, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
        if emit_bind_out:
            atomic_write(emit_bind_out, payload.encode("utf-8"))
            sys.stdout.write("wrote %s (%s)\n"
                             % (emit_bind_out, ", ".join(sorted(bind_obj["models"].keys()))))
        else:
            sys.stdout.write(payload)
        return 0

    if abi_matrix:
        if cdecl is None:
            sys.stderr.write("brat ffi --abi-matrix needs --header <file.h>\n")
            return 2
        mat = ffi.verify_abi_algebra(body, cdecl, header_name)
        if want_json:
            sys.stdout.write(json.dumps(mat, ensure_ascii=False, indent=2) + "\n")
        else:
            for t_ent in mat["targets"]:
                status = "ok" if t_ent["errors"] == 0 else ("FAIL (%d)" % t_ent["errors"])
                cc_tag = " [cc]" if t_ent["cc_verified"] else ""
                sys.stdout.write("  [%s] %-26s (ptr=%d, long=%d, align8=%d): %d proven, %s%s\n"
                                 % (t_ent["model"], t_ent["target"], t_ent["ptr_width"],
                                    t_ent["long_width"], t_ent["wide_align"],
                                    t_ent["proven"], status, cc_tag))
                for fn in t_ent["findings"]:
                    sys.stdout.write("    error: %s\n" % fn)
            for sname, by_tri in sorted(mat["differential"].items()):
                parts = []
                for tri, srec in by_tri.items():
                    if srec.get("brat"):
                        parts.append("%s=%dB" % (tri.split("-")[0], srec["brat"]["size"]))
                if parts:
                    sys.stdout.write("  struct %s: %s\n" % (sname, ", ".join(parts)))
        return 0 if mat["ok"] else 1

    rep = ffi.check(body, cdecl, header_name) if cdecl is not None else None
    obj_reps = [ffi.verify_object(body, op, cdecl=cdecl, target=target) for op in objects]
    if obj_reps:
        if rep is None:
            rep = ffi.Report()
        for orep in obj_reps:
            rep.lines.extend(orep.lines)
            rep.errors += orep.errors
            rep.warnings += orep.warnings
            rep.proven += orep.proven
    # The second opinion, when a header is there to compile against: the C
    # compiler re-declares every prototype and asserts every struct offset.
    cc_out, cc_ok = "", True
    if header and any(x and x[0] in ("extern", "externstruct") for x in body):
        try:
            cc_ok, cc_out = ffi.compile_probe(body, os.path.abspath(header), cdecl=cdecl,
                                              target=target, include_dirs=inc_dirs,
                                              defines=defs)
        except Exception as e:  # noqa: BLE001 - a missing cc must not hide the report
            cc_out, cc_ok = "compiler stage unavailable: %s: %s" % (type(e).__name__, e), True
    if binpath is not None:
        try:
            now = ffi.contract(body, header=header, target=target, objects=objects)
        except Exception as e:  # noqa: BLE001 - a report, not a crash
            sys.stderr.write("brat ffi: cannot describe this surface: %s\n" % e)
            return 1
        return _ffi_bin_report(ffi, binpath, now, want_json)

    missing = []
    try:
        raw_missing = ffi.missing_symbols(body)
        if objects:
            for nm, r in raw_missing:
                if "linker resolves" in r:
                    continue
                if any(ffi._extern_matches_object(nm, op) for op in objects):
                    continue
                missing.append((nm, r))
        else:
            missing = raw_missing
    except Exception as e:  # noqa: BLE001 - a symbol probe must not hide the report
        missing = [(None, "symbol check unavailable: %s" % e)]

    if want_json:
        out = {
            "version": 1,
            "file": os.path.abspath(rest[0]),
            "header": os.path.abspath(header) if header else None,
            "objects": [os.path.abspath(o) for o in objects],
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


def _interface_signature(name, signature):
    params, result = signature
    return "fn %s(%s) -> %s" % (name, ", ".join(params), result)


def _api_diff(old_api, new_api):
    changes = []
    for name in sorted(old_api):
        old_sig = old_api[name]
        if name not in new_api:
            changes.append({"kind": "removed", "name": name,
                            "old": _interface_signature(name, old_sig)})
        elif new_api[name] != old_sig:
            changes.append({"kind": "changed", "name": name,
                            "old": _interface_signature(name, old_sig),
                            "new": _interface_signature(name, new_api[name])})
    for name in sorted(new_api):
        if name not in old_api:
            changes.append({"kind": "added", "name": name,
                            "new": _interface_signature(name, new_api[name])})
    changes.sort(key=lambda change: change["name"])
    bump = ("major" if any(change["kind"] != "added" for change in changes)
            else "minor" if changes else "none")
    return changes, bump


def _api_diff_error(path, error):
    if isinstance(error, OSError):
        return "cannot read " + path
    return str(error)


def _render_api_diff(old_path, new_path, changes, bump):
    lines = ["API diff: %s -> %s" % (os.path.basename(old_path), os.path.basename(new_path)),
             "Recommended version bump: " + bump]
    if not changes:
        lines.append("No public signature changes.")
    else:
        for change in changes:
            if change["kind"] == "added":
                lines.append("+ " + change["new"])
            elif change["kind"] == "removed":
                lines.append("- " + change["old"])
            else:
                lines.append("~ %s => %s" % (change["old"], change["new"]))
    return "\n".join(lines) + "\n"


def do_api(args):
    """Show module exports or compare two `.bri` interfaces for semver impact."""
    native = _native_runner()
    if native is not None:
        os.execv(native, [native, "api"] + list(args))
    want_json = False
    want_diff = False
    files = []
    for arg in args:
        if arg == "--":
            continue
        if arg == "--json":
            want_json = True
        elif arg == "--diff":
            want_diff = True
        elif arg.startswith("-"):
            sys.stderr.write("brat api: unknown flag %s\n" % arg)
            return 2
        else:
            files.append(arg)
    if want_diff:
        if len(files) != 2:
            sys.stderr.write("usage: brat api --diff OLD.bri NEW.bri [--json]\n")
            return 2
        old_path, new_path = files
        old_abs, new_abs = os.path.abspath(old_path), os.path.abspath(new_path)
        failing_path = old_path
        try:
            from brat import modules as _bmod
            old_api = _bmod.parse_interface(old_path)
            failing_path = new_path
            new_api = _bmod.parse_interface(new_path)
            changes, bump = _api_diff(old_api, new_api)
        except Exception as e:
            message = _api_diff_error(failing_path, e)
            if want_json:
                report = {"version": 1, "old": old_abs, "new": new_abs,
                          "recommended_bump": None, "changes": [], "error": message}
                sys.stdout.write(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
            else:
                sys.stderr.write("brat api: %s\n" % message)
            return 1
        if want_json:
            report = {"version": 1, "old": old_abs, "new": new_abs,
                      "recommended_bump": bump, "changes": changes}
            sys.stdout.write(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
        else:
            sys.stdout.write(_render_api_diff(old_path, new_path, changes, bump))
        return 0
    if len(files) != 1:
        sys.stderr.write("usage: brat api FILE [--json]\n       brat api --diff OLD.bri NEW.bri [--json]\n")
        return 2
    loaded, err = _load_modules(files[0])
    if loaded is None:
        if want_json:
            sys.stdout.write(json.dumps({"version": 1, "root": os.path.abspath(files[0]),
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
        sys.stdout.write(json.dumps({"version": 1, "root": os.path.abspath(files[0]),
                                     "modules": mods},
                                    ensure_ascii=False, indent=2) + "\n")
        return 0
    sys.stdout.write("# %s\n\n" % os.path.basename(files[0]))
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


def _leading_doc_comment(lines, one_based_line):
    """Extract contiguous `//` or `///` doc lines immediately above `one_based_line`."""
    if not one_based_line or one_based_line <= 1 or one_based_line > len(lines) + 1:
        return ""
    out = []
    idx = one_based_line - 2
    while idx >= 0:
        raw = lines[idx].strip()
        if raw.startswith("///"):
            out.append(raw[3:].strip())
        elif raw.startswith("//"):
            out.append(raw[2:].strip())
        else:
            break
        idx -= 1
    out.reverse()
    return "\n".join(out).strip()


def _extract_file_docs(path, brat_mod_ref):
    src, err = read_source(path)
    if err is not None:
        return None, err
    lines = src.splitlines()
    try:
        prog = brat_mod_ref.parse_prog(brat_mod_ref.Cur(brat_mod_ref.lex(src)))
    except SyntaxError as e:
        return None, str(e)
    line_map = getattr(brat_mod_ref, "_LINES", {})
    if not line_map:
        try:
            from brat import parser as _bp
            line_map = _bp._LINES
        except Exception:
            line_map = {}

    bri_sigs = None
    bri_path = path[:-5] + ".bri" if path.endswith(".brat") else None
    if bri_path and os.path.isfile(bri_path):
        try:
            from brat import modules as _bmod
            bri_sigs = _bmod.parse_interface(bri_path)
        except Exception:
            bri_sigs = None

    items = []
    for st in prog[1]:
        if not isinstance(st, tuple) or not st:
            continue
        ln = line_map.get(id(st), 0)
        is_pub = (st[0] == "pub")
        node = st[1] if is_pub else st
        if not isinstance(node, tuple) or not node:
            continue
        if not ln:
            ln = line_map.get(id(node), 0)
        doc = _leading_doc_comment(lines, ln)
        kind = node[0]
        if kind == "fn":
            fname = node[1]
            exported = (fname in bri_sigs) if bri_sigs is not None else is_pub
            if not exported and bri_sigs is not None:
                continue
            params = []
            for p in node[2]:
                pm = p[1] if len(p) > 1 and p[1] != "ref" else ""
                pa = p[2] or "dyn"
                params.append(("%s %s: %s" % (pm, p[0], pa)).strip())
            ret = node[5] if len(node) > 5 and node[5] else "none"
            items.append({
                "kind": "fn",
                "name": fname,
                "public": bool(exported or bri_sigs is None),
                "signature": "fn %s(%s) -> %s" % (fname, ", ".join(params), ret),
                "line": ln,
                "doc": doc,
            })
        elif kind == "extern":
            ename = node[2]
            params = []
            for p in node[3]:
                pm = p[1] if len(p) > 1 and p[1] != "ref" else ""
                pa = p[2] or "dyn"
                params.append(("%s %s: %s" % (pm, p[0], pa)).strip())
            ret = node[4] or "none"
            effs = list(node[8]) if len(node) > 8 and node[8] else []
            ctrs = list(node[9]) if len(node) > 9 and node[9] else []
            items.append({
                "kind": "extern_fn",
                "name": ename,
                "public": True,
                "lib": node[1],
                "signature": "extern fn %s(%s) -> %s" % (ename, ", ".join(params), ret),
                "effects": effs,
                "contracts": ctrs,
                "line": ln,
                "doc": doc,
            })
        elif kind == "externstruct":
            sname = node[1]
            packed = bool(len(node) > 3 and node[3])
            is_union = bool(len(node) > 4 and node[4])
            flds = ["%s: %s" % (fnm, fty or "dyn") for fnm, fty in node[2]]
            kw = "union" if is_union else ("%sstruct" % ("packed " if packed else ""))
            items.append({
                "kind": "extern_union" if is_union else "extern_struct",
                "name": sname,
                "public": True,
                "packed": packed,
                "signature": "extern %s %s { %s }" % (kw, sname, ", ".join(flds)),
                "line": ln,
                "doc": doc,
            })
        elif kind == "enum" and is_pub:
            enm = node[1]
            vars_s = [vn + ("(%s)" % ", ".join(vf) if vf else "") for vn, vf in node[2]]
            items.append({
                "kind": "enum",
                "name": enm,
                "public": True,
                "signature": "pub enum %s { %s }" % (enm, ", ".join(vars_s)),
                "line": ln,
                "doc": doc,
            })
        elif kind == "let" and len(node) > 4 and node[4] and is_pub:
            cname = node[1]
            cann = node[5] if len(node) > 5 and node[5] else "dyn"
            items.append({
                "kind": "const",
                "name": cname,
                "public": True,
                "signature": "pub const %s: %s" % (cname, cann),
                "line": ln,
                "doc": doc,
            })
    return {
        "file": os.path.abspath(path),
        "module": os.path.splitext(os.path.basename(path))[0],
        "has_interface": bri_sigs is not None,
        "items": items,
    }, None


def do_doc(args):
    """`brat doc <file_or_dir> [--json] [-o OUT]`: generate API reference docs."""
    native = _native_runner()
    if native is not None:
        os.execv(native, [native, "doc"] + list(args))
    want_json = False
    out_path = None
    targets = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--":
            i += 1
            continue
        if a == "--json":
            want_json = True
            i += 1
            continue
        if a == "-o" and i + 1 < len(args):
            out_path = args[i + 1]
            i += 2
            continue
        if a.startswith("-"):
            sys.stderr.write("brat doc: unknown flag %r\n" % a)
            return 2
        targets.append(a)
        i += 1
    if not targets:
        sys.stderr.write("usage: brat doc <file_or_dir> [--json] [-o OUT]\n")
        return 2
    brat = brat_mod()
    brat_files = []
    for t in targets:
        if os.path.isdir(t):
            for base, dirs, fns in os.walk(t):
                dirs[:] = sorted(d for d in dirs if d not in (".git", "__pycache__", "bin"))
                for fn in sorted(fns):
                    if fn.endswith(".brat"):
                        brat_files.append(os.path.join(base, fn))
        else:
            brat_files.append(t)
    modules = []
    for bf in brat_files:
        doc_mod, err = _extract_file_docs(bf, brat)
        if err is not None:
            sys.stderr.write("brat doc: %s: %s\n" % (bf, err))
            return 1
        modules.append(doc_mod)
    if want_json:
        rendered = json.dumps({"version": 1, "modules": modules}, ensure_ascii=False, indent=2) + "\n"
    else:
        chunks = []
        for m in modules:
            chunks.append("# Module `%s`" % m["module"])
            if m["has_interface"]:
                chunks.append("_Interface verified via `.bri`._")
            chunks.append("")
            if not m["items"]:
                chunks.append("_No documented declarations._\n")
                continue
            for it in m["items"]:
                chunks.append("## `%s`" % it["signature"])
                if it.get("effects") or it.get("contracts"):
                    tags = list(it.get("effects", [])) + list(it.get("contracts", []))
                    chunks.append("- Contracts: `" + "`, `".join(tags) + "`")
                if it["doc"]:
                    chunks.append(it["doc"])
                chunks.append("")
        rendered = "\n".join(chunks).rstrip() + "\n"
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(rendered)
    else:
        sys.stdout.write(rendered)
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
    "i8": "i8", "u8": "u8", "i16": "i16", "u16": "u16",
    "i32": "i32", "u32": "u32", "usize": "usize",
    "i64": "i64", "u64": "u64",
    "f32": "f32", "f64": "f64", "bool": "bool", "char": "u32",
}


def _map_rust_type(t, structs=(), enums=()):
    """Rust type as written -> (BRAT type, ffi-safe?).

    `#[repr(C)]` makes the layout C's; this is about the scalar mapping and
    about refusing what has no stable C layout instead of guessing. `String`,
    `&str`, `Vec`, references to non-`repr(C)` types: skipped with a comment.
    """
    t = " ".join(t.split())
    base = t.replace("const ", "").strip()
    if (base in ("&str", "&mut str", "*str", "*mut str")
            or base.startswith(("&[", "&mut [", "*[", "*mut ["))
            or "dyn " in base):
        return "ptr", False
    if base.startswith("["):
        return "ptr", False
    if base.startswith("&") or base.startswith("*"):
        return "ptr", True
    if base == "isize":
        return ("i64" if _ptr_bits() == 64 else "i32"), True
    if base in _RUST_PRIM:
        return _RUST_PRIM[base], True
    if (base in ("i128", "u128", "String", "str", "Vec", "Option", "Result", "Box", "Rc", "Arc")
            or "<" in base):
        return "ptr", False
    if base in enums:
        return "i32", True          # a fieldless repr(U) enum is its integer
    if base in structs:
        return base, True           # an extern struct, by value
    return "ptr", False             # unknown by-value named type has unknown C layout


def _rust_items(src):
    """(structs, enums, fns) from a Rust source, `#[repr(C)]` only for layouts."""
    ffi = ffi_mod()
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


    known_structs = {n for n, _f in structs if ffi.bindgen_name_ok(n)}
    known_enums = {n for n, _v in enums}
    while True:
        failed = set()
        for sname, raw_fields in structs:
            if sname not in known_structs:
                continue
            if not raw_fields:
                failed.add(sname)
                continue
            for fname, fty in raw_fields:
                if not ffi.bindgen_name_ok(fname):
                    failed.add(sname)
                    break
                bty, safe = _map_rust_type(fty, known_structs, known_enums)
                if not safe or bty == sname:
                    failed.add(sname)
                    break
        if not failed:
            break
        known_structs -= failed
    for sname, raw_fields in structs:
        if not ffi.bindgen_name_ok(sname):
            skipped.append("%s: a Rust struct name the BRAT parser reads as a keyword" % sname)
            continue
        if sname in known_structs:
            continue
        for fname, fty in raw_fields:
            if not ffi.bindgen_name_ok(fname):
                skipped.append("%s.%s: a Rust field name the BRAT parser reads as a keyword" % (sname, fname))
                break
            bty, safe = _map_rust_type(fty, known_structs, known_enums)
            if not safe or bty == sname:
                skipped.append("%s.%s: `%s` has no C layout" % (sname, fname, fty))
                break
    structs = [(n, rf) for n, rf in structs if n in known_structs]
    for m in re.finditer(r"\b(?:(?:pub\s+)?unsafe\s+)?extern\s+\"C\"\s+fn\s+"
                         r"([A-Za-z_]\w*)\s*\(", src):
        name = m.group(1)
        open_at = src.index("(", m.end() - 1)
        close_at = balanced(src, open_at, "(", ")")
        if close_at < 0:
            continue
        if not ffi.bindgen_name_ok(name, is_fn=True):
            skipped.append("%s: a Rust function name the BRAT parser reads as a keyword" % name)
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
                    if part in ("self", "&self", "&mut self"):
                        skipped.append("%s: method, not an FFI function" % name)
                        ok = False
                        break
                    pname, pty = "a%d" % idx, part
                else:
                    pname, pty = pm.group(1), pm.group(2).strip()
                    if not ffi.bindgen_name_ok(pname):
                        pname = "a%d" % idx
                bty, safe = _map_rust_type(pty, known_structs, known_enums)
                if not safe:
                    skipped.append("%s: parameter `%s` has no C layout" % (name, pty))
                    ok = False
                    break
                params.append((pname, bty))
        if ok:
            fns.append((name, params, ret))
    return structs, enums, fns, skipped


def _bindgen_rust(src, path):
    ffi = ffi_mod()
    structs, enums, fns, skipped = _rust_items(src)
    known_structs = {n for n, _f in structs}
    known_enums = {n for n, _v in enums}
    lines = ["// generated by brat bindgen from %s" % os.path.basename(path)]
    for name, raw_fields in structs:
        fields = []
        for fname, fty in raw_fields:
            bty, _safe = _map_rust_type(fty, known_structs, known_enums)
            fields.append((fname, bty))
        if fields:
            lines.append("extern struct %s { %s }"
                         % (name, ", ".join("%s: %s" % f for f in fields)))
    for _ename, variants in enums:
        lines.append("")
        for vname, value in variants:
            if ffi.bindgen_name_ok(vname):
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
    native = _native_runner()
    if native is not None:
        return subprocess.call([native, "bindgen"] + list(args))
    rest = [a for a in args if a != "--"]
    out = None
    _bindgen_target = None
    _bindgen_lang = "brat"
    _bindgen_lib = None
    inline_arrays = False
    preprocess = False
    inc_dirs = []
    defs = []
    files = []
    i = 0
    while i < len(rest):
        if rest[i] == "--inline-arrays":
            inline_arrays = True
            i += 1
            continue
        if rest[i] == "--preprocess":
            preprocess = True
            i += 1
            continue
        if rest[i] == "-I" and i + 1 < len(rest):
            inc_dirs.append(rest[i + 1])
            i += 2
            continue
        if rest[i].startswith("-I") and len(rest[i]) > 2:
            inc_dirs.append(rest[i][2:])
            i += 1
            continue
        if rest[i] == "-D" and i + 1 < len(rest):
            defs.append(rest[i + 1])
            i += 2
            continue
        if rest[i].startswith("-D") and len(rest[i]) > 2:
            defs.append(rest[i][2:])
            i += 1
            continue
        if rest[i] == "--lib" and i + 1 < len(rest):
            _bindgen_lib = rest[i + 1]
            i += 2
            continue
        if rest[i].startswith("--lib="):
            _bindgen_lib = rest[i].split("=", 1)[1]
            i += 1
            continue
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
    ffi = ffi_mod()
    ffi.set_target(_bindgen_target)
    try:
        if (preprocess or defs or inc_dirs) and os.path.exists(path):
            htext = ffi.preprocess_header(path, include_dirs=inc_dirs, defines=defs, target=_bindgen_target)
        else:
            htext = ffi.read_header(path) if os.path.exists(path) else src
    except RuntimeError as e:
        sys.stderr.write("brat bindgen: %s\n" % e)
        return 1
    cdecl = ffi.parse_header(htext)
    if lang != "brat":
        text, _notes = ffi.LANG_EMITTERS[lang](cdecl, path, _bindgen_lib)
    else:
        text, _notes = ffi.emit_bindings([], cdecl, os.path.basename(path),
                                         lib=_bindgen_lib,
                                         inline_arrays=inline_arrays)
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
    native = _native_runner()
    if native is not None:
        os.execv(native, [native, "explain"] + list(args))
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
    if cmd in ("-V", "--version", "version"):
        if rest:
            print_usage()
            return 2
        sys.stdout.write("brat %s\n" % version_mod().__version__)
        return 0
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
    if cmd == "doc":
        return do_doc(rest)
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
        binary = _native_runner()
        if binary is not None:
            return subprocess.call([binary, "mcp"])
        mcp = _load_mod("brat_mcp", "mcp.py")
        return mcp.main()
    sys.stderr.write("brat: unknown command %r\n" % cmd)
    print_usage()
    return 2

if __name__ == "__main__":
    sys.exit(main())