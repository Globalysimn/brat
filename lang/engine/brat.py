import importlib.util as _ilu
import os as _o
import sys as _s
import threading as _threading

_s.path.insert(0, _o.path.dirname(_o.path.abspath(__file__)))
_s.setrecursionlimit(10000)

_STACK_BYTES = 512 * 1024 * 1024


def _run_with_budget(fn):
    """Run `fn` with room for RECURSION_BUDGET BRAT frames, re-raising here."""
    box = {}

    def target():
        try:
            box['rc'] = fn()
        except BaseException as e:  # noqa: BLE001
            box['exc'] = e

    try:
        _threading.stack_size(_STACK_BYTES)
    except (ValueError, RuntimeError):
        pass
    _s.setrecursionlimit(RECURSION_BUDGET * 12)
    t = _threading.Thread(target=target)
    t.start()
    t.join()
    if 'exc' in box:
        raise box['exc']
    return box.get('rc')

from brat.lexer import Cur, lex
from brat.runtime import Done, run, RECURSION_BUDGET
from brat.parser import parse_prog, take_stray_braces
from brat.modules import load_file, load_source, ModuleError

__all__ = [
    "Cur", "Done", "ModuleError", "RECURSION_BUDGET", "lex", "load_file",
    "load_source", "parse_prog", "run", "take_stray_braces",
]

HERE = _o.path.dirname(_o.path.abspath(__file__))

SUBCOMMANDS = (
    'run', 'check', 'fmt', 'build', 'emit-c', 'test', 'pkg', 'dist',
    'new', 'deps', 'api', 'bindgen', 'ffi', 'lsp', 'mcp', 'explain',
)

_CLI = None


def cli_mod():
    """The cli.py front-end that owns every subcommand implementation."""
    global _CLI
    if _CLI is None:
        spec = _ilu.spec_from_file_location('brat_cli_front', _o.path.join(HERE, 'cli.py'))
        if spec is None or spec.loader is None:
            raise ImportError('cannot load cli.py')
        mod = _ilu.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _CLI = mod
    return _CLI


def type_name(v):
    """BRAT type name of a runtime value, for diagnostics."""
    if v is None:
        return 'None'
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
        t = v.get('$type')
        return 'struct %s' % t if t is not None else 'map'
    if isinstance(v, tuple):
        return str(v[0]) if v else 'tuple'
    return type(v).__name__


def exit_code(code):
    """SPEC.md 9: `main` returns i64, and that i64 is the process exit code.

    Returns `(rc, diagnostic)`. `diagnostic` is None when rc == 0 by contract.

      * None            -- `main` fell off the end (or bare `return`): 0
      * int             -- the exit code, verbatim
      * ('err', payload)-- an uncaught Err reached the top: SPEC.md 5 wants
                           `unhandled` + exit 1, exactly like a top-level
                           `raise`/`?`/`unwrap`
      * anything else   -- `main` is annotated `-> i64` but handed back a
                           non-i64: a type error, exit 1
    """
    if code is None:
        return 0, None
    if isinstance(code, bool):
        return 1, 'TypeError: main must return i64, got bool'
    if isinstance(code, int):
        return code, None
    if isinstance(code, tuple) and len(code) == 2 and code[0] == 'err':
        return 1, 'unhandled: ' + str(code)
    return 1, 'TypeError: main must return i64, got ' + type_name(code)


def run_file(path, args=None):
    """Bare `brat file.brat [args...]`: run one program. Returns an exit code."""
    try:
        src = open(path, encoding='utf-8').read()
    except OSError as e:
        _s.stderr.write('brat: cannot read %s: %s\n' % (path, e))
        return 1
    except UnicodeDecodeError as e:
        _s.stderr.write('brat: %s is not valid UTF-8: byte 0x%02x at offset %d\n'
                        % (path, e.object[e.start], e.start))
        return 1
    argv = list(args or ())
    if argv and argv[0] == '--':
        argv = argv[1:]
    try:
        code = _run_with_budget(lambda: run(src, _o.path.dirname(_o.path.abspath(path)), argv))
    except Done as r:
        # stderr, like the native runtime: an uncaught Err is not program output.
        _s.stderr.write('unhandled: ' + str(r.v) + '\n')
        return 1
    except RecursionError:
        # CPython's own limit fired before the BRAT-frame budget; still a refusal.
        _s.stderr.write('recursion too deep: the interpreter budget is %d '
                        'BRAT frames\n' % RECURSION_BUDGET)
        return 1
    except Exception as e:
        ln = cli_mod().parse_error_line(e)
        if ln is not None:
            return cli_mod().syntax_error(path, src, e)
        ln = getattr(e, '_brat_ln', None)
        _s.stderr.write('%s: %s%s\n' % (type(e).__name__, e, (' at line %d' % ln) if ln else ''))
        return 1
    rc, msg = exit_code(code)
    if msg:
        _s.stderr.write(msg + '\n')
    return rc


def main(argv=None):
    argv = _s.argv if argv is None else argv
    for _stream in (_s.stdout, _s.stderr):
        try:
            _stream.reconfigure(encoding='utf-8', errors='replace')
        except (AttributeError, ValueError):
            pass
    if len(argv) < 2:
        cli_mod().print_usage()
        return 1
    head = argv[1]
    rest = list(argv[2:])
    if head in SUBCOMMANDS and not _o.path.isfile(head):
        return cli_mod().main([argv[0], head] + rest)
    return run_file(head, rest)


if __name__ == '__main__':
    _s.exit(main())