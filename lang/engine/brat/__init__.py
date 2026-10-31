"""BRAT language package: lexer, parser, runtime."""
import sys as _sys

__version__ = "0.1"

# The parser is a recursive descent, and the grammar is one Python frame per
# precedence level per nesting level. The default limit of 1000 therefore runs
# out at a hundred nested brackets, which is a shape a file or an editor can
# produce without trying. The parser refuses its own nesting depth first
# (`parser.MAX_NEST`), and this leaves room for that bound plus the checker.
if _sys.getrecursionlimit() < 20000:
    _sys.setrecursionlimit(20000)

from .lexer import Cur, lex
from .runtime import Done, run
from .parser import parse_prog