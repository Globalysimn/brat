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
    "short": ("i16", 2), "short int": ("i16", 2), "signed short": ("i16", 2),
    "unsigned short": ("u16", 2), "unsigned short int": ("u16", 2),
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
    # spelling: `u8`. (A BRAT `bool` is not an extern slot yet -- see FFI_IDEAL.)
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
# proof is about. `codegen_c` and `emit.cpp` lay the record out with this table
# and the C compiler checks the answer, so a width that is wrong for the target
# fails the build instead of the program.
def field_shape(t=None):
    pw = ptr_width()
    a8 = wide_align()
    shape = {"i8": (1, 1), "u8": (1, 1), "i16": (2, 2), "u16": (2, 2),
             "i32": (4, 4), "u32": (4, 4), "f32": (4, 4),
             "i64": (8, a8), "u64": (8, a8), "f64": (8, a8),
             "usize": (pw, pw), "str": (pw, pw), "ptr": (pw, pw)}
    if t is None:
        return shape
    if t.startswith("ptr[") or t.startswith("fn"):
        return (pw, pw)
    return shape.get(t)


def struct_layout(structs, packed, shape=None):
    """Size, alignment and field offsets of extern structs, by the target's rules.

    `structs` is name -> [(field, BRAT type)], `packed` the names declared
    `extern packed struct`. A struct whose field type has no width is left out:
    the emitter then emits no assertion for it rather than a guessed one.
    """
    shape = shape or field_shape
    layout = {}
    in_progress = set()

    def of(name, packed_):
        if name in layout:
            return layout[name]
        fields = structs.get(name)
        if fields is None or name in in_progress:
            return None
        in_progress.add(name)
        off, align, slots = 0, 1, []
        for fname, ftype in fields:
            if ftype in structs:
                sub = of(ftype, packed_)
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
            if packed_:
                falign = 1
            off = (off + falign - 1) // falign * falign
            slots.append((fname, off))
            off += size
            align = max(align, falign)
        if packed_:
            align = 1
        else:
            off = (off + align - 1) // align * align
        layout[name] = (off, align, slots)
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
        out.append((pm.group("tag"), body, alias, "packed" in prefix))
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
            # `char **`, `void **`: a pointer to a pointer. Erased, because the
            # element is itself a pointer and `ptr[T]` names a word or a record.
            return ("ptr", None)
        if base == "char" and not named:
            # `const char *` is a string, and only it is: `str` is BRAT's
            # read-only view, so handing one to a parameter that may write would
            # corrupt an immutable, shared value. A writable `char *` is a byte
            # buffer, and `ptr[u8]` is what a caller can legally give it.
            raw = set(_words(t.replace("*", " ")))
            # `unsigned char *` / `signed char *` are byte buffers whatever their
            # constness: only a plain `char` pointer is text. Ask the signedness
            # first, or `const unsigned char *` becomes a string.
            if "unsigned" in raw:
                return ("ptr", "u8")
            if "signed" in raw:
                return ("ptr", "i8")
            if "const" in raw:
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
        self.typedefs = {}   # name -> c_type it aliases
        self.aliases = set()  # names a `typedef` introduced: spellable bare
        self.tags = set()     # names that exist only as `struct X`: need the keyword
        self.anon = set()     # records holding an anonymous union/struct member
        self.arrays = set()   # records with inline array fields (not a pointer)
        self.abi_unsupported = {}

    def c_name(self, name):
        """How C spells this record's type in *this* header.

        `typedef struct { ... } Point;` gives `Point`, but `struct Point { ... };`
        gives `struct Point` and nothing shorter. A proof that writes the bare
        name for the second one does not compile, and a proof that cannot compile
        is worse than no proof: it looks like a mismatch in the header.
        """
        if name in self.tags and name not in self.aliases:
            return "struct %s" % name
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
    src = _strip(text)
    out = CDecl()
    uncommented = _COMMENT.sub(" ", text)
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
    for tag, body, alias, packd in _scan_records(src):
        fields = _fields(body)
        if "{" in body:
            # An anonymous union or struct inside a record has no field name of
            # its own, so BRAT cannot declare the record field by field: the
            # offsets would depend on a member the header does not name. It is
            # skipped with a note rather than generated broken.
            out.anon.add(alias or tag or "")
        if re.search(r"\[[^\]]*\]", body):
            # `_split_decl` treats parameter arrays as pointers because C adjusts
            # them at a function boundary; an array member inside a record stays
            # inline and changes size/offsets, so this layout is not representable.
            out.arrays.add(alias or tag or "")
            out.anon.add(alias or tag or "")
        if alias:
            out.aliases.add(alias)
            out.structs[alias] = fields
            if alias in packed or packd:
                out.packed.add(alias)
            if tag:
                out.structs.setdefault(tag, fields)
        elif tag:
            out.structs[tag] = fields
            out.tags.add(tag)
            if packd:
                out.packed.add(tag)
        if any(n is None or (isinstance(n, str) and n.startswith("_anon"))
               for n, _ in fields):
            # A field with no declarator name -- a function pointer spelled
            # without one, an array of records, an anonymous member -- means the
            # body was not understood. Declaring the record anyway would invent
            # field names, so it is marked unknown and skipped.
            out.anon.add(alias or tag or "")
    flat = re.sub(r"\{[^{}]*\}", " ", src)     # struct bodies are gone by now
    # `typedef z_stream FAR *z_streamp;` has no space between the star and the
    # new name, so requiring one dropped every pointer typedef -- `z_streamp`,
    # `voidpf`, `gz_headerp` -- and with them every zlib function that takes one.
    for m in re.finditer(r"typedef\s+([^;{}()]+?)\s*([A-Za-z_]\w*)\s*;", flat):
        target, alias = m.group(1).strip(), m.group(2)
        out.aliases.add(alias)
        if ("struct" in target.split()[:1] or target.startswith("struct")) \
                and "*" not in target:
            out.structs.setdefault(alias, out.structs.get(target.split()[-1], []))
        else:
            # `typedef struct gzFile_s *gzFile;` names a *pointer*: recording it
            # as the record made every function taking a handle report "whose
            # body is opaque", which is the opposite of what an opaque handle
            # means -- it is a value BRAT passes on, not a layout it reads.
            out.typedefs[alias] = target

    body = src
    for m in re.finditer(r"\{[^{}]*\}", body):
        body = body.replace(m.group(0), " ")
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
    if head.replace("*", "").strip() in ("", "struct", "union", "enum"):
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


def _fields(body):
    """`float x; const char *name;` -> [('x', 'float'), ('name', 'const char *')]."""
    fields = []
    for part in body.split(";"):
        p = part.strip()
        if not p:
            continue
        got = _split_decl(p)
        if got:
            fields.append(got)
        else:
            fields.append((None, p))
    return [(n or "_anon%d" % i, t) for i, (n, t) in enumerate(fields)]


# A function-pointer declarator: `float (*cb)(float)`, `Pt (*)(int)`, with the
# name optional and the return type possibly a star. The parameter list of the
# *callback* is the second pair of parentheses, and it is the part that decides
# the ABI: a `float` where C reads a `double` is a wrong register, not a warning.
_CB_C = re.compile(r"^\s*(?P<ret>[A-Za-z_][\w\s\*]*?)\s*\(\s*\*\s*"
                   r"(?P<name>[A-Za-z_]\w*)?\s*\)\s*\((?P<params>[^()]*)\)\s*$")


def c_callback(ctype):
    """(return_c, [param_c]) for a C function-pointer declarator, else None."""
    m = _CB_C.match(ctype or "")
    if not m:
        return None
    params = [x.strip() for x in _split_params(m.group("params")) if x.strip()]
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
    bcb, ccb = brat_callback(brat_ann), c_callback(ctype)
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
            return kind == "ptr" and extra == "void" and not const_pointee(ctype)
        if elem is None:
            return None
        if kind != "ptr" or const_pointee(ctype) or elem in brat_structs:
            return False
        words = set(_words(ctype.replace("*", " ")))
        if elem in ("i8", "u8") and "char" in words and not words.intersection(
                {"signed", "unsigned", "int8_t", "uint8_t"}):
            return False
        return pointee_ok(elem, extra, brat_structs)

    ok = True
    if len(bparams) != len(cparams):
        rep.error("extern fn %s: %s is declared `%s` (%d argument(s)), %s has `%s` (%d)"
                  % (name, where, brat_ann, len(bparams), header_name, ctype, len(cparams)))
        ok = False
    for i, (bp, cp) in enumerate(zip(bparams, cparams)):
        kind, _extra = resolve(cdecl, cp)
        pointer_ok = pointer_match(bp, cp, kind, _extra)
        if element_of(bp) is not None or bp in ("ptr", "str"):
            if not pointer_ok:
                rep.error("extern fn %s: %s argument %d is `%s`, %s has `%s`"
                          % (name, where, i + 1, bp, header_name, cp))
                ok = False
        elif not same(bp, kind):
            rep.error("extern fn %s: %s argument %d is `%s`, %s has `%s`"
                      % (name, where, i + 1, bp, header_name, cp))
            ok = False
    crkind, _ccextra = resolve(cdecl, cret)
    if bret == "none":
        if crkind != "none":
            rep.error("extern fn %s: %s returns `%s`, BRAT declares no result"
                      % (name, where, cret))
            ok = False
    elif pointer_match(bret, cret, crkind, _ccextra) is False:
        rep.error("extern fn %s: %s returns `%s`, %s has `%s`"
                  % (name, bret, header_name, cret))
        ok = False
    elif pointer_match(bret, cret, crkind, _ccextra) is None and not same(bret, crkind):
        rep.error("extern fn %s: %s returns `%s`, BRAT declares `%s`"
                  % (name, where, cret, bret))
        ok = False
    return ok


def resolve(cdecl, t):
    """Follow typedefs and struct tags to (brat_type, c_spelling)."""
    kind, extra, = _base_type(t)
    if kind == "typedef" and extra in cdecl.typedefs:
        return resolve(cdecl, cdecl.typedefs[extra])
    if kind == "typedef" and extra in cdecl.structs:
        return ("struct", extra)
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


def const_pointee(ct):
    """True when a C pointer type is a pointer *to const*: `const char *`,
    `const void *`, `const struct Foo *`. That is the header promising the
    callee will not write through it, and it is the only case where BRAT's
    read-only `str` may be handed over."""
    return isinstance(ct, str) and "*" in ct and "const" in _words(ct.replace("*", " "))


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
            and a in ("u8", "u16", "i8", "i16", "i32", "u32"):
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
    for name, fields in sorted(brat_structs.items()):
        if name in getattr(cdecl, "arrays", ()):
            rep.error("extern struct %s: inline array fields in %s are not representable"
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
            if cbt == "struct" and isinstance(bann, str) and bann != cextra:
                rep.error("extern struct %s.%s: BRAT declares `%s`, %s has `%s`"
                          % (name, bname, bann, header_name, cextra))
                bad = True
            elif cbt == "ptr":
                elem = element_of(bann)
                if bann == "ptr" and _WIDTH.get(cextra) is not None:
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
            rep.ok()

    for s in prog_body:
        if not s or s[0] != "extern":
            continue
        _kind, libname, name, params, ret, varargs = s[0], s[1], s[2], s[3], s[4], s[5]
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
            if (isinstance(bann, str) and bann.startswith("fn")) or c_callback(cp) is not None:
                # A callback is a pointer whose *pointee* is a signature: the
                # header's `float (*cb)(float)` and BRAT's `fn(f32)->f32` are the
                # same type or they are not, and "both are pointers" is not a proof.
                if not same_callback(rep, name, where, bann, cp, cdecl, header_name,
                                     brat_structs):
                    bad = True
                continue
            kind, extra = resolve(cdecl, cp)
            if kind == "struct":
                if bann != extra and extra not in brat_structs and bann not in brat_structs:
                    pass
                elif bann is None or bann == extra:
                    pass
                elif bann not in brat_structs and extra not in brat_structs:
                    pass
                if not (bann == extra or (bann in brat_structs and extra in cdecl.structs)):
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
                if want == "ptr" and base == "str" and not const_pointee(cp):
                    rep.error("extern fn %s: %s is declared `str`, which is read-only, but "
                              "%s has `%s`: declare a `ptr` for a buffer C may write"
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
                elif want == "ptr" and elem is None and _WIDTH.get(extra) is not None:
                    rep.error("extern fn %s: %s uses erased `ptr`, incompatible with scalar `%s *`; use `ptr[T]`"
                              % (name, where, extra))
                    bad = True
                elif want == "ptr" and elem is None and extra in cdecl.structs:
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
            if ret != rextra and ret not in brat_structs:
                rep.error("extern fn %s: BRAT returns `%s`, %s returns `%s`"
                          % (name, ret, header_name, rextra))
                bad = True
        elif rkind in ("ptr", "str"):
            if rkind == "str" and ret not in ("str", "ptr", "i64"):
                rep.error("extern fn %s: BRAT returns `%s`, %s returns `%s`"
                          % (name, ret, header_name, cret))
                bad = True
            elif rkind == "ptr" and ret == "ptr" and _WIDTH.get(rextra) is not None:
                rep.error("extern fn %s: BRAT returns erased `ptr`, incompatible with scalar `%s *`; use `ptr[T]`"
                          % (name, rextra))
                bad = True
            elif rkind == "ptr" and ret == "ptr" and rextra in cdecl.structs:
                rep.error("extern fn %s: BRAT returns erased `ptr` for `%s *`; use `ptr[%s]`"
                          % (name, rextra, rextra))
                bad = True
            elif rkind == "ptr" and ret not in ("ptr", "i64", "u64", "usize") \
                    and element_of(ret) is None:
                rep.error("extern fn %s: BRAT returns `%s`, %s returns `%s`"
                          % (name, ret, header_name, cret))
                bad = True
            elif rkind == "ptr" and element_of(ret) is not None \
                    and not pointee_ok(element_of(ret), rextra, brat_structs):
                rep.error("extern fn %s: BRAT returns `%s`, %s returns `%s`"
                          % (name, ret, header_name, cret))
                bad = True
        elif ret is not None and rkind not in ("none", "typedef") and not same(ret, rkind):
            rep.error("extern fn %s: BRAT returns `%s` (%s), %s returns `%s` (%s)"
                      % (name, ret, _size_note(ret), header_name, rkind, _size_note(rkind)))
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


def _brat_structs(prog_body):
    structs, packed = {}, set()
    for s in prog_body:
        if s and s[0] == "externstruct":
            structs[s[1]] = list(s[2])
            if len(s) > 3 and s[3]:
                packed.add(s[1])
    return structs, packed


def contract(prog_body, header=None, target=None):
    """The extern surface of a program as data: what the binary may assume.

    Every `extern` declaration becomes an entry, every `extern struct` becomes a
    size, an alignment and a field offset, and the target's widths are recorded
    next to them, because a layout is only meaningful for one data model. When a
    header was used to prove the declarations, its sha256 goes in as well, so a
    later `brat ffi verify --bin` can say whether the proof still describes the
    header in front of it.
    """
    set_target(target)
    structs, packed = _brat_structs(prog_body)
    layout = struct_layout(structs, packed)
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
            out_structs.append({"name": name, "size": None, "align": None,
                                "packed": name in packed, "fields": []})
            continue
        out_structs.append({
            "name": name,
            "size": lay[0],
            "align": lay[1],
            "packed": name in packed,
            "fields": [{"name": fname, "offset": foff, "type": ftype}
                       for (fname, foff), (_fn, ftype) in zip(lay[2], structs[name])],
        })
    triple, model = current_target()
    proved = None
    if header:
        proved = {"header": os.path.basename(header), "sha256": _sha256_file(header)}
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
    """The C type the emitters declare, or None when it contains a record value."""
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
    lines = ['#include <stddef.h>', '#include <stdint.h>',
             '#include "%s"' % header, ""]
    # A width that came from the target's usual answer, not from the language.
    # `long` on a 32-bit target is four bytes while `C_SCALARS` says eight, so a
    # declaration compared by width would agree with a header it does not match.
    for sp in sorted(widths):
        lines.append('_Static_assert(sizeof(%s) == %d, '
                     '"C `%s` is not %d bytes on this target, so BRAT bound it '
                     'at the wrong width");' % (sp, PLATFORM_WIDTH_C[sp], sp,
                                                PLATFORM_WIDTH_C[sp]))
    if widths:
        lines.append("")
    packed = {s[1] for s in prog_body if s and s[0] == "externstruct" and len(s) > 3 and s[3]}
    for name, fields in structs.items():
        # The header's own spelling: `struct Foo` when there is no typedef.
        cname = cdecl.c_name(name) if cdecl is not None else name
        members = " ".join(
            "%s %s;" % ("void *" if isinstance(fann, str)
                        and fann.startswith("ptr[") else PRIM_C.get(fann, fann), fname)
            for fname, fann in fields)
        kw = "typedef struct __attribute__((packed)) { " if name in packed else "typedef struct { "
        lines.append("%s%s } bf_%s;" % (kw, members, name))
        lines.append('_Static_assert(sizeof(bf_%s) == sizeof(%s), '
                     '"extern struct %s: size differs from the header");' % (name, cname, name))
        for fname, _ in fields:
            lines.append('_Static_assert(offsetof(bf_%s, %s) == offsetof(%s, %s), '
                         '"extern struct %s.%s: offset differs from the header");'
                         % (name, fname, cname, fname, name, fname))
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


def compile_probe(prog_body, header, cc="cc", timeout=120, cdecl=None, target=None):
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
    with tempfile.TemporaryDirectory(prefix="brat-ffi-probe-") as tmp:
        path = os.path.join(tmp, "probe.c")
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        r = subprocess.run(argv + ["-std=c11", "-fsyntax-only",
                                   "-Werror=incompatible-pointer-types", path],
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
    for name, (cret, cparams, cvar) in sorted(cdecl.fns.items()):
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
    out = ["// Go bindings for %s" % os.path.basename(header_name or ""),
           "// Generated by brat bindgen from the same C view `brat ffi --header` proves.",
           "", "package bindings", "", "/*",
           "#include <stdlib.h>", "#include <stdbool.h>",
           '#include "%s"' % (header_name or ""), "*/",
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
    for name, rows, packed in structs:
        out.append("\t%s = C.struct_%s" % (name, name))
    out.append(")")
    out.append("")
    for name, (cret, cparams, cvar) in sorted(cdecl.fns.items()):
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


def emit_bindings(prog_body, cdecl, header_name, lib=None):
    """BRAT declarations for a header, in the shape `check` proves.

    This is the only bindgen: `brat bindgen`, `brat ffi --emit` and the `check`
    proof all read one C view, so a type that is generated and a type that is
    verified can never drift apart. What cannot be represented exactly is
    reported in the file, not rounded: a fixed-size array becomes `ptr` with a
    note, an opaque struct stays `ptr`, and a name BRAT cannot spell is skipped
    with the reason written down.
    """
    out = []
    notes = []

    def skip(why):
        notes.append(why)

    # The basename, not the path: `brat bindgen h.h` and `brat ffi --emit
    # header /tmp/h.h` are the same conversion, so their output must be the
    # same bytes, and a temp path in a comment would hide that.
    out.append("// generated by brat bindgen from %s"
               % os.path.basename(header_name or ""))
    if lib:
        out.append("// library: %s" % lib)

    used = set()
    pointer_structs = {name for name, fields in cdecl.structs.items()
                       if fields and name not in getattr(cdecl, "anon", ())
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
                continue
            b = brat_pointer(extra) if kind == "ptr" else \
                BRAT_OF_C.get(kind, extra if kind == "struct" else None)
            if b is None:
                skip("struct %s.%s: `%s` has no BRAT spelling" % (name, fname, ctype))
                b = "ptr"
            members.append((fname, b))
        if ok and members:
            used.add(name)
            struct_lines.append("extern struct %s { %s }"
                                % (name, ", ".join("%s: %s" % m for m in members)))
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
            if kind == "struct":
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
            # The header's own parameter name when BRAT can spell it: it makes
            # the generated file readable, and `a0` when it cannot.
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
