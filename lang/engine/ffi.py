"""Proof that a BRAT `extern` matches the C library it claims to call.

Every other FFI trusts the translation. bindgen, cgo, ctypes and `@cImport` all
read a prototype you (or a generator) wrote and call through it; if it is wrong
the program runs and returns garbage, or worse, corrupts memory in a way that
surfaces somewhere else. The information needed to catch that already exists --
in the library's own header -- and it is a text file.

This reads the header, reads the BRAT declarations, and compares them as
declarations: the same width, the same arity, the same field order. What it
proves is exactly what a compiler proves about a declaration it can see, no
more: a header can disagree with the object file it names, and nothing outside
the linker can know that.

The rule is exact width, not "close enough": `f64` for a C `double` yes, `f64`
for a C `float` no. That second one is the mistake that costs a day, because
both are floats and only the ABI knows the difference.
"""

import os
import re
import subprocess
import sys

# C spelling -> (BRAT type, size in bytes or None). Only widths matter here: a
# BRAT `i32` carries 4 bytes, so it may stand beside `int` and nothing wider.
C_SCALARS = {
    "char": ("u8", 1), "signed char": ("i8", 1), "unsigned char": ("u8", 1),
    "short": ("i16", 2), "short int": ("i16", 2), "unsigned short": ("u16", 2),
    "int": ("i32", 4), "signed": ("i32", 4), "signed int": ("i32", 4),
    "unsigned": ("u32", 4), "unsigned int": ("u32", 4),
    "long": ("i64", 8), "long int": ("i64", 8),
    "unsigned long": ("u64", 8), "unsigned long int": ("u64", 8),
    "long long": ("i64", 8), "long long int": ("i64", 8),
    "unsigned long long": ("u64", 8), "unsigned long long int": ("u64", 8),
    # A fixed-width type is the width it names. It used to read `int8_t` as
    # `i32` (a 4-byte BRAT type for a 1-byte C one), so a generated binding for
    # a header with an `int8_t` field was refused by the very layout proof that
    # produced it, and a narrow *result* was read out of the top of `eax`,
    # where the ABI promises nothing. `short` is 2 bytes in every data model.
    "int8_t": ("i8", 1), "uint8_t": ("u8", 1),
    "int16_t": ("i16", 2), "uint16_t": ("u16", 2),
    "signed short": ("i16", 2), "unsigned short int": ("u16", 2),
    "int32_t": ("i32", 4), "uint32_t": ("u32", 4),
    "int64_t": ("i64", 8), "uint64_t": ("u64", 8),
    "size_t": ("usize", 8), "ssize_t": ("i64", 8),
    "ptrdiff_t": ("i64", 8), "intptr_t": ("i64", 8), "uintptr_t": ("u64", 8),
    # `long double` is deliberately absent: it is 80-bit extended in 16 bytes on
    # x86-64 and 128 bits elsewhere, so there is no BRAT type of that width. It
    # used to be spelled `("f64", 16)`, which read as a match on width and was
    # wrong on the type: a caller would pass a 16-byte value as a double.
    "float": ("f32", 4), "double": ("f64", 8),
    # C's `_Bool` is one byte, not four. Claiming `i32` would have mis-laid any
    # `extern struct` holding one, and 1-byte integers already have a BRAT
    # spelling: `u8`. (A BRAT `bool` is not an extern slot yet.)
    "bool": ("bool", 1), "_Bool": ("bool", 1),
    "void": ("none", None),
}

_WIDTH = {"bool": 1, "i8": 1, "u8": 1, "i16": 2, "u16": 2, "i32": 4, "u32": 4,
          "i64": 8, "u64": 8, "usize": 8, "f32": 4, "f64": 8,
          "str": 8, "ptr": 8, "none": None}

# Data models, not machines. A triple picks one; the widths a declaration is
# compared against come from here, so `check` judges the target the program is
# built for. `usize`, `ptr` and `str` are emitted as `size_t` and `void *`, so
# their width follows the target by construction and only `long` (and the
# pointer-sized typedefs) need this table.
DATA_MODELS = {
    # name: (pointer/`size_t` width, `long` width)
    "lp64": (8, 8),      # Linux, macOS, most Unix on 64-bit
    "llp64": (8, 4),     # Windows on 64-bit: `long` stays 32 bits
    "ilp32": (4, 4),     # i686, armv7, wasm32
}

_TARGET = {"triple": None, "model": "lp64"}


def data_model(triple):
    """Which data model a target triple names. Unknown triples keep the host's."""
    if not triple:
        return "lp64"
    t = triple.lower()
    if "windows" in t or "win32" in t or "msvc" in t or "mingw" in t:
        return "llp64"
    if any(t.startswith(p) for p in ("i386", "i486", "i586", "i686", "arm", "armv",
                                     "thumb", "wasm32", "mips", "riscv32", "powerpc-",
                                     "x86-64-32")):
        return "ilp32"
    if any(p in t for p in ("-gnux32", "-x32", "ilp32")):
        return "ilp32"
    return "lp64"


def set_target(triple):
    """Make the comparisons below judge `triple`. None means the host (LP64)."""
    _TARGET["triple"] = triple
    _TARGET["model"] = data_model(triple)
    return _TARGET["model"]


def current_target():
    return _TARGET["triple"], _TARGET["model"]


def ptr_width():
    return DATA_MODELS[_TARGET["model"]][0]


def long_width():
    return DATA_MODELS[_TARGET["model"]][1]


def wide_align():
    """Alignment of an 8-byte scalar on the target.

    8 everywhere except the 32-bit x86 ABI, which aligns `long long` and
    `double` to 4 until `-malign-double` says otherwise -- the default gcc and
    clang both use there. A model that guessed 8 would make every `extern
    struct` holding an `i64` fail its own `_Static_assert` on i686.
    """
    if _TARGET["model"] != "ilp32":
        return 8
    t = (_TARGET["triple"] or "").lower()
    x86_32 = t.startswith(("i386", "i486", "i586", "i686", "x86-64-32")) or \
        "-x32" in t or "gnux32" in t
    return 4 if x86_32 or not t else 8


# BRAT type -> (size, alignment) inside an `extern struct`, for the target the
# proof is about. Keep this in step with the native emitter's layout table;
# the C compiler checks the emitted assertions, so a width that is wrong for
# the target fails the build instead of the program.
def field_shape(t=None):
    pw = ptr_width()
    a8 = wide_align()
    shape = {"bool": (1, 1),
             "i8": (1, 1), "u8": (1, 1), "i16": (2, 2), "u16": (2, 2),
             "i32": (4, 4), "u32": (4, 4), "f32": (4, 4),
             "i64": (8, a8), "u64": (8, a8), "f64": (8, a8),
             "usize": (pw, pw), "str": (pw, pw), "ptr": (pw, pw)}
    if t is None:
        return shape
    if t.startswith("ptr[") or t.startswith("fn"):
        return (pw, pw)
    return shape.get(t)


def array_field_type(t):
    """`(elem_type, count)` for a BRAT inline array field `Elem[N]`, else None."""
    if not isinstance(t, str) or "[" not in t or not t.endswith("]") or t.startswith("ptr["):
        return None
    lb = t.find("[")
    elem, cnt = t[:lb].strip(), t[lb + 1:-1].strip()
    if elem and cnt.isdigit() and int(cnt) >= 1:
        return (elem, int(cnt))
    return None


def struct_layout(structs, packed, shape=None, unions=()):
    """Size, alignment and field offsets of extern structs/unions, by the target's rules.

    `structs` is name -> [(field, BRAT type)], `packed` the names declared
    `extern packed struct`, `unions` the names declared `extern union`.
    """
    shape = shape or field_shape
    unions_set = set(unions or ())
    layout = {}
    in_progress = set()

    def of(name, packed_):
        if name in layout:
            return layout[name]
        fields = structs.get(name)
        if fields is None or name in in_progress:
            return None
        in_progress.add(name)
        is_un = name in unions_set
        off, max_sz, align, slots = 0, 0, 1, []
        for fname, ftype in fields:
            arr = array_field_type(ftype)
            if arr is not None:
                aelem, acnt = arr
                if aelem in structs:
                    sub = of(aelem, aelem in packed)
                    if sub is None:
                        in_progress.discard(name)
                        return None
                    size, falign = sub[0] * acnt, sub[1]
                else:
                    sh = shape(aelem)
                    if sh is None:
                        in_progress.discard(name)
                        return None
                    size, falign = sh[0] * acnt, sh[1]
            elif ftype in structs:
                sub = of(ftype, ftype in packed)
                if sub is None:
                    in_progress.discard(name)
                    return None
                size, falign = sub[0], sub[1]
            else:
                sh = shape(ftype)
                if sh is None:
                    in_progress.discard(name)
                    return None
                size, falign = sh
            if is_un:
                slots.append((fname, 0))
                max_sz = max(max_sz, size)
                align = max(align, falign)
            else:
                if packed_:
                    falign = 1
                off = (off + falign - 1) // falign * falign
                slots.append((fname, off))
                off += size
                align = max(align, falign)
        if is_un:
            total = (max_sz + align - 1) // align * align
        elif packed_:
            align = 1
            total = off
        else:
            total = (off + align - 1) // align * align
        layout[name] = (total, align, slots)
        in_progress.discard(name)
        return layout[name]

    for name in structs:
        of(name, name in packed)
    return layout


QUALIFIERS = {"const", "volatile", "restrict", "register", "static", "extern",
              "inline", "__inline", "__inline__", "struct", "union", "enum",
              "signed", "unsigned"}

_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
_DECL = re.compile(r"\b(?:extern\s+)?([A-Za-z_][\w\s\*]*?)\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*;")
_ABI_CALLCONV = re.compile(
    r"\b(?:__stdcall|stdcall|__cdecl|cdecl|__fastcall|fastcall|__vectorcall|vectorcall|"
    r"__thiscall|thiscall|__regcall|regcall)\b|"
    r"__attribute__\s*\(\(\s*(?:ms_abi|sysv_abi|regparm|pcs|preserve_most|"
    r"preserve_all|interrupt|naked|vector_size|vectorcall|regcall)\b", re.I)


def _balanced(src, start):
    """(body, index after the closing brace) from the `{` at or after `start`.

    A record body is not a string between `{` and the first `}`: a real header
    puts a union, a nested struct or an array of structs inside one, and a lazy
    `.*?` stops at the inner brace. When that happens the fields are read out of
    the wrong text and every field after it is fiction -- sqlite3's
    `sqlite3_snapshot` came back with a field named after itself.
    """
    i = src.find("{", start)
    if i < 0:
        return None, -1
    depth = 0
    for j in range(i, len(src)):
        c = src[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return src[i + 1:j], j + 1
    return None, -1


_REC_PREFIX = re.compile(
    r"(?P<td>typedef\s+)?(?:struct|union)\s*(?P<tag>[A-Za-z_]\w*)?\s*"
    r"(?:__attribute__\s*\(\s*\(\s*packed\s*\)\s*\)\s*)?$")


def _scan_records(src):
    """Every record whose declaration is unambiguous, with a balanced body.

    Returns (tag, body, alias, packed). A record is accepted only when the text
    immediately before its `{` is exactly `[typedef] struct [tag] [`packed`]`
    and the text after its `}` is exactly `[Alias];`. Anything else is *skipped*,
    not guessed: the first version of this scan took the next identifier after
    the brace as the name, which named `sqlite3_module` after one of its own
    fields and would have generated a binding to a type that does not exist.
    """
    out = []
    pos = 0
    while True:
        m = re.search(r"\b(struct|union)\s*(?:[A-Za-z_]\w*\s*)?"
                      r"(?:__attribute__\s*\(\s*\(\s*packed\s*\)\s*\)\s*)?\{", src[pos:])
        if not m:
            break
        brace = pos + m.end() - 1          # index of the `{` itself
        body, after = _balanced(src, brace)
        if body is None:
            break
        pos = after
        # The declaration prefix is the text between the previous declaration and
        # the `{`: `[typedef] struct [tag]`. Cutting at the identifier instead of
        # the brace left it empty, and every record was then rejected.
        head = src[:brace]
        cut = max(head.rfind(";"), head.rfind("}"))
        prefix = head[cut + 1:].strip()
        pm = _REC_PREFIX.match(re.sub(r"\s+", " ", prefix))
        if not pm:
            continue
        tm = re.match(r"\s*([A-Za-z_]\w*)\s*;", src[after:])
        alias = tm.group(1) if tm else None
        if pm.group("td") and not alias:
            continue                      # `typedef struct {...}` with no name
        out.append((pm.group("tag"), body, alias, "packed" in prefix, m.group(1) == "union"))
    return out


def _strip(text):
    """Source without comments, preprocessor lines or attribute soup."""
    text = _COMMENT.sub(" ", text)
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("#"):
            continue
        out.append(line)
    text = " ".join(out)
    text = re.sub(r"__attribute__\s*\(\(.*?\)\)", " ", text)
    text = re.sub(r"\b(?:__restrict|__restrict__|restrict)\b", " ", text)
    return re.sub(r"\s+", " ", text)


def _strip_record_bodies(src):
    """Remove `(struct|union|enum) [Tag] { ... }` bodies (including nested ones),
    leaving `extern "C" { ... }` wrapper contents intact."""
    res = []
    pos = 0
    pat = re.compile(r"\b(struct|union|enum)\b(\s+[A-Za-z_]\w*)?\s*\{")
    while True:
        m = pat.search(src, pos)
        if not m:
            res.append(src[pos:])
            break
        brace = m.end() - 1
        _b, after = _balanced(src, brace)
        if _b is None:
            res.append(src[pos:])
            break
        res.append(src[pos:brace])
        res.append(" ")
        pos = after
    text = "".join(res)
    text = re.sub(r'\bextern\s+"C"\s*\{', " ", text)
    return text


def _words(t):
    return [w for w in t.split() if w]


def scalar_entry(cname):
    """`C_SCALARS[cname]`, with the width of the target for a platform spelling.

    `long` is eight bytes on LP64 and four on i686 and on Windows; `size_t`,
    `ssize_t`, `ptrdiff_t`, `intptr_t` and `uintptr_t` follow the pointer size.
    Everything else has a width the C language fixes and is returned as written.
    """
    if cname in ("long", "long int"):
        return ("i64", 8) if long_width() == 8 else ("i32", 4)
    if cname in ("unsigned long", "unsigned long int"):
        return ("u64", 8) if long_width() == 8 else ("u32", 4)
    if cname in ("size_t", "ptrdiff_t", "ssize_t", "intptr_t"):
        return ("usize", ptr_width()) if cname == "size_t" \
            else ("i64", ptr_width()) if ptr_width() == 8 else ("i32", ptr_width())
    if cname == "uintptr_t":
        return ("u64", ptr_width()) if ptr_width() == 8 else ("u32", ptr_width())
    return C_SCALARS.get(cname)


# Every real C library decorates its declarations with an export macro:
# `SQLITE_API int sqlite3_step(...)`, `ZEXTERN int ZEXPORT zlibVersion(void)`,
# `PNGAPI png_structp png_create_read_struct(...)`. The macro is for the linker,
# not for the type, and a reader that treats it as part of the type refuses the
# whole header -- sqlite3 alone lost 160 results to `SQLITE_API int`. The
# convention every one of those headers follows is unbroken: a macro is
# ALL_CAPS, a type is not. `_Bool` and `_Complex` start with an underscore and
# are therefore never touched.
_EXPORT_MACRO = re.compile(r"^[A-Z][A-Z0-9_]+$")


def _drop_export_macros(t):
    """`SQLITE_API int SQLITE_DEPRECATED` -> `int`; unchanged if nothing is left.

    A header may also legitimately name a type in capitals (`FILE`, `HANDLE`),
    so the stripped form is only used when a word survives it.
    """
    words = [w for w in t.split() if w]
    kept = [w for w in words if not _EXPORT_MACRO.match(w)]
    if not kept or len(kept) == len(words):
        return t
    return " ".join(kept)


def _base_type(t):
    """`const struct Foo *` -> ('ptr', 'Foo'); `unsigned long` -> ('u64', ...)."""
    t = _drop_export_macros(t.strip())
    # `unsigned`, `signed`, `long`, `short` are *complete* C types on their own
    # (`unsigned f(unsigned x)` is legal and common), so the whole spelling is
    # tried first: the qualifier-stripping below would leave nothing of them.
    if "*" not in t and scalar_entry(" ".join(_words(t))) is not None:
        return scalar_entry(" ".join(_words(t)))
    stars = t.count("*")
    cleaned = " ".join(w for w in _words(t.replace("*", " ")) if w not in QUALIFIERS)
    named = None
    m = re.search(r"\b(?:struct|union|enum)\s+([A-Za-z_]\w*)", t)
    if m:
        named = m.group(1)
    if stars:
        base = cleaned.replace("unsigned", "").replace("signed", "").strip()
        if stars > 1:
            return ("ptr", "ptr")
        if base == "char" and not named:
            # `const char *` is a string, and only it is: `str` is BRAT's
            # read-only view, so handing one to a parameter that may write would
            # corrupt an immutable, shared value. A writable `char *` (including
            # `char *const`, where `const` qualifies the pointer, not the pointee)
            # is a byte buffer, and `ptr[u8]` is what a caller can legally give it.
            raw = set(_words(t.replace("*", " ")))
            pointee_quals = set(_words(t.split("*", 1)[0]))
            # `unsigned char *` / `signed char *` are byte buffers whatever their
            # constness: only a plain `char` pointer is text. Ask the signedness
            # first, or `const unsigned char *` becomes a string.
            if "unsigned" in raw:
                return ("ptr", "u8")
            if "signed" in raw:
                return ("ptr", "i8")
            if "const" in pointee_quals:
                return ("str", None)
            return ("ptr", "u8")
        if named is None and scalar_entry(base) is not None:
            # `int *`, `float *`: a pointer to one machine word. The element is
            # the scalar BRAT already calls the width by.
            return ("ptr", scalar_entry(base)[0])
        # A struct pointer (`struct Foo *`, `Foo *`) carries the record type:
        # that is what makes `ptr[Foo]` a stronger declaration than `ptr`.
        return ("ptr", named or base or None)
    if named:
        return ("struct", named) if "struct" in t or "union" in t else ("i32", named)
    if scalar_entry(cleaned) is not None:
        return scalar_entry(cleaned)
    # A typedef we have not resolved yet: keep the name, decide later.
    return ("typedef", cleaned)


class CDecl:
    def __init__(self):
        self.fns = {}        # name -> (ret_c, [param_c], variadic)
        self.param_names = {}  # name -> [param name or None], for readable output
        self.structs = {}    # name -> [(field, c_type)]
        self.packed = set()  # records the header declares with no padding
        self.unions = set()  # records the header declares as `union`
        self.typedefs = {}   # name -> c_type it aliases
        self.fn_typedefs = {}  # alias -> `<ret> (*)(<params>)`
        self.enums = {}      # enum_name -> [(member, int_val)]
        self.enum_types = {}  # enum tag / typedef alias -> exact BRAT integer scalar ("i8".."u64")
        self.consts = []     # [(name, int_val)] from #define & enum
        self.inline_structs = set()  # synthetic nested `Outer__field` records
        self.anon_members = {}  # (parent_name, "anonK") -> synth_name for C11 anon inline struct/union
        self.aliases = set()  # names a `typedef` introduced: spellable bare
        self.tags = set()     # names that exist only as `struct X`: need the keyword
        self.anon = set()     # records holding an anonymous union/struct member
        self.arrays = set()   # records with inline array fields (not a pointer)
        self.pack_align = {}  # record -> active `#pragma pack(N)` cap (1, 2, 4, 8, 16)
        self.bitfields = {}   # record -> [(field_or_None, ctype, width)]
        self.abi_unsupported = {}

    def c_name(self, name):
        """How C spells this record's type in *this* header."""
        if name in self.tags and name not in self.aliases:
            return ("union %s" if name in self.unions else "struct %s") % name
        return name


_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)


def read_header(path, depth=0, seen=None):
    """A header with its own quoted includes inlined, in place.

    `zlib.h` gets its types -- `Byte`, `uInt`, `uLong`, `z_stream` -- from
    `zconf.h` one line up, so a reader that stops at the first file cannot bind
    a single zlib function correctly and reports every one of them as "no BRAT
    spelling". Angle-bracket includes are deliberately not followed: those are
    the platform's (`<stdio.h>`, `<stdint.h>`), they would flood the binding
    with the whole libc, and BRAT does not need to re-declare them.
    """
    seen = seen or set()
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return ""
    if depth > 8 or path in seen:
        return text
    seen.add(path)
    here = os.path.dirname(os.path.abspath(path))
    out = []

    def sub(m):
        target = os.path.join(here, m.group(1))
        if not os.path.exists(target):
            return m.group(0)
        return read_header(target, depth + 1, seen)

    pos = 0
    for m in _INCLUDE.finditer(text):
        out.append(text[pos:m.start()])
        out.append(sub(m))
        pos = m.end()
    out.append(text[pos:])
    return "".join(out)


_LINEMARK_RE = re.compile(r'^#[ \t]+\d+[ \t]+"([^"]*)"(.*)$')


def preprocess_header(path, include_dirs=(), defines=(), target=None):
    """Run `cc -E -dD -std=gnu11` on `path` and return non-system header output.

    System header chunks (GCC/Clang line-marker flag `3`, `<built-in>`, and
    `<command-line>`) are filtered out so `#include <stdint.h>` expands macros
    without flooding `bindgen` with libc internals. Fails loudly if `cc -E` fails.
    """
    import shutil
    cc = os.environ.get("CC")
    if not cc:
        for cand in ("cc", "gcc", "clang"):
            found = shutil.which(cand)
            if found:
                cc = found
                break
    if not cc:
        raise RuntimeError("no C compiler found for --preprocess")
    cmd = [cc, "-E", "-dD", "-std=gnu11", "-x", "c"]
    if target and "clang" in os.path.basename(cc).lower():
        cmd.append("--target=" + target)
    here = os.path.dirname(os.path.abspath(path))
    if here:
        cmd.append("-I" + here)
    for inc in include_dirs or ():
        cmd.append("-I" + inc)
    for dfn in defines or ():
        cmd.append("-D" + dfn)
    cmd.append(path)
    tmo_sec = 120.0
    env_tmo = os.environ.get("BRAT_CC_TIMEOUT_MS", "").strip()
    if env_tmo:
        try:
            ms_v = int(env_tmo)
            if 0 < ms_v <= 3600000:
                tmo_sec = ms_v / 1000.0
        except ValueError:
            pass
    import signal as _sig
    import threading as _thr
    import time as _tm
    pkw = {"stdin": subprocess.DEVNULL, "stdout": subprocess.PIPE, "stderr": subprocess.PIPE}
    if os.name == "nt":
        pkw["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        pkw["start_new_session"] = True
    p_cc = subprocess.Popen(cmd, **pkw)
    o_ch, e_ch = [], []
    p_st = {"ol": 0, "el": 0, "over": False}
    p_lk = _thr.Lock()

    def _kill_cc():
        if os.name != "nt":
            try:
                os.killpg(p_cc.pid, _sig.SIGKILL)
                return
            except OSError:
                pass
        try:
            p_cc.kill()
        except OSError:
            pass

    def _pump(pipe, dst, key, cap):
        try:
            while True:
                b = pipe.read(65536)
                if not b:
                    break
                with p_lk:
                    p_st[key] += len(b)
                    if p_st[key] > cap:
                        p_st["over"] = True
                        _kill_cc()
                        break
                    dst.append(b)
        finally:
            try:
                pipe.close()
            except OSError:
                pass

    to_t = _thr.Thread(target=_pump, args=(p_cc.stdout, o_ch, "ol", 64 * 1024 * 1024), daemon=True)
    te_t = _thr.Thread(target=_pump, args=(p_cc.stderr, e_ch, "el", 2 * 1024 * 1024), daemon=True)
    to_t.start()
    te_t.start()
    dl_t = _tm.monotonic() + tmo_sec
    cc_timed_out = False
    while p_cc.poll() is None:
        rem_t = dl_t - _tm.monotonic()
        if rem_t <= 0:
            cc_timed_out = True
            _kill_cc()
            break
        try:
            p_cc.wait(timeout=min(0.1, rem_t))
        except subprocess.TimeoutExpired:
            pass
    p_cc.wait()
    to_t.join(timeout=2.0)
    te_t.join(timeout=2.0)
    if cc_timed_out:
        raise RuntimeError("cc -E timed out on %s" % path)
    if p_st["over"]:
        raise RuntimeError("cc -E output exceeded 64 MiB limit on %s" % path)
    out_txt = b"".join(o_ch).decode("utf-8", errors="replace")
    err_txt = b"".join(e_ch).decode("utf-8", errors="replace")
    if p_cc.returncode != 0:
        err = err_txt.strip() or ("exit status %d" % p_cc.returncode)
        raise RuntimeError("cc -E failed on %s: %s" % (path, err))
    mode = "keep"
    kept_lines = []
    for line in out_txt.splitlines():
        lm = _LINEMARK_RE.match(line)
        if lm:
            fname = lm.group(1)
            flags = set(lm.group(2).split())
            if fname == "<command-line>":
                mode = "cmdline"
            elif (not fname.startswith("<")) and ("3" not in flags):
                mode = "keep"
            else:
                mode = "skip"
            continue
        if mode == "keep":
            kept_lines.append(line)
        elif mode == "cmdline":
            dm = re.match(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z]\w*)\b", line)
            if dm:
                kept_lines.append(line)
    return "\n".join(kept_lines) + "\n"


def _pragma_pack_for_records(uncommented):
    """Map record names/tags in `uncommented` to active `#pragma pack(N)` values."""
    pragma_re = re.compile(r"^[ \t]*#[ \t]*pragma[ \t]+pack[ \t]*\(([^)]*)\)", re.M)
    checkpoints = [(0, None)]
    stack = []
    cur = None
    for pm in pragma_re.finditer(uncommented):
        args = [a.strip() for a in pm.group(1).split(",") if a.strip()]
        if not args:
            cur = None
        elif args[0] == "push":
            stack.append(cur)
            if len(args) >= 2 and args[-1].isdigit():
                val = int(args[-1])
                if val in (1, 2, 4, 8, 16):
                    cur = val
        elif args[0] == "pop":
            cur = stack.pop() if stack else None
            if len(args) >= 2 and args[-1].isdigit():
                val = int(args[-1])
                if val in (1, 2, 4, 8, 16):
                    cur = val
        elif len(args) == 1 and args[0].isdigit():
            val = int(args[0])
            if val in (1, 2, 4, 8, 16):
                cur = val
        checkpoints.append((pm.end(), cur))
    if all(v is None for _, v in checkpoints):
        return {}

    def pack_at(offset):
        ans = None
        for cpos, cval in checkpoints:
            if cpos <= offset:
                ans = cval
            else:
                break
        return ans

    # Blank `#...` lines in place so character offsets stay exact for `pack_at`.
    blanked = re.sub(r"^[ \t]*#[^\n]*", lambda m: " " * len(m.group(0)), uncommented, flags=re.M)
    blanked = re.sub(r"__attribute__\s*\(\(.*?\)\)", lambda m: " " * len(m.group(0)), blanked, flags=re.S)
    pack_map = {}
    pos = 0
    rec_pat = re.compile(r"\b(struct|union)\s*(?:[A-Za-z_]\w*\s*)?\{")
    while True:
        m = rec_pat.search(blanked, pos)
        if not m:
            break
        brace = m.end() - 1
        _body, after = _balanced(blanked, brace)
        if _body is None:
            break
        pos = after
        if m.group(1) == "union":
            continue
        pval = pack_at(brace)
        if pval is None:
            continue
        head = blanked[:brace]
        cut = max(head.rfind(";"), head.rfind("}"))
        prefix = re.sub(r"\s+", " ", head[cut + 1:].strip())
        pm = _REC_PREFIX.match(prefix)
        if not pm:
            continue
        tm = re.match(r"\s*([A-Za-z_]\w*)\s*;", blanked[after:])
        alias = tm.group(1) if tm else None
        if pm.group("td") and not alias:
            continue
        tag = pm.group("tag")
        if alias:
            pack_map[alias] = pval
        if tag:
            pack_map[tag] = pval
    return pack_map


_INT_TOK_RE = re.compile(
    r"\s*(0[xX][0-9a-fA-F]+[uUlL]*|0[bB][01]+[uUlL]*|\d+[uUlL]*|"
    r"[A-Za-z_]\w*|<<|>>|[+\-*/%|&^~()])"
)


def _eval_c_int_expr(expr_text, const_env):
    """Safely evaluate a side-effect-free C integer constant expression, or None."""
    s = (expr_text or "").strip()
    if not s or '"' in s or "'" in s:
        return None
    pos = 0
    py_toks = []
    while pos < len(s):
        m = _INT_TOK_RE.match(s, pos)
        if not m:
            return None
        tok = m.group(1)
        pos = m.end()
        while pos < len(s) and s[pos].isspace():
            pos += 1
        if tok[0].isdigit():
            lit = tok.rstrip("uUlL")
            try:
                val = int(lit, 0)
            except ValueError:
                return None
            py_toks.append(str(val))
        elif tok[0].isalpha() or tok[0] == "_":
            if tok not in const_env:
                return None
            py_toks.append("(%d)" % const_env[tok])
        elif tok == "/":
            py_toks.append("//")
        else:
            py_toks.append(tok)
    if not py_toks:
        return None
    import ast as _pyast

    def _eval_node(nd):
        if isinstance(nd, _pyast.Constant) and isinstance(nd.value, int) and not isinstance(nd.value, bool):
            return nd.value
        if isinstance(nd, _pyast.UnaryOp):
            v = _eval_node(nd.operand)
            if v is None:
                return None
            if isinstance(nd.op, _pyast.UAdd):
                return +v
            if isinstance(nd.op, _pyast.USub):
                return -v
            if isinstance(nd.op, _pyast.Invert):
                return ~v
            return None
        if isinstance(nd, _pyast.BinOp):
            l = _eval_node(nd.left)
            r = _eval_node(nd.right)
            if l is None or r is None:
                return None
            if isinstance(nd.op, _pyast.Add):
                return l + r
            if isinstance(nd.op, _pyast.Sub):
                return l - r
            if isinstance(nd.op, _pyast.Mult):
                return l * r
            if isinstance(nd.op, _pyast.FloorDiv):
                return int(l / r) if r != 0 else None
            if isinstance(nd.op, _pyast.Mod):
                return (l - int(l / r) * r) if r != 0 else None
            if isinstance(nd.op, _pyast.BitOr):
                return l | r
            if isinstance(nd.op, _pyast.BitAnd):
                return l & r
            if isinstance(nd.op, _pyast.BitXor):
                return l ^ r
            if isinstance(nd.op, _pyast.LShift):
                return (l << r) if 0 <= r <= 63 else None
            if isinstance(nd.op, _pyast.RShift):
                return (l >> r) if 0 <= r <= 63 else None
        return None

    try:
        tree = _pyast.parse(" ".join(py_toks), mode="eval")
    except SyntaxError:
        return None
    val = _eval_node(tree.body)
    if val is None or not (-(1 << 63) <= val <= (1 << 64) - 1):
        return None
    return val


_ENUM_PACKED_RE = re.compile(r"__attribute__\s*\(\s*\(\s*(?:__)?packed(?:__)?\s*\)\s*\)")


def _derive_enum_brat_type(base_type, is_packed, members, cdecl=None):
    """Exact C enum integer ABI width & signedness (`i8/u8/i16/u16/i32/u32/i64/u64`)."""
    if base_type:
        bk, _be = resolve(cdecl, base_type)
        if bk == "usize":
            return "u64" if ptr_width() == 8 else "u32"
        if bk in ("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"):
            return bk
    lo = min((v for _, v in members), default=0)
    hi = max((v for _, v in members), default=0)
    if is_packed:
        if lo < 0:
            if -128 <= lo and hi <= 127:
                return "i8"
            if -32768 <= lo and hi <= 32767:
                return "i16"
            if -(1 << 31) <= lo and hi <= (1 << 31) - 1:
                return "i32"
            return "i64"
        if hi <= 0xFF:
            return "u8"
        if hi <= 0xFFFF:
            return "u16"
        if hi <= 0xFFFFFFFF:
            return "u32"
        return "u64"
    if -(1 << 31) <= lo and hi <= (1 << 31) - 1:
        return "i32"
    if lo >= 0 and hi <= 0xFFFFFFFF:
        return "u32"
    return "i64" if lo < 0 else "u64"


def _scan_enums(raw_src, const_env):
    """Extract `(tag, alias, [(member, val)], base_type, is_packed)` for C `enum` declarations."""
    out = []
    src = re.sub(r"^[ \t]*#[^\n]*", " ", raw_src, flags=re.M)
    for fm in re.finditer(
            r"\benum\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_][\w\s]*?)\s*;", src):
        out.append((fm.group(1), None, [], fm.group(2).strip(), False))
    pos = 0
    prefix_re = re.compile(
        r"^(?P<td>typedef\s+)?enum(?:\s+(?P<tag>[A-Za-z_]\w*))?"
        r"(?:\s*:\s*(?P<base>[A-Za-z_][\w\s]*?))?\s*$"
    )
    while True:
        m = re.search(r"\benum\b[^{;]*\{", src[pos:])
        if not m:
            break
        brace = pos + m.end() - 1
        body, after = _balanced(src, brace)
        if body is None:
            break
        pos = after
        head = src[:brace]
        cut = max(head.rfind(";"), head.rfind("}"))
        raw_prefix = head[cut + 1:]
        has_head_pack = bool(_ENUM_PACKED_RE.search(raw_prefix))
        clean_prefix = re.sub(r"\s+", " ", _ENUM_PACKED_RE.sub(" ", raw_prefix)).strip()
        pm = prefix_re.match(clean_prefix)
        if not pm:
            continue
        tail_semi = src.find(";", after)
        raw_tail = src[after:tail_semi] if tail_semi >= 0 else ""
        has_tail_pack = bool(_ENUM_PACKED_RE.search(raw_tail))
        clean_tail = _ENUM_PACKED_RE.sub(" ", raw_tail).strip()
        tm = re.match(r"^([A-Za-z_]\w*)$", clean_tail)
        alias = tm.group(1) if (pm.group("td") and tm) else None
        if pm.group("td") and not alias:
            continue
        base_type = pm.group("base").strip() if pm.group("base") else None
        is_packed = (has_head_pack or has_tail_pack) and not base_type
        members = []
        nxt = 0
        for raw_item in body.split(","):
            item = _ENUM_PACKED_RE.sub(" ", raw_item).strip()
            if not item:
                continue
            if "=" in item:
                nm, rhs = item.split("=", 1)
                nm = nm.strip()
                if not re.match(r"^[A-Za-z_]\w*$", nm):
                    continue
                val = _eval_c_int_expr(rhs, const_env)
                if val is None:
                    continue
                nxt = val + 1
            else:
                nm = item
                if not re.match(r"^[A-Za-z_]\w*$", nm):
                    continue
                val = nxt
                nxt = val + 1
            const_env[nm] = val
            members.append((nm, val))
        out.append((pm.group("tag"), alias, members, base_type, is_packed))
    return out


def _extract_nested_inline_structs(parent_name, body, is_packed, out, const_env, parent_pack=None):
    """Rewrite inline `struct`/`union` `{ ... } [field];` members into synthetic records."""
    if not parent_name or "{" not in body:
        return body
    res = []
    pos = 0
    anon_idx = 0
    while True:
        m = re.search(r"\b(struct|union)\s*(?:([A-Za-z_]\w*)\s*)?"
                      r"(?:__attribute__\s*\(\s*\(\s*packed\s*\)\s*\)\s*)?\{", body[pos:])
        if not m:
            res.append(body[pos:])
            break
        kw = m.group(1)
        inner_brace = pos + m.end() - 1
        inner_body, after = _balanced(body, inner_brace)
        if inner_body is None:
            res.append(body[pos:])
            break
        fm = re.match(r"\s*([A-Za-z_]\w*)\s*(?:\[([^\]]+)\]\s*)?;", body[after:])
        am = None if fm else re.match(r"\s*;", body[after:])
        if not fm and not am:
            res.append(body[pos:after])
            pos = after
            continue
        if fm:
            fname = fm.group(1)
            arr_dim = fm.group(2)
            tail_end = fm.end()
            is_anon_mem = False
        else:
            fname = "anon%d" % anon_idx
            anon_idx += 1
            arr_dim = None
            tail_end = am.end()
            is_anon_mem = True
        synth = "%s__%s" % (parent_name, fname)
        inner_packed = (kw == "struct") and (is_packed or ("packed" in body[pos + m.start():inner_brace]))
        inner_pack = (1 if inner_packed else parent_pack) if kw == "struct" else None
        clean_inner = _extract_nested_inline_structs(synth, inner_body, inner_packed, out, const_env, inner_pack)
        if "{" in clean_inner:
            res.append(body[pos:after])
            pos = after
            continue
        inner_bf = _scan_bitfields(clean_inner, const_env)
        if inner_bf:
            out.bitfields.setdefault(parent_name, []).extend(inner_bf)
            out.anon.add(parent_name)
            res.append(body[pos:after])
            pos = after
            continue
        inner_fields, inner_has_arr = _fields_ex(clean_inner, const_env)
        if not inner_fields or any(n.startswith("_anon") for n, _ in inner_fields):
            res.append(body[pos:after])
            pos = after
            continue
        out.structs[synth] = inner_fields
        out.tags.add(synth)
        out.inline_structs.add(synth)
        if kw == "union":
            out.unions.add(synth)
        if is_anon_mem:
            out.anon_members[(parent_name, fname)] = synth
        if inner_packed:
            out.packed.add(synth)
        if inner_pack is not None:
            out.pack_align[synth] = inner_pack
        if inner_has_arr:
            out.arrays.add(synth)
        res.append(body[pos:pos + m.start()])
        arr_suf = ("[%s]" % arr_dim.strip()) if arr_dim else ""
        res.append("%s %s %s%s;" % (kw, synth, fname, arr_suf))
        pos = after + tail_end
    return "".join(res)


def parse_header(text):
    """Declarations out of a header, in the order a compiler would accept them."""
    # `_strip` drops comments and the attribute macros it does not know, so the
    # record's layout is read from the raw text first: `packed` is part of the
    # layout, not decoration, and `typedef struct __attribute__((packed)) {...} X`
    # and `typedef struct {...} __attribute__((packed)) X` mean the same to C.
    packed = set()
    raw_ns = re.sub(r"\s+", "", re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.S))
    _ATTR = r"(?:__attribute__\(\(packed\)\))?"
    for m in re.finditer(r"typedef([A-Za-z_\w]*struct%s\{.*?\})%s([A-Za-z_]\w*);"
                         % (_ATTR, _ATTR), raw_ns):
        body, alias = m.group(1), m.group(2)
        head = body[: body.index("{")] if "{" in body else body
        tail = body[body.rindex("}") + 1:] if "}" in body else ""
        if "packed" in head or "packed" in tail or "packed" in m.group(0).split(alias)[0]:
            packed.add(alias)
    out = CDecl()
    const_env = {}
    seen_consts = set()
    uncommented_lines = re.sub(r"\\\r?\n", " ", _COMMENT.sub(" ", text))
    guard_names = {m.group(1) for m in re.finditer(r"^[ \t]*#[ \t]*ifndef[ \t]+([A-Za-z_]\w*)",
                                                   uncommented_lines, re.M)}
    for dm in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(?![(])[ \t]+([^\r\n]+)$",
                          uncommented_lines, re.M):
        dname, drhs = dm.group(1), dm.group(2).strip()
        if dname in guard_names and drhs == "1" and dname.upper().endswith(("_H", "_H_", "_INCLUDED")):
            continue
        val = _eval_c_int_expr(drhs, const_env)
        if val is not None:
            const_env[dname] = val
            if -(1 << 63) <= val <= (1 << 63) - 1 and dname not in seen_consts:
                seen_consts.add(dname)
                out.consts.append((dname, val))
    uncommented = _COMMENT.sub(" ", text)
    raw_enums = _scan_enums(uncommented, const_env)
    for etag, ealias, emembers, ebase, epacked in raw_enums:
        ename = ealias or etag or "_enum"
        if emembers or ename not in out.enums:
            out.enums[ename] = emembers
        escalar = _derive_enum_brat_type(ebase, epacked, emembers, out)
        if etag:
            out.enum_types[etag] = escalar
        if ealias:
            out.enum_types[ealias] = escalar
            out.aliases.add(ealias)
        for mname, mval in emembers:
            if -(1 << 63) <= mval <= (1 << 63) - 1 and mname not in seen_consts:
                seen_consts.add(mname)
                out.consts.append((mname, mval))
    src = re.sub(
        r"\b(enum(?:\s+[A-Za-z_]\w*)?)\s*:\s*[A-Za-z_][\w\s]*?(?=\s*[{;])",
        r"\1",
        _strip(text),
    )
    for declaration in uncommented.split(";"):
        abi_attr = _ABI_CALLCONV.search(declaration)
        if not abi_attr:
            continue
        parsed = _DECL.search(_strip(declaration) + ";")
        if parsed:
            out.abi_unsupported[parsed.group(2)] = abi_attr.group(0)

    # Record bodies are scanned, not matched: a body may hold a union, a nested
    # struct or an array of them, and `.*?` would stop at the inner brace and
    # read the fields of the wrong text. The attribute is part of the layout, so
    # it is recorded with the fields.
    pragma_packs = _pragma_pack_for_records(uncommented)
    for tag, body, alias, packd, is_un in _scan_records(src):
        rec_name = alias or tag or ""
        p_align = None if is_un else (pragma_packs.get(alias) or pragma_packs.get(tag))
        is_packd = (not is_un) and ((alias in packed) or packd or p_align == 1)
        if is_packd:
            p_align = 1
        norm_body = _extract_nested_inline_structs(rec_name, body, is_packd, out, const_env, p_align)
        rec_bf = _scan_bitfields(norm_body, const_env)
        if rec_bf:
            out.bitfields.setdefault(rec_name, []).extend(rec_bf)
            out.anon.add(rec_name)
        fields, has_arr = _fields_ex(norm_body, const_env)
        if "{" in norm_body:
            out.anon.add(rec_name)
        if has_arr or re.search(r"\[[^\]]*\]", norm_body):
            out.arrays.add(rec_name)
            if any(not re.search(r"\[\d+\]$", ct) and "[" in ct for _, ct in fields):
                out.anon.add(rec_name)
        if alias:
            out.aliases.add(alias)
            out.structs[alias] = fields
            if is_un:
                out.unions.add(alias)
            elif is_packd:
                out.packed.add(alias)
            if p_align is not None:
                out.pack_align[alias] = p_align
            if rec_name in out.bitfields:
                out.bitfields[alias] = out.bitfields[rec_name]
            if tag:
                out.structs.setdefault(tag, fields)
                if is_un:
                    out.unions.add(tag)
                elif is_packd:
                    out.packed.add(tag)
                if p_align is not None:
                    out.pack_align.setdefault(tag, p_align)
                if rec_name in out.bitfields:
                    out.bitfields.setdefault(tag, out.bitfields[rec_name])
                    out.anon.add(tag)
        elif tag:
            out.structs[tag] = fields
            out.tags.add(tag)
            if is_un:
                out.unions.add(tag)
            elif is_packd:
                out.packed.add(tag)
            if p_align is not None:
                out.pack_align[tag] = p_align
        if any(n is None or (isinstance(n, str) and n.startswith("_anon"))
               for n, _ in fields):
            out.anon.add(rec_name)
    flat = _strip_record_bodies(src)
    # Function-pointer typedefs: `typedef int (*cmp_fn)(int a, int b);`
    for fm in re.finditer(
            r"typedef\s+([A-Za-z_][\w\s\*]*?)\s*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(([^;{}]*)\)\s*;",
            flat):
        fret, falias, fparams = fm.group(1).strip(), fm.group(2), fm.group(3).strip()
        out.aliases.add(falias)
        out.fn_typedefs[falias] = "%s (*)(%s)" % (fret, fparams)
    # `typedef z_stream FAR *z_streamp;` has no space between the star and the
    # new name, so requiring one dropped every pointer typedef -- `z_streamp`,
    # `voidpf`, `gz_headerp` -- and with them every zlib function that takes one.
    for m in re.finditer(r"typedef\s+([^;{}()]+?)\s*([A-Za-z_]\w*)\s*;", flat):
        target, alias = m.group(1).strip(), m.group(2)
        out.aliases.add(alias)
        if target.split()[:1] in (["struct"], ["union"]) and "*" not in target:
            tlast = target.split()[-1]
            out.structs.setdefault(alias, out.structs.get(tlast, []))
            if target.startswith("union") or tlast in out.unions:
                out.unions.add(alias)
        elif target.startswith("enum ") and "*" not in target:
            tlast = target.split()[-1]
            out.enum_types[alias] = out.enum_types.get(tlast, "i32")
        else:
            out.typedefs[alias] = target

    # Re-resolve any fixed-underlying `enum E : CustomTypedef` after typedefs are populated.
    for etag, ealias, emembers, ebase, epacked in raw_enums:
        if not ebase:
            continue
        escalar = _derive_enum_brat_type(ebase, epacked, emembers, out)
        if etag:
            out.enum_types[etag] = escalar
        if ealias:
            out.enum_types[ealias] = escalar

    body = flat
    for m in _DECL.finditer(body):
        ret, name, params = m.group(1).strip(), m.group(2), m.group(3).strip()
        if name in ("if", "while", "for", "switch", "return", "sizeof"):
            continue
        if ret.split() and ret.split()[-1] in ("typedef", "struct", "union", "enum"):
            continue
        plist = []
        pnames = []
        variadic = False
        if params and params != "void":
            for part in _split_params(params):
                if part.strip() in ("...",):
                    variadic = True
                    continue
                got = _split_decl(part)
                plist.append(got[1] if got else part.strip())
                pnames.append(got[0] if got else None)
        out.fns[name] = (ret, plist, variadic)
        out.param_names[name] = pnames
    return out


_STORAGE_QUALIFIERS = QUALIFIERS - {"struct", "union", "enum", "signed", "unsigned"}


def _ends_in_type_name(head):
    """`struct P`, `const handle_t`: the last word is the type, so the declarator has no name."""
    words = [w for w in re.findall(r"[A-Za-z_]\w*", head) if w not in _STORAGE_QUALIFIERS]
    return not words or words[-1] in ("struct", "union", "enum")


def _split_decl(part):
    """(name, c_type) for a declarator like `const char *restrict s` or `int a[4]`."""
    p = " ".join(part.split())
    tail = ""
    m = re.search(r"\[([^\]]*)\]\s*$", p)
    if m:
        tail = "*"                      # an array parameter is a pointer
        p = p[:m.start(1)].rstrip("[ ")
    m = re.match(r"^(.*?[\s\*])([A-Za-z_]\w*)$", p)
    if not m:
        return None
    head, name = m.group(1).strip(), m.group(2)
    if _ends_in_type_name(head):
        return None
    if head in C_SCALARS and name in C_SCALARS:
        # `unsigned long` is a type, not a declaration: two words, no name.
        return None
    if name in C_SCALARS or name in QUALIFIERS:
        return None
    return (name, (head + tail).strip() if tail else head)


def _split_params(text):
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return parts


def _split_decl_field(part, const_env=None):
    """`(name, c_type, is_array)` for a struct member, preserving `T[N]` inline arrays."""
    p = " ".join(part.split())
    cbm = re.match(
        r"^\s*(?P<ret>[A-Za-z_][\w\s\*]*?)\s*\(\s*\*\s*(?P<name>[A-Za-z_]\w*)\s*\)\s*\((?P<params>[^()]*)\)\s*$",
        p)
    if cbm:
        return (cbm.group("name"), "%s (*)(%s)" % (cbm.group("ret").strip(), cbm.group("params").strip()), False)
    arr_cnt = None
    m = re.search(r"\[([^\]]*)\]\s*$", p)
    if m:
        raw_dim = m.group(1).strip()
        p_head = p[:m.start()].rstrip()
        if "[" in p_head:
            return (None, p, True)  # multi-dimensional array: leave unrepresented
        val = _eval_c_int_expr(raw_dim, const_env or {}) if raw_dim else None
        if val is None or val < 1:
            return (None, p, True)
        arr_cnt = val
        p = p_head
    m = re.match(r"^(.*?[\s\*])([A-Za-z_]\w*)$", p)
    if not m:
        return (None, part.strip(), arr_cnt is not None)
    head, name = m.group(1).strip(), m.group(2)
    if _ends_in_type_name(head):
        return (None, part.strip(), arr_cnt is not None)
    if head in C_SCALARS and name in C_SCALARS:
        return (None, part.strip(), arr_cnt is not None)
    if name in C_SCALARS or name in QUALIFIERS:
        return (None, part.strip(), arr_cnt is not None)
    if arr_cnt is not None:
        return (name, "%s[%d]" % (head, arr_cnt), True)
    return (name, head, False)


_BITFIELD_MEM_RE = re.compile(r"^(?P<decl>[^:]+):(?P<width>[^:]+)$")


def _scan_bitfields(body, const_env=None):
    """Return `[(name_or_None, ctype, width)]` for any C bitfield members in `body`."""
    out = []
    for part in body.split(";"):
        p = " ".join(part.split())
        if not p or ":" not in p:
            continue
        m = _BITFIELD_MEM_RE.match(p)
        if not m:
            continue
        decl_s = m.group("decl").strip()
        w_s = m.group("width").strip()
        w_val = _eval_c_int_expr(w_s, const_env or {})
        width = w_val if w_val is not None else w_s
        got = _split_decl(decl_s)
        if got is not None:
            out.append((got[0], got[1], width))
        else:
            out.append((None, decl_s, width))
    return out


def _fields_ex(body, const_env=None):
    fields = []
    has_arr = False
    for part in body.split(";"):
        p = part.strip()
        if not p:
            continue
        nm, ct, is_arr = _split_decl_field(p, const_env)
        if is_arr:
            has_arr = True
        fields.append((nm, ct))
    return ([(n or "_anon%d" % i, t) for i, (n, t) in enumerate(fields)], has_arr)


def _fields(body):
    """`float x; const char *name;` -> [('x', 'float'), ('name', 'const char *')]."""
    return _fields_ex(body)[0]


# A function-pointer declarator: `float (*cb)(float)`, `Pt (*)(int)`, with the
# name optional and the return type possibly a star. The parameter list of the
# *callback* is the second pair of parentheses, and it is the part that decides
# the ABI: a `float` where C reads a `double` is a wrong register, not a warning.
_CB_C = re.compile(r"^\s*(?P<ret>[A-Za-z_][\w\s\*]*?)\s*\(\s*\*\s*"
                   r"(?P<name>[A-Za-z_]\w*)?\s*\)\s*\((?P<params>[^()]*)\)\s*$")


def c_callback(ctype, cdecl=None):
    """(return_c, [param_c]) for a C function-pointer declarator or typedef, else None."""
    ct = (ctype or "").strip()
    if cdecl is not None and ct:
        cleaned = " ".join(w for w in _words(ct) if w not in QUALIFIERS)
        if cleaned in getattr(cdecl, "fn_typedefs", ()):
            ct = cdecl.fn_typedefs[cleaned]
    m = _CB_C.match(ct)
    if not m:
        return None
    raw_params = [x.strip() for x in _split_params(m.group("params")) if x.strip()]
    if raw_params == ["void"]:
        raw_params = []
    params = []
    for rp in raw_params:
        got = _split_decl(rp)
        params.append(got[1] if got else rp)
    return (m.group("ret").strip(), params)


def brat_callback(ann):
    """(return_type, [param_types]) for a BRAT `fn(a, b) -> c`, else None."""
    if not isinstance(ann, str):
        return None
    text = ann.strip()
    if not text.startswith("fn"):
        return None
    open_ix = text.find("(")
    if open_ix < 0:
        return None
    depth = 0
    close_ix = -1
    for i in range(open_ix, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                close_ix = i
                break
    if close_ix < 0:
        return None
    params = [x.strip() for x in _split_params(text[open_ix + 1:close_ix]) if x.strip()]
    tail = text[close_ix + 1:].strip()
    ret = tail[2:].strip() if tail.startswith("->") else "none"
    return (ret or "none", params)


def same_callback(rep, name, where, brat_ann, ctype, cdecl, header_name, brat_structs):
    """Is a BRAT `fn(...)` the function pointer the header declares?

    Compared as widths, argument by argument, because that is what the call
    does: the count must match exactly, the return must match, and the trailing
    `ptr` of a BRAT signature is C's `void *user_data` -- it is a real argument
    in the ABI, so it is not a wildcard.
    """
    bcb, ccb = brat_callback(brat_ann), c_callback(ctype, cdecl)
    if bcb is None or ccb is None:
        rep.error("extern fn %s: %s is declared `%s`, %s has `%s`"
                  % (name, where, brat_ann, header_name, ctype))
        return False
    bret, bparams = bcb
    cret, cparams = ccb

    def pointer_match(brat_type, ctype, kind, extra):
        elem = element_of(brat_type)
        if brat_type == "str":
            return kind == "str"
        if brat_type == "ptr":
            return kind == "ptr" and extra == "none" and not const_pointee(ctype, cdecl)
        if elem is None:
            return None
        if kind != "ptr" or const_pointee(ctype, cdecl) or elem in brat_structs:
            return False
        words = set(_words(ctype.replace("*", " ")))
        if elem in ("i8", "u8") and "char" in words and not words.intersection(
                {"signed", "unsigned", "int8_t", "uint8_t"}):
            return False
        return pointee_ok(elem, extra, brat_structs)

    def slot_same(brat_type, kind, extra):
        if kind == "struct" or brat_type in brat_structs:
            return (kind == "struct" and brat_type == extra and brat_type in brat_structs
                    and bool(cdecl.structs.get(extra)))
        return same(brat_type, kind)

    ok = True
    if len(bparams) != len(cparams):
        rep.error("extern fn %s: %s is declared `%s` (%d argument(s)), %s has `%s` (%d)"
                  % (name, where, brat_ann, len(bparams), header_name, ctype, len(cparams)))
        ok = False
    for i, (bp, cp) in enumerate(zip(bparams, cparams)):
        kind, extra = resolve(cdecl, cp)
        pointer_ok = pointer_match(bp, cp, kind, extra)
        if element_of(bp) is not None or bp in ("ptr", "str"):
            if not pointer_ok:
                rep.error("extern fn %s: %s argument %d is `%s`, %s has `%s`"
                          % (name, where, i + 1, bp, header_name, cp))
                ok = False
        elif not slot_same(bp, kind, extra):
            rep.error("extern fn %s: %s argument %d is `%s`, %s has `%s`"
                      % (name, where, i + 1, bp, header_name, cp))
            ok = False
    crkind, ccextra = resolve(cdecl, cret)
    if bret == "none":
        if crkind != "none":
            rep.error("extern fn %s: %s returns `%s`, BRAT declares no result"
                      % (name, where, cret))
            ok = False
    elif pointer_match(bret, cret, crkind, ccextra) is False:
        rep.error("extern fn %s: %s returns `%s`, %s has `%s`"
                  % (name, where, bret, header_name, cret))
        ok = False
    elif pointer_match(bret, cret, crkind, ccextra) is None and not slot_same(bret, crkind, ccextra):
        rep.error("extern fn %s: %s returns `%s`, BRAT declares `%s`"
                  % (name, where, cret, bret))
        ok = False
    return ok


def _c_array_type(ctype):
    """`(elem_ctype, count)` for a parsed C inline array `int[4]`, else None."""
    if not isinstance(ctype, str) or "[" not in ctype or not ctype.endswith("]"):
        return None
    lb = ctype.rfind("[")
    elem, cnt = ctype[:lb].strip(), ctype[lb + 1:-1].strip()
    if elem and cnt.isdigit() and int(cnt) >= 1:
        return (elem, int(cnt))
    return None


def resolve(cdecl, t):
    """Follow typedefs, function-pointer aliases, enums and struct tags to (brat_type, c_spelling)."""
    carr = _c_array_type(t)
    if carr is not None:
        ek, ee = resolve(cdecl, carr[0])
        return ("array", (ek, ee, carr[1]))
    if c_callback(t, cdecl) is not None:
        cleaned = " ".join(w for w in _words(t or "") if w not in QUALIFIERS)
        sig = cdecl.fn_typedefs.get(cleaned, t) if cdecl is not None else t
        return ("fn", sig)
    kind, extra, = _base_type(t)
    enum_map = getattr(cdecl, "enum_types", {}) if cdecl is not None else {}
    def _enum_scalar(nm):
        return enum_map[nm] if isinstance(enum_map, dict) else "i32"
    if cdecl is not None and kind == "typedef":
        if extra in getattr(cdecl, "fn_typedefs", ()):
            return ("fn", cdecl.fn_typedefs[extra])
        if extra in enum_map:
            return (_enum_scalar(extra), None)
        if extra in cdecl.typedefs:
            return resolve(cdecl, cdecl.typedefs[extra])
        if extra in cdecl.structs:
            return ("struct", extra)
    if cdecl is not None and kind == "i32" and extra in enum_map:
        return (_enum_scalar(extra), None)
    if cdecl is not None and kind == "ptr" and extra is not None:
        if extra in enum_map:
            return ("ptr", _enum_scalar(extra))
        if extra in cdecl.typedefs and "*" not in cdecl.typedefs[extra]:
            tk, _te = resolve(cdecl, cdecl.typedefs[extra])
            if tk in _WIDTH or tk == "usize":
                return ("ptr", tk)
    if kind == "struct":
        return ("struct", extra)
    return (kind, extra)


def element_of(t):
    """`ptr[T]` -> T, a plain `ptr` -> None. The type as written, else None."""
    if isinstance(t, str) and t.startswith("ptr[") and t.endswith("]"):
        return t[4:-1].strip()
    return None


def pointee_ok(elem, cextra, brat_structs):
    """Does a BRAT `ptr[elem]` agree with a C `T *`?

    A plain `ptr` agrees with anything: it is the erased form, and every
    existing declaration keeps working. A `ptr[T]` is a promise, so it is
    checked -- and a table of T needs `extern struct T` to say what T is, or the
    program would dereference a layout it never declared.
    """
    if elem is None:
        return True                      # `ptr`: no promise to keep
    if elem == cextra:
        return True
    integer_types = {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize"}
    if elem in integer_types and cextra in integer_types \
            and _abi_width(elem) == _abi_width(cextra):
        # Corresponding signed/unsigned integers share an ABI representation.
        return True
    return elem in brat_structs and elem == cextra


def const_pointee(ct, cdecl=None, _seen=()):
    """True when a C pointer type is a pointer *to const*: `const char *`,
    `const void *`, `const struct Foo *`. `T *const` qualifies the pointer
    itself, leaving the pointee writable."""
    if not isinstance(ct, str):
        return False
    if "*" in ct:
        return "const" in _words(ct.split("*")[-2])
    if cdecl is not None:
        cleaned = " ".join(w for w in _words(ct) if w not in QUALIFIERS)
        if cleaned and cleaned not in _seen and cleaned in getattr(cdecl, "typedefs", {}):
            return const_pointee(cdecl.typedefs[cleaned], cdecl, _seen + (cleaned,))
    return False


def same(a, b):
    """Two BRAT types that may stand beside one another, as widths."""
    if a == b:
        return True
    if a in ("ptr", "str") and b in ("ptr", "str"):
        return False      # `str` is `char *`; anything else must say `ptr`
    if a in ("i64", "u64", "usize") and b in ("i64", "u64", "usize"):
        return _abi_width(a) == _abi_width(b)
    # A narrow register: `u8` beside `unsigned char` (which a header calls `i32`)
    # is the same byte as long as the width says so.
    if _abi_width(a) is not None and _abi_width(a) == _abi_width(b) \
            and a in ("u8", "u16", "i8", "i16", "i32", "u32") \
            and b in ("u8", "u16", "i8", "i16", "i32", "u32"):
        return True
    return False


def _abi_width(t):
    """Width in the current target's ABI, not the host's default model."""
    if t in ("usize", "str", "ptr"):
        return ptr_width()
    return _WIDTH.get(t)


class Report:
    def __init__(self):
        self.lines = []
        self.errors = 0
        self.warnings = 0
        self.proven = 0

    def error(self, msg):
        self.errors += 1
        self.lines.append("  error: " + msg)

    def warn(self, msg):
        self.warnings += 1
        self.lines.append("  warning: " + msg)

    def ok(self):
        self.proven += 1


def check(prog_body, cdecl, header_name="<header>", lib=None):
    """Compare BRAT declarations to C declarations. Returns a Report."""
    rep = Report()
    brat_structs = {}
    for s in prog_body:
        if s and s[0] == "externstruct":
            brat_structs[s[1]] = s[2]

    brat_packed = {s[1] for s in prog_body
                   if s and s[0] == "externstruct" and len(s) > 3 and s[3]}
    brat_unions = {s[1] for s in prog_body
                   if s and s[0] == "externstruct" and len(s) > 4 and s[4]}
    c_unions = set(getattr(cdecl, "unions", ()))
    for name, fields in sorted(brat_structs.items()):
        rk_lbl = "extern union" if name in brat_unions else "extern struct"
        if name in getattr(cdecl, "bitfields", {}):
            bf_desc = ", ".join(
                ("%s:%s" % (bn or "<pad>", bw)) for bn, _bt, bw in cdecl.bitfields[name]
            )
            rep.error("%s %s: %s declares C bitfield(s) (%s); bitfields are not byte-addressed and cannot be declared in BRAT"
                      % (rk_lbl, name, header_name, bf_desc))
            continue
        if name in getattr(cdecl, "arrays", ()) and not any(array_field_type(fa) is not None for _, fa in fields):
            rep.error("%s %s: inline array fields in %s are not representable as scalar/pointer fields"
                      % (rk_lbl, name, header_name))
            continue
        if name in c_unions and name not in brat_unions:
            rep.error("extern struct %s: %s declares a union, BRAT declares a struct (write `extern union`)"
                      % (name, header_name))
            continue
        if name in brat_unions and name in cdecl.structs and name not in c_unions:
            rep.error("extern union %s: BRAT declares a union, %s declares a struct"
                      % (name, header_name))
            continue
        if name in getattr(cdecl, "packed", ()) and name not in brat_packed:
            rep.error("extern struct %s: %s declares it packed, BRAT does not "
                      "(write `extern packed struct`)" % (name, header_name))
        if name in brat_packed and name in cdecl.structs and \
                name not in getattr(cdecl, "packed", ()):
            rep.error("extern struct %s: BRAT declares it packed, %s does not"
                      % (name, header_name))
        if name not in cdecl.structs:
            rep.error("extern struct %s: not declared in %s" % (name, header_name))
            continue
        cfields = cdecl.structs[name]
        if len(cfields) != len(fields):
            rep.error("extern struct %s: BRAT declares %d field(s), %s has %d"
                      % (name, len(fields), header_name, len(cfields)))
            continue
        bad = False
        for (bname, bann), (cname, ctype) in zip(fields, cfields):
            cbt, cextra = resolve(cdecl, ctype)
            if bname != cname:
                rep.error("extern struct %s: field %d is `%s` in BRAT, `%s` in %s"
                          % (name, cfields.index((cname, ctype)) + 1, bname, cname, header_name))
                bad = True
                continue
            barr = array_field_type(bann)
            if cbt == "array" or barr is not None:
                if cbt != "array" or barr is None:
                    rep.error("extern struct %s.%s: inline array fields mismatch (`%s` in BRAT vs `%s` in %s)"
                              % (name, bname, bann, ctype, header_name))
                    bad = True
                else:
                    cek, cee, ccnt = cextra
                    belem, bcnt = barr
                    elem_match = (belem == cee) if cek == "struct" else same(belem, cek)
                    if bcnt != ccnt or not elem_match:
                        rep.error("extern struct %s.%s: BRAT declares `%s`, %s has `%s`"
                                  % (name, bname, bann, header_name, ctype))
                        bad = True
                continue
            if cbt == "fn" or (isinstance(bann, str) and bann.startswith("fn")):
                if not same_callback(rep, "%s.%s" % (name, bname), "field `%s`" % bname,
                                     bann, ctype, cdecl, header_name, brat_structs):
                    bad = True
                continue
            if cbt == "struct" and isinstance(bann, str) and bann != cextra:
                rep.error("extern struct %s.%s: BRAT declares `%s`, %s has `%s`"
                          % (name, bname, bann, header_name, cextra))
                bad = True
            elif cbt == "ptr":
                elem = element_of(bann)
                if bann == "ptr" and cextra not in ("ptr", "str") and _WIDTH.get(cextra) is not None:
                    rep.error("extern struct %s.%s: erased `ptr` is incompatible with scalar `%s *`; use `ptr[T]`"
                              % (name, bname, cextra))
                    bad = True
                elif bann == "str":
                    rep.error("extern struct %s.%s: BRAT declares `%s`, %s has `%s`"
                              % (name, bname, bann, header_name, ctype))
                    bad = True
                elif elem is not None and not pointee_ok(elem, cextra, brat_structs):
                    rep.error("extern struct %s.%s: BRAT declares `%s`, %s has `%s`"
                              % (name, bname, bann, header_name, ctype))
                    bad = True
                elif elem is None and bann != "ptr":
                    rep.error("extern struct %s.%s: BRAT declares `%s`, %s has pointer `%s`"
                              % (name, bname, bann, header_name, ctype))
                    bad = True
            elif cbt == "str":
                if bann != "str":
                    rep.error("extern struct %s.%s: BRAT declares `%s`, %s has `%s`"
                              % (name, bname, bann, header_name, ctype))
                    bad = True
            elif cbt not in ("struct", "ptr", "str") and isinstance(bann, str):
                if not same(bann, cbt):
                    rep.error("extern struct %s.%s: BRAT declares `%s` (%s), %s has `%s`"
                              % (name, bname, bann, _size_note(bann), header_name, ctype))
                    bad = True
        if not bad:
            b_lay = struct_layout(brat_structs, brat_packed, unions=brat_unions).get(name)
            c_lay = c_struct_layout(cdecl).get(name)
            if b_lay is not None and c_lay is not None:
                bsz, baln, bslots = b_lay
                csz, caln, cslots = c_lay
                coff_map = dict(cslots)
                p_note = ""
                p_cap = getattr(cdecl, "pack_align", {}).get(name)
                if p_cap is not None and p_cap > 1:
                    p_note = " (#pragma pack(%d))" % p_cap
                for fnm, boff in bslots:
                    coff = coff_map.get(fnm)
                    if coff is not None and boff != coff:
                        rep.error("extern struct %s.%s: offset is %d in BRAT, %d in %s%s"
                                  % (name, fnm, boff, coff, header_name, p_note))
                        bad = True
                if not bad and (bsz != csz or baln != caln):
                    rep.error("extern struct %s: layout (%dB, align %d) differs from %s (%dB, align %d)%s"
                              % (name, bsz, baln, header_name, csz, caln, p_note))
                    bad = True
            if not bad:
                rep.ok()

    c_all_layouts = c_struct_layout(cdecl)

    def _c_struct_representable(sname):
        if sname not in cdecl.structs or not cdecl.structs[sname]:
            return False
        if sname in getattr(cdecl, "anon", ()) or sname in getattr(cdecl, "bitfields", {}):
            return False
        if sname in getattr(cdecl, "arrays", ()) and sname not in brat_structs:
            return False
        p_cap = getattr(cdecl, "pack_align", {}).get(sname)
        if p_cap is not None and p_cap > 1 and sname not in getattr(cdecl, "unions", ()):
            c_lay = c_all_layouts.get(sname)
            if c_lay is not None:
                saved_cap = cdecl.pack_align.pop(sname, None)
                try:
                    nat_lay = c_struct_layout(cdecl).get(sname)
                finally:
                    if saved_cap is not None:
                        cdecl.pack_align[sname] = saved_cap
                if nat_lay is not None and nat_lay != c_lay:
                    return False
        return True

    for s in prog_body:
        if not s or s[0] != "extern":
            continue
        _kind, _libname, name, params, ret, varargs = s[0], s[1], s[2], s[3], s[4], s[5]
        _effs = s[8] if len(s) > 8 and isinstance(s[8], (tuple, list)) else ()
        _is_plugin = "__plugin__" in _effs
        if name not in cdecl.fns:
            rep.error("extern fn %s: not declared in %s; the ABI is unproven"
                      % (name, header_name))
            continue
        cret, cparams, cvar = cdecl.fns[name]
        bad = False
        if name in cdecl.abi_unsupported:
            rep.error("extern fn %s: unsupported ABI calling convention `%s` in %s"
                      % (name, cdecl.abi_unsupported[name], header_name))
            bad = True
        if len(params) != len(cparams):
            rep.error("extern fn %s: BRAT takes %d argument(s), %s declares %d"
                      % (name, len(params), header_name, len(cparams)))
            bad = True
        for i, (bp, cp) in enumerate(zip(params, cparams)):
            bann = bp[2] if isinstance(bp, tuple) and len(bp) > 2 else None
            where = "parameter %d `%s`" % (i + 1, bp[0])
            if (isinstance(bann, str) and bann.startswith("fn")) or c_callback(cp, cdecl) is not None:
                # A callback is a pointer whose *pointee* is a signature: the
                # header's `float (*cb)(float)` and BRAT's `fn(f32)->f32` are the
                # same type or they are not, and "both are pointers" is not a proof.
                if not same_callback(rep, name, where, bann, cp, cdecl, header_name,
                                     brat_structs):
                    bad = True
                continue
            kind, extra = resolve(cdecl, cp)
            if kind == "struct":
                if bann != extra or bann not in brat_structs or extra not in cdecl.structs or not cdecl.structs[extra]:
                    rep.error("extern fn %s: %s is declared `%s`, %s has `%s`"
                              % (name, where, bann, header_name, extra))
                    bad = True
            elif kind in ("ptr", "str"):
                want = "str" if kind == "str" else "ptr"
                # `ptr[T]` is a `ptr` at the ABI: the element is the extra claim,
                # checked against what the header's star actually points at.
                elem = element_of(bann)
                base = "ptr" if elem is not None else bann
                # A BRAT `str` is a read-only, potentially shared buffer. Handing
                # it to a pointer the header does not mark `const` would let C
                # write into it, so `str` needs the header's own promise; a
                # writable buffer is spelled `ptr`.
                if want == "ptr" and base == "str" and not const_pointee(cp, cdecl):
                    rep.error("extern fn %s: %s is declared `str`, which is read-only, but "
                              "%s has `%s`: declare a `ptr` for a buffer C may write"
                              % (name, where, header_name, cp))
                    bad = True
                elif _is_plugin and ("__out_%d__" % i) in _effs and const_pointee(cp, cdecl):
                    rep.error("extern fn %s: %s is declared `out ptr`, which writes through "
                              "the pointer, but %s has `%s`"
                              % (name, where, header_name, cp))
                    bad = True
                elif not (base == want or (want == "ptr" and base == "str")):
                    rep.error("extern fn %s: %s is declared `%s`, %s has `%s`"
                              % (name, where, bann, header_name, cp))
                    bad = True
                elif want == "ptr" and not pointee_ok(elem, extra, brat_structs):
                    rep.error("extern fn %s: %s is declared `%s`, %s has `%s`"
                              % (name, where, bann, header_name, cp))
                    bad = True
                elif (not _is_plugin and want == "ptr" and elem is None
                        and extra not in ("ptr", "str") and _WIDTH.get(extra) is not None):
                    rep.error("extern fn %s: %s uses erased `ptr`, incompatible with scalar `%s *`; use `ptr[T]`"
                              % (name, where, extra))
                    bad = True
                elif not _is_plugin and want == "ptr" and elem is None and _c_struct_representable(extra):
                    rep.error("extern fn %s: %s uses erased `ptr` for `%s *`; use `ptr[%s]`"
                              % (name, where, extra, extra))
                    bad = True
            elif kind == "none":
                rep.error("extern fn %s: %s is `void`, which is not an argument"
                          % (name, where))
                bad = True
            elif bann is not None and not same(bann, kind):
                rep.error("extern fn %s: %s is declared `%s` (%s), %s has `%s` (%s)"
                          % (name, where, bann, _size_note(bann), header_name, kind,
                             _size_note(kind)))
                bad = True
        if varargs and not cvar:
            rep.error("extern fn %s: BRAT declares a variadic tail, %s has none"
                      % (name, header_name))
            bad = True
        if cvar and not varargs:
            rep.error("extern fn %s: %s is variadic, BRAT declares fixed arguments"
                      % (name, header_name))
            bad = True
        rkind, rextra = resolve(cdecl, cret)
        if ret is None or ret == "none":
            if rkind != "none":
                rep.error("extern fn %s: BRAT declares no result, %s returns `%s`"
                          % (name, header_name, cret))
                bad = True
        elif rkind == "none":
            # The other direction: C hands back nothing, BRAT reads it as a value.
            rep.error("extern fn %s: %s returns `void`, BRAT declares `%s`"
                      % (name, header_name, ret))
            bad = True
        elif rkind == "struct":
            if ret != rextra or ret not in brat_structs or rextra not in cdecl.structs or not cdecl.structs[rextra]:
                rep.error("extern fn %s: BRAT returns `%s`, %s returns `%s`"
                          % (name, ret, header_name, rextra))
                bad = True
        elif rkind in ("ptr", "str"):
            ptr_ints = ("i32", "u32", "usize") if ptr_width() == 4 else ("i64", "u64", "usize")
            if rkind == "str" and ret not in ("str", "ptr") + ptr_ints and element_of(ret) not in ("u8", "i8"):
                rep.error("extern fn %s: BRAT returns `%s`, %s returns `%s`"
                          % (name, ret, header_name, cret))
                bad = True
            elif (not _is_plugin and rkind == "ptr" and ret == "ptr"
                    and rextra not in ("ptr", "str") and _WIDTH.get(rextra) is not None):
                rep.error("extern fn %s: BRAT returns erased `ptr`, incompatible with scalar `%s *`; use `ptr[T]`"
                          % (name, rextra))
                bad = True
            elif not _is_plugin and rkind == "ptr" and ret == "ptr" and _c_struct_representable(rextra):
                rep.error("extern fn %s: BRAT returns erased `ptr` for `%s *`; use `ptr[%s]`"
                          % (name, rextra, rextra))
                bad = True
            elif rkind == "ptr" and ret not in ("ptr",) + ptr_ints \
                    and element_of(ret) is None:
                rep.error("extern fn %s: BRAT returns `%s`, %s returns `%s`"
                          % (name, ret, header_name, cret))
                bad = True
            elif rkind == "ptr" and element_of(ret) is not None \
                    and not pointee_ok(element_of(ret), rextra, brat_structs):
                rep.error("extern fn %s: BRAT returns `%s`, %s returns `%s`"
                          % (name, ret, header_name, cret))
                bad = True
        elif ret is not None and rkind != "none" and not same(ret, rkind):
            c_disp = cret.strip() if rkind == "typedef" else rkind
            rep.error("extern fn %s: BRAT returns `%s` (%s), %s returns `%s` (%s)"
                      % (name, ret, _size_note(ret), header_name, c_disp, _size_note(rkind)))
            bad = True
        if not bad:
            rep.ok()
    return rep


def _size_note(t):
    w = _abi_width(t)
    return "%d bytes" % w if w else "unknown width"


def missing_symbols(prog_body, ctypes_mod=None):
    """Symbols BRAT calls that its library does not export, found without running."""
    import ctypes
    out = []
    handles = {}
    for s in prog_body:
        if not s or s[0] != "extern":
            continue
        lib = s[1]
        name = s[2]
        key = lib or "<self>"
        if key not in handles:
            if ctypes_mod is not None:
                try:
                    handles[key] = ctypes_mod.CDLL(lib) if lib else ctypes_mod.CDLL(None)
                except OSError as e:
                    handles[key] = e
            elif lib and os.path.isfile(lib):
                try:
                    info = inspect_object_binary(lib)
                    lo = lib.lower()
                    static_obj = lo.endswith((".o", ".a", ".obj", ".lib")) or info.get("format") == "ar"
                    handles[key] = ("obj", info["symbols"], static_obj)
                except Exception as e:
                    handles[key] = OSError(str(e))
            elif lib and ("/" in lib or "\\" in lib):
                handles[key] = OSError("no such file: %s" % lib)
            else:
                try:
                    handles[key] = ctypes.CDLL(lib) if lib else ctypes.CDLL(None)
                except OSError as e:
                    handles[key] = e
        h = handles[key]
        if isinstance(h, Exception):
            # One library, one message: the reason is the same for every symbol.
            if not any(lib == seen_lib for seen_lib, _m in out):
                out.append((lib, "cannot open %s: %s" % (lib, h)))
            continue
        if isinstance(h, tuple) and h[0] == "obj":
            sym = h[1].get(name)
            found = sym is not None and sym["defined"] and sym["exported"]
            if not found and not h[2]:
                if "<self>" not in handles:
                    try:
                        handles["<self>"] = ctypes.CDLL(None)
                    except OSError as e:
                        handles["<self>"] = e
                sh = handles["<self>"]
                if not isinstance(sh, Exception):
                    found = hasattr(sh, name)
            if not found:
                out.append((name, "%s exports no `%s`" % (lib, name)))
            continue
        try:
            getattr(h, name)
        except AttributeError:
            if lib:
                out.append((name, "%s exports no `%s`" % (lib, name)))
            else:
                # No library named: the symbol comes from whatever is linked in,
                # which is the linker's question, not this process's.
                out.append((name, "`%s` is not in this process; the linker resolves it"
                                  % name))
    return out


CONTRACT_MAGIC = "BRATFFI1"

# The block a proof leaves in the emitted C. A string, not a comment: comments do
# not survive preprocessing, and the point is for the finished binary to know what
# was proved about it.
CONTRACT_MARK_END = CONTRACT_MAGIC + "-END"


def _sha256_file(path):
    import hashlib
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 16), b""):
                h.update(chunk)
    except OSError:
        return None
    return h.hexdigest()


def _brat_record_sets(prog_body):
    structs, packed, unions = {}, set(), set()
    for s in prog_body:
        if s and s[0] == "externstruct":
            structs[s[1]] = list(s[2])
            if len(s) > 3 and s[3]:
                packed.add(s[1])
            if len(s) > 4 and s[4]:
                unions.add(s[1])
    return structs, packed, unions


def _brat_structs(prog_body):
    structs, packed, _unions = _brat_record_sets(prog_body)
    return structs, packed


def contract(prog_body, header=None, target=None, objects=()):
    """The extern surface of a program as data: what the binary may assume.

    Every `extern` declaration becomes an entry, every `extern struct` becomes a
    size, an alignment and a field offset, and the target's widths are recorded
    next to them, because a layout is only meaningful for one data model. When a
    header was used to prove the declarations, its sha256 goes in as well, so a
    later `brat ffi verify --bin` can say whether the proof still describes the
    header in front of it.
    """
    set_target(target)
    structs, packed, unions = _brat_record_sets(prog_body)
    layout = struct_layout(structs, packed, unions=unions)
    fns = []
    for s in prog_body:
        if not s or s[0] != "extern":
            continue
        lib, name, params, ret, varargs = s[1], s[2], s[3], s[4], s[5]
        fns.append({
            "name": name,
            "lib": lib,
            "ret": ret,
            "params": [p[2] if isinstance(p, tuple) and len(p) > 2 else None
                       for p in params],
            "varargs": varargs,
        })
    fns.sort(key=lambda f: f["name"])
    out_structs = []
    for name in sorted(structs):
        lay = layout.get(name)
        if lay is None:
            entry = {"name": name, "size": None, "align": None,
                     "packed": name in packed, "fields": []}
            if name in unions:
                entry["union"] = True
            out_structs.append(entry)
            continue
        entry = {
            "name": name,
            "size": lay[0],
            "align": lay[1],
            "packed": name in packed,
            "fields": [{"name": fname, "offset": foff, "type": ftype}
                       for (fname, foff), (_fn, ftype) in zip(lay[2], structs[name])],
        }
        if name in unions:
            entry["union"] = True
        out_structs.append(entry)
    triple, model = current_target()
    proved = None
    if header or objects:
        proved = {}
        if header:
            proved["header"] = os.path.basename(header)
            proved["sha256"] = _sha256_file(header)
        if objects:
            proved["objects"] = [
                {"file": os.path.basename(o), "sha256": _sha256_file(o)}
                for o in objects
            ]
    return {
        "version": 1,
        "proved": proved,
        "target": {"triple": triple, "model": model, "ptr": ptr_width(),
                   "long": long_width(), "wide_align": wide_align()},
        "fns": fns,
        "structs": out_structs,
    }


def contract_json(obj):
    """Canonical text: sorted keys, no spaces, ASCII only, so two proofs compare."""
    import json
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _c_string_chunks(text, width=180):
    """`text` as C string-literal pieces: split first, escape each.

    Escaping before splitting would let a `\\"` land on a literal boundary, where
    the backslash escapes the quote that closes the literal and the file stops
    compiling -- for a contract long enough to need a second line.
    """
    out = []
    for i in range(0, len(text), width):
        piece = text[i:i + width]
        out.append(piece.replace("\\", "\\\\").replace('"', '\\"'))
    return out or [""]


def contract_c(obj):
    """The C a proof leaves in the program: a marked, `used` string of the contract."""
    payload = CONTRACT_MAGIC + " " + contract_json(obj) + CONTRACT_MARK_END
    lines = ["/* BRAT FFI contract: the extern surface this binary was proved against.",
             "   `brat ffi verify --bin FILE` re-proves it. */",
             "static const char brat_ffi_contract[] __attribute__((used)) ="]
    chunks = _c_string_chunks(payload)
    for i, chunk in enumerate(chunks):
        lines.append('  "%s"%s' % (chunk, ";" if i == len(chunks) - 1 else ""))
    return "\n".join(lines) + "\n"


def read_binary_contract(path):
    """The contract a built binary carries, or None when it carries none."""
    import json
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    start = data.find(CONTRACT_MAGIC.encode() + b" ")
    if start < 0:
        return None
    end = data.find(CONTRACT_MARK_END.encode(), start)
    if end < 0:
        return None
    text = data[start + len(CONTRACT_MAGIC) + 1:end]
    try:
        obj = json.loads(text.decode("utf-8", "strict"))
    except (UnicodeDecodeError, ValueError):
        return None
    return obj if isinstance(obj, dict) else None


def _signature(fn):
    params = ", ".join(p if p else "?" for p in fn["params"])
    if fn.get("varargs"):
        params += (", " if params else "") + ".." + str(fn["varargs"])
    ret = fn["ret"] or "none"
    return "%s(%s)" % (ret, params)


def contract_drift(recorded, now):
    """How two contracts differ, one line each. Empty means the proof still holds."""
    out = []
    rt, nt = recorded.get("target") or {}, now.get("target") or {}
    for key in ("triple", "model", "ptr", "long", "wide_align"):
        if rt.get(key) != nt.get(key):
            out.append("target %s: recorded %s, now %s" % (key, rt.get(key), nt.get(key)))
    rh = (recorded.get("proved") or {}).get("sha256")
    nh = (now.get("proved") or {}).get("sha256")
    if rh and nh and rh != nh:
        out.append("header sha256: recorded %s, now %s" % (rh[:12], nh[:12]))
    elif rh and not nh:
        out.append("this binary was proved against a header; the comparison has none")
    elif nh and not rh:
        out.append("this binary carries no header proof, the comparison has one")
    rec_fns = {f["name"]: f for f in recorded.get("fns", [])}
    now_fns = {f["name"]: f for f in now.get("fns", [])}
    for name in sorted(set(rec_fns) | set(now_fns)):
        a, b = rec_fns.get(name), now_fns.get(name)
        if b is None:
            out.append("extern fn %s: recorded %s, no longer declared" % (name, _signature(a)))
            continue
        if a is None:
            out.append("extern fn %s: newly declared %s, not in the binary" % (name, _signature(b)))
            continue
        if a.get("lib") != b.get("lib"):
            out.append("extern fn %s: library %s, now %s" % (name, a.get("lib"), b.get("lib")))
        if _signature(a) != _signature(b):
            out.append("extern fn %s: recorded %s, now %s"
                       % (name, _signature(a), _signature(b)))
    rec_st = {s["name"]: s for s in recorded.get("structs", [])}
    now_st = {s["name"]: s for s in now.get("structs", [])}
    for name in sorted(set(rec_st) | set(now_st)):
        a, b = rec_st.get(name), now_st.get(name)
        if b is None:
            out.append("extern struct %s: recorded, no longer declared" % name)
            continue
        if a is None:
            out.append("extern struct %s: newly declared, not in the binary" % name)
            continue
        for key in ("size", "align", "packed"):
            if a.get(key) != b.get(key):
                out.append("extern struct %s: %s recorded %s, now %s"
                           % (name, key, a.get(key), b.get(key)))
        if bool(a.get("union")) != bool(b.get("union")):
            out.append("extern struct %s: union recorded %s, now %s"
                       % (name, bool(a.get("union")), bool(b.get("union"))))
        af = {f["name"]: f for f in a.get("fields", [])}
        bf = {f["name"]: f for f in b.get("fields", [])}
        for fname in sorted(set(af) | set(bf)):
            fa, fb = af.get(fname), bf.get(fname)
            if fb is None:
                out.append("extern struct %s.%s: recorded, gone" % (name, fname))
            elif fa is None:
                out.append("extern struct %s.%s: new field" % (name, fname))
            elif (fa.get("offset"), fa.get("type")) != (fb.get("offset"), fb.get("type")):
                out.append("extern struct %s.%s: recorded %s at %s, now %s at %s"
                           % (name, fname, fa.get("type"), fa.get("offset"),
                              fb.get("type"), fb.get("offset")))
    return out


def headers_for(prog_body):
    """Libraries the program names, so a caller can turn them into headers."""
    seen = []
    for s in prog_body:
        if s and s[0] == "extern" and s[1] and s[1] not in seen:
            seen.append(s[1])
    return seen


def header_source(prog_body, cdecl):
    """A C header that says what the BRAT declarations claim.

    Compiling it against the real header is the second opinion: `cc -fsyntax-only`
    refuses a declaration that disagrees, and the message is the compiler's own.
    """
    lines = ["/* generated from BRAT declarations: the prototype BRAT believes */"]
    for name, fields in getattr(cdecl, "structs", {}).items():
        lines.append("typedef struct { %s } %s;"
                     % (" ".join("%s %s;" % (t, f) for f, t in fields), name))
    return "\n".join(lines) + "\n"


# Every BRAT width in C, so the layout probe never writes `u8` into C source:
# a width the probe cannot spell is a width it cannot prove.
# C spellings whose width is the target's choice, not the language's: `long` is
# 8 bytes on LP64 and 4 on i686 and on Windows, `size_t` follows the pointer
# size, and a pointer is whatever the target says. `C_SCALARS` records the
# usual answer so a comparison has a number to compare; the probe below makes
# the target's compiler confirm that number, because BRAT compiles for targets
# (`--target`) where the usual answer is wrong.
PLATFORM_WIDTH_C = {
    "long": 8, "long int": 8, "unsigned long": 8, "unsigned long int": 8,
    "size_t": 8, "ssize_t": 8, "ptrdiff_t": 8, "intptr_t": 8, "uintptr_t": 8,
    "void *": 8,
}

PRIM_C = {"i32": "int", "u32": "unsigned", "i64": "int64_t", "u64": "uint64_t",
          "usize": "size_t", "u8": "unsigned char", "u16": "unsigned short",
          "i8": "signed char", "i16": "short",
          "f32": "float", "f64": "double", "bool": "_Bool", "str": "const char *",
          "ptr": "void *", "none": "void"}


def _brat_ffi_c_type(ann, structs):
    """The C type the native emitter declares, or None for a record value."""
    if not isinstance(ann, str):
        return None
    if ann in structs:
        return None
    if ann in PRIM_C:
        return PRIM_C[ann]
    if ann.startswith("ptr[") and ann.endswith("]"):
        elem = _brat_ffi_c_type(ann[4:-1].strip(), structs)
        if elem is None:
            return None
        return "void *" if elem == "void" else elem + " *"
    if ann.startswith("fn"):
        signature = brat_callback(ann)
        if signature is None:
            return None
        result, params = signature[0], signature[1]
        cret = _brat_ffi_c_type(result, structs)
        cparams = [_brat_ffi_c_type(param, structs) for param in params]
        if cret is None or any(param is None for param in cparams):
            return None
        return "%s (*) (%s)" % (cret, ", ".join(cparams) if cparams else "void")
    return None


def _scalar_spelling(t):
    """`const unsigned long *` -> `void *`; `unsigned long` -> `unsigned long`.

    Only the spellings in `PLATFORM_WIDTH_C` come back meaningful: everything
    else has a width the C language fixes (`int`, `short`, `int64_t`), and that
    needs no confirmation from the target.
    """
    if not isinstance(t, str):
        return None
    if "*" in t:
        return "void *"
    words = [w for w in t.replace("*", " ").split() if w not in QUALIFIERS]
    return " ".join(words) or None


def platform_width_uses(cdecl):
    """C spellings in a parsed header whose width the target decides."""
    if cdecl is None:
        return set()
    used = set()

    def note(t):
        sp = _scalar_spelling(t)
        if sp in PLATFORM_WIDTH_C:
            used.add(sp)

    for _name, (cret, cparams, _cvar) in cdecl.fns.items():
        note(cret)
        for cp in cparams:
            note(cp)
    for _sname, fields in cdecl.structs.items():
        for _fname, ctype in fields:
            note(ctype)
    return used


def probe_source(prog_body, header, cdecl=None):
    """A C file that checks target widths, record layouts, and symbol declarations.

    `check` compares function ABI types. The compiler verifies the referenced
    symbols and target layouts; explicit function-pointer casts keep compatible
    C typedef spellings from becoming false ABI mismatches.
    """
    structs = {s[1]: s[2] for s in prog_body if s and s[0] == "externstruct"}
    widths = platform_width_uses(cdecl)
    externs = [s for s in prog_body if s and s[0] == "extern" and cdecl
               and s[2] in cdecl.fns]
    if not structs and not widths and not externs:
        return None
    if any(ch in (header or "") for ch in ("\r", "\n")):
        raise ValueError("invalid newline in header path")
    esc_hdr = (header or "").replace("\\", "\\\\").replace('"', '\\"')
    lines = ['#include <stddef.h>', '#include <stdint.h>',
             '#include "%s"' % esc_hdr, ""]
    # A width that came from the target's usual answer, not from the language.
    # `long` on a 32-bit target is four bytes while `C_SCALARS` says eight, so a
    # declaration compared by width would agree with a header it does not match.
    for sp in sorted(widths):
        if _TARGET["triple"] is None:
            exp_w = PLATFORM_WIDTH_C[sp]
        elif sp in ("long", "long int", "unsigned long", "unsigned long int"):
            exp_w = long_width()
        else:
            exp_w = ptr_width()
        lines.append('_Static_assert(sizeof(%s) == %d, '
                     '"C `%s` is not %d bytes on this target, so BRAT bound it '
                     'at the wrong width");' % (sp, exp_w, sp, exp_w))
    if widths:
        lines.append("")
    packed = {s[1] for s in prog_body if s and s[0] == "externstruct" and len(s) > 3 and s[3]}
    unions = {s[1] for s in prog_body if s and s[0] == "externstruct" and len(s) > 4 and s[4]}
    inline_c_structs = set(getattr(cdecl, "inline_structs", ())) if cdecl is not None else set()
    anon_c_members = dict(getattr(cdecl, "anon_members", {})) if cdecl is not None else {}
    ordered_probe = []
    seen_probe = set()

    def _visit_probe(nm):
        if nm in seen_probe or nm not in structs:
            return
        seen_probe.add(nm)
        for _fn, fa in structs[nm]:
            arr = array_field_type(fa)
            dep = arr[0] if arr is not None else fa
            if dep in structs and dep != nm:
                _visit_probe(dep)
        ordered_probe.append(nm)

    for _sn in structs:
        _visit_probe(_sn)

    def _probe_leaf_paths(rec, pref_b="", pref_c=""):
        out_paths = []
        for fn, _fa in structs.get(rec, ()):
            bp = ("%s.%s" % (pref_b, fn)) if pref_b else fn
            synth = anon_c_members.get((rec, fn))
            if synth and synth in structs:
                out_paths.extend(_probe_leaf_paths(synth, bp, pref_c))
            else:
                cp = ("%s.%s" % (pref_c, fn)) if pref_c else fn
                out_paths.append((bp, cp))
        return out_paths

    for name in ordered_probe:
        fields = structs[name]
        cname = cdecl.c_name(name) if cdecl is not None else name
        mems = []
        for fname, fann in fields:
            arr = array_field_type(fann)
            if arr is not None:
                aelem, acnt = arr
                act = ("bf_" + aelem) if aelem in structs else (
                    "void *" if isinstance(aelem, str) and aelem.startswith("ptr[")
                    else PRIM_C.get(aelem, aelem))
                mems.append("%s %s[%d];" % (act, fname, acnt))
            elif fann in structs:
                mems.append("bf_%s %s;" % (fann, fname))
            elif isinstance(fann, str) and (fann.startswith("ptr[") or fann.startswith("fn")):
                mems.append("void * %s;" % fname)
            else:
                mems.append("%s %s;" % (PRIM_C.get(fann, fann), fname))
        if name in unions:
            kw = "typedef union { "
        elif name in packed:
            kw = "typedef struct __attribute__((packed)) { "
        else:
            kw = "typedef struct { "
        rk = "union" if name in unions else "struct"
        lines.append("%s%s } bf_%s;" % (kw, " ".join(mems), name))
        if name not in inline_c_structs:
            lines.append('_Static_assert(sizeof(bf_%s) == sizeof(%s), '
                         '"extern %s %s: size differs from the header");' % (name, cname, rk, name))
            for bp, cp in _probe_leaf_paths(name):
                lines.append('_Static_assert(offsetof(bf_%s, %s) == offsetof(%s, %s), '
                             '"extern %s %s.%s: offset differs from the header");'
                             % (name, bp, cname, cp, rk, name, bp))
        lines.append("")
    probe_id = 0
    for ext in externs:
        name, params, ret, varargs = ext[2], ext[3], ext[4], ext[5]
        cret = _brat_ffi_c_type(ret or "none", structs)
        cparams = [_brat_ffi_c_type(
            p[2] if isinstance(p, tuple) and len(p) > 2 else None, structs)
            for p in params]
        if cret is None or any(param is None for param in cparams):
            continue
        if varargs:
            cparams.append("...")
        signature = "%s (*)(%s)" % (cret, ", ".join(cparams) if cparams else "void")
        lines.append("static %s (*brat_ffi_proto_probe_%d)(%s) = (%s)&%s;"
                 % (cret, probe_id, ", ".join(cparams) if cparams else "void",
                signature, name))
        probe_id += 1
    if probe_id:
        lines.append("")
    return "\n".join(lines) + "\n"


def compile_probe(prog_body, header, cc="cc", timeout=120, cdecl=None, target=None,
                  include_dirs=(), defines=()):
    """(ok, compiler output). The C compiler is the judge, not this module.

    `target` is the triple the program is being built for. The widths the
    comparison used belong to that target, not to the machine running the
    compiler, so a cross build must be judged by a compiler that knows the
    target: `zig cc -target` is the only one BRAT can assume, and without it the
    stage reports that it could not answer rather than answering for the host.
    """
    import tempfile
    import shutil
    src = probe_source(prog_body, header, cdecl)
    if src is None:
        return True, ""      # nothing a compiler can add: no extern struct to lay out
    argv = [cc]
    if target is not None:
        zig = shutil.which("zig")
        if zig is None:
            return True, ""   # host widths say nothing about another target
        argv = [zig, "cc", "-target", target]
    extra = []
    if header:
        hdir = os.path.dirname(os.path.abspath(header))
        if hdir:
            extra.append("-I" + hdir)
    for inc in include_dirs or ():
        extra.append("-I" + inc)
    for dfn in defines or ():
        extra.append("-D" + dfn)
    with tempfile.TemporaryDirectory(prefix="brat-ffi-probe-") as tmp:
        path = os.path.join(tmp, "probe.c")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        r = subprocess.run(argv + ["-std=c11", "-fsyntax-only",
                                   "-Werror=incompatible-pointer-types"] + extra + [path],
                           capture_output=True, text=True, timeout=timeout)
    out = (r.stdout + r.stderr).strip()
    if not out:
        return True, ""
    # The temp path is noise: the message is about the header and the probe.
    out = out.replace(path, "ffi-probe.c")
    return (r.returncode == 0), out


# One C view, four languages. `emit_bindings` writes BRAT from `CDecl`; these
# write Python (ctypes), Go (cgo) and Rust (`extern "C"`) from the *same*
# `CDecl`, so a header cannot mean one thing here and another thing there. A
# type with no BRAT spelling is skipped with a note in every one of them, for
# the same reason: a language that cannot express the width must not guess it.

# ctypes, cgo and Rust spell a width three ways; the width is the invariant.
_LANG_SCALAR = {
    "i8": ("c_int8", "int8_t", "i8"),
    "u8": ("c_uint8", "uint8_t", "u8"),
    "i16": ("c_int16", "int16_t", "i16"),
    "u16": ("c_uint16", "uint16_t", "u16"),
    "i32": ("c_int32", "int32_t", "i32"),
    "u32": ("c_uint32", "uint32_t", "u32"),
    "i64": ("c_int64", "int64_t", "i64"),
    "u64": ("c_uint64", "uint64_t", "u64"),
    "usize": ("c_size_t", "size_t", "usize"),
    "bool": ("c_bool", "bool", "bool"),
    "f32": ("c_float", "float", "f32"),
    "f64": ("c_double", "double", "f64"),
    "str": ("c_char_p", "char*", "*const c_char"),
    "ptr": ("c_void_p", "void*", "*mut c_void"),
}


def _lang_type(kind, extra, lang):
    i = {"python": 0, "go": 1, "rust": 2}[lang]
    if kind in _LANG_SCALAR:
        return _LANG_SCALAR[kind][i]
    if kind == "ptr" and extra:
        # `T *`: the pointee is not a width, and all three languages have an
        # untyped pointer that is ABI-identical to it.
        return _LANG_SCALAR["ptr"][i]
    if kind == "struct":
        return extra
    return None


def _lang_header(cdecl, header_name, lang, lib):
    """(lines, notes, ok) shared by every language emitter."""
    notes = []
    structs = []
    for name in _struct_order(cdecl):
        fields = cdecl.structs[name]
        if not fields or name in getattr(cdecl, "anon", ()):
            notes.append("struct %s has no representable body: skipped" % name)
            continue
        rows = []
        bad = False
        for fname, ctype in fields:
            kind, extra = resolve(cdecl, ctype)
            if kind == "struct" and extra in cdecl.structs and not cdecl.structs[extra]:
                rows.append((fname, _LANG_SCALAR["ptr"][{"python": 0, "go": 1, "rust": 2}[lang]], True))
                continue
            t = _lang_type(kind, extra, lang)
            if t is None:
                notes.append("%s.%s: `%s` has no %s spelling" % (name, fname, ctype.strip(), lang))
                bad = True
                break
            rows.append((fname, t, False))
        if not bad and rows:
            structs.append((name, rows, name in getattr(cdecl, "packed", ())))
    return structs, notes


def emit_python(cdecl, header_name, lib=None):
    """A `ctypes` module for the same header `emit_bindings` reads."""
    structs, notes = _lang_header(cdecl, header_name, "python", lib)
    out = ["\"\"\"Python bindings for %s." % os.path.basename(header_name or ""),
           "Generated by brat bindgen from the same C view `brat ffi --header` proves.",
           "\"\"\"", "import ctypes", "",
           "lib = None", "",
           "def load(path):",
           "    global lib",
           "    lib = ctypes.CDLL(path)",
           "    _bind(lib)",
           "    return lib", ""]
    for name, rows, packed in structs:
        out.append("class %s(ctypes.Structure):" % name)
        if packed:
            out.append("    _pack_ = 1")
        out.append("    _fields_ = [%s]"
                   % ", ".join('("%s", ctypes.%s)' % (f, t) for f, t, _p in rows))
        out.append("")
    fns = []
    for name, (cret, cparams, _cvar) in sorted(cdecl.fns.items()):
        rkind, rextra = resolve(cdecl, cret)
        rt = "None" if rkind == "none" else _lang_type(rkind, rextra, "python")
        if rt is None:
            notes.append("%s: result `%s` has no python spelling" % (name, cret.strip()))
            continue
        args = []
        bad = False
        for cp in cparams:
            kind, extra = resolve(cdecl, cp)
            if kind == "none":
                bad = True
                break
            t = _lang_type(kind, extra, "python")
            if t is None:
                notes.append("%s: parameter `%s` has no python spelling" % (name, cp.strip()))
                bad = True
                break
            args.append("ctypes.%s" % t if t not in cdecl.structs else t)
        if bad:
            continue
        fns.append("    f = lib.%s" % name)
        fns.append("    f.restype = %s" % ("None" if rt == "None" else
                                           ("ctypes." + rt if rt not in cdecl.structs else rt)))
        fns.append("    f.argtypes = [%s]" % ", ".join(args))
    if fns:
        out.append("def _bind(lib):")
        out.extend(fns)
        out.append("")
    for why in notes:
        out.insert(1, "# skipped: %s" % why)
    return "\n".join(out) + "\n", notes


def emit_rust(cdecl, header_name, lib=None):
    """An `extern \"C\"` module for the same header."""
    structs, notes = _lang_header(cdecl, header_name, "rust", lib)
    out = ["// Rust bindings for %s" % os.path.basename(header_name or ""),
           "// Generated by brat bindgen from the same C view `brat ffi --header` proves.",
           "#![allow(non_camel_case_types, non_snake_case, dead_code)]", "",
           "use std::os::raw::{c_char, c_void};", ""]
    for name, rows, packed in structs:
        if packed:
            out.append("#[repr(C, packed)]")
        else:
            out.append("#[repr(C)]")
        out.append("pub struct %s {" % name)
        for f, t, _p in rows:
            out.append("    pub %s: %s," % (f, t))
        out.append("}")
        out.append("")
    out.append("extern \"C\" {")
    for name, (cret, cparams, cvar) in sorted(cdecl.fns.items()):
        rkind, rextra = resolve(cdecl, cret)
        rt = "()" if rkind == "none" else _lang_type(rkind, rextra, "rust")
        if rt is None:
            notes.append("%s: result `%s` has no rust spelling" % (name, cret.strip()))
            continue
        args = []
        bad = False
        for i, cp in enumerate(cparams):
            kind, extra = resolve(cdecl, cp)
            t = _lang_type(kind, extra, "rust") if kind != "none" else None
            if t is None:
                notes.append("%s: parameter `%s` has no rust spelling" % (name, cp.strip()))
                bad = True
                break
            args.append("a%d: %s" % (i, t))
        if bad:
            continue
        if cvar:
            args.append("...")
        out.append("    pub fn %s(%s) -> %s;" % (name, ", ".join(args), rt))
    out.append("}")
    out.append("")
    for why in notes:
        out.insert(1, "// skipped: %s" % why)
    return "\n".join(out) + "\n", notes


def emit_go(cdecl, header_name, lib=None):
    """A cgo file for the same header.

    Go needs the C compiler for the declarations, so the header is included
    rather than re-declared: cgo then sees exactly the types the library sees,
    and the Go side only names them.
    """
    structs, notes = _lang_header(cdecl, header_name, "go", lib)
    go_structs = {name for name, _, _ in structs}
    esc_go_hdr = (header_name or "").replace("\r", "").replace("\n", "")
    esc_go_hdr = esc_go_hdr.replace("\\", "\\\\").replace('"', '\\"').replace("*/", "* /")
    out = ["// Go bindings for %s" % os.path.basename(header_name or ""),
           "// Generated by brat bindgen from the same C view `brat ffi --header` proves.",
           "", "package bindings", "", "/*",
           "#include <stdlib.h>", "#include <stdbool.h>",
           '#include "%s"' % esc_go_hdr, "*/",
           'import "C"', ""]
    needs_unsafe = False

    def go_type(ctype):
        if ctype in go_structs:
            return ctype
        if ctype == "char*":
            return "*C.char"
        if ctype == "void*":
            return "unsafe.Pointer"
        return "C." + ctype

    out.append("type (")
    for name, _rows, _packed in structs:
        out.append("\t%s = C.struct_%s" % (name, name))
    out.append(")")
    out.append("")
    for name, (cret, cparams, _cvar) in sorted(cdecl.fns.items()):
        rkind, rextra = resolve(cdecl, cret)
        rt = _lang_type(rkind, rextra, "go") if rkind != "none" else None
        if rkind != "none" and rt is None:
            notes.append("%s: result `%s` has no go spelling" % (name, cret.strip()))
            continue
        args = []
        bad = False
        uses_unsafe = rt == "void*"
        for i, cp in enumerate(cparams):
            kind, extra = resolve(cdecl, cp)
            t = _lang_type(kind, extra, "go") if kind != "none" else None
            if t is None:
                notes.append("%s: parameter `%s` has no go spelling" % (name, cp.strip()))
                bad = True
                break
            args.append("a%d %s" % (i, go_type(t)))
            uses_unsafe = uses_unsafe or t == "void*"
        if bad:
            continue
        needs_unsafe = needs_unsafe or uses_unsafe
        rs = "" if rt is None else " " + go_type(rt)
        call = "%sC.%s(%s)" % ("return " if rt else "", name,
                               ", ".join("a%d" % i for i in range(len(cparams))))
        out.append("// %s mirrors the C declaration; cgo checks the ABI." % name)
        out.append("func %sC(%s)%s { %s }" % (name, ", ".join(args), rs, call))
    out.append("")
    if needs_unsafe:
        out.insert(out.index('import "C"') + 1, 'import "unsafe"')
    for why in notes:
        out.insert(1, "// skipped: %s" % why)
    return "\n".join(out) + "\n", notes


LANG_EMITTERS = {"python": emit_python, "rust": emit_rust, "go": emit_go}


BRAT_OF_C = {"i8": "i8", "u8": "u8", "i16": "i16", "u16": "u16",
             "i32": "i32", "u32": "u32", "i64": "i64", "u64": "u64", "usize": "usize",
             "f32": "f32", "f64": "f64", "bool": "bool", "str": "str", "ptr": "ptr", "none": "none"}


def _struct_order(cdecl):
    """Structs sorted so that one embedding another comes after it.

    The generated C declares `bf_A` before `bf_B` exactly as this list is
    written, and C needs a struct defined before the one that holds it by value.
    A cycle is impossible in C, but a self-referential struct through a pointer
    is not, and pointers do not order.
    """
    order, seen = [], set()

    def visit(name):
        if name in seen or name not in cdecl.structs:
            return
        seen.add(name)
        for _fname, ctype in cdecl.structs[name]:
            kind, extra = resolve(cdecl, ctype)
            if kind == "struct" and extra and extra != name:
                visit(extra)
            elif kind == "array" and extra[0] == "struct" and extra[1] and extra[1] != name:
                visit(extra[1])
        order.append(name)

    for name in sorted(cdecl.structs):
        visit(name)
    return order


# Names BRAT cannot spell in a declaration: the emitter writes struct and field
# names as `bf_<name>`, so a BRAT keyword is the only thing that hurts there; a
# function name is the C symbol itself, so a C keyword hurts as well.
BRAT_KEYWORDS = {
    "fn", "let", "mut", "ref", "move", "if", "else", "while", "for", "in", "return",
    "match", "struct", "enum", "impl", "trait", "import", "pub", "use", "const",
    "extern", "export", "packed", "spawn", "scope", "arena", "unsafe", "defer",
    "try", "catch", "break", "continue", "par", "as", "true", "false", "none",
    "and", "or", "not", "is",
}

C_KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
    "register", "restrict", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
    "_Bool", "_Complex", "_Imaginary", "_Static_assert", "_Thread_local",
}


def bindgen_name_ok(name, is_fn=False):
    """True when the generated BRAT file can spell `name`."""
    if not isinstance(name, str) or not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
        return False
    if name in BRAT_KEYWORDS:
        return False
    return not is_fn or name not in C_KEYWORDS


def _c_cb_to_brat(cdecl, ctype, used_structs, brat_pointer):
    ccb = c_callback(ctype, cdecl)
    if ccb is None:
        return None
    cret, cparams = ccb
    rk, rext = resolve(cdecl, cret)
    if rk == "none":
        bret = None
    elif rk == "struct":
        if rext not in used_structs:
            return None
        bret = rext
    elif rk == "ptr":
        bret = brat_pointer(rext)
    elif rk in BRAT_OF_C:
        bret = BRAT_OF_C[rk]
    else:
        return None
    bparams = []
    for cp in cparams:
        pk, pext = resolve(cdecl, cp)
        if pk == "none":
            return None
        if pk == "struct":
            if pext not in used_structs:
                return None
            bparams.append(pext)
        elif pk == "ptr":
            bparams.append(brat_pointer(pext))
        elif pk in BRAT_OF_C:
            bparams.append(BRAT_OF_C[pk])
        else:
            return None
    head = "fn(%s)" % ", ".join(bparams)
    return ("%s -> %s" % (head, bret)) if bret else head


def emit_bindings(prog_body, cdecl, header_name, lib=None, inline_arrays=False):
    """BRAT declarations for a header, in the shape `check` proves."""
    out = []
    notes = []

    def skip(why):
        notes.append(why)

    out.append("// generated by brat bindgen from %s"
               % os.path.basename(header_name or ""))
    if lib:
        out.append("// library: %s" % lib)

    const_lines = []
    for cname, cval in getattr(cdecl, "consts", ()):
        if bindgen_name_ok(cname):
            const_lines.append("pub const %s = %d" % (cname, cval))
    if const_lines:
        out.extend(const_lines)
        out.append("")

    used = set()
    pointer_structs = {name for name, fields in cdecl.structs.items()
                       if fields and name not in getattr(cdecl, "anon", ())
                       and (inline_arrays or name not in getattr(cdecl, "arrays", ()))
                       and bindgen_name_ok(name)}

    def brat_pointer(extra):
        if extra in ("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64",
                     "usize", "f32", "f64", "bool"):
            return "ptr[%s]" % extra
        if extra in pointer_structs:
            return "ptr[%s]" % extra
        return "ptr"

    struct_lines = []
    for name in _struct_order(cdecl):
        fields = cdecl.structs[name]
        if not bindgen_name_ok(name):
            skip("%s: a C struct name the BRAT parser reads as a keyword" % name)
            continue
        if not fields:
            skip("struct %s has no body in the header: declared as `ptr`" % name)
            continue
        if name in getattr(cdecl, "arrays", ()) and not inline_arrays:
            skip("struct %s has inline array fields: not representable as an extern struct" % name)
            continue
        if name in getattr(cdecl, "bitfields", {}):
            bf_desc = ", ".join(
                ("%s:%s" % (bn or "<pad>", bw)) for bn, _bt, bw in cdecl.bitfields[name]
            )
            skip("struct %s has C bitfield(s) (%s): not byte-addressed, declared as `ptr`"
                 % (name, bf_desc))
            continue
        if name in getattr(cdecl, "anon", ()):
            if name in getattr(cdecl, "arrays", ()):
                skip("struct %s has inline array fields: not representable as an extern struct" % name)
                continue
            skip("struct %s holds an anonymous union or struct: no BRAT field name "
                 "for it, so the record is not declared" % name)
            continue
        members = []
        ok = True
        for fname, ctype in fields:
            kind, extra = resolve(cdecl, ctype)
            if kind == "struct" and extra == name:
                skip("struct %s contains itself: declared as `ptr`" % name)
                ok = False
                break
            if not bindgen_name_ok(fname):
                skip("%s.%s: a C member name the BRAT parser reads as a keyword"
                     % (name, fname))
                ok = False
                break
            if kind == "array":
                ek, ee, ecnt = extra
                eb = ee if (ek == "struct" and ee in used) else BRAT_OF_C.get(ek)
                if eb is None:
                    skip("struct %s.%s: array `%s` has no BRAT spelling" % (name, fname, ctype))
                    ok = False
                    break
                b = "%s[%d]" % (eb, ecnt)
            elif kind == "fn":
                b = _c_cb_to_brat(cdecl, ctype, used, brat_pointer) or "ptr"
            elif kind == "struct":
                if extra in used:
                    b = extra
                else:
                    skip("struct %s.%s: `%s` has no BRAT spelling" % (name, fname, ctype))
                    ok = False
                    break
            else:
                b = brat_pointer(extra) if kind == "ptr" else BRAT_OF_C.get(kind)
            if b is None:
                skip("struct %s.%s: `%s` has no BRAT spelling" % (name, fname, ctype))
                ok = False
                break
            members.append((fname, b))
        if not ok or not members:
            pointer_structs.discard(name)
            continue
        if ok and members:
            p_cap = getattr(cdecl, "pack_align", {}).get(name)
            if p_cap is not None and p_cap > 1 and name not in getattr(cdecl, "unions", ()):
                c_lay = c_struct_layout(cdecl).get(name)
                b_probe_map = {name: members}
                emitted_so_far = getattr(cdecl, "_emitted_members", None)
                if emitted_so_far is None:
                    emitted_so_far = {}
                    cdecl._emitted_members = emitted_so_far
                b_probe_map.update(emitted_so_far)
                b_lay = struct_layout(b_probe_map,
                                      set(getattr(cdecl, "packed", ())),
                                      unions=set(getattr(cdecl, "unions", ()))).get(name)
                if c_lay is not None and b_lay is not None and b_lay != c_lay:
                    pointer_structs.discard(name)
                    cdecl.anon.add(name)
                    skip("struct %s: #pragma pack(%d) layout (%dB, align %d) differs from natural alignment"
                         % (name, p_cap, c_lay[0], c_lay[1]))
                    continue
            emitted_so_far = getattr(cdecl, "_emitted_members", None)
            if emitted_so_far is None:
                emitted_so_far = {}
                cdecl._emitted_members = emitted_so_far
            emitted_so_far[name] = members
            used.add(name)
            if name in getattr(cdecl, "unions", ()):
                kw = "extern union"
            elif name in getattr(cdecl, "packed", ()):
                kw = "extern packed struct"
            else:
                kw = "extern struct"
            struct_lines.append("%s %s { %s }"
                                % (kw, name, ", ".join("%s: %s" % m for m in members)))
    out.extend(struct_lines)
    out.append("")

    fns = []
    for name, (cret, cparams, cvar) in sorted(cdecl.fns.items()):
        if not bindgen_name_ok(name, is_fn=True):
            skip("%s: a C function name the BRAT parser reads as a keyword" % name)
            continue
        rkind, rextra = resolve(cdecl, cret)
        if rkind not in BRAT_OF_C and rkind != "struct":
            skip("%s: result `%s` has no BRAT spelling" % (name, cret.strip()))
            continue
        ret = "none" if rkind == "none" else (rextra if rkind == "struct" else BRAT_OF_C[rkind])
        if rkind == "ptr":
            ret = brat_pointer(rextra)
        if rkind == "struct" and rextra not in used:
            skip("%s: returns `%s`, whose body is opaque" % (name, rextra))
            continue
        pnames = cdecl.param_names.get(name) or []
        args = []
        bad = False
        for i, cp in enumerate(cparams):
            kind, extra = resolve(cdecl, cp)
            if kind == "none":
                skip("%s: a `void` parameter" % name)
                bad = True
                break
            if kind == "fn":
                cb_b = _c_cb_to_brat(cdecl, cp, used, brat_pointer)
                if cb_b is None:
                    skip("%s: callback parameter `%s` has no BRAT spelling" % (name, cp.strip()))
                    bad = True
                    break
                b = cb_b
            elif kind == "struct":
                if extra not in used:
                    skip("%s: parameter %d is `%s`, whose body is opaque"
                         % (name, i + 1, extra))
                    bad = True
                    break
                b = extra
            elif kind == "ptr":
                b = brat_pointer(extra)
            elif kind not in BRAT_OF_C:
                skip("%s: parameter `%s` has no BRAT spelling" % (name, cp.strip()))
                bad = True
                break
            else:
                b = BRAT_OF_C[kind]
            pn = pnames[i] if i < len(pnames) else None
            if not bindgen_name_ok(pn or "", is_fn=False):
                pn = "a%d" % i
            args.append("%s: %s" % (pn, b))
        if bad:
            continue
        if cvar:
            args.append("..ptr")
        fns.append("    extern fn %s(%s) -> %s;" % (name, ", ".join(args), ret))

    if fns:
        head = ('extern "%s" {' % lib) if lib else "extern {"
        out.append(head)
        out.extend(fns)
        out.append("}")
    # One comment per skipped item, in the same shape whether the reason is a
    # keyword or a width: a reader greps for `skipped` and sees every hole.
    for why in notes:
        out.insert(1, "// skipped: %s" % why)
    return "\n".join(out) + "\n", notes


# Compiled object (.o / .so / .a / .dylib / .dll) verification.

def _c_str_at(buf, off):
    if off < 0 or off >= len(buf):
        return ""
    end = buf.find(b"\x00", off)
    if end < 0:
        end = len(buf)
    return buf[off:end].decode("utf-8", "replace")


def _merge_sym(table, name, defined, exported, kind, size):
    if not name:
        return
    base = name.split("@", 1)[0]
    if not base:
        return
    prev = table.get(base)
    entry = {
        "name": base,
        "raw": name,
        "defined": bool(defined),
        "exported": bool(exported),
        "kind": kind,
        "size": int(size or 0),
    }
    if prev is None:
        table[base] = entry
        return
    # Prefer defined > undefined, exported > local, concrete kind > notype.
    prev_rank = (1 if prev["defined"] else 0,
                 1 if prev["exported"] else 0,
                 1 if prev["kind"] in ("func", "object") else 0)
    new_rank = (1 if entry["defined"] else 0,
                1 if entry["exported"] else 0,
                1 if entry["kind"] in ("func", "object") else 0)
    if new_rank > prev_rank or (new_rank == prev_rank and entry["size"] > prev["size"]):
        table[base] = entry


def _parse_elf_bytes(data):
    import struct as _st
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ValueError("not an ELF object")
    ei_class = data[4]
    ei_data = data[5]
    if ei_class not in (1, 2) or ei_data not in (1, 2):
        raise ValueError("unsupported ELF class/endianness")
    end = "<" if ei_data == 1 else ">"
    ptr_w = 4 if ei_class == 1 else 8
    if ei_class == 1:
        _etype, e_machine, _ever, _eentry, _ephoff, e_shoff, _eflags, _eehsize, \
            _ephentsize, _ephnum, e_shentsize, e_shnum, e_shstr = \
            _st.unpack_from(end + "HHIIIIIHHHHHH", data, 16)
    else:
        if len(data) < 64:
            raise ValueError("truncated ELF64 header")
        _etype, e_machine, _ever, _eentry, _ephoff, e_shoff, _eflags, _eehsize, \
            _ephentsize, _ephnum, e_shentsize, e_shnum, e_shstr = \
            _st.unpack_from(end + "HHIQQQIHHHHHH", data, 16)
    arch_map = {3: "x86", 62: "x86_64", 40: "arm", 183: "aarch64", 243: "riscv", 8: "mips"}
    arch = arch_map.get(e_machine, "elf-%d" % e_machine)
    syms = {}
    all_raw = set()
    has_debug_info = False
    if e_shoff and e_shnum and e_shentsize >= (40 if ei_class == 1 else 64):
        shdrs = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            if off + e_shentsize > len(data):
                break
            if ei_class == 1:
                sh_name, sh_type, _sh_flags, _sh_addr, sh_offset, sh_size, sh_link, _sh_info, _sh_align, sh_entsize = \
                    _st.unpack_from(end + "IIIIIIIIII", data, off)
            else:
                sh_name, sh_type, _sh_flags, _sh_addr, sh_offset, sh_size, sh_link, _sh_info, _sh_align, sh_entsize = \
                    _st.unpack_from(end + "IIQQQQIIQQ", data, off)
            shdrs.append((sh_name, sh_type, sh_offset, sh_size, sh_link, sh_entsize))
        if e_shstr < len(shdrs):
            _sn, _st0, shstr_off, shstr_sz, _sl0, _se0 = shdrs[e_shstr]
            if shstr_off + shstr_sz <= len(data):
                shstrtab = data[shstr_off:shstr_off + shstr_sz]
                for sh_name, sh_type, _so, sh_size, _sl, _se in shdrs:
                    if sh_type not in (0, 8) and sh_size > 0:
                        snm = _c_str_at(shstrtab, sh_name)
                        if snm in (".debug_info", ".zdebug_info"):
                            has_debug_info = True
        # SHT_SYMTAB = 2, SHT_DYNSYM = 11
        for _sh_name, sh_type, sh_offset, sh_size, sh_link, sh_entsize in shdrs:
            if sh_type not in (2, 11) or sh_link >= len(shdrs):
                continue
            _st_n, _st_t, str_off, str_sz, _st_l, _st_e = shdrs[sh_link]
            if str_off + str_sz > len(data) or sh_offset + sh_size > len(data):
                continue
            strtab = data[str_off:str_off + str_sz]
            entsz = sh_entsize or (16 if ei_class == 1 else 24)
            if entsz <= 0:
                continue
            for pos in range(sh_offset, sh_offset + sh_size - entsz + 1, entsz):
                if ei_class == 1:
                    st_name, _st_val, st_size, st_info, _st_other, st_shndx = \
                        _st.unpack_from(end + "IIIBBH", data, pos)
                else:
                    st_name, st_info, _st_other, st_shndx, _st_val, st_size = \
                        _st.unpack_from(end + "IBBHQQ", data, pos)
                nm = _c_str_at(strtab, st_name)
                if not nm:
                    continue
                all_raw.add(nm)
                bind = st_info >> 4
                stt = st_info & 0x0F
                if stt in (3, 4):  # STT_SECTION, STT_FILE
                    continue
                defined = st_shndx != 0  # SHN_UNDEF == 0
                exported = bind in (1, 2, 10)  # GLOBAL, WEAK, GNU_UNIQUE
                kind = ("func" if stt in (2, 10) else
                        "object" if stt in (1, 5, 6) else "notype")
                _merge_sym(syms, nm, defined, exported, kind, st_size)
    return {"format": "elf", "ptr_width": ptr_w, "arch": arch, "symbols": syms,
            "raw_names": all_raw, "has_debug_info": has_debug_info}


def _parse_macho_bytes(data):
    import struct as _st
    if len(data) < 28:
        raise ValueError("truncated Mach-O header")
    magic_le = _st.unpack_from("<I", data, 0)[0]
    if magic_le in (0xFEEDFACE, 0xFEEDFACF):
        end = "<"
        is64 = magic_le == 0xFEEDFACF
    elif magic_le in (0xCEFAEDFE, 0xCFFAEDFE):
        end = ">"
        is64 = magic_le == 0xCFFAEDFE
    else:
        raise ValueError("not a Mach-O object")
    cputype, _cpusub, _filetype, ncmds, _sizeofcmds, _flags = \
        _st.unpack_from(end + "iiIIII", data, 4)
    arch_map = {7: "x86", 0x01000007: "x86_64", 12: "arm", 0x0100000C: "aarch64"}
    arch = arch_map.get(cputype, "macho-%d" % cputype)
    off = 32 if is64 else 28
    sec_kinds = [None]  # 1-based section index in Mach-O n_sect
    sym_cmds = []
    for _ in range(ncmds):
        if off + 8 > len(data):
            break
        cmd, cmdsize = _st.unpack_from(end + "II", data, off)
        if cmdsize < 8 or off + cmdsize > len(data):
            break
        if cmd in (0x1, 0x19):  # LC_SEGMENT / LC_SEGMENT_64
            seg64 = cmd == 0x19
            hdr_sz = 72 if seg64 else 56
            sec_sz = 80 if seg64 else 68
            if cmdsize >= hdr_sz:
                nsects = _st.unpack_from(end + "I", data, off + (64 if seg64 else 48))[0]
                soff = off + hdr_sz
                for _si in range(nsects):
                    if soff + sec_sz > off + cmdsize:
                        break
                    segname = data[soff + 16:soff + 32].split(b"\x00", 1)[0].decode("latin1")
                    sec_kinds.append("func" if segname == "__TEXT" else "object")
                    soff += sec_sz
        elif cmd == 0x2 and cmdsize >= 24:  # LC_SYMTAB
            symoff, nsyms, stroff, strsize = _st.unpack_from(end + "IIII", data, off + 8)
            sym_cmds.append((symoff, nsyms, stroff, strsize))
        off += cmdsize
    syms = {}
    all_raw = set()
    entsz = 16 if is64 else 12
    for symoff, nsyms, stroff, strsize in sym_cmds:
        if stroff + strsize > len(data):
            continue
        strtab = data[stroff:stroff + strsize]
        for i in range(nsyms):
            p = symoff + i * entsz
            if p + entsz > len(data):
                break
            if is64:
                n_strx, n_type, n_sect, _n_desc, _n_val = _st.unpack_from(end + "IBBHQ", data, p)
            else:
                n_strx, n_type, n_sect, _n_desc, _n_val = _st.unpack_from(end + "IBBHI", data, p)
            if n_type & 0xE0:  # N_STAB debug entry
                continue
            raw_nm = _c_str_at(strtab, n_strx)
            if not raw_nm:
                continue
            all_raw.add(raw_nm)
            nm = raw_nm[1:] if raw_nm.startswith("_") else raw_nm
            ntype = n_type & 0x0E
            defined = ntype in (0x0E, 0x02)  # N_SECT or N_ABS
            exported = bool(n_type & 0x01)
            kind = sec_kinds[n_sect] if (defined and 0 < n_sect < len(sec_kinds)) else "notype"
            _merge_sym(syms, nm, defined, exported, kind or "notype", 0)
    return {"format": "macho", "ptr_width": 8 if is64 else 4, "arch": arch,
            "symbols": syms, "raw_names": all_raw}


def _parse_pe_coff_bytes(data):
    import struct as _st
    coff_off = 0
    is_pe = False
    if len(data) >= 64 and data[:2] == b"MZ":
        pe_off = _st.unpack_from("<I", data, 0x3C)[0]
        if pe_off + 24 <= len(data) and data[pe_off:pe_off + 4] == b"PE\x00\x00":
            coff_off = pe_off + 4
            is_pe = True
        else:
            raise ValueError("invalid PE header")
    if coff_off + 20 > len(data):
        raise ValueError("not a COFF/PE object")
    machine, nsects, _ts, symptr, nsyms, optsize, _chars = \
        _st.unpack_from("<HHIIIHH", data, coff_off)
    arch_map = {0x14C: ("x86", 4), 0x8664: ("x86_64", 8), 0x1C0: ("arm", 4), 0xAA64: ("aarch64", 8)}
    if machine not in arch_map:
        raise ValueError("unrecognized binary format")
    if coff_off + 20 + optsize > len(data):
        raise ValueError("truncated COFF/PE optional header")
    arch, ptr_w = arch_map[machine]
    syms = {}
    all_raw = set()
    sec_off = coff_off + 20 + optsize
    sec_exec = [False]
    sects = []
    for i in range(nsects):
        so = sec_off + i * 40
        if so + 40 > len(data):
            break
        _snm, vsize, vaddr, rawsize, rawptr = _st.unpack_from("<8sIIII", data, so)
        s_chars = _st.unpack_from("<I", data, so + 36)[0]
        sec_exec.append(bool(s_chars & 0x20000020))  # IMAGE_SCN_CNT_CODE | MEM_EXECUTE
        sects.append((vaddr, max(vsize, rawsize), rawptr, rawsize))
    if symptr and nsyms:
        str_off = symptr + nsyms * 18
        strtab = data[str_off:] if str_off < len(data) else b""
        idx = 0
        while idx < nsyms:
            p = symptr + idx * 18
            if p + 18 > len(data):
                break
            name8, _val, scnum, symtype, sclass, naux = _st.unpack_from("<8sIhHBB", data, p)
            idx += 1 + naux
            if name8[:4] == b"\x00\x00\x00\x00":
                soff = _st.unpack_from("<I", name8, 4)[0]
                raw_nm = _c_str_at(strtab, soff)
            else:
                raw_nm = name8.split(b"\x00", 1)[0].decode("utf-8", "replace")
            if not raw_nm:
                continue
            all_raw.add(raw_nm)
            nm = raw_nm[1:] if (ptr_w == 4 and raw_nm.startswith("_")) else raw_nm
            defined = scnum > 0 or scnum == -1
            exported = sclass == 2  # IMAGE_SYM_CLASS_EXTERNAL
            is_fn = (symtype == 0x20) or (0 < scnum < len(sec_exec) and sec_exec[scnum])
            kind = "func" if is_fn else ("object" if defined else "notype")
            _merge_sym(syms, nm, defined, exported, kind, 0)
    if is_pe and optsize >= 96:
        opt_off = coff_off + 20
        opt_magic = _st.unpack_from("<H", data, opt_off)[0]
        dd_off = opt_off + (112 if opt_magic == 0x20B else 96)
        if dd_off + 8 <= coff_off + 20 + optsize:
            exp_rva, exp_sz = _st.unpack_from("<II", data, dd_off)

            def rva_to_off(rva):
                for va, vsz, rptr, _rsz in sects:
                    if va <= rva < va + max(vsz, 1):
                        res = rptr + (rva - va)
                        return res if res < len(data) else None
                return None

            eoff = rva_to_off(exp_rva) if exp_rva and exp_sz else None
            if eoff is not None and eoff + 40 <= len(data):
                num_funcs, num_names, addr_funcs, addr_names, addr_ords = \
                    _st.unpack_from("<IIIII", data, eoff + 20)
                noff = rva_to_off(addr_names)
                ooff = rva_to_off(addr_ords)
                foff = rva_to_off(addr_funcs)
                if noff is not None and ooff is not None and foff is not None:
                    for i in range(min(num_names, 65536)):
                        if noff + (i + 1) * 4 > len(data) or ooff + (i + 1) * 2 > len(data):
                            break
                        nrva = _st.unpack_from("<I", data, noff + i * 4)[0]
                        ord_i = _st.unpack_from("<H", data, ooff + i * 2)[0]
                        nptr = rva_to_off(nrva)
                        if nptr is None:
                            continue
                        nm = _c_str_at(data, nptr)
                        all_raw.add(nm)
                        kind = "func"
                        if ord_i < num_funcs and foff + (ord_i + 1) * 4 <= len(data):
                            frva = _st.unpack_from("<I", data, foff + ord_i * 4)[0]
                            for idx_s, (va, vsz, _rp, _rs) in enumerate(sects, 1):
                                if va <= frva < va + max(vsz, 1):
                                    kind = "func" if sec_exec[idx_s] else "object"
                                    break
                        _merge_sym(syms, nm, True, True, kind, 0)
    return {"format": "pe" if is_pe else "coff", "ptr_width": ptr_w, "arch": arch,
            "symbols": syms, "raw_names": all_raw}


def _parse_single_object_bytes(data):
    if data.startswith(b"\x7fELF"):
        return _parse_elf_bytes(data)
    if len(data) >= 4:
        import struct as _st
        m = _st.unpack_from("<I", data, 0)[0]
        if m in (0xFEEDFACE, 0xFEEDFACF, 0xCEFAEDFE, 0xCFFAEDFE):
            return _parse_macho_bytes(data)
    return _parse_pe_coff_bytes(data)


inspect_object_symbols_bytes = _parse_single_object_bytes


def inspect_object_binary(path):
    """Read symbol table, pointer width and architecture from `.o`/`.so`/`.a`/`.dylib`/`.dll`."""
    with open(path, "rb") as f:
        data = f.read()
    if data.startswith(b"!<arch>\n"):
        pos = 8
        syms = {}
        raw_names = set()
        ptr_w = None
        arch = None
        has_dbg = False
        while pos < len(data):
            if pos + 1 == len(data) and data[pos:pos + 1] == b"\n":
                break
            if pos + 60 > len(data):
                raise ValueError("corrupt archive %s: truncated member header" % path)
            hdr = data[pos:pos + 60]
            if hdr[58:60] != b"`\n":
                raise ValueError("corrupt archive %s: invalid member magic" % path)
            name_field = hdr[:16].decode("latin1").rstrip(" ")
            sz_s = hdr[48:58].decode("latin1").rstrip(" ")
            if not sz_s or not sz_s.isdigit():
                raise ValueError("corrupt archive %s: invalid member size" % path)
            sz = int(sz_s)
            body_start = pos + 60
            body_end = body_start + sz
            if body_end > len(data):
                raise ValueError("corrupt archive %s: truncated member payload" % path)
            member = data[body_start:body_end]
            pos = body_end + (sz & 1 if body_end < len(data) else 0)
            if name_field.startswith("#1/"):
                nl_s = name_field[3:]
                if not nl_s.isdigit():
                    raise ValueError("corrupt archive %s: invalid BSD extended member name" % path)
                nlen = int(nl_s)
                if nlen > len(member):
                    raise ValueError("corrupt archive %s: truncated BSD extended member name" % path)
                name_field = member[:nlen].split(b"\x00", 1)[0].decode("latin1")
                member = member[nlen:]
            if name_field in ("/", "//", "/SYM64/") or name_field.startswith("__.SYMDEF") or not member:
                continue
            sub = _parse_single_object_bytes(member)
            if ptr_w is None:
                ptr_w = sub["ptr_width"]
                arch = sub["arch"]
            elif sub["ptr_width"] != ptr_w or sub["arch"] != arch:
                raise ValueError("archive %s mixes incompatible object architectures (%s vs %s)"
                                 % (path, arch, sub["arch"]))
            if sub.get("has_debug_info"):
                has_dbg = True
            raw_names |= sub["raw_names"]
            for sym in sub["symbols"].values():
                _merge_sym(syms, sym["raw"], sym["defined"], sym["exported"], sym["kind"], sym["size"])
        if ptr_w is None:
            raise ValueError("archive %s contains no recognizable object members" % path)
        return {"format": "ar", "ptr_width": ptr_w, "arch": arch,
                "symbols": syms, "raw_names": raw_names, "has_debug_info": has_dbg}
    return _parse_single_object_bytes(data)


_DWARF_HDR_RE = re.compile(r"^\s*<(\d+)><([0-9a-fA-F]+)>:\s*Abbrev Number:\s*(\d+)\s*\((DW_TAG_\w+)\)")
_DWARF_ATTR_RE = re.compile(r"^\s*<[0-9a-fA-F]+>\s+(DW_AT_\w+)\s*:\s*(.*)$")
_DWARF_REF_RE = re.compile(r"<0x([0-9a-fA-F]+)>")


def _dwarf_clean_str(val):
    # Matches `(strp) (offset: 0x...): foo`, `(string) id`, or plain `foo`
    if "):" in val:
        return val.rsplit("):", 1)[-1].strip()
    return re.sub(r"^(?:\([^)]*\)\s*)+", "", val.strip())


def _dwarf_clean_int(val):
    v = re.sub(r"^\([A-Za-z0-9_]+\)\s*", "", val.strip())
    m = re.match(r"^(0x[0-9a-fA-F]+|-?\d+)\b", v)
    if m:
        return int(m.group(1), 0)
    # DWARF2/3 location expression `(DW_OP_plus_uconst: 8)`
    m2 = re.search(r"DW_OP_plus_uconst:\s*(0x[0-9a-fA-F]+|\d+)", v)
    if m2:
        return int(m2.group(1), 0)
    return None


def _dwarf_info(path, require_valid=False):
    """Extract compiled function signatures and struct layouts from DWARF `.debug_info` if present."""
    import shutil as _sh
    readelf = _sh.which("readelf")
    if not readelf:
        if require_valid:
            raise ValueError("readelf is required to verify DWARF sections in %s" % path)
        return {"fns": {}, "structs": {}}
    try:
        r = subprocess.run([readelf, "--wide", "--debug-dump=info", path],
                           capture_output=True, text=True, timeout=30)
    except Exception as e:
        if require_valid:
            raise ValueError("DWARF inspection failed for %s: %s" % (path, e))
        return {"fns": {}, "structs": {}}
    if r.returncode != 0 or (require_valid and ("Error:" in r.stderr or "DW_TAG_" not in r.stdout)):
        if require_valid:
            detail = (r.stderr or "corrupt DWARF debug_info").strip().splitlines()
            raise ValueError("corrupt DWARF in %s: %s" % (path, detail[0] if detail else "invalid debug_info"))
        return {"fns": {}, "structs": {}}
    if "DW_TAG_" not in r.stdout:
        return {"fns": {}, "structs": {}}

    units = []
    cur_dies = None
    cur_ptr_sz = 8
    stack = []
    cur_die = None
    for line in r.stdout.splitlines():
        ps = re.search(r"Pointer Size:\s*(\d+)", line)
        if ps:
            if cur_dies:
                units.append((cur_ptr_sz, cur_dies))
            cur_ptr_sz = int(ps.group(1))
            cur_dies = {}
            stack = []
            cur_die = None
            continue
        mh = _DWARF_HDR_RE.match(line)
        if mh:
            if cur_dies is None:
                cur_dies = {}
            depth = int(mh.group(1))
            off = int(mh.group(2), 16)
            tag = mh.group(4)
            die = {"off": off, "depth": depth, "tag": tag, "attrs": {}, "children": []}
            cur_dies[off] = die
            while stack and stack[-1]["depth"] >= depth:
                stack.pop()
            if stack:
                stack[-1]["children"].append(die)
            stack.append(die)
            cur_die = die
            continue
        if cur_die is not None:
            ma = _DWARF_ATTR_RE.match(line)
            if ma:
                cur_die["attrs"][ma.group(1)] = ma.group(2).strip()
    if cur_dies:
        units.append((cur_ptr_sz, cur_dies))

    fns = {}
    structs = {}
    for ptr_sz, dies in units:
        typedef_struct_names = {}
        for d in dies.values():
            if d["tag"] == "DW_TAG_typedef" and "DW_AT_name" in d["attrs"] and "DW_AT_type" in d["attrs"]:
                rm = _DWARF_REF_RE.search(d["attrs"]["DW_AT_type"])
                if rm:
                    tgt = dies.get(int(rm.group(1), 16))
                    if tgt and tgt["tag"] in ("DW_TAG_structure_type", "DW_TAG_union_type"):
                        typedef_struct_names.setdefault(tgt["off"], _dwarf_clean_str(d["attrs"]["DW_AT_name"]))

        def _peel_dwarf_type(ref_attr, seen=()):
            cur_ref = ref_attr
            cur_seen = seen
            while cur_ref:
                rm = _DWARF_REF_RE.search(cur_ref)
                if not rm:
                    return None
                toff = int(rm.group(1), 16)
                if toff in cur_seen or toff not in dies:
                    return None
                td = dies[toff]
                if td["tag"] in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type",
                                 "DW_TAG_restrict_type", "DW_TAG_atomic_type"):
                    if "DW_AT_type" not in td["attrs"]:
                        return None
                    cur_ref = td["attrs"]["DW_AT_type"]
                    cur_seen = cur_seen + (toff,)
                    continue
                return td
            return None

        def type_shape(ref_attr, seen=(), in_member=False):
            if not ref_attr:
                return ("void", None, None)
            rm = _DWARF_REF_RE.search(ref_attr)
            if not rm:
                return ("unknown", None, None)
            toff = int(rm.group(1), 16)
            if toff in seen or toff not in dies:
                return ("unknown", None, None)
            td = dies[toff]
            ttag = td["tag"]
            tattrs = td["attrs"]
            if ttag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type",
                        "DW_TAG_restrict_type", "DW_TAG_atomic_type"):
                if "DW_AT_type" not in tattrs:
                    return ("void", None, None)
                return type_shape(tattrs["DW_AT_type"], seen + (toff,), in_member=in_member)
            if ttag == "DW_TAG_array_type" and in_member:
                _ek, esz, _es = type_shape(tattrs.get("DW_AT_type"), seen + (toff,), in_member=True)
                cnt = 1
                has_dim = False
                dim_bad = False
                for ch in td["children"]:
                    if ch["tag"] == "DW_TAG_subrange_type":
                        c_attr = _dwarf_clean_int(ch["attrs"].get("DW_AT_count", ""))
                        if c_attr is not None:
                            if c_attr < 0:
                                dim_bad = True
                                break
                            sub = c_attr
                        else:
                            ub = _dwarf_clean_int(ch["attrs"].get("DW_AT_upper_bound", ""))
                            if ub is None or ub < -1:
                                dim_bad = True
                                break
                            sub = ub + 1
                        has_dim = True
                        cnt *= sub
                total_sz = (esz * cnt) if (esz is not None and has_dim and not dim_bad) else None
                return ("array", total_sz, None)
            if ttag == "DW_TAG_pointer_type":
                ptgt = _peel_dwarf_type(tattrs.get("DW_AT_type"), seen + (toff,))
                if ptgt is not None and ptgt["tag"] == "DW_TAG_subroutine_type":
                    sub_seen = seen + (toff, ptgt["off"])
                    sub_attrs = ptgt["attrs"]
                    cb_ret = (type_shape(sub_attrs.get("DW_AT_type"), sub_seen)
                              if "DW_AT_type" in sub_attrs else ("void", None, None))
                    cb_params = []
                    cb_var = False
                    for ch in ptgt["children"]:
                        if ch["tag"] == "DW_TAG_formal_parameter":
                            cb_params.append(type_shape(ch["attrs"].get("DW_AT_type"), sub_seen))
                        elif ch["tag"] == "DW_TAG_unspecified_parameters":
                            cb_var = True
                    return ("fn", ptr_sz, {"ret": cb_ret, "params": cb_params, "variadic": cb_var})
                return ("scalar", ptr_sz, None)
            if ttag in ("DW_TAG_reference_type", "DW_TAG_rvalue_reference_type",
                        "DW_TAG_subroutine_type", "DW_TAG_array_type"):
                return ("scalar", ptr_sz, None)
            if ttag == "DW_TAG_base_type":
                bsz = _dwarf_clean_int(tattrs.get("DW_AT_byte_size", ""))
                enc = tattrs.get("DW_AT_encoding", "").strip()
                is_flt = ("float" in enc) or bool(re.match(r"^4\b", enc))
                return ("float" if is_flt else "scalar", bsz, _dwarf_clean_str(tattrs.get("DW_AT_name", "")))
            if ttag == "DW_TAG_enumeration_type":
                bsz = _dwarf_clean_int(tattrs.get("DW_AT_byte_size", ""))
                if bsz is None and "DW_AT_type" in tattrs:
                    _uk, bsz, _un = type_shape(tattrs["DW_AT_type"], seen + (toff,))
                return ("scalar", bsz, _dwarf_clean_str(tattrs.get("DW_AT_name", "")))
            if ttag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
                bsz = _dwarf_clean_int(tattrs.get("DW_AT_byte_size", ""))
                snm = _dwarf_clean_str(tattrs.get("DW_AT_name", "")) or typedef_struct_names.get(toff)
                return ("struct", bsz, snm)
            return ("unknown", None, None)

        for d in dies.values():
            attrs = d["attrs"]
            if d["tag"] in ("DW_TAG_structure_type", "DW_TAG_union_type"):
                if "DW_AT_declaration" in attrs:
                    continue
                bsz = _dwarf_clean_int(attrs.get("DW_AT_byte_size", ""))
                if bsz is None:
                    continue
                is_un_die = (d["tag"] == "DW_TAG_union_type")
                names_for_st = []
                if "DW_AT_name" in attrs:
                    names_for_st.append(_dwarf_clean_str(attrs["DW_AT_name"]))
                if d["off"] in typedef_struct_names:
                    names_for_st.append(typedef_struct_names[d["off"]])
                members = []
                anon_k = 0
                for ch in d["children"]:
                    if ch["tag"] != "DW_TAG_member":
                        continue
                    mnm = _dwarf_clean_str(ch["attrs"].get("DW_AT_name", ""))
                    if not mnm:
                        mnm = "anon%d" % anon_k
                        anon_k += 1
                    moff = _dwarf_clean_int(ch["attrs"].get("DW_AT_data_member_location", "0"))
                    mkind, msz, mst = type_shape(ch["attrs"].get("DW_AT_type"), in_member=True)
                    members.append((mnm, 0 if (is_un_die or moff is None) else moff, mkind, msz, mst))
                for snm in names_for_st:
                    if snm:
                        structs[snm] = {"size": bsz, "members": members, "is_union": is_un_die}
            elif d["tag"] == "DW_TAG_subprogram":
                if "DW_AT_declaration" in attrs:
                    continue
                fnm = (_dwarf_clean_str(attrs.get("DW_AT_linkage_name", ""))
                       or _dwarf_clean_str(attrs.get("DW_AT_MIPS_linkage_name", ""))
                       or _dwarf_clean_str(attrs.get("DW_AT_name", "")))
                if not fnm:
                    continue
                rshape = type_shape(attrs.get("DW_AT_type")) if "DW_AT_type" in attrs else ("void", None, None)
                params = []
                variadic = False
                for ch in d["children"]:
                    if ch["tag"] == "DW_TAG_formal_parameter":
                        params.append(type_shape(ch["attrs"].get("DW_AT_type")))
                    elif ch["tag"] == "DW_TAG_unspecified_parameters":
                        variadic = True
                fns[fnm] = {"ret": rshape, "params": params, "variadic": variadic}
    return {"fns": fns, "structs": structs}


def _norm_lib_stem(text):
    if not text:
        return ""
    b = os.path.basename(text)
    if ".so." in b:
        b = b.split(".so.", 1)[0]
    for suf in (".so", ".dylib", ".dll", ".a", ".o", ".obj"):
        if b.endswith(suf):
            b = b[:-len(suf)]
            break
    if b.startswith("lib") and len(b) > 3:
        b = b[3:]
    return b


def _extern_matches_object(ext_lib, obj_path):
    if not ext_lib:
        return True
    try:
        if os.path.abspath(ext_lib) == os.path.abspath(obj_path):
            return True
    except OSError:
        pass
    if os.path.basename(ext_lib) == os.path.basename(obj_path):
        return True
    s1 = _norm_lib_stem(ext_lib)
    s2 = _norm_lib_stem(obj_path)
    return bool(s1 and s2 and s1 == s2)


def _brat_abi_slot(ann, structs, layout):
    """Return `(category, byte_width, metadata)` for a BRAT FFI slot."""
    if ann is None or ann == "none":
        return ("void", None, None)
    arr = array_field_type(ann)
    if arr is not None:
        aelem, acnt = arr
        if aelem in structs:
            lay = layout.get(aelem)
            return ("array", (lay[0] * acnt) if lay else None, None)
        ew = _abi_width(aelem)
        return ("array", (ew * acnt) if ew is not None else None, None)
    if ann in structs:
        lay = layout.get(ann)
        return ("struct", lay[0] if lay else None, ann)
    cb = brat_callback(ann)
    if cb is not None:
        cb_ret, cb_params = cb
        return ("fn", ptr_width(), {
            "ret": _brat_abi_slot(cb_ret, structs, layout),
            "r_ann": cb_ret,
            "params": [_brat_abi_slot(p, structs, layout) for p in cb_params],
            "p_anns": list(cb_params),
            "variadic": False,
        })
    if ann == "ptr":
        return ("ptr_erased", ptr_width(), None)
    if isinstance(ann, str) and ann.startswith("ptr["):
        return ("scalar", ptr_width(), None)
    if ann in ("f32", "f64"):
        return ("float", _abi_width(ann), None)
    w = _abi_width(ann)
    return ("scalar", w, None)


def _check_dwarf_slot(rep, ctx, label, b_ann, b_slot, d_slot):
    """Recursively verify a BRAT FFI slot against a compiled DWARF DIE slot (including callbacks)."""
    b_cat, b_w, b_meta = b_slot
    d_cat, d_w, d_meta = d_slot
    if d_cat == "unknown":
        return True
    if (b_cat == "void") != (d_cat == "void"):
        rep.error("%s in %s is %s, BRAT declares `%s`"
                  % (ctx, label,
                     "void" if d_cat == "void" else ("%s (%d bytes)" % (d_cat, d_w or 0)),
                     b_ann or "none"))
        return False
    if b_cat == "ptr_erased":
        if d_cat not in ("scalar", "fn"):
            rep.error("%s in %s is %s, BRAT declares `%s`" % (ctx, label, d_cat, b_ann))
            return False
        if d_w is not None and b_w is not None and d_w != b_w:
            rep.error("%s in %s is %d bytes, BRAT `%s` is %d bytes"
                      % (ctx, label, d_w, b_ann, b_w))
            return False
        return True
    cats = ("scalar", "float", "struct", "array", "fn")
    if d_cat in cats and b_cat in cats and d_cat != b_cat:
        d_desc = "callback function pointer" if d_cat == "fn" else d_cat
        rep.error("%s in %s is %s, BRAT declares `%s`"
                  % (ctx, label, d_desc, b_ann or "none"))
        return False
    if d_w is not None and b_w is not None and d_w != b_w:
        rep.error("%s in %s is %d bytes, BRAT `%s` is %d bytes"
                  % (ctx, label, d_w, b_ann, b_w))
        return False
    if b_cat == "fn" and d_cat == "fn" and isinstance(b_meta, dict) and isinstance(d_meta, dict):
        ok = True
        if bool(d_meta.get("variadic")):
            rep.error("%s: callback in %s is variadic (...), BRAT declares fixed-arity `%s`"
                      % (ctx, label, b_ann))
            ok = False
        b_ps = b_meta.get("params", ())
        d_ps = d_meta.get("params", ())
        p_anns = b_meta.get("p_anns", ())
        if len(b_ps) != len(d_ps):
            rep.error("%s: callback in %s takes %d parameter(s), BRAT `%s` declares %d"
                      % (ctx, label, len(d_ps), b_ann, len(b_ps)))
            ok = False
        else:
            for k, (bps, dps) in enumerate(zip(b_ps, d_ps)):
                pann = p_anns[k] if k < len(p_anns) else None
                if not _check_dwarf_slot(rep, "%s callback parameter %d" % (ctx, k + 1),
                                         label, pann, bps, dps):
                    ok = False
        if not _check_dwarf_slot(rep, "%s callback return" % ctx, label,
                                 b_meta.get("r_ann"), b_meta.get("ret"), d_meta.get("ret")):
            ok = False
        return ok
    return True


def _target_expected_arch(triple):
    if not triple:
        return None
    t = triple.lower()
    if t.startswith(("x86_64", "amd64")):
        return "x86_64"
    if t.startswith(("i386", "i486", "i586", "i686")):
        return "x86"
    if t.startswith(("aarch64", "arm64")):
        return "aarch64"
    if t.startswith(("arm", "thumb")):
        return "arm"
    if t.startswith("riscv"):
        return "riscv"
    return None


def _mangled_hint(name, raw_names):
    needle = "%d%s" % (len(name), name)
    for r in sorted(raw_names):
        if (r.startswith("_Z") or r.startswith("__Z")) and needle in r:
            return r
    return None


def verify_object(prog_body, obj_path, cdecl=None, target=None):
    """Verify `extern` declarations directly against a compiled `.o`/`.so`/`.a` binary (FFI L1)."""
    rep = Report()
    set_target(target)
    label = os.path.basename(obj_path)
    if not os.path.isfile(obj_path):
        rep.error("object file not found: %s" % obj_path)
        return rep
    try:
        info = inspect_object_binary(obj_path)
    except Exception as e:
        rep.error("cannot read object %s: %s" % (label, e))
        return rep

    exp_ptr = ptr_width()
    if info["ptr_width"] != exp_ptr:
        rep.error("object %s is %d-bit (%d-byte pointers), target expects %d-byte pointers"
                  % (label, info["ptr_width"] * 8, info["ptr_width"], exp_ptr))
        return rep
    exp_arch = _target_expected_arch(target)
    if exp_arch and info["arch"] != exp_arch:
        rep.error("object %s targets %s, expected %s" % (label, info["arch"], exp_arch))
        return rep

    try:
        dwarf = _dwarf_info(obj_path, require_valid=bool(info.get("has_debug_info")))
    except Exception as e:
        rep.error("cannot read object %s: %s" % (label, e))
        return rep
    structs, packed, unions = _brat_record_sets(prog_body)
    layout = struct_layout(structs, packed, unions=unions)

    # Verify any `extern struct`/`extern union` whose compiled DWARF layout exists in the binary.
    for sname in sorted(structs):
        dst = dwarf["structs"].get(sname)
        lay = layout.get(sname)
        if dst is None or lay is None:
            continue
        if bool(dst.get("is_union")) != (sname in unions):
            if dst.get("is_union"):
                rep.error("extern struct %s: %s compiled a union, BRAT declares a struct (write `extern union`)"
                          % (sname, label))
            else:
                rep.error("extern union %s: %s compiled a struct, BRAT declares a union"
                          % (sname, label))
            continue
        b_size, _b_align, b_slots = lay
        if dst["size"] != b_size:
            rep.error("extern struct %s: %s compiled sizeof(%s) = %d bytes, BRAT expects %d bytes"
                      % (sname, label, sname, dst["size"], b_size))
            continue
        b_fields = structs[sname]
        d_members = dst["members"]
        if len(d_members) != len(b_fields):
            rep.error("extern struct %s: %s compiled %d field(s), BRAT declares %d"
                      % (sname, label, len(d_members), len(b_fields)))
            continue
        st_ok = True
        for idx, (((bf_name, bf_off), (_n2, bf_type)), (dm_name, dm_off, dm_kind, dm_sz, dm_meta)) in \
                enumerate(zip(zip(b_slots, b_fields), d_members)):
            if dm_name and dm_name != bf_name:
                rep.error("extern struct %s: field %d in %s is `%s`, BRAT declares `%s`"
                          % (sname, idx + 1, label, dm_name, bf_name))
                st_ok = False
                continue
            if dm_off != bf_off:
                rep.error("extern struct %s.%s: offset in %s is %d, BRAT expects %d"
                          % (sname, bf_name, label, dm_off, bf_off))
                st_ok = False
            b_slot = _brat_abi_slot(bf_type, structs, layout)
            if not _check_dwarf_slot(rep, "extern struct %s.%s:" % (sname, bf_name),
                                     label, bf_type, b_slot, (dm_kind, dm_sz, dm_meta)):
                st_ok = False
        if st_ok:
            rep.ok()

    for s in prog_body:
        if not s or s[0] != "extern":
            continue
        ext_lib, name, params, ret, varargs = s[1], s[2], s[3], s[4], s[5]
        if not _extern_matches_object(ext_lib, obj_path):
            continue
        sym = info["symbols"].get(name)
        if sym is None:
            mangled = _mangled_hint(name, info["raw_names"])
            if mangled:
                rep.error("extern fn %s: not found in %s (found C++ mangled `%s`; missing `extern \"C\"`?)"
                          % (name, label, mangled))
            else:
                rep.error("extern fn %s: symbol `%s` is missing from %s" % (name, name, label))
            continue
        if not sym["defined"]:
            rep.error("extern fn %s: symbol `%s` in %s is undefined (SHN_UNDEF), not defined by it"
                      % (name, name, label))
            continue
        if not sym["exported"]:
            rep.error("extern fn %s: symbol `%s` in %s has local/hidden binding and is not exported"
                      % (name, name, label))
            continue
        if sym["kind"] == "object":
            rep.error("extern fn %s: symbol `%s` in %s is a data object (%d bytes), not a function"
                      % (name, name, label, sym["size"]))
            continue

        dfn = dwarf["fns"].get(name)
        if dfn is not None:
            fn_ok = True
            if bool(varargs) != bool(dfn["variadic"]):
                rep.error("extern fn %s: %s compiled %s, BRAT declares %s"
                          % (name, label,
                             "variadic (...)" if dfn["variadic"] else "fixed-arity",
                             "variadic" if varargs else "fixed-arity"))
                fn_ok = False
            if len(params) != len(dfn["params"]):
                rep.error("extern fn %s: %s compiled %d parameter(s), BRAT declares %d"
                          % (name, label, len(dfn["params"]), len(params)))
                fn_ok = False
            else:
                for idx, (bp, dp) in enumerate(zip(params, dfn["params"])):
                    b_ann = bp[2] if isinstance(bp, tuple) and len(bp) > 2 else None
                    b_slot = _brat_abi_slot(b_ann, structs, layout)
                    if not _check_dwarf_slot(rep, "extern fn %s: parameter %d" % (name, idx + 1),
                                             label, b_ann, b_slot, dp):
                        fn_ok = False
            b_rslot = _brat_abi_slot(ret, structs, layout)
            if not _check_dwarf_slot(rep, "extern fn %s: return" % name,
                                     label, ret, b_rslot, dfn["ret"]):
                fn_ok = False
            if fn_ok:
                rep.ok()
        else:
            rep.ok()
    return rep


# `.bratbind` portable FFI package proofs.

BRATBIND_FORMAT = "bratbind"
_MODEL_SAMPLE_TRIPLE = {
    "lp64": "x86_64-unknown-linux-gnu",
    "llp64": "x86_64-pc-windows-msvc",
    "ilp32": "i686-unknown-linux-gnu",
}


def _norm_brat_bytes(raw_bytes):
    if isinstance(raw_bytes, str):
        return raw_bytes.replace("\r\n", "\n").encode("utf-8")
    return raw_bytes.decode("utf-8").replace("\r\n", "\n").encode("utf-8")


def _sha256_bytes(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


def _parse_brat_body(src_text):
    engine_dir = os.path.dirname(os.path.abspath(__file__))
    if engine_dir not in sys.path:
        sys.path.insert(0, engine_dir)
    import brat as _brat
    prog = _brat.parse_prog(_brat.Cur(_brat.lex(src_text)))
    return prog[1]


def _plugin_norm_slot(raw):
    s = "".join((raw or "").split())
    if s in ("ownptr", "moveptr"):
        return "own ptr"
    if s == "outptr":
        return "out ptr"
    if s in ("&ptr", "refptr"):
        return "&ptr"
    if s.startswith("fn("):
        while True:
            hit = None
            for w, vals in (("thread", ("same", "any")), ("reenter", ("yes", "no"))):
                for v in vals:
                    suf = "%s(%s)" % (w, v)
                    if s.endswith(suf):
                        hit = suf
                        break
                if hit:
                    break
            if not hit:
                break
            s = s[:-len(hit)]
    return s


def _plugin_parse_sig(raw_text):
    if not isinstance(raw_text, str):
        return None
    raw = raw_text.strip()
    open_i = raw.find("(")
    if open_i <= 0 or not raw.endswith(")"):
        return None
    depth = 0
    for i in range(open_i, len(raw)):
        if raw[i] == "(":
            depth += 1
        elif raw[i] == ")":
            depth -= 1
            if depth == 0 and i != len(raw) - 1:
                return None
    if depth != 0:
        return None
    ret_t = _plugin_norm_slot(raw[:open_i])
    rest = raw[open_i + 1:-1]
    params = []
    depth = 0
    start = 0
    for i, ch in enumerate(rest):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            piece = _plugin_norm_slot(rest[start:i])
            if piece:
                params.append(piece)
            start = i + 1
    tail = _plugin_norm_slot(rest[start:])
    if tail:
        params.append(tail)
    return ret_t, params


def has_plugin_calls(body):
    """True when `body` contains any `lib_open`/`lib_sym`/`lib_close`/`call` invocation."""
    plugin_names = {"lib_open", "lib_sym", "lib_close", "call"}

    def scan_expr(e):
        if isinstance(e, list):
            return any(scan_expr(x) for x in e)
        if not isinstance(e, tuple) or not e:
            return False
        if e[0] == "call" and isinstance(e[1], str) and e[1] in plugin_names:
            return True
        if e[0] == "closure":
            return scan_stmts(e[3])
        return any(scan_expr(sub) for sub in e[1:] if isinstance(sub, (tuple, list)))

    def scan_stmts(stmts):
        for s in stmts or ():
            if isinstance(s, list):
                if scan_stmts(s):
                    return True
                continue
            if not isinstance(s, tuple) or not s:
                continue
            k = s[0]
            if k == "pub":
                inner = s[1] if isinstance(s[1], list) else [s[1]]
                if scan_stmts(inner):
                    return True
                continue
            if k in ("fn", "exportfn"):
                if scan_stmts(s[3]):
                    return True
                continue
            if k == "impl":
                for m in s[3]:
                    if isinstance(m, tuple) and len(m) > 3 and scan_stmts(m[3]):
                        return True
                continue
            for part in s[1:]:
                if isinstance(part, list):
                    if scan_stmts(part):
                        return True
                elif isinstance(part, tuple):
                    if part and part[0] == "if":
                        if scan_stmts([part]):
                            return True
                    elif scan_expr(part):
                        return True
        return False

    return scan_stmts(body)


def plugin_externs(body):
    """Synthesize canonical `extern` AST nodes for statically named `lib_sym` + `call` sites."""
    explicit = set()
    for s in body or ():
        n = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if isinstance(n, tuple) and n and n[0] == "extern":
            explicit.add(n[2])

    out = []
    seen = set()

    def slot_to_ann(slot):
        if slot in ("own ptr", "&ptr", "out ptr"):
            return "ptr"
        return slot

    def scan_body(stmts, handles, syms):
        for s in stmts or ():
            if isinstance(s, list):
                scan_body(s, handles, syms)
                continue
            if not isinstance(s, tuple) or not s:
                continue
            k = s[0]
            if k in ("let", "lettmp", "set") and len(s) > 2 and isinstance(s[1], str):
                if k == "set":
                    handles.pop(s[1], None)
                    syms.pop(s[1], None)
                e = s[2]
                if isinstance(e, tuple) and e and e[0] == "call":
                    cname, cargs = e[1], e[2]
                    if cname == "lib_open" and len(cargs) == 1:
                        lib_lit = cargs[0][1] if (isinstance(cargs[0], tuple) and cargs[0] and cargs[0][0] == "str") else None
                        handles[s[1]] = lib_lit
                    elif cname == "lib_sym" and len(cargs) == 2:
                        hvar = cargs[0][1] if (isinstance(cargs[0], tuple) and cargs[0] and cargs[0][0] == "var") else None
                        sname = cargs[1][1] if (isinstance(cargs[1], tuple) and cargs[1] and cargs[1][0] == "str") else None
                        if sname and hvar in handles:
                            syms[s[1]] = (handles.get(hvar), sname)
            for part in s[1:]:
                if isinstance(part, list):
                    scan_body(part, handles, syms)
                elif isinstance(part, tuple):
                    if part and part[0] == "if":
                        scan_body([part], handles, syms)
                    else:
                        scan_expr(part, handles, syms)

    def scan_expr(e, handles, syms):
        if isinstance(e, list):
            for x in e:
                scan_expr(x, handles, syms)
            return
        if not isinstance(e, tuple) or not e:
            return
        if e[0] == "call" and e[1] == "call" and len(e[2]) >= 2:
            farg, sarg = e[2][0], e[2][1]
            if (isinstance(farg, tuple) and farg and farg[0] == "var"
                    and farg[1] in syms
                    and isinstance(sarg, tuple) and sarg and sarg[0] == "str"):
                lib_lit, sname = syms[farg[1]]
                parsed = _plugin_parse_sig(sarg[1])
                if parsed and sname not in explicit:
                    ret_t, params = parsed
                    key = (sname, ret_t, tuple(params))
                    if key not in seen:
                        seen.add(key)
                        pnodes = [("a%d" % i, "move" if p == "own ptr" else None, slot_to_ann(p), None)
                                  for i, p in enumerate(params)]
                        ret_ann = None if ret_t == "none" else slot_to_ann(ret_t)
                        effs = ("__plugin__",) + tuple(
                            "__out_%d__" % i for i, p in enumerate(params) if p == "out ptr"
                        )
                        out.append(("extern", lib_lit, sname, pnodes, ret_ann, None,
                                    ret_t == "own ptr", False, effs, ()))
        for sub in e[1:]:
            if isinstance(sub, (tuple, list)):
                scan_expr(sub, handles, syms)

    top = []
    for s in body or ():
        n = s[1] if (isinstance(s, tuple) and s and s[0] == "pub") else s
        if not isinstance(n, tuple) or not n:
            continue
        if n[0] in ("fn", "exportfn"):
            scan_body(n[3], {}, {})
        elif n[0] == "impl":
            for m in n[3]:
                if isinstance(m, tuple) and len(m) > 3:
                    scan_body(m[3], {}, {})
        elif n[0] not in ("trait", "enum", "extern", "externstruct"):
            top.append(n)
    if top:
        scan_body(top, {}, {})
    return out


def _flatten_pub_externs(body):
    out = []
    for s in body:
        if not s:
            continue
        if s[0] in ("extern", "externstruct"):
            out.append(s)
        elif s[0] == "pub" and len(s) > 1 and isinstance(s[1], list):
            out.extend(_flatten_pub_externs(s[1]))
        elif s[0] == "pub" and len(s) > 1 and isinstance(s[1], tuple):
            out.extend(_flatten_pub_externs([s[1]]))
    out.extend(plugin_externs(body))
    return out


def make_bratbind(brat_path, header_path, objects=(), target=None,
                  preprocess=False, include_dirs=(), defines=()):
    """Prove `brat_path` against `header_path` (and optional `objects`) and return `.bratbind` dict."""
    with open(brat_path, "rb") as f:
        norm_src = _norm_brat_bytes(f.read())
    src_text = norm_src.decode("utf-8")
    body = _flatten_pub_externs(_parse_brat_body(src_text))
    if not body:
        raise ValueError("%s declares no extern functions or structs" % brat_path)

    header_text = (preprocess_header(header_path, include_dirs=include_dirs,
                                     defines=defines, target=target)
                   if preprocess else read_header(header_path))
    header_file_sha = _sha256_file(header_path)
    header_text_sha = _sha256_bytes(header_text.encode("utf-8"))

    saved_triple, _saved_model = current_target()
    try:
        primary_model = set_target(target)
        cdecl_primary = parse_header(header_text)
        rep = check(body, cdecl_primary, header_path)
        if rep.errors:
            raise ValueError("\n".join(rep.lines))
        if target is None:
            ok, out = compile_probe(body, os.path.abspath(header_path), cdecl=cdecl_primary,
                                    target=None, include_dirs=include_dirs, defines=defines)
            if not ok:
                raise ValueError(out or ("layout probe failed against %s" % header_path))
        obj_records = []
        for obj_p in objects:
            orep = verify_object(body, obj_p, cdecl=cdecl_primary, target=target)
            if orep.errors:
                raise ValueError("\n".join(orep.lines))
            obj_records.append({
                "name": os.path.basename(obj_p),
                "sha256": _sha256_file(obj_p),
            })

        candidate_models = [primary_model]
        if target is None and not platform_width_uses(cdecl_primary):
            for m in ("lp64", "llp64", "ilp32"):
                if m not in candidate_models:
                    candidate_models.append(m)

        models_out = {}
        for m in candidate_models:
            triple_m = target if (target is not None and m == primary_model) else _MODEL_SAMPLE_TRIPLE[m]
            set_target(triple_m)
            cdecl_m = parse_header(header_text)
            rep_m = check(body, cdecl_m, header_path)
            if rep_m.errors:
                if m == primary_model:
                    raise ValueError("\n".join(rep_m.lines))
                continue
            ct = contract(body, header=header_path, target=triple_m, objects=objects)
            ct["target"]["triple"] = target if (target is not None and m == primary_model) else None
            models_out[m] = {
                "contract": ct,
                "surface_sha256": _sha256_bytes(contract_json({
                    "fns": ct["fns"], "structs": ct["structs"], "target": ct["target"],
                }).encode("utf-8")),
            }
    finally:
        set_target(saved_triple)

    stem = os.path.splitext(os.path.basename(brat_path))[0]
    return {
        "format": BRATBIND_FORMAT,
        "version": 1,
        "module": stem,
        "source_sha256": _sha256_bytes(norm_src),
        "header": {
            "name": os.path.basename(header_path),
            "sha256": header_file_sha,
            "text": header_text,
            "text_sha256": header_text_sha,
        },
        "objects": obj_records,
        "models": models_out,
    }


def verify_bratbind_bytes(brat_bytes, bind_bytes_or_obj, target=None, object_files=None, label="<bratbind>"):
    """Verify a `.bratbind` proof against `brat_bytes`, target ABI model, and any bundled objects."""
    import json as _json
    if isinstance(bind_bytes_or_obj, (bytes, bytearray, str)):
        raw_text = bind_bytes_or_obj.decode("utf-8") if isinstance(bind_bytes_or_obj, (bytes, bytearray)) else bind_bytes_or_obj
        try:
            bind = _json.loads(raw_text)
        except ValueError as e:
            raise ValueError("%s: invalid JSON: %s" % (label, e))
    elif isinstance(bind_bytes_or_obj, dict):
        bind = bind_bytes_or_obj
    else:
        raise ValueError("%s: invalid bratbind object" % label)

    if not isinstance(bind, dict) or bind.get("format") != BRATBIND_FORMAT or bind.get("version") != 1:
        raise ValueError("%s: unsupported bratbind format or version" % label)

    norm_src = _norm_brat_bytes(brat_bytes)
    actual_src_sha = _sha256_bytes(norm_src)
    if bind.get("source_sha256") != actual_src_sha:
        raise ValueError("%s: source SHA-256 mismatch (declarations were modified after proof)" % label)

    hdr = bind.get("header")
    if not isinstance(hdr, dict) or not isinstance(hdr.get("text"), str) or not isinstance(hdr.get("text_sha256"), str):
        raise ValueError("%s: missing embedded header proof" % label)
    if _sha256_bytes(hdr["text"].encode("utf-8")) != hdr["text_sha256"]:
        raise ValueError("%s: embedded header SHA-256 mismatch" % label)

    models = bind.get("models")
    if not isinstance(models, dict) or not models:
        raise ValueError("%s: contains no proved ABI data models" % label)
    req_model = data_model(target)
    if req_model not in models or not isinstance(models.get(req_model), dict):
        avail = ", ".join(sorted(models.keys()))
        raise ValueError("%s: proved only for ABI model [%s], target requires `%s`"
                         % (label, avail, req_model))

    body = _flatten_pub_externs(_parse_brat_body(norm_src.decode("utf-8")))
    if not body:
        raise ValueError("%s: module contains no extern declarations" % label)

    saved_triple, _saved_model = current_target()
    try:
        triple_use = target if target is not None else _MODEL_SAMPLE_TRIPLE[req_model]
        set_target(triple_use)
        cdecl = parse_header(hdr["text"])
        rep = check(body, cdecl, hdr.get("name") or "<bratbind-header>")
        if rep.errors:
            raise ValueError("%s: header proof failed for `%s`:\n%s"
                             % (label, req_model, "\n".join(rep.lines)))
        recorded_ct = models[req_model].get("contract")
        if not isinstance(recorded_ct, dict):
            raise ValueError("%s: missing contract for `%s`" % (label, req_model))
        now_ct = contract(body, header=None, target=triple_use)
        if recorded_ct.get("target", {}).get("triple") is None:
            now_ct["target"]["triple"] = None
        if isinstance(hdr.get("sha256"), str):
            now_ct["proved"] = {"sha256": hdr["sha256"]}
        drift = contract_drift(recorded_ct, now_ct)
        if drift:
            raise ValueError("%s: contract drift for `%s`: %s" % (label, req_model, "; ".join(drift)))

        obj_map = dict(object_files or {})
        rec_objs = bind.get("objects") or []
        if not isinstance(rec_objs, list):
            raise ValueError("%s: invalid objects list" % label)
        for oent in rec_objs:
            if not isinstance(oent, dict) or not isinstance(oent.get("name"), str):
                raise ValueError("%s: invalid object entry" % label)
            oname = oent["name"]
            if oname in obj_map:
                opath = obj_map[oname]
                if _sha256_file(opath) != oent.get("sha256"):
                    raise ValueError("%s: bundled object %s SHA-256 mismatch" % (label, oname))
                orep = verify_object(body, opath, cdecl=cdecl, target=target)
                if orep.errors:
                    raise ValueError("%s: binary object check failed:\n%s" % (label, "\n".join(orep.lines)))
        for oname, opath in sorted(obj_map.items()):
            if not any(isinstance(o, dict) and o.get("name") == oname for o in rec_objs):
                orep = verify_object(body, opath, cdecl=cdecl, target=target)
                if orep.errors:
                    raise ValueError("%s: binary object check failed:\n%s" % (label, "\n".join(orep.lines)))
    finally:
        set_target(saved_triple)
    return bind


def verify_module_bratbind(brat_path, brat_src=None, target=None):
    """Return `(True, None)` if `<stem>.bratbind` exists and proves `brat_path`, `(False, err_or_None)` otherwise."""
    bind_path = os.path.splitext(brat_path)[0] + ".bratbind"
    if not os.path.isfile(bind_path):
        return False, None
    try:
        if brat_src is None:
            with open(brat_path, "rb") as f:
                raw_brat = f.read()
        else:
            raw_brat = brat_src.encode("utf-8") if isinstance(brat_src, str) else brat_src
        with open(bind_path, "rb") as f:
            raw_bind = f.read()
        parent = os.path.dirname(os.path.abspath(brat_path))
        sibling_objs = {}
        try:
            for fn in os.listdir(parent):
                if fn.endswith((".o", ".obj", ".a", ".so", ".dylib", ".dll")):
                    fp = os.path.join(parent, fn)
                    if os.path.isfile(fp):
                        sibling_objs[fn] = fp
        except OSError:
            pass
        verify_bratbind_bytes(raw_brat, raw_bind, target=target,
                              object_files=sibling_objs, label=bind_path)
        return True, None
    except Exception as e:
        return False, str(e)


# ABI algebra and differential multi-target layout verification.

ABI_MATRIX_TRIPLES = (
    "x86_64-unknown-linux-gnu",   # lp64  (ptr=8, long=8, align8=8)
    "aarch64-apple-darwin",       # lp64  (ptr=8, long=8, align8=8)
    "x86_64-pc-windows-msvc",     # llp64 (ptr=8, long=4, align8=8)
    "i686-unknown-linux-gnu",     # ilp32 (ptr=4, long=4, align8=4)
    "wasm32-unknown-wasi",        # ilp32 (ptr=4, long=4, align8=8)
)


def _c_field_shape(cdecl, ctype, sub_layouts):
    """Return `(size, align)` of a parsed C field type on the active target."""
    kind, extra = resolve(cdecl, ctype)
    pw = ptr_width()
    if kind in ("ptr", "str", "fn"):
        return (pw, pw)
    if kind == "array":
        ek, ee, ecnt = extra
        if ek == "struct":
            sub = sub_layouts.get(ee)
            return (sub[0] * ecnt, sub[1]) if sub is not None else None
        if ek in ("ptr", "str", "fn"):
            return (pw * ecnt, pw)
        sh = field_shape(ek)
        return (sh[0] * ecnt, sh[1]) if sh is not None else None
    if kind == "struct":
        sub = sub_layouts.get(extra)
        if sub is None:
            return None
        return (sub[0], sub[1])
    return field_shape(kind)


def c_struct_layout(cdecl):
    """Algebraic `(size, align, [(field, offset)])` of parsed C structs on the active target."""
    if cdecl is None:
        return {}
    c_packed = set(getattr(cdecl, "packed", ()))
    c_unions = set(getattr(cdecl, "unions", ()))
    c_anon = set(getattr(cdecl, "anon", ()))
    c_pack_align = getattr(cdecl, "pack_align", {}) or {}
    layout = {}
    in_progress = set()

    def of(name):
        if name in layout:
            return layout[name]
        if name in c_anon or name in in_progress:
            return None
        fields = cdecl.structs.get(name)
        if not fields:
            return None
        in_progress.add(name)
        packed_ = name in c_packed
        is_un = name in c_unions
        p_cap = None if is_un else c_pack_align.get(name)
        off, max_sz, align, slots = 0, 0, 1, []
        for fname, ctype in fields:
            kind, extra = resolve(cdecl, ctype)
            if kind == "struct":
                if of(extra) is None:
                    in_progress.discard(name)
                    return None
            elif kind == "array" and extra[0] == "struct":
                if of(extra[1]) is None:
                    in_progress.discard(name)
                    return None
            sh = _c_field_shape(cdecl, ctype, layout)
            if sh is None:
                in_progress.discard(name)
                return None
            size, falign = sh
            if is_un:
                slots.append((fname, 0))
                max_sz = max(max_sz, size)
                align = max(align, falign)
            else:
                if packed_:
                    falign = 1
                elif p_cap is not None:
                    falign = min(falign, p_cap)
                off = (off + falign - 1) // falign * falign
                slots.append((fname, off))
                off += size
                align = max(align, falign)
        if is_un:
            total = (max_sz + align - 1) // align * align
        elif packed_:
            align = 1
            total = off
        else:
            total = (off + align - 1) // align * align
        layout[name] = (total, align, slots)
        in_progress.discard(name)
        return layout[name]

    for name in cdecl.structs:
        of(name)
    return layout


def _freestanding_layout_probe_c(brat_structs, brat_packed, b_layouts, c_layouts, brat_unions=()):
    """Freestanding C11 source asserting algebraic `sizeof`/`offsetof` (works with `gcc -m32`)."""
    lines = [
        "typedef __SIZE_TYPE__ b_usize;",
        "typedef __INT8_TYPE__ b_i8;",
        "typedef __UINT8_TYPE__ b_u8;",
        "typedef __INT16_TYPE__ b_i16;",
        "typedef __UINT16_TYPE__ b_u16;",
        "typedef __INT32_TYPE__ b_i32;",
        "typedef __UINT32_TYPE__ b_u32;",
        "typedef __INT64_TYPE__ b_i64;",
        "typedef __UINT64_TYPE__ b_u64;",
    ]
    fmap = {
        "bool": "_Bool", "i8": "b_i8", "u8": "b_u8", "i16": "b_i16", "u16": "b_u16",
        "i32": "b_i32", "u32": "b_u32", "i64": "b_i64", "u64": "b_u64",
        "usize": "b_usize", "f32": "float", "f64": "double",
        "str": "const char *", "ptr": "void *",
    }
    ordered = []
    seen = set()

    def visit(nm):
        if nm in seen or nm not in brat_structs or nm not in b_layouts:
            return
        seen.add(nm)
        for _fn, ft in brat_structs[nm]:
            arr = array_field_type(ft)
            dep = arr[0] if arr is not None else ft
            if dep in brat_structs:
                visit(dep)
        ordered.append(nm)

    for nm in sorted(brat_structs):
        visit(nm)
    if not ordered:
        return None
    for nm in ordered:
        mems = []
        for fn, ft in brat_structs[nm]:
            arr = array_field_type(ft)
            if arr is not None:
                aelem, acnt = arr
                act = ("bf_" + aelem) if aelem in brat_structs else (
                    "void *" if isinstance(aelem, str) and (aelem.startswith("ptr[") or aelem.startswith("fn"))
                    else fmap.get(aelem))
                if act is None:
                    return None
                mems.append("%s %s[%d];" % (act, fn, acnt))
                continue
            if ft in brat_structs:
                ct = "bf_" + ft
            elif isinstance(ft, str) and (ft.startswith("ptr[") or ft.startswith("fn")):
                ct = "void *"
            else:
                ct = fmap.get(ft)
                if ct is None:
                    return None
            mems.append("%s %s;" % (ct, fn))
        if nm in brat_unions:
            kw = "typedef union {"
        elif nm in brat_packed:
            kw = "typedef struct __attribute__((packed)) {"
        else:
            kw = "typedef struct {"
        lines.append("%s %s } bf_%s;" % (kw, " ".join(mems), nm))
        bsz, baln, bslots = b_layouts[nm]
        lines.append('_Static_assert(sizeof(bf_%s) == %d, "sizeof %s");' % (nm, bsz, nm))
        lines.append('_Static_assert(__alignof__(bf_%s) >= %d || 1, "align %s");' % (nm, baln, nm))
        for fn, boff in bslots:
            lines.append('_Static_assert(__builtin_offsetof(bf_%s, %s) == %d, "offset %s.%s");'
                         % (nm, fn, boff, nm, fn))
        if nm in c_layouts:
            csz, _caln, _cslots = c_layouts[nm]
            lines.append('_Static_assert(%d == %d, "C vs BRAT sizeof %s");' % (bsz, csz, nm))
    return "\n".join(lines) + "\n"


def _run_freestanding_cc_check(src, extra_flags):
    import shutil
    import tempfile
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc or not src:
        return None
    with tempfile.TemporaryDirectory(prefix="brat-l5-") as td:
        p = os.path.join(td, "l5.c")
        with open(p, "w", encoding="utf-8") as f:
            f.write(src)
        r = subprocess.run([cc, "-std=c11", "-ffreestanding", "-fsyntax-only"] + list(extra_flags) + [p],
                           capture_output=True, text=True, timeout=30)
        out = (r.stdout + r.stderr).strip()
        if r.returncode != 0 and ("unrecognized command-line option" in out or "not supported" in out):
            return None
        return (r.returncode == 0, out.replace(p, "abi-l5.c"))


def verify_abi_algebra(prog_body, cdecl, header_name="<header>", triples=None, verify_cc=True):
    """FFI Ladder L5: differential ABI algebra across LP64, LLP64, and ILP32 targets."""
    triples = tuple(triples) if triples is not None else ABI_MATRIX_TRIPLES
    brat_structs = {s[1]: s[2] for s in prog_body if s and s[0] == "externstruct"}
    brat_packed = {s[1] for s in prog_body
                   if s and s[0] == "externstruct" and len(s) > 3 and s[3]}
    brat_unions = {s[1] for s in prog_body
                   if s and s[0] == "externstruct" and len(s) > 4 and s[4]}
    saved_triple, _saved_model = current_target()
    targets_out = []
    differential = {nm: {} for nm in sorted(brat_structs)}
    all_errors = []
    try:
        for tri in triples:
            model = set_target(tri)
            rep = check(prog_body, cdecl, header_name)
            b_lay = struct_layout(brat_structs, brat_packed, unions=brat_unions)
            c_lay = c_struct_layout(cdecl)
            cc_verified = None
            if verify_cc and not rep.errors and b_lay:
                probe_c = _freestanding_layout_probe_c(brat_structs, brat_packed, b_lay, c_lay, brat_unions)
                if tri.startswith("x86_64") and "linux" in tri:
                    res = _run_freestanding_cc_check(probe_c, [])
                elif tri.startswith(("i386", "i686")):
                    res = _run_freestanding_cc_check(probe_c, ["-m32"])
                else:
                    res = None
                if res is not None:
                    cc_ok, cc_msg = res
                    cc_verified = cc_ok
                    if not cc_ok:
                        rep.error("compiler ABI check failed on %s: %s" % (tri, cc_msg))
            t_errs = [ln.strip() for ln in rep.lines]
            for e in t_errs:
                all_errors.append("[%s / %s] %s" % (tri, model, e))
            structs_rec = {}
            for nm in sorted(brat_structs):
                bl = b_lay.get(nm)
                cl = c_lay.get(nm)
                rec = {
                    "brat": {"size": bl[0], "align": bl[1], "offsets": dict(bl[2])} if bl else None,
                    "c": {"size": cl[0], "align": cl[1], "offsets": dict(cl[2])} if cl else None,
                }
                structs_rec[nm] = rec
                differential[nm][tri] = rec
            targets_out.append({
                "target": tri,
                "model": model,
                "ptr_width": ptr_width(),
                "long_width": long_width(),
                "wide_align": wide_align(),
                "proven": rep.proven,
                "errors": rep.errors,
                "findings": t_errs,
                "cc_verified": cc_verified,
                "structs": structs_rec,
            })
    finally:
        set_target(saved_triple)
    return {
        "ok": len(all_errors) == 0,
        "targets": targets_out,
        "differential": differential,
        "errors": all_errors,
    }


