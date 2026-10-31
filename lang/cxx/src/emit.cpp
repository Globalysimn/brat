// The C emitter: BRAT's tree becomes C that links against crt.h.
//
// This is the C++ twin of lang/engine/codegen_c.py and the contract is byte
// equality, so every string is built in the order the Python builds it. The
// notes that explain *why* a rule exists live in the Python file; the ones here
// are the ones that only make sense against this code.

#include <stdexcept>

#include "emit.h"

#include "ir.h"

// The typer's ABI helpers: a callback slot is refused with the same rule here.
#include "check.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <charconv>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace codegen {
namespace {

using StmtList = std::vector<const Stmt *>;
using ExprList = std::vector<const Expr *>;
using Names = std::set<std::string>;


inline void app(std::string &) {}
inline void app(std::string &o, const std::string &s) { o += s; }
inline void app(std::string &o, const char *s) { o += s; }
inline void app(std::string &o, char c) { o += c; }
inline void app(std::string &o, int v) { o += std::to_string(v); }
inline void app(std::string &o, long v) { o += std::to_string(v); }
inline void app(std::string &o, long long v) { o += std::to_string(v); }
inline void app(std::string &o, unsigned long v) { o += std::to_string(v); }

// `cat("a", n, "b")`: the Python's `%` formatting, spelled once.
template <class... T> std::string cat(T &&...t) {
    std::string o;
    (app(o, std::forward<T>(t)), ...);
    return o;
}

const std::set<std::string> C_KW = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "int", "long", "register", "return", "short", "signed", "sizeof",
    "static", "struct", "switch", "typedef", "union", "unsigned", "void",
    "volatile", "while",
};

// `b_` + a C-safe spelling of a BRAT name.
std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// An `else {}` that binds nothing is Python's empty list, which is no else.
const Stmt *real_else(const Stmt &s) {
    if (!s.els) return nullptr;
    if (s.els->k == SK::Block && s.els->body.empty()) return nullptr;
    return s.els.get();
}

// The head of a dotted name: what a `let a.b` binds.
std::string head_of(const std::string &name) { return name.substr(0, name.find('.')); }

// A parameter list for the bodies that take none: a defer, a spawn.
const std::vector<Param> &no_params() {
    static const std::vector<Param> empty;
    return empty;
}

// Names an expression assigns to. The parser has no assignment expression, so
// this walk collects nothing today; it is here because the Python runs it.
void expr_set_vars(const Expr &e, std::set<std::string> &acc) {
    for (const auto &k : e.kids) expr_set_vars(*k, acc);
    if (e.fn) {
        for (const auto &p : e.fn->params)
            if (p.def) expr_set_vars(*p.def, acc);
    }
}

ExprP clone(const Expr &e);
StmtP clone_stmt(const Stmt &s);

ExprP clone(const Expr &e) {
    auto c = std::make_unique<Expr>();
    c->k = e.k;
    c->i = e.i;
    c->f = e.f;
    c->is_float = e.is_float;
    c->flag = e.flag;
    c->a = e.a;
    c->b = e.b;
    c->names = e.names;
    for (const auto &k : e.kids) c->kids.push_back(clone(*k));
    if (e.fn) {
        auto f = std::make_unique<FnDecl>();
        f->name = e.fn->name;
        f->ret = e.fn->ret;
        f->tparams = e.fn->tparams;
        for (const auto &p : e.fn->params) {
            Param q;
            q.name = p.name;
            q.mode = p.mode;
            q.ann = p.ann;
            q.def = p.def ? clone(*p.def) : nullptr;
            f->params.push_back(std::move(q));
        }
        for (const auto &b : e.fn->body) f->body.push_back(clone_stmt(*b));
        c->fn = std::move(f);
    }
    return c;
}

FnDecl clone_fn(const FnDecl &f);

StmtP clone_stmt(const Stmt &s) {
    auto c = std::make_unique<Stmt>();
    c->k = s.k;
    c->line = s.line;
    c->a = s.a;
    c->ann = s.ann;
    c->mut = s.mut;
    c->konst = s.konst;
    if (s.e1) c->e1 = clone(*s.e1);
    if (s.e2) c->e2 = clone(*s.e2);
    if (s.e3) c->e3 = clone(*s.e3);
    for (const auto &b : s.body) c->body.push_back(clone_stmt(*b));
    for (const auto &b : s.body2) c->body2.push_back(clone_stmt(*b));
    if (s.els) c->els = clone_stmt(*s.els);
    for (const auto &p : s.params) {
        Param q;
        q.name = p.name;
        q.mode = p.mode;
        q.ann = p.ann;
        q.def = p.def ? clone(*p.def) : nullptr;
        c->params.push_back(std::move(q));
    }
    c->names = s.names;
    c->variants = s.variants;
    for (const auto &f : s.fns) c->fns.push_back(clone_fn(f));
    c->sigs = s.sigs;
    c->fields = s.fields;
    c->opt1 = s.opt1;
    c->ext.lib = s.ext.lib;
    c->ext.name = s.ext.name;
    c->ext.ret = s.ext.ret;
    c->ext.varargs = s.ext.varargs;
    c->ext.owned = s.ext.owned;
    c->ext.checked = s.ext.checked;
    for (const auto &p : s.ext.params) {
        Param q;
        q.name = p.name;
        q.mode = p.mode;
        q.ann = p.ann;
        q.def = p.def ? clone(*p.def) : nullptr;
        c->ext.params.push_back(std::move(q));
    }
    return c;
}

FnDecl clone_fn(const FnDecl &s) {
    FnDecl f;
    f.name = s.name;
    f.ret = s.ret;
    f.tparams = s.tparams;
    for (const auto &p : s.params) {
        Param q;
        q.name = p.name;
        q.mode = p.mode;
        q.ann = p.ann;
        q.def = p.def ? clone(*p.def) : nullptr;
        f.params.push_back(std::move(q));
    }
    for (const auto &b : s.body) f.body.push_back(clone_stmt(*b));
    return f;
}

std::string cname(const std::string &n0) {
    std::string n;
    for (char c : n0) {
        if (c == '.') {
            n.push_back('_');
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') {
            n.push_back(c);
        } else {
            n.push_back('_');
        }
    }
    if (n.empty() || (n[0] >= '0' && n[0] <= '9')) n.insert(n.begin(), '_');
    if (C_KW.count(n)) n.push_back('_');
    return "b_" + n;
}

// C name of an FFI struct or member. The source name is used as written because
// the layout must match the library's, but a C keyword or a name the runtime
// already declares (`Val`) cannot be spelled in generated C, so every such name
// gets the same prefix. `b_` names are all taken by cname, so it cannot meet one.
std::string ffi_c(const std::string &n) { return "bf_" + n; }

// Integer widths whose C result widens to `int64_t` on the way back.
bool ffi_int_result(const std::string &t) {
    static const std::set<std::string> s = {"i8", "u8", "i16", "u16", "i32", "u32", "i64"};
    return s.count(t) > 0;
}

// One table for the whole FFI boundary, kept in step with _FFI_C in
// codegen_c.py: two ladders that disagree (a declaration saying `int64_t`, a
// call site saying `short`) is how an ABI bug is born -- it compiles, links and
// corrupts the next stack slot. A width that is not here is refused instead of
// being spelled as a machine word.
static const std::map<std::string, std::string> &ffi_scalar_c() {
    static const std::map<std::string, std::string> t = {
        {"i8", "signed char"}, {"u8", "unsigned char"},
        {"i16", "short"},      {"u16", "unsigned short"},
        {"i32", "int"},        {"u32", "unsigned"},
        {"i64", "int64_t"},    {"u64", "uint64_t"},
        {"usize", "size_t"},   {"f32", "float"},
        {"f64", "double"},     {"str", "const char *"},
        {"ptr", "void *"},
    };
    return t;
}

// C literal body for a string: escapes only what has to be escaped. Bytes above
// 0x7F are hex-escaped one UTF-8 character at a time, and a hex escape is
// greedy, so each character is closed off with `""`.
std::string clit(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\\') {
            o += "\\\\";
        } else if (c == '"') {
            o += "\\\"";
        } else if (c == '\n') {
            o += "\\n";
        } else if (c == '\r') {
            o += "\\r";
        } else if (c == '\t') {
            o += "\\t";
        } else if (c == '?') {
            // A trigraph: under strict ISO C `??/` is a backslash, so a literal
            // holding `??/` would not mean what it says. Adjacent literals are
            // one literal, so splitting the character keeps its bytes and stops
            // the pair from forming. Mirrors `_clit` in codegen_c.py.
            o += "?\"\"";
        } else if (c >= 32 && c < 127) {
            o.push_back(static_cast<char>(c));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            o += buf;
            bool more = i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80;
            if (!more) o += "\"\"";
        }
    }
    return o;
}

bool ident_start(char c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool ident_char(char c) {
    return ident_start(c) || (c >= '0' && c <= '9');
}

// `{name}` / `{a.b.c}` placeholders, split into ("lit", text) / ("var", path).
std::vector<std::pair<std::string, std::string>> split_interp(const std::string &s) {
    std::vector<std::pair<std::string, std::string>> parts;
    std::string buf;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '{') {
            size_t j = i + 1;
            bool ok = false;
            if (j < s.size() && ident_start(s[j])) {
                j++;
                while (j < s.size() && ident_char(s[j])) j++;
                size_t save = j;
                while (j < s.size() && s[j] == '.' && j + 1 < s.size() && ident_start(s[j + 1])) {
                    j += 2;
                    while (j < s.size() && ident_char(s[j])) j++;
                }
                if (j < s.size() && s[j] == '}') {
                    ok = true;
                } else {
                    j = save;
                }
            }
            if (ok) {
                if (!buf.empty()) {
                    parts.emplace_back("lit", buf);
                    buf.clear();
                }
                parts.emplace_back("var", s.substr(i + 1, j - i - 1));
                i = j + 1;
                continue;
            }
        }
        buf.push_back(s[i]);
        i++;
    }
    if (!buf.empty()) parts.emplace_back("lit", buf);
    return parts;
}

// `_cstr_path`: the two characters that can end the literal, and nothing else.
// The backslash is escaped first, so a path holding `\n` does not reach C as a
// newline escape; a byte above 0x7F is left as it is, because a UTF-8 path is
// valid inside a C literal while a hex escape would change its bytes.
std::string cstr_path(const std::string &p) {
    std::string o;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '\\') {
            o += "\\\\";
        } else if (p[i] == '"') {
            o += "\\\"";
        } else {
            o.push_back(p[i]);
        }
    }
    return o;
}

// The C temp behind `vvecv(_v1)` / `vstruct(_m2)`, or empty.
std::optional<std::string> container_tmp(const std::string &val) {
    for (const char *pre : {"vvecv(", "vstruct("}) {
        std::string p(pre);
        if (val.size() > p.size() + 1 && val.compare(0, p.size(), p) == 0 &&
            val.back() == ')') {
            return val.substr(p.size(), val.size() - p.size() - 1);
        }
    }
    return std::nullopt;
}

// The C temp that owns a value an operand built, if the operand built one. Only a
// call: a container literal's temp is a `Vec *`, released by the code that built
// it, and `vdrop_temp` takes a `Val`.
std::optional<std::string> owned_temp(const Expr &e, const std::string &val) {
    if (e.k == EK::Call || e.k == EK::MCall || e.k == EK::Closure)
        return (!val.empty() && val[0] == '_') ? std::optional<std::string>(val) : std::nullopt;
    // `"a" + "b" == "ab"`: the `+` binds its result to a temp, and the comparison
    // that consumes it is the end of its life -- nobody else names it. Only `+`:
    // `-`, `*`, `/` and `%` answer with numbers, which are no heap value.
    if (e.k == EK::Bin && e.a == "+")
        return (!val.empty() && val[0] == '_') ? std::optional<std::string>(val) : std::nullopt;
    return std::nullopt;
}


StmtList view(const std::vector<StmtP> &v) {
    StmtList o;
    for (const auto &s : v) {
        if (s->k == SK::Block) {
            StmtList sub = view(s->body);
            o.insert(o.end(), sub.begin(), sub.end());
        } else {
            o.push_back(s.get());
        }
    }
    return o;
}

ExprList ptrs(const std::vector<ExprP> &v, size_t from = 0) {
    ExprList o;
    for (size_t i = from; i < v.size(); i++) o.push_back(v[i].get());
    return o;
}

// The `.` name of a var/field chain, when the expression is one.
std::optional<std::string> name_path(const Expr &e) {
    if (e.k == EK::Var) return e.a;
    if (e.k == EK::Field) {
        auto b = name_path(*e.kids[0]);
        if (!b) return std::nullopt;
        return *b + "." + e.b;
    }
    return std::nullopt;
}

void expr_vars(const Expr &e, Names &acc);
void stmt_vars(const Stmt &s, Names &acc);

void expr_vars(const Expr &e, Names &acc) {
    if (e.k == EK::Var) acc.insert(e.a);
    for (const auto &k : e.kids) expr_vars(*k, acc);
    if (e.fn) {
        for (const auto &p : e.fn->params)
            if (p.def) expr_vars(*p.def, acc);
        for (const auto &s : e.fn->body) stmt_vars(*s, acc);
    }
}

// The names a statement binds anywhere below it, in the order the reference walks.
void stmt_vars(const Stmt &s, Names &acc) {
    switch (s.k) {
    case SK::Let: case SK::LetTmp:
    case SK::Set:
        acc.insert(s.a.substr(0, s.a.find('.')));
        break;
    case SK::ForIter:
        acc.insert(s.a);
        for (const auto &t : view(s.body)) stmt_vars(*t, acc);
        break;
    case SK::ForRange:
        acc.insert(s.a);
        for (const auto &t : view(s.body)) stmt_vars(*t, acc);
        break;
    case SK::If:
        for (const auto &t : view(s.body)) stmt_vars(*t, acc);
        if (const Stmt *els = real_else(s)) {
            if (els->k == SK::Block) {
                for (const auto &t : view(els->body)) stmt_vars(*t, acc);
            } else {
                stmt_vars(*els, acc);
            }
        }
        break;
    case SK::While:
        for (const auto &t : view(s.body)) stmt_vars(*t, acc);
        break;
    case SK::Try:
        for (const auto &t : view(s.body)) stmt_vars(*t, acc);
        for (const auto &t : view(s.body2)) stmt_vars(*t, acc);
        if (s.opt1) acc.insert(*s.opt1);
        break;
    case SK::Spawn: case SK::Scope: case SK::Unsafe:
        for (const auto &t : view(s.body)) stmt_vars(*t, acc);
        break;
    case SK::Block:
        for (const auto &t : view(s.body)) stmt_vars(*t, acc);
        break;
    default:
        break;
    }
}

// Every name a statement list mentions as a value: what a `spawn` child must
// copy so that it holds the frame the parent had at the launch. Iterative --
// a flat `1+1+...+1` chain is a tree thousands deep, and this walk runs on it.
void refs_in_stmt(const Stmt &s, Names &acc);

void refs_in_expr(const Expr *e, Names &acc) {
    if (!e) return;
    std::vector<const Expr *> stack{e};
    while (!stack.empty()) {
        const Expr *cur = stack.back();
        stack.pop_back();
        if (cur->k == EK::Var) {
            acc.insert(cur->a.substr(0, cur->a.find('.')));
            continue;
        }
        for (const auto &kid : cur->kids)
            if (kid) stack.push_back(kid.get());
        if (cur->fn)
            for (const auto &st : view(cur->fn->body))
                if (st) refs_in_stmt(*st, acc);
    }
}

// Names a body binds itself: `let`, and the variables of a `for`. A parameter
// next to one of these shadows a script-level binding of the same name.
void bound_in(const StmtList &body, Names &acc) {
    std::vector<const Stmt *> stack(body.begin(), body.end());
    while (!stack.empty()) {
        const Stmt *s = stack.back();
        stack.pop_back();
        if (!s) continue;
        if ((s->k == SK::Let || s->k == SK::LetTmp || s->k == SK::ForRange ||
             s->k == SK::ForIter) && !s->a.empty())
            acc.insert(s->a.substr(0, s->a.find('.')));
        for (const auto &b : s->body) stack.push_back(b.get());
        for (const auto &b : s->body2) stack.push_back(b.get());
        if (s->els) stack.push_back(s->els.get());
        for (const auto &f : s->fns)
            for (const auto &b : f.body) stack.push_back(b.get());
    }
}

void refs_in(const StmtList &body, Names &acc) {
    std::vector<const Stmt *> stack;
    for (const Stmt *s : body)
        if (s) stack.push_back(s);
    while (!stack.empty()) {
        const Stmt *s = stack.back();
        stack.pop_back();
        refs_in_stmt(*s, acc);
    }
}

void refs_in_stmt(const Stmt &s, Names &acc) {
    for (const ExprP *e : {&s.e1, &s.e2, &s.e3})
        if (*e) refs_in_expr(e->get(), acc);
    if (!s.a.empty() && s.k == SK::Set) acc.insert(s.a.substr(0, s.a.find('.')));
    if (s.els) refs_in_stmt(*s.els, acc);
    for (const auto &b : s.body)
        if (b) refs_in_stmt(*b, acc);
    for (const auto &b : s.body2)
        if (b) refs_in_stmt(*b, acc);
    for (const auto &p : s.params)
        if (p.def) refs_in_expr(p.def.get(), acc);
    for (const auto &f : s.fns) {
        for (const auto &p : f.params)
            if (p.def) refs_in_expr(p.def.get(), acc);
        for (const auto &b : f.body)
            if (b) refs_in_stmt(*b, acc);
    }
}

void collect_vars(const StmtList &body, Names &acc) {
    for (const Stmt *s : body) stmt_vars(*s, acc);
}

// In source order, duplicates included: that is the order blocks release in.
std::vector<std::string> block_let_names(const StmtList &body) {
    std::vector<std::string> out;
    for (const Stmt *s : body)
        if (s->k == SK::Let || s->k == SK::LetTmp) out.push_back(s->a.substr(0, s->a.find('.')));
    return out;
}


bool block_defers(const StmtList &body) {
    for (const Stmt *s : body)
        if (s->k == SK::Defer) return true;
    return false;
}

// A binding as the emitter sees it: the C name, and what its death means.
enum class Own { Copy, Borrow, Owned };

struct Sym {
    std::string cn;
    Own own = Own::Copy;
    // The element of a `ptr[T]` binding: "" for a plain `ptr`, the struct or
    // scalar name otherwise. `p[i]` reads through it, so a dereference needs no
    // second argument and no string type name.
    std::string ptr;
    // The element of a `slice[T]` binding: a window over C records needs the
    // layout to hand back a record instead of an address.
    std::string elem;
};

// One entry in `fns`: either a function body or an enum constructor.
struct FnEntry {
    const FnDecl *decl = nullptr;
    std::string ctor_tag;
    int ctor_arity = 0;
    bool ctor = false;
};

struct OrderItem {
    bool meth = false;
    std::string a, b;
};

struct Ext {
    std::string name, lib;
    const std::vector<Param> *params = nullptr;
    std::optional<std::string> ret, varargs;
    bool checked = false;
};

struct Closure {
    const std::vector<Param> *params = nullptr;
    StmtList body;
    std::set<std::string> caps;
};

struct Late {
    std::string name;
    const FnDecl *decl = nullptr;
};

struct Cb {
    std::string name, ret;
    std::vector<std::string> params;
};

// A value and the statements that must run before it.
struct V {
    std::vector<std::string> pre;
    std::string val;
};

// Methods whose result is a heap value the caller owns. `parse_int` and
// `parse_float` answer with an `Ok`/`Err` tag, which is as much a heap value as a
// string: left out, `print("8080".parse_int()?)` dropped the payload and kept the
// tag, 152 bytes a run.
const std::set<std::string> HEAP_METHODS = {
    "upper", "lower", "trim", "replace", "repeat", "copy", "join", "split",
    "split_once", "lines", "chars", "slice", "map", "filter", "keys", "vals",
    "items", "entries", "values", "recv", "parse_int", "parse_float",
    "rev",
};

// Builtins that hand back a fresh value the caller owns: a new vector, a new
// string, or an Ok/Err tag built on the spot. Missing one of these is not a
// type error anywhere -- it just means the binding is classified as an alias,
// never released, and LeakSanitizer reports it once per call. `args()` did.
const std::set<std::string> HEAP_CALLS = {
    "str", "own", "str_from_ptr", "args", "read_file", "write_file",
    "tcp_listen", "tcp_connect", "tcp_accept", "tcp_recv", "sbuf", "sbuf_str",
    "chan"};

struct Emitter {
    int tmp = 0;
    std::map<std::string, FnEntry> fns;
    std::vector<OrderItem> order;
    std::map<std::pair<std::string, std::string>, int> mtab;
    std::map<std::string, std::string> vtab;
    std::map<std::string, std::vector<std::string>> traits;
    std::vector<Ext> externs;
    std::map<std::string, size_t> ext_ix;
    std::set<std::string> errno_externs;
    std::map<std::string, const std::vector<StructField> *> structs;
    std::set<std::string> packed;
    std::vector<std::string> struct_order;
    std::set<std::string> enums;
    std::set<std::string> exported;
    std::map<std::string, std::optional<std::string>> fn_ret;
    std::map<std::string, int> codes;
    std::set<std::string> user_meths;
    std::vector<Closure> clos;
    std::vector<Late> late;
    StmtList top_exe;
    std::set<std::string> global_lets;
    std::vector<std::string> out;
    std::map<std::string, Sym> sym;
    std::optional<std::pair<std::string, std::set<std::string>>> cap;
    std::vector<std::string> dfb;
    std::set<std::string> dfb_locals;
    int try_n = 0;
    std::set<std::string> alias_ret;
    std::set<std::string> cur_params;  // parameters of the function being emitted
    std::vector<std::string> scopes;
    // Lexical boundaries for each loop body: early exits must drain every
    // defer frame and task scope entered since the loop began.
    std::vector<std::pair<size_t, size_t>> loop_exit_depths;
    std::string fin = "__fin_top";
    std::optional<std::string> err_goto, err_base;
    std::map<std::string, FnEntry> fast;
    std::map<std::string, std::string> fastc, fastkinds;
    std::set<std::string> fails;
    // -O1: the nodes ir::analyze proved need no check, and the proofs to print.
    int opt = 0;
    bool opt_done = false;
    std::set<const Expr *> unchecked;
    std::vector<ir::Proof> *proofs = nullptr;
    std::map<std::string, std::set<std::string>> fastvec;
    std::string fkind = "int64_t";
    std::map<std::string, std::string> fsym;
    bool need_idiv = false, need_imod = false, need_fdiv = false, need_fmod = false;
    bool need_viat = false, need_vset = false;
    std::set<std::string> fvecvars;
    int nesting = 0;
    int nested_seq = 0;
    std::map<std::string, std::string> fn_alias;
    std::set<std::string> assigned;
    std::set<std::string> reclaim, relet;
    // BRAT_MOVE_RT=0 turns the drops off, the bisect knob codegen_c.py has: a
    // program emitted that way leaks on purpose so an emitted change can be
    // diffed against older output.
    bool move_rt = std::getenv("BRAT_MOVE_RT") == nullptr ||
                   std::strcmp(std::getenv("BRAT_MOVE_RT"), "0") != 0;
    std::set<std::string> moved, captured, shared, dropped, alias_temps;
    int clos_done = 0, clos_declared = 0, late_done = 0, late_declared = 0;
    std::string base;
    // A defer body is a statement built here; `clos` points into this deque.
    std::deque<Stmt> synth;
    // Trees built on the fly for a static path (`Type.method(...)`); a closure
    // registered from one points into it, so it has to outlive the walk.
    std::deque<ExprP> owned;
    std::vector<Cb> cb_wrappers;
    std::vector<std::string> out_cache;

    /* Temps are plain `Val`, never `volatile`: a raise sets a thread-local flag
       and the frame leaves by `goto`, so no longjmp can cross them, and volatile
       cost a fifth of the time in a dynamic loop. */
    std::string new_tmp(const char *p = "t") { tmp++; return cat("_", p, tmp); }
    void ln(const std::string &ind, const std::string &s) { out.push_back(ind + s); }
    int fn_code(const std::string &key) {
        auto it = codes.find(key);
        if (it != codes.end()) return it->second;
        int c = static_cast<int>(codes.size());
        codes[key] = c;
        return c;
    }
    std::string cfn(const std::string &key) { return cat("bf_c", fn_code(key)); }
    std::string fn_key(const std::string &name) {
        auto it = fn_alias.find(name);
        return it == fn_alias.end() ? name : it->second;
    }

    // A callback slot this call cannot honour, refused where it is written.
    //
    // Writing `brat_cb_<name>` for a name with no BRAT function behind it is C
    // that does not compile; writing it for a function of the wrong arity or
    // the wrong widths is C that compiles and calls through a lie. Both are
    // refused here, in the typer's words, so `cc` and `check` say one thing.
    void check_cb_arg(const std::string &ann, const Expr &a, const std::string &name, int pos) {
        if (a.k != EK::Str)
            throw EmitError(cat("arg ", pos, " of `", name, "`: callback must be a function name"));
        auto it = fns.find(fn_key(a.a));
        if (it == fns.end() || it->second.ctor || it->second.decl == nullptr)
            throw EmitError(cat("unknown callback `", a.a, "`"));
        const std::vector<Param> &params = it->second.decl->params;
        std::string want = normalize_abi_type(ann);
        auto sig = callback_signature(want);
        if (!sig)
            throw EmitError(cat("callback `", a.a, "` does not have a verifiable C ABI signature"));
        if (params.size() != sig->first.size())
            throw EmitError(cat("callback `", a.a, "` takes ", static_cast<int>(params.size()),
                                " parameter(s), ", want, " wants ",
                                static_cast<int>(sig->first.size())));
        std::string actual = "fn(";
        bool match = true;
        for (size_t i = 0; i < params.size(); i++) {
            if (i) actual += ",";
            std::string one = normalize_abi_type(params[i].ann);
            actual += one;
            match = match && one == normalize_abi_type(sig->first[i]);
        }
        auto ret_it = fn_ret.find(fn_key(a.a));
        std::string ret = ret_it == fn_ret.end() ? "none" : normalize_abi_type(ret_it->second);
        actual += ")->" + ret;
        if (!match || ret != normalize_abi_type(sig->second))
            throw EmitError(cat("callback `", a.a, "` has signature ", actual,
                                ", expected ", want));
    }
    std::string err_bail();
    std::string err_check_line(const std::vector<std::string> &drops = {});
    void emit_err_check(const std::string &ind, const std::vector<std::string> &drops = {});

    bool int_expr(const Expr &e, const std::map<std::string, FnEntry> &cand, const std::string &kind,
                  const std::set<std::string> &intvars, const std::set<std::string> &vecvars);
    bool intish(const Expr &e, const std::string &kind, const std::set<std::string> &intvars);
    bool booly(const Expr &e);
    bool print_ok(const Expr &e, const std::map<std::string, FnEntry> &cand, const std::string &kind,
                  const std::set<std::string> &intvars);
    // `returned`, when given, collects one entry per `return`: true when the
    // value is float. A `f64` fast path that never returns one is refused below,
    // so `fn a(x: f64) -> f64 { return 3 }` prints `3` on both backends.
    bool int_body(const StmtList &body, const std::map<std::string, FnEntry> &cand, const std::string &kind,
                  std::set<std::string> intvars, const std::set<std::string> &vecvars,
                  std::vector<bool> *returned = nullptr);
    void compute_fast();
    void run_opt();
    bool has_setidx(const StmtList &body);
    std::set<std::string> ops_in(const StmtList &body);
    std::string fx(const Expr &e);
    // A fast-path expression that can panic must not feed a value on: fchk
    // evaluates it into a temp and bails while the error is still pending.
    bool fraise(const Expr &e);
    // Names whose body can leave an error pending (raise, ?, div, index, unwrap).
    // A fast-path call to one of them needs a verr_pending() check; a call to a
    // plain arithmetic helper does not. Mirrors effects.may_fail in Python.
    std::set<std::string> may_fail();
    std::string fchk(const Expr &e, const std::string &ind);
    void fprint(const ExprList &args, const std::string &ind);
    void fst(const Stmt &s, const std::string &ind);
    std::set<std::string> drop_recursive_fast(const std::map<std::string, FnEntry> &cand);
    void emit_fast_fn(const std::string &name);

    void collect_prog(const std::vector<StmtP> &prog);
    void collect_top(const Stmt &s);
    void collect_nested_fn(const FnDecl &f);
    void collect_nested_expr(const Expr &e);
    void collect_nested(const Stmt &s);

    V ex(const Expr &e);
    V ex_str(const std::string &s);
    std::string path_ref(const std::string &path);
    V ex_bin(const Expr &e);
    V ex_plugin(const std::string &name, const ExprList &args);
    V ex_slice(const std::string &name, const ExprList &args);
    V ex_vec(const ExprList &items);
    V ex_maplit(const ExprList &kids);
    V ex_struct(const Expr &e);
    V args_vec(const ExprList &args);
    void emit_loop_drops(const StmtList &body, const std::string &ind);
    std::set<std::string> heap_returns();
    V call_result(std::vector<std::string> pre, const std::string &vec, const std::string &call);
    V read_only(const Expr &arg, const std::string &fn);
    V read_only_call(const Expr &arg, const std::function<std::string(const std::string &)> &mk);
    V release_lit_args(const std::vector<std::string> &pre, const ExprList &nodes,
                       const std::vector<std::string> &vals, const std::string &call);
    V part_of_temp(const V &a, const Expr &src, const std::string &open,
                   const std::string &close);
    void need_arity(const ExprList &args, size_t n, const std::string &name);
    V ex_call(const Expr &e);
    V ex_tag(const std::string &tag, const ExprList &args);
    V ex_dotted(const std::string &name, const ExprList &args);
    std::vector<const Expr *> pad_defaults(const std::vector<Param> &params,
                                           std::vector<const Expr *> args, const std::string &name);
    V ex_direct(const std::string &name, std::vector<const Expr *> args);
    V ex_vcall(const std::string &name, const ExprList &args);
    V ex_mcall(const Expr &e);
    V ex_map_meth(const std::string &so, const std::vector<std::string> &po, const std::string &meth,
                  const ExprList &args);
    V ex_strvec_meth(const std::string &so, const std::vector<std::string> &po, const std::string &meth,
                     const ExprList &args);
    V ex_hof(const Expr &obj, const std::string &meth, const Expr *arg, const Expr *first);
    V ex_mapfilter(const Expr &obj, const std::string &meth, const Expr *arg);
    V ex_closure(const Expr &e);
    V ex_parfor(const Expr &e);

    void st(const Stmt &s, const std::string &ind);
    std::string var_def(const std::string &name, Own own, const std::string &ptr = "",
                        const std::string &elem = "");
    std::string slice_elem(const Expr &e);
    // `&x` inside the call being built: (name, c name, scratch, width)
    struct OutBind { std::string name, cn, scratch; int width; };
    std::vector<OutBind> out_binds;
    bool in_out_arg = false;
    std::pair<std::vector<std::string>, std::string> out_addr(const Expr &e, const std::string *ann);
    std::vector<std::string> out_flush();
    std::string ptr_elem(const Expr &base);
    V ex_ptr_idx(const std::string &elem, const Expr &base, const Expr &idx);
    void st_ptr_setidx(const std::string &elem, const Stmt &s, const std::string &ind);
    void emit_block(const StmtList &body, const std::string &ind);
    void drop_alias_temps(const StmtList &body, const std::string &ind);
    void emit_defer(const Expr &e, const std::string &ind);
    void emit_loop_exit(const std::string &k, const std::string &ind);
    std::string var_ref(const std::string &name);
    std::optional<std::string> place_root(const Expr &e);
    std::string keep(const Expr &e, const std::string &val);
    std::vector<std::string> capture(const std::string &cm, const std::string &var);
    Own own_of(const Expr &e);
    bool drop_allowed(const std::string &name);
    bool part_of_local(const Expr &e, const std::string &root);
    void emit_drop(const std::string &name, const std::string &ind);
    void emit_scope_drops(const std::string &ind);
    void emit_fn_drops(const std::string &ind, const std::string &skip = "",
                       bool at_return = false, const Names &keep = Names());
    std::vector<std::string> sorted_sym_names() const;

    void define_fn(const std::string &cname_, const std::vector<Param> &params, const StmtList &body,
                   const std::set<std::string> *caps, const std::string *fastraw,
                   const std::string *fastkind, const std::string *fastname);
    void emit_fn_by_key(const std::string &key);
    void emit_prelude_tables();
    void run_pipeline();
    void emit_late();
    void emit_main_open();
    void emit_main_close();
    std::string emit_program();
    std::string val_to_c_struct(const std::string &sname, const std::string &valexpr);
    std::string c_struct_to_val(const std::string &sname, const std::string &cexpr);
    void emit_forward_decls();
    void emit_closure_protos();
    void emit_closures_queue();
    void emit_prelude_headers();
};


static std::string fmt17g(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

static bool may_raise(const Expr &e);

bool Emitter::int_expr(const Expr &e, const std::map<std::string, FnEntry> &cand,
                       const std::string &kind, const std::set<std::string> &intvars,
                       const std::set<std::string> &vecvars) {
    switch (e.k) {
    case EK::Num:
        if (!e.is_float) return true;
        return kind == "f64";
    case EK::Bool:
        return true;
    case EK::Var:
        return !vecvars.count(e.a);
    case EK::Un:
        return (e.a == "!" || e.a == "-") && int_expr(*e.kids[0], cand, kind, intvars, vecvars);
    case EK::Move:
        return int_expr(*e.kids[0], cand, kind, intvars, vecvars);
    case EK::Idx:
        return kind == "i64v" && e.kids[0]->k == EK::Var && vecvars.count(e.kids[0]->a) &&
               intish(*e.kids[1], kind, intvars);
    case EK::Bin: {
        static const std::set<std::string> ops = {"+", "-", "*", "/", "%", "==", "!=", "<",
                                                  ">", "<=", ">=", "&&", "||"};
        if (!ops.count(e.a)) return false;
        if (!int_expr(*e.kids[0], cand, kind, intvars, vecvars)) return false;
        if (!int_expr(*e.kids[1], cand, kind, intvars, vecvars)) return false;
        if ((e.a == "/" || e.a == "%") && kind == "f64" && intish(*e.kids[0], kind, intvars) &&
            intish(*e.kids[1], kind, intvars))
            return false;
        if ((e.a == "&&" || e.a == "||") && (may_raise(*e.kids[0]) || may_raise(*e.kids[1])))
            return false;
        return true;
    }
    case EK::Call:
        if (e.a == "len" && kind == "i64v" && e.kids.size() == 1) {
            const Expr &a = *e.kids[0];
            if (a.k == EK::Var && vecvars.count(a.a)) return true;
        }
        {
            auto it = cand.find(e.a);
            if (it == cand.end() || fastkinds[e.a] != kind) return false;
            for (const auto &a : e.kids)
                if (!int_expr(*a, cand, kind, intvars, vecvars)) return false;
            return true;
        }
    default:
        return false;
    }
}

bool Emitter::intish(const Expr &e, const std::string &kind, const std::set<std::string> &intvars) {
    switch (e.k) {
    case EK::Num:
        return !e.is_float;
    case EK::Bool:
        return true;
    case EK::Var:
        return intvars.count(e.a);
    case EK::Un:
        return intish(*e.kids[0], kind, intvars);
    case EK::Move:
        return intish(*e.kids[0], kind, intvars);
    case EK::Idx:
        return kind == "i64v";
    case EK::Bin:
        if (e.a == "==" || e.a == "!=" || e.a == "<" || e.a == ">" || e.a == "<=" ||
            e.a == ">=" || e.a == "&&" || e.a == "||")
            return true;
        return intish(*e.kids[0], kind, intvars) && intish(*e.kids[1], kind, intvars);
    case EK::Call: {
        auto it = fastkinds.find(e.a);
        return kind == "i64" && it != fastkinds.end() && it->second == "i64";
    }
    default:
        return false;
    }
}

bool Emitter::print_ok(const Expr &e, const std::map<std::string, FnEntry> &cand,
                       const std::string &kind, const std::set<std::string> &intvars) {
    if (e.k != EK::Call || e.a != "print") return false;
    for (const auto &ap : e.kids) {
        const Expr &a = *ap;
        if (a.k == EK::Str) {
            for (const auto &p : split_interp(a.a))
                if (p.first == "var" && p.second.find('.') != std::string::npos) return false;
            continue;
        }
        if (booly(a)) return false;
        if (!int_expr(a, cand, kind, intvars, std::set<std::string>())) return false;
    }
    return true;
}

bool Emitter::int_body(const StmtList &body, const std::map<std::string, FnEntry> &cand,
                       const std::string &kind, std::set<std::string> intvars,
                       const std::set<std::string> &vecvars, std::vector<bool> *returned) {
    for (const Stmt *sp : body) {
        const Stmt &s = *sp;
        switch (s.k) {
        case SK::Let:
        case SK::LetTmp: {
            if (!int_expr(*s.e1, cand, kind, intvars, vecvars)) return false;
            if (intish(*s.e1, kind, intvars)) {
                intvars.insert(s.a);
            } else {
                intvars.erase(s.a);
            }
            break;
        }
        case SK::Set:
            if (s.a.find('.') != std::string::npos || !int_expr(*s.e1, cand, kind, intvars, vecvars))
                return false;
            if (intish(*s.e1, kind, intvars)) {
                intvars.insert(s.a);
            } else {
                intvars.erase(s.a);
            }
            break;
        case SK::SetIdx:
            if (kind != "i64v") return false;
            if (!(s.e1->k == EK::Var && vecvars.count(s.e1->a))) return false;
            if (!intish(*s.e2, kind, intvars) || !intish(*s.e3, kind, intvars)) return false;
            break;
        case SK::Return:
            if (!int_expr(*s.e1, cand, kind, intvars, vecvars)) return false;
            if (returned) returned->push_back(intish(*s.e1, kind, intvars));
            break;
        case SK::Expr:
            if (print_ok(*s.e1, cand, kind, intvars)) break;
            if (!int_expr(*s.e1, cand, kind, intvars, vecvars)) return false;
            break;
        case SK::If: {
            if (fraise(*s.e1)) return false;
            if (!int_expr(*s.e1, cand, kind, intvars, vecvars)) return false;
            if (!int_body(view(s.body), cand, kind, intvars, vecvars, returned)) return false;
            if (const Stmt *els = real_else(s)) {
                StmtList one = els->k == SK::Block ? view(els->body) : StmtList{els};
                if (!int_body(one, cand, kind, intvars, vecvars, returned)) return false;
            }
            break;
        }
        case SK::While:
            if (fraise(*s.e1)) return false;
            if (!int_expr(*s.e1, cand, kind, intvars, vecvars)) return false;
            if (!int_body(view(s.body), cand, kind, intvars, vecvars, returned)) return false;
            break;
        case SK::ForRange: {
            if (fraise(*s.e1) || fraise(*s.e2)) return false;
            if (!intish(*s.e1, kind, intvars) || !intish(*s.e2, kind, intvars)) return false;
            std::set<std::string> sub = intvars;
            sub.insert(s.a);
            if (!int_body(view(s.body), cand, kind, sub, vecvars, returned)) return false;
            break;
        }
        case SK::ForIter: {
            if (kind != "i64v") return false;
            if (!(s.e1->k == EK::Var && vecvars.count(s.e1->a))) return false;
            std::set<std::string> sub = intvars;
            sub.insert(s.a);
            if (!int_body(view(s.body), cand, kind, sub, vecvars, returned)) return false;
            break;
        }
        case SK::Break:
        case SK::Continue:
            break;
        default:
            return false;
        }
    }
    return true;
}

void Emitter::compute_fast() {
    std::map<std::string, std::string> kinds;
    fastvec.clear();
    for (const auto &[name, entry] : fns) {
        if (entry.ctor || entry.decl == nullptr) continue;
        const std::vector<Param> &params = entry.decl->params;
        std::vector<std::string> anns;
        bool ok = true;
        for (const auto &p : params) {
            if (p.ann && (*p.ann == "i64" || *p.ann == "f64" || *p.ann == "vec[i64]")) {
                anns.push_back(*p.ann);
            } else {
                ok = false;
            }
        }
        // A fast function is raw C over `int64_t`/`double`; a script-level `let`
        // is a `Val` in the file scope, so a body that names one cannot be raw C.
        // Excluding it here, before it is given a kind, is what makes the
        // fixpoint drop its callers too; a caller that stayed fast would call a
        // function that no longer exists. `refs_in` and not `collect_vars`: the
        // question is what the body reads, and `return total` binds nothing.
        std::set<std::string> body_names, own_names;
        refs_in(view(entry.decl->body), body_names);
        bound_in(view(entry.decl->body), own_names);
        bool names_global = false;
        for (const auto &n : body_names)
            if (global_lets.count(n) && !own_names.count(n)) names_global = true;
        if (names_global) continue;
        bool all_i64 = true;
        for (const auto &a : anns)
            if (a != "i64" && a != "vec[i64]") all_i64 = false;
        bool has_vec = std::find(anns.begin(), anns.end(), "vec[i64]") != anns.end();
        if (ok && !anns.empty() && has_vec && all_i64) {
            kinds[name] = "i64v";
            std::set<std::string> vs;
            for (const auto &p : params)
                if (p.ann && *p.ann == "vec[i64]") vs.insert(p.name);
            fastvec[name] = vs;
        } else if (ok && (anns.empty() || std::set<std::string>(anns.begin(), anns.end()).size() == 1)) {
            kinds[name] = anns.empty() ? "i64" : anns[0];
        } else {
            continue;
        }
    }
    fastkinds = kinds;
    fails = may_fail();
    std::map<std::string, FnEntry> cand = fns;
    for (auto it = cand.begin(); it != cand.end();) {
        if (it->second.ctor || it->second.decl == nullptr) {
            it = cand.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = cand.begin(); it != cand.end();) {
        if (!kinds.count(it->first)) {
            it = cand.erase(it);
        } else {
            ++it;
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = cand.begin(); it != cand.end();) {
            const std::string name = it->first;
            const std::vector<Param> &params = fns[name].decl->params;
            std::string kind = kinds[name];
            std::set<std::string> intvars;
            if (kind == "i64")
                for (const auto &p : params) intvars.insert(p.name);
            auto snapshot = cand;   // int_body gets the live set, the Python passes a copy
            std::vector<bool> returned;
            const bool ok = int_body(view(fns[name].decl->body), snapshot, kind, intvars,
                                     fastvec[name], &returned);
            bool any_int = false;
            for (bool r : returned) any_int = any_int || r;
            if (!ok || (kind == "f64" && any_int)) {
                it = cand.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
    for (const auto &n : drop_recursive_fast(cand)) cand.erase(n);
    fast = cand;
    fastkinds.clear();
    for (const auto &[n, e] : cand) fastkinds[n] = kinds[n];
    int i = 0;
    for (const auto &[n, e] : fast) fastc[n] = cat("bf_f", i++);
    for (const auto &[name, entry] : fast) {
        std::string kind = fastkinds[name];
        for (const auto &op : ops_in(view(entry.decl->body))) {
            if (op == "/") {
                if (kind == "i64" || kind == "i64v") {
                    need_idiv = true;
                } else {
                    need_fdiv = true;
                }
            }
            if (op == "%") {
                if (kind == "i64" || kind == "i64v") {
                    need_imod = true;
                } else {
                    need_fmod = true;
                }
            }
        }
        if (kind == "i64v") {
            need_viat = true;
            if (has_setidx(view(entry.decl->body))) need_vset = true;
        }
    }
}

void Emitter::run_opt() {
    if (opt_done) return;   // run_pipeline runs twice; the pass reads the same tree
    opt_done = true;
    // Only functions the fast path already lowered to plain i64 are modelled;
    // ir::analyze refuses anything else, and a refused function keeps its
    // checks. `bf_idiv`/`bf_imod` stay in the output even when nothing calls
    // them: the helpers are part of what S3 emitted.
    for (const auto &[name, entry] : fast) {
        if (fastkinds[name] != "i64") continue;
        ir::Result r;
        ir::analyze(name, entry.decl->params, entry.decl->body, r);
        unchecked.insert(r.unchecked.begin(), r.unchecked.end());
        if (proofs) proofs->insert(proofs->end(), r.proofs.begin(), r.proofs.end());
    }
}

bool Emitter::has_setidx(const StmtList &body) {
    for (const Stmt *s : body) {
        if (s->k == SK::SetIdx) return true;
        if (has_setidx(view(s->body))) return true;
        if (s->els && has_setidx({s->els.get()})) return true;
        if (has_setidx(view(s->body2))) return true;
    }
    return false;
}

void ops_walk(const Expr &e, std::set<std::string> &ops) {
    if (e.k == EK::Bin) ops.insert(e.a);
    for (const auto &k : e.kids) ops_walk(*k, ops);
    if (e.fn)
        for (const auto &p : e.fn->params)
            if (p.def) ops_walk(*p.def, ops);
}

std::set<std::string> Emitter::ops_in(const StmtList &body) {
    std::set<std::string> ops;
    std::function<void(const Stmt &)> ws = [&](const Stmt &s) {
        if (s.e1) ops_walk(*s.e1, ops);
        if (s.e2) ops_walk(*s.e2, ops);
        if (s.e3) ops_walk(*s.e3, ops);
        for (const Stmt *t : view(s.body)) ws(*t);
        for (const Stmt *t : view(s.body2)) ws(*t);
        if (s.els) ws(*s.els);
    };
    for (const Stmt *s : body) ws(*s);
    return ops;
}

// INT64_MIN has no C literal: `-9223372036854775808` parses as an unsigned
// constant with a unary minus on top, which gcc warns about and which is
// implementation-defined under `-Werror`. Spell it the way <stdint.h> does.
std::string clit64(int64_t n) {
    if (n == INT64_MIN) return "(-9223372036854775807LL - 1LL)";
    return cat("(", n, "LL)");
}

std::string Emitter::fx(const Expr &e) {
    bool dbl = fkind == "f64";
    switch (e.k) {
    case EK::Num:
        if (dbl) {
            if (!e.is_float) return cat("(", fmt17g(static_cast<double>(e.i)), ")");
            // `%.17g` of 1e10 is "10000000000", which C reads as an int: in an
            // f64 fast function `1e10 * 1e10` then multiplies two int64s and
            // overflows. A float literal keeps its point.
            std::string lit = fmt17g(e.f);
            if (lit.find('.') == std::string::npos && lit.find('e') == std::string::npos &&
                lit.find('E') == std::string::npos)
                lit += ".0";
            return cat("(", lit, ")");
        }
        return clit64(e.i);
    case EK::Bool:
        return dbl ? cat("(", e.flag ? "1" : "0", ")") : cat("(", e.flag ? 1 : 0, "LL)");
    case EK::Var:
        return fsym[e.a];
    case EK::Un: {
        std::string v = fx(*e.kids[0]);
        if (e.a == "!") return cat("(!(", v, "))");
        return dbl ? cat("(-(", v, "))") : cat("((int64_t)(0ULL - (uint64_t)(", v, ")))");
    }
    case EK::Move:
        return fx(*e.kids[0]);
    case EK::Idx:
        return cat("bf_viat(", fsym[e.kids[0]->a], ", ", fx(*e.kids[1]), ")");
    case EK::Bin: {
        std::string a = fx(*e.kids[0]), b = fx(*e.kids[1]);
        const std::string &op = e.a;
        if (op == "/" || op == "%") {
            if (!dbl && opt >= 1 && unchecked.count(&e)) return cat("(", a, " ", op, " ", b, ")");
            return cat(dbl ? (op == "/" ? "bf_fdiv" : "bf_fmod") : (op == "/" ? "bf_idiv" : "bf_imod"),
                       "(", a, ", ", b, ")");
        }
        if (op == "&&") return cat("(", a, " ? ", b, " : ", a, ")");
        if (op == "||") return cat("(", a, " ? ", a, " : ", b, ")");
        if (!dbl && (op == "+" || op == "-" || op == "*"))
            return cat("((int64_t)((uint64_t)(", a, ") ", op, " (uint64_t)(", b, ")))");
        return cat("(", a, " ", op, " ", b, ")");
    }
    case EK::Call:
        if (e.a == "len") return cat("((", fsym[e.kids[0]->a], ")->len)");
        {
            std::string args;
            for (size_t i = 0; i < e.kids.size(); i++) {
                if (i) args += ", ";
                args += fx(*e.kids[i]);
            }
            return cat(fastc[e.a], "(", args, ")");
        }
    default:
        throw EmitError("fast node");
    }
}

std::set<std::string> Emitter::may_fail() {
    std::function<bool(const Expr &)> touches = [&](const Expr &e) {
        if (e.k == EK::Quest || e.k == EK::Idx) return true;
        if (e.k == EK::Bin && (e.a == "/" || e.a == "%")) return true;
        if (e.k == EK::Call && (e.a == "raise" || e.a == "unwrap" || e.a == "assert" ||
                                e.a == "exit"))
            return true;
        for (const auto &k : e.kids)
            if (touches(*k)) return true;
        return false;
    };
    std::set<std::string> fail;
    std::map<std::string, std::set<std::string>> edges;
    for (const auto &[name, entry] : fns) {
        if (entry.ctor) continue;
        if (entry.decl == nullptr) {
            fail.insert(name);          // extern: anything can happen out there
            continue;
        }
        StmtList body = view(entry.decl->body);
        std::function<void(const Stmt &)> ws = [&](const Stmt &s) {
            for (const Expr *e : {s.e1.get(), s.e2.get(), s.e3.get()})
                if (e && touches(*e)) fail.insert(name);
            for (const Stmt *t : view(s.body)) ws(*t);
            for (const Stmt *t : view(s.body2)) ws(*t);
            if (s.els) ws(*s.els);
        };
        for (const Stmt *s : body) ws(*s);
        std::function<void(const Expr &)> cs = [&](const Expr &e) {
            if (e.k == EK::Call) edges[name].insert(e.a);
            for (const auto &k : e.kids) cs(*k);
        };
        std::function<void(const Stmt &)> cw = [&](const Stmt &s) {
            for (const Expr *e : {s.e1.get(), s.e2.get(), s.e3.get()})
                if (e) cs(*e);
            for (const Stmt *t : view(s.body)) cw(*t);
            for (const Stmt *t : view(s.body2)) cw(*t);
            if (s.els) cw(*s.els);
        };
        for (const Stmt *s : body) cw(*s);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &[name, callees] : edges) {
            if (fail.count(name)) continue;
            for (const auto &c : callees)
                if (fail.count(c)) {
                    fail.insert(name);
                    changed = true;
                    break;
                }
        }
    }
    return fail;
}

bool Emitter::fraise(const Expr &e) {
    if (e.k == EK::Bin && (e.a == "/" || e.a == "%")) return true;
    if (e.k == EK::Idx) return true;
    if (e.k == EK::Call) {
        if (e.a == "len") return false;   // a length is a field read, it cannot fail
        return !fastc.count(e.a) || fails.count(e.a) > 0;
    }
    for (const auto &k : e.kids)
        if (fraise(*k)) return true;
    return false;
}

std::string Emitter::fchk(const Expr &e, const std::string &ind) {
    if (!fraise(e)) return fx(e);
    const bool dbl = fkind == "f64";
    std::string t = new_tmp("t");
    ln(ind, cat(dbl ? "double " : "int64_t ", t, " = ", fx(e), ";"));
    ln(ind, cat("if (verr_pending()) return ", dbl ? "0.0" : "0", ";"));
    return t;
}

void Emitter::fprint(const ExprList &args, const std::string &ind) {
    bool dbl = fkind == "f64";
    std::string fmt, vals;
    for (size_t ai = 0; ai < args.size(); ai++) {
        const Expr &a = *args[ai];
        if (ai) fmt.push_back(' ');
        if (a.k == EK::Str) {
            for (const auto &[kind, val] : split_interp(a.a)) {
                if (kind == "lit") {
                    for (char c : val) {
                        if (c == '%') fmt += "%%";
                        else fmt.push_back(c);
                    }
                } else {
                    fmt += dbl ? "%s" : "%lld";
                    vals += dbl ? cat(", vfmt_double(", fsym[val], ")")
                                : cat(", (long long)(", fsym[val], ")");
                }
            }
        } else if (a.k == EK::Num && a.is_float) {
            fmt += "%s";
            vals += cat(", vfmt_double(", repr_f64(a.f), ")");
        } else if (dbl) {
            fmt += "%s";
            vals += cat(", vfmt_double(", fchk(a, ind), ")");
        } else {
            fmt += "%lld";
            vals += cat(", (long long)(", fchk(a, ind), ")");
        }
    }
    ln(ind, cat("printf(\"", clit(fmt), "\\n\"", vals, ");"));
}

void Emitter::fst(const Stmt &s, const std::string &ind) {
    switch (s.k) {
    case SK::Let:
    case SK::LetTmp:
    case SK::Set:
        ln(ind, cat(fsym[s.a], " = ", fchk(*s.e1, ind), ";"));
        break;
    case SK::Return:
        ln(ind, cat("return ", fchk(*s.e1, ind), ";"));
        break;
    case SK::Expr:
        if (s.e1->k == EK::Call && s.e1->a == "print") {
            fprint(ptrs(s.e1->kids), ind);
        } else {
            ln(ind, cat("(void)(", fchk(*s.e1, ind), ");"));
        }
        break;
    case SK::If: {
        ln(ind, cat("if (", fx(*s.e1), ") {"));
        for (const Stmt *t : view(s.body)) fst(*t, ind + "  ");
        if (const Stmt *els = real_else(s)) {
            ln(ind, "} else {");
            StmtList one = els->k == SK::Block ? view(els->body) : StmtList{els};
            for (const Stmt *t : one) fst(*t, ind + "  ");
        }
        ln(ind, "}");
        break;
    }
    case SK::While:
        ln(ind, cat("while (", fx(*s.e1), ") {"));
        for (const Stmt *t : view(s.body)) fst(*t, ind + "  ");
        ln(ind, "}");
        break;
    case SK::ForRange: {
        std::string kk = new_tmp("k"), lo = new_tmp("lo"), hi = new_tmp("hi");
        ln(ind, cat("int64_t ", lo, " = ", fx(*s.e1), ", ", hi, " = ", fx(*s.e2), ";"));
        ln(ind, cat("for (int64_t ", kk, " = ", lo, "; ", kk, " < ", hi, "; ++", kk, ") {"));
        ln(ind + "  ", cat(fsym[s.a], " = ", kk, ";"));
        for (const Stmt *t : view(s.body)) fst(*t, ind + "  ");
        ln(ind, "}");
        break;
    }
    case SK::ForIter: {
        std::string vv = fsym[s.e1->a];
        std::string kk = new_tmp("k");
        ln(ind, cat("for (int64_t ", kk, " = 0; ", kk, " < ", vv, "->len; ++", kk, ") {"));
        ln(ind + "  ", cat(fsym[s.a], " = bf_viat(", vv, ", ", kk, ");"));
        ln(ind + "  ", "if (verr_pending()) return 0;");
        for (const Stmt *t : view(s.body)) fst(*t, ind + "  ");
        ln(ind, "}");
        break;
    }
    case SK::SetIdx:
        ln(ind, cat("bf_vset(", fsym[s.e1->a], ", ", fchk(*s.e2, ind), ", ", fchk(*s.e3, ind), ");"));
        break;
    case SK::Break:
        ln(ind, "break;");
        break;
    case SK::Continue:
        ln(ind, "continue;");
        break;
    default:
        throw EmitError("fast stmt");
    }
}

// Every `call` name anywhere under a statement.
static void collect_calls(const Stmt &s, std::set<std::string> &out);

static void collect_calls_expr(const Expr &e, std::set<std::string> &out) {
    if (e.k == EK::Call) out.insert(e.a);
    for (const auto &k : e.kids) collect_calls_expr(*k, out);
    if (e.fn) {
        for (const auto &p : e.fn->params)
            if (p.def) collect_calls_expr(*p.def, out);
        for (const auto &st : e.fn->body) collect_calls(*st, out);
    }
}

static void collect_calls(const Stmt &s, std::set<std::string> &out) {
    if (s.e1) collect_calls_expr(*s.e1, out);
    if (s.e2) collect_calls_expr(*s.e2, out);
    if (s.e3) collect_calls_expr(*s.e3, out);
    for (const Stmt *t : view(s.body)) collect_calls(*t, out);
    for (const Stmt *t : view(s.body2)) collect_calls(*t, out);
    if (const Stmt *els = real_else(s)) collect_calls(*els, out);
}

std::set<std::string> Emitter::drop_recursive_fast(const std::map<std::string, FnEntry> &cand) {
    std::map<std::string, const std::vector<StmtP> *> bodies;
    for (const auto &[n, e] : cand) bodies[n] = &e.decl->body;
    std::set<std::string> drop;
    std::map<std::string, std::set<std::string>> edges;
    for (const auto &[n, b] : bodies) {
        std::set<std::string> calls;
        for (const Stmt *s : view(*b)) collect_calls(*s, calls);
        std::set<std::string> hit;
        for (const auto &c : calls)
            if (bodies.count(c)) hit.insert(c);
        edges[n] = hit;
    }
    std::function<bool(const std::string &, const std::string &, std::set<std::string> &)> reaches =
        [&](const std::string &src, const std::string &dst, std::set<std::string> &seen) {
            if (src == dst) return true;
            if (seen.count(src)) return false;
            seen.insert(src);
            for (const auto &c : edges[src])
                if (reaches(c, dst, seen)) return true;
            return false;
        };
    for (const auto &[n, e] : edges) {
        for (const auto &c : e) {
            std::set<std::string> seen;
            if (reaches(c, n, seen)) {
                drop.insert(n);
                break;
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &[n, e] : edges) {
            if (drop.count(n)) continue;
            for (const auto &c : e) {
                if (drop.count(c)) {
                    drop.insert(n);
                    changed = true;
                    break;
                }
            }
        }
    }
    return drop;
}

void Emitter::emit_fast_fn(const std::string &name) {
    const FnDecl &decl = *fast[name].decl;
    fkind = fastkinds[name];
    std::string ct = fkind == "f64" ? "double" : "int64_t";
    fvecvars = fastvec.count(name) ? fastvec[name] : std::set<std::string>();
    std::vector<std::string> names;
    for (const auto &p : decl.params) names.push_back(p.name);
    fsym.clear();
    std::string args;
    for (const auto &n : names) {
        std::string cn = cname(n);
        fsym[n] = cn;
        if (!args.empty()) args += ", ";
        args += fvecvars.count(n) ? cat("Vec *", cn) : cat(ct, " ", cn);
    }
    ln("", cat("static ", ct, " ", fastc[name], "(", args.empty() ? "void" : args, ") {"));
    std::set<std::string> vars(names.begin(), names.end());
    collect_vars(view(decl.body), vars);
    for (const auto &n : vars) {
        if (std::find(names.begin(), names.end(), n) != names.end()) continue;
        fsym[n] = cname(n);
        ln("  ", cat(ct, " BRAT_UNUSED ", cname(n), ";"));
    }
    for (const auto &t : decl.body) fst(*t, "  ");
    ln("  ", cat("return ", fkind == "f64" ? "0.0" : "0", ";"));
    ln("", "}");
    ln("", "");
    fsym.clear();
    fkind = "int64_t";
    fvecvars.clear();
}


static bool is_top_kind(SK k) {
    switch (k) {
    case SK::Fn: case SK::Impl: case SK::Trait: case SK::Enum: case SK::Pub:
    case SK::Extern: case SK::ExternStruct: case SK::ExportFn:
        return true;
    default:
        return false;
    }
}

// True when some `return` in this body hands back a caller's storage.
static bool returns_param(const std::vector<StmtP> &body, const std::set<std::string> &params);
static bool stmt_returns_param_list(const std::vector<StmtP> &v, const std::set<std::string> &params);

static bool stmt_returns_param(const Stmt &s, const std::set<std::string> &params) {
    if (s.k == SK::Return && s.e1) {
        const Expr *e = s.e1.get();
        while (e->k == EK::Field || e->k == EK::Idx || e->k == EK::Move) e = e->kids[0].get();
        if (e->k == EK::Var && params.count(e->a)) return true;
    }
    if (stmt_returns_param_list(s.body, params)) return true;
    if (stmt_returns_param_list(s.body2, params)) return true;
    if (const Stmt *els = real_else(s))
        if (stmt_returns_param(*els, params)) return true;
    for (const auto &f : s.fns)
        if (stmt_returns_param_list(f.body, params)) return true;
    return false;
}

static bool stmt_returns_param_list(const std::vector<StmtP> &v, const std::set<std::string> &params) {
    for (const auto &s : v)
        if (stmt_returns_param(*s, params)) return true;
    return false;
}

static bool returns_param(const std::vector<StmtP> &body, const std::set<std::string> &params) {
    return stmt_returns_param_list(body, params);
}

void Emitter::collect_prog(const std::vector<StmtP> &prog) {
    for (const auto &sp : prog)
        if (is_top_kind(sp->k)) collect_top(*sp);
    for (const auto &sp : prog) {
        if (is_top_kind(sp->k)) continue;
        top_exe.push_back(sp.get());
        if (sp->k == SK::Let || sp->k == SK::LetTmp) global_lets.insert(sp->a);
    }
}

void Emitter::collect_top(const Stmt &s) {
    switch (s.k) {
    case SK::Pub:
        collect_top(*s.body[0]);
        return;
    case SK::ExternStruct:
        if (!structs.count(s.a)) struct_order.push_back(s.a);
        structs[s.a] = &s.fields;
        // `extern packed struct`: no padding, 1-byte alignment. It has to reach
        // the typedef, or every field offset after the first one is wrong.
        if (s.opt1 && *s.opt1 == "packed") packed.insert(s.a);
        return;
    case SK::ExportFn: {
        const FnDecl &f = s.fns[0];
        std::string name = *f.name;
        if (!fns.count(name)) {
            fns[name].decl = &f;
            fn_ret[name] = f.ret;
            order.push_back({false, name, ""});
            collect_nested_fn(f);
        }
        exported.insert(name);
        return;
    }
    case SK::Extern: {
        Ext e;
        e.name = s.ext.name;
        e.lib = s.ext.lib.value_or("");
        e.params = &s.ext.params;
        e.ret = s.ext.ret;
        e.varargs = s.ext.varargs;
        e.checked = s.ext.checked;
        if (!ext_ix.count(e.name)) ext_ix[e.name] = externs.size();
        externs.push_back(e);
        if (s.ext.checked) errno_externs.insert(e.name);
        return;
    }
    case SK::Fn: {
        const FnDecl &f = s.fns[0];
        std::string name = *f.name;
        if (!fns.count(name)) {
            fns[name].decl = &f;
            fn_ret[name] = f.ret;
            std::set<std::string> pnames;
            for (const auto &p : f.params) pnames.insert(p.name);
            if (returns_param(f.body, pnames)) alias_ret.insert(name);
            order.push_back({false, name, ""});
        }
        collect_nested_fn(f);
        return;
    }
    case SK::Impl: {
        if (s.opt1) {
            auto it = traits.find(*s.opt1);
            if (it == traits.end()) throw EmitError("unknown trait " + *s.opt1);
            for (const auto &m : it->second) {
                bool found = false;
                for (const auto &f : s.fns)
                    if (f.name && *f.name == m) found = true;
                if (!found) throw EmitError("impl " + *s.opt1 + " for " + s.a + " missing " + m);
            }
        }
        for (const auto &f : s.fns) {
            std::string key = s.a + "." + *f.name;
            if (!fns.count(key)) {
                fns[key].decl = &f;
                order.push_back({true, s.a, *f.name});
            }
        }
        return;
    }
    case SK::Enum: {
        enums.insert(s.a);
        for (const auto &v : s.variants) {
            std::string tag = (v.name == "None" && v.fields.empty()) ? "none" : lower(v.name);
            vtab[tag] = s.a;
            if (!fns.count(v.name)) {
                FnEntry e;
                e.ctor = true;
                e.ctor_tag = tag;
                e.ctor_arity = static_cast<int>(v.fields.size());
                fns[v.name] = e;
            }
        }
        return;
    }
    case SK::Trait:
        if (!traits.count(s.a)) {
            std::vector<std::string> names;
            for (const auto &g : s.sigs) names.push_back(g.name);
            traits[s.a] = names;
        }
        return;
    default:
        top_exe.push_back(&s);
    }
}

void Emitter::collect_nested_fn(const FnDecl &f) {
    for (const auto &p : f.params)
        if (p.def) collect_nested_expr(*p.def);
    for (const Stmt *s : view(f.body)) collect_nested(*s);
}

void Emitter::collect_nested_expr(const Expr &e) {
    for (const auto &k : e.kids) collect_nested_expr(*k);
    if (e.fn) {
        for (const auto &p : e.fn->params)
            if (p.def) collect_nested_expr(*p.def);
        for (const auto &s : e.fn->body) collect_nested(*s);
    }
}

// Register every `fn` declared inside a body, with a stable mangled name.
// The name is assigned in discovery order and the walk runs once per program,
// so both emission passes agree on it.
void Emitter::collect_nested(const Stmt &s) {
    switch (s.k) {
    case SK::Fn:
    case SK::ExportFn: {
        const FnDecl &f = s.fns[0];
        std::string name = *f.name;
        nested_seq++;
        std::string mangled = cat(name, "@", nested_seq);
        fns[mangled].decl = &f;
        fn_alias[name] = mangled;
        late.push_back({mangled, &f});
        collect_nested_fn(f);
        return;
    }
    case SK::Let: case SK::LetTmp: case SK::Set: case SK::Expr:
    case SK::Return: case SK::Defer:
        if (s.e1) collect_nested_expr(*s.e1);
        return;
    case SK::SetIdx:
        collect_nested_expr(*s.e1);
        collect_nested_expr(*s.e2);
        collect_nested_expr(*s.e3);
        return;
    case SK::SetField:
        collect_nested_expr(*s.e1);
        collect_nested_expr(*s.e2);
        return;
    case SK::If:
        collect_nested_expr(*s.e1);
        for (const Stmt *t : view(s.body)) collect_nested(*t);
        if (s.els) collect_nested(*s.els);
        return;
    case SK::While:
    case SK::Spawn:
    case SK::Scope:
    case SK::Unsafe:
        if (s.e1) collect_nested_expr(*s.e1);
        for (const Stmt *t : view(s.body)) collect_nested(*t);
        return;
    case SK::ForRange:
        collect_nested_expr(*s.e1);
        collect_nested_expr(*s.e2);
        for (const Stmt *t : view(s.body)) collect_nested(*t);
        return;
    case SK::ForIter:
        collect_nested_expr(*s.e1);
        for (const Stmt *t : view(s.body)) collect_nested(*t);
        return;
    case SK::Try:
        for (const Stmt *t : view(s.body)) collect_nested(*t);
        for (const Stmt *t : view(s.body2)) collect_nested(*t);
        return;
    case SK::Impl:
        for (const auto &f : s.fns) collect_nested_fn(f);
        return;
    case SK::Pub:
        if (!s.body.empty()) collect_nested(*s.body[0]);
        return;
    case SK::Extern:
        for (const auto &p : s.ext.params)
            if (p.def) collect_nested_expr(*p.def);
        return;
    default:
        return;
    }
}

const std::set<std::string> STRVEC_METH = {
    "split", "lines", "trim", "lower", "upper", "starts_with", "ends_with",
    "contains", "parse_int", "replace", "index_of", "repeat", "split_once",
    "chars", "parse_float", "join", "push", "pop", "first", "last", "sort",
    "rev", "ins", "del", "slice", "sum", "min", "max", "enumerate", "copy",
    "zip", "chunks",
};

const std::map<std::string, std::pair<std::string, int>> SV_SIMPLE = {
    {"index_of", {"vindex_of", 1}}, {"repeat", {"vrepeat", 1}},
    {"split_once", {"vsplit_once", 1}}, {"ins", {"vins", 2}},
    {"sum", {"vsum", 0}}, {"min", {"vmin", 0}}, {"max", {"vmax", 0}},
    {"enumerate", {"venum", 0}}, {"chars", {"vchars", 0}},
    {"parse_float", {"vparse_float", 0}}, {"copy", {"vcopy", 0}},
    {"zip", {"vzip", 1}}, {"chunks", {"vchunks", 1}},
};

const std::set<std::string> VBUILTIN_DIRECT = {"del", "copy", "slice"};

const std::set<std::string> MAP_METH = {"get", "has", "keys", "del", "set",
                                        "items", "vals", "copy", "get_or"};

const std::set<std::string> FFI_TYPES = {"i8", "u8", "i16", "u16", "i32", "u32",
                                         "i64", "u64", "usize", "f32", "f64",
                                         "str", "ptr"};

const std::map<std::string, std::pair<std::string, std::string>> FFI_RANGE = {
    {"i8", {"-128LL", "127LL"}},
    {"u8", {"0LL", "255LL"}},
    {"i16", {"-32768LL", "32767LL"}},
    {"u16", {"0LL", "65535LL"}},
    {"i32", {"-2147483648LL", "2147483647LL"}},
    {"u32", {"0LL", "4294967295LL"}},
    {"u64", {"0LL", "9223372036854775807LL"}},
    {"usize", {"0LL", "9223372036854775807LL"}},
};

// `"i64(i64,f64)"` -> ("i64", {"i64", "f64"}); throws when the literal is malformed.
static std::pair<std::string, std::vector<std::string>> parse_sig(const Expr &node) {
    if (node.k != EK::Str) throw EmitError("call: signature must be a string literal");
    std::string text;
    for (char c : node.a)
        if (c != ' ') text.push_back(c);
    size_t p = text.find('(');
    if (p == std::string::npos || text.back() != ')')
        throw EmitError("call: bad signature `" + text + "`");
    std::string ret = text.substr(0, p);
    std::string rest = text.substr(p + 1, text.size() - p - 2);
    std::vector<std::string> params;
    std::string cur;
    for (char c : rest) {
        if (c == ',') {
            if (!cur.empty()) params.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) params.push_back(cur);
    return {ret, params};
}

// Methods that answer with a part of the receiver (an element): the part is lifted
// out of the container before the temporary receiver is released.
const std::set<std::string> PART_RESULT = {"min", "max", "first", "last", "pop", "get"};

// Methods that mutate the receiver and hand it back: the result is owned exactly
// when the receiver was. `m.keys().sort()` is a container with no other name,
// `s.sort()` is `s`, and dropping that freed a live vector.
const std::set<std::string> SELF_RESULT = {"sort", "sort_by"};

const std::set<std::string> FRESH_RESULT = {
    "split", "split_once", "join", "upper", "lower", "trim", "replace", "repeat",
    "chars", "lines", "zip", "chunks", "enumerate", "copy", "keys", "items",
    "vals", "sum", "index_of", "starts_with", "ends_with", "contains", "has",
    "parse_int", "parse_float", "rev"};
// True when evaluating `e` can raise: it calls something, or it divides. The
// emitter uses this to check between a failing operand and the operator that
// would otherwise read the placeholder value it left behind.
static bool may_raise(const Expr &e) {
    if (e.k == EK::Call || e.k == EK::MCall) return true;
    if (e.k == EK::Bin && (e.a == "/" || e.a == "%")) return true;
    for (const auto &kid : e.kids)
        if (kid && may_raise(*kid)) return true;
    return false;
}

static bool dec_able(const Expr &e, const std::set<std::string> &heap) {
    switch (e.k) {
    case EK::Str: case EK::RStr: case EK::Num: case EK::Bool: case EK::None:
    case EK::Bin: case EK::VecLit: case EK::MapLit: case EK::StructLit: case EK::Closure:
    case EK::Or:
        return true;
    case EK::Call:
        // `unwrap` returns a reference the caller owns (see vtag_payload), and
        // `Ok(x)`/`Err(x)`/`Some(x)` build a fresh tag on the spot.
        if (e.a == "unwrap" || e.a == "Ok" || e.a == "Err" || e.a == "Some") return true;
        // `__at` is the desugared `a[i]`, which hands back an owned reference.
        if (e.a == "__at") return true;
        return heap.count(e.a) || HEAP_CALLS.count(e.a);
    // `x?` yields an owned reference for the same reason.
    case EK::Quest: return true;
    case EK::MCall:
        // A part lifted out of a temporary receiver is handed over by
        // vdrop_recv_part, so the holder already owns that reference.
        if (PART_RESULT.count(e.b) || SELF_RESULT.count(e.b))
            return !e.kids.empty() && dec_able(*e.kids[0], heap);
        // `FRESH_RESULT` and `HEAP_METHODS` answer the same question from two
        // directions -- which method builds a value of its own -- and a method
        // missing from both was treated as an alias and never released (`rev`
        // was). Asked of the union, so a name only has to be in one of them.
        return heap.count(e.b) || HEAP_METHODS.count(e.b) || FRESH_RESULT.count(e.b);
    case EK::Idx:
        // `a[i]` hands back a reference the holder owns (see vat_owned).
        return true;
    case EK::Move:
        return dec_able(*e.kids[0], heap);
    default:
        return false;
    }
}

V Emitter::ex(const Expr &e) {
    switch (e.k) {
    case EK::Num:
        if (e.is_float) return {{}, cat("vfloat(", repr_f64(e.f), ")")};
        return {{}, cat("vint(", clit64(e.i), ")")};
    case EK::Bool:
        return {{}, cat("vbool(", e.flag ? 1 : 0, ")")};
    case EK::Str:
        return ex_str(e.a);
    case EK::RStr:
        return {{}, cat("vlit(\"", clit(e.a), "\")")};
    case EK::None:
        return {{}, "vnil()"};
    case EK::Var:
        return {{}, var_ref(e.a)};
    case EK::ParFor:
        return ex_parfor(e);
    case EK::Un: {
        if (e.a == "&") {
            auto a = out_addr(*e.kids[0], nullptr);
            return {a.first, a.second};
        }
        V x = ex(*e.kids[0]);
        if (e.a == "!") return {x.pre, cat("vbool(!vis_true(", x.val, "))")};
        return {x.pre, cat("vneg(", x.val, ")")};
    }
    case EK::Bin:
        return ex_bin(e);
    case EK::Or: {
        V a = ex(*e.kids[0]);
        V b = ex(*e.kids[1]);
        std::string t = new_tmp();
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("Val ", t, " = (", a.val, ");"));
        for (auto &p : b.pre) pre.push_back(p);
        // `pick_first([]) or 99` answers with the tag the call built or with the
        // fallback. Whichever it is, the binding that receives it owns one
        // reference: the left side is released when the fallback wins, and the
        // fallback is counted up front so both branches hand over one.
        const std::string fb = keep(*e.kids[1], b.val);
        const bool held = dec_able(*e.kids[0], heap_returns()) ||
                          (e.kids[0]->k == EK::Call && may_raise(*e.kids[0]));
        if (held)
            pre.push_back(cat("if (vor_fb(", t, ")) { vdrop_temp(", t, "); ", t, " = (", fb, "); }"));
        else
            pre.push_back(cat("if (vor_fb(", t, ")) { ", t, " = (", fb, "); }"));
        return {pre, t};
    }
    case EK::Quest: {
        V a = ex(*e.kids[0]);
        return part_of_temp(a, *e.kids[0], "vquest_ok(vquest(", "))");
    }
    case EK::VecLit:
        return ex_vec(ptrs(e.kids));
    case EK::MapLit:
        return ex_maplit(ptrs(e.kids));
    case EK::StructLit:
        return ex_struct(e);
    case EK::Field: {
        V a = ex(*e.kids[0]);
        std::string t = new_tmp("f");
        std::string fname = clit(e.b);
        std::string bail = err_bail();
        // `v[0].x` and `f().x` read a field out of a value nobody owns: the
        // receiver arrived as an owned reference and was used twice below, so it
        // was never released. Held in a temp, the field is lifted with its own
        // reference and the receiver goes, the way `f()[0]` already did.
        const bool owned = dec_able(*e.kids[0], heap_returns());
        std::string recv = a.val;
        if (owned) {
            recv = new_tmp("r");
            a.pre.push_back("Val " + recv + " = (" + a.val + ");");
        }
        a.pre.push_back(cat("if ((", recv, ").tag != V_STRUCT) { vraise(\"no field ",
                            fname, "\"); ", bail, " }"));
        a.pre.push_back(cat("Val *", t, " = map_get((", recv, ").as.map, \"", fname, "\");"));
        a.pre.push_back(cat("if (!", t, ") { vraise(\"no field ", fname, "\"); ", bail, " }"));
        if (owned) {
            std::string v = new_tmp("v");
            a.pre.push_back("Val " + v + " = vrc_inc(*" + t + ");");
            a.pre.push_back("vdrop_temp(" + recv + ");");
            return {a.pre, v};
        }
        return {a.pre, cat("(*", t, ")")};
    }
    case EK::Idx: {
        if (std::string pe = ptr_elem(*e.kids[0]); !pe.empty())
            return ex_ptr_idx(pe, *e.kids[0], *e.kids[1]);
        V a = ex(*e.kids[0]);
        V b = ex(*e.kids[1]);
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        if (!dec_able(*e.kids[0], heap_returns()))
            return {pre, cat("vat_owned(", a.val, ", ", b.val, ")")};
        // Indexing a temporary: the element is lifted out and the container is
        // then released, or every `f()[0]` leaves a container behind.
        std::string recv = new_tmp("r");
        std::string t = new_tmp("c");
        pre.push_back("Val " + recv + " = (" + a.val + ");");
        pre.push_back("Val " + t + " = vat_owned(" + recv + ", " + b.val + ");");
        pre.push_back("vdrop_temp(" + recv + ");");
        return {pre, t};
    }
    case EK::MCall:
        return ex_mcall(e);
    case EK::Call:
        return ex_call(e);
    case EK::Closure:
        return ex_closure(e);
    case EK::Move: {
        V x = ex(*e.kids[0]);
        auto src = place_root(e);
        if (src && sym.count(*src) && e.kids[0]->k == EK::Var) {
            moved.insert(*src);
            x.val = cat("vmove(&", sym[*src].cn, ")");
        }
        return x;
    }
    }
    throw EmitError("node");
}

V Emitter::ex_str(const std::string &s) {
    auto parts = split_interp(s);
    if (parts.empty()) return {{}, "vlit(\"\")"};
    if (parts.size() == 1 && parts[0].first == "lit")
        return {{}, cat("vlit(\"", clit(parts[0].second), "\")")};
    std::string t = new_tmp("s");
    std::vector<std::string> pre = {cat("Val ", t, " = vlit(\"\");")};
    for (const auto &[kind, val] : parts) {
        if (kind == "lit") {
            pre.push_back(cat(t, " = vadd(", t, ", vlit(\"", clit(val), "\"));"));
        } else {
            pre.push_back(cat(t, " = vadd(", t, ", vstr_of(", path_ref(val), "));"));
        }
    }
    return {pre, t};
}

std::string Emitter::path_ref(const std::string &path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    std::string e = var_ref(parts[0]);
    for (size_t i = 1; i < parts.size(); i++)
        e = cat("vfield(", e, ", \"", clit(parts[i]), "\")");
    return e;
}

V Emitter::ex_bin(const Expr &e) {
    const std::string &op = e.a;
    if (op == "&&" || op == "||") {
        V a = ex(*e.kids[0]);
        V b = ex(*e.kids[1]);
        std::string t = new_tmp();
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("Val ", t, " = (", a.val, ");"));
        pre.push_back(op == "&&" ? cat("if (vis_true(", t, ")) {")
                                 : cat("if (!vis_true(", t, ")) {"));
        for (auto &p : b.pre) pre.push_back("  " + p);
        pre.push_back(cat("  ", t, " = (", b.val, ");"));
        pre.push_back("}");
        return {pre, t};
    }
    V a = ex(*e.kids[0]);
    V b = ex(*e.kids[1]);
    // `n + f()` must not add the placeholder value `f` left behind when it
    // failed: the pending error travels to the enclosing try, not into `+`.
    std::vector<std::string> pre = a.pre;
    // An operand that builds a fresh heap value -- `f().left() == "a"`,
    // `vrev(v) == [3, 2, 1]`, `"a" + "b" == "ab"` -- belongs to this operator
    // only while it reads it. Bound to a temp here and released once the result
    // exists: nothing else names it, and leaving it leaked one value per
    // evaluation. A container literal is excluded because its `Vec *` temp is
    // released below, through `vrc_dec`.
    std::vector<std::string> drops;
    auto hold = [&](const Expr &node, std::string &val) {
        // A call that fails leaves its error tag in the temp even when its
        // declared result is a number, and the tag has nobody once the frame that
        // raised is gone: `n = n - boom()` leaked one message per raise.
        // Only a plain call: `s.sort()` answers with its receiver, and the
        // receiver may be a local that must stay alive.
        if (!dec_able(node, heap_returns()) && !(node.k == EK::Call && may_raise(node))) return;
        switch (node.k) {
        case EK::Num: case EK::Bool: case EK::None: case EK::Str: case EK::RStr:
        case EK::Var: case EK::VecLit: case EK::MapLit: case EK::StructLit: case EK::Move:
            return;
        default: break;
        }
        if (val.empty() || val[0] != '_') {
            std::string t = new_tmp("v");
            pre.push_back(cat("Val ", t, " = (", val, ");"));
            val = t;
        }
        drops.push_back(val);
    };
    hold(*e.kids[0], a.val);
    if (!a.pre.empty() && may_raise(*e.kids[0])) pre.push_back(err_check_line(drops));
    for (auto &p : b.pre) pre.push_back(p);
    hold(*e.kids[1], b.val);
    if (!b.pre.empty() && may_raise(*e.kids[1])) pre.push_back(err_check_line(drops));
    if (op == "+") {
        std::string t = new_tmp("v");
        pre.push_back(cat("Val ", t, " = vadd(", a.val, ", ", b.val, ");"));
        const Expr *nodes[2] = {e.kids[0].get(), e.kids[1].get()};
        const std::string vals[2] = {a.val, b.val};
        for (int i = 0; i < 2; i++) {
            auto tmp = container_tmp(vals[i]);
            if (tmp && (nodes[i]->k == EK::VecLit || nodes[i]->k == EK::MapLit))
                pre.push_back(cat("vrc_dec(vvecv(", *tmp, "));"));
        }
        for (auto &d : drops) pre.push_back(cat("vdrop_temp(", d, ");"));
        if (!drops.empty()) pre.push_back(err_check_line());
        return {pre, t};
    }
    // A container built for a comparison is read, not consumed: `v == [1, 2, 3]`
    // left the literal's vector behind on every evaluation.
    std::vector<std::string> cdrops;
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
        const Expr *nodes[2] = {e.kids[0].get(), e.kids[1].get()};
        const std::string vals[2] = {a.val, b.val};
        for (int i = 0; i < 2; i++) {
            auto tmp = container_tmp(vals[i]);
            if (!tmp) continue;
            auto k = nodes[i]->k;
            if (k == EK::VecLit || k == EK::MapLit || k == EK::StructLit)
                cdrops.push_back(cat("vrc_dec(vvecv(", *tmp, "));"));
        }
    }
    std::string res;
    if (op == "-") res = cat("vsub(", a.val, ", ", b.val, ")");
    else if (op == "*") res = cat("vmul(", a.val, ", ", b.val, ")");
    else if (op == "/") res = cat("vdiv(", a.val, ", ", b.val, ")");
    else if (op == "%") res = cat("vmod(", a.val, ", ", b.val, ")");
    else if (op == "==") res = cat("vbool(veq(", a.val, ", ", b.val, "))");
    else if (op == "!=") res = cat("vbool(!veq(", a.val, ", ", b.val, "))");
    else if (op == "<" || op == ">" || op == "<=" || op == ">=")
        res = cat("vbool(vcmpt(", a.val, ", ", b.val, ", \"", op, "\") ", op, " 0)");
    if (!res.empty()) {
        if (drops.empty() && cdrops.empty()) return {pre, res};
        std::string t = new_tmp("v");
        pre.push_back(cat("Val ", t, " = (", res, ");"));
        // A temp the comparison finished with is nobody's on the success path
        // too. Releasing it only when a raise was pending is what leaked one
        // string per `assert("a" + "b" == "ab")`: the error path was covered and
        // the path a correct program takes was not.
        for (auto &d : drops) pre.push_back(cat("vdrop_temp(", d, ");"));
        pre.push_back(err_check_line());
        for (auto &d : cdrops) pre.push_back(d);
        return {pre, t};
    }
    if (op == "in") {
        // `x in [7, 11]` builds the container only to search it.
        auto tmp = container_tmp(b.val);
        const bool dropc = tmp && e.kids[1]->k == EK::VecLit;
        std::string r = cat("vbool(vcontains(", b.val, ", ", a.val, "))");
        if (drops.empty() && !dropc) return {pre, r};
        std::string t = new_tmp("v");
        pre.push_back(cat("Val ", t, " = (", r, ");"));
        for (auto &d : drops) pre.push_back(cat("vdrop_temp(", d, ");"));
        pre.push_back(err_check_line());
        if (dropc) pre.push_back(cat("vrc_dec(vvecv(", *tmp, "));"));
        return {pre, t};
    }
    throw EmitError("operator " + op);
}

const std::set<std::string> SIG_TYPES = {"i64", "f64", "str", "ptr", "none"};

V Emitter::ex_plugin(const std::string &name, const ExprList &args) {
    if (name == "lib_open") {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return {a.pre, cat("vlib_open(", a.val, ")")};
    }
    if (name == "lib_sym") {
        need_arity(args, 2, name);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        return {pre, cat("vlib_sym(", a.val, ", ", b.val, ")")};
    }
    if (name == "lib_close") {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return {a.pre, cat("vlib_close(", a.val, ")")};
    }
    auto [ret_t, params] = parse_sig(*args[1]);
    if (!SIG_TYPES.count(ret_t)) throw EmitError("call: unsupported return type `" + ret_t + "`");
    for (const auto &t : params)
        if (!SIG_TYPES.count(t) || t == "none")
            throw EmitError("call: unsupported argument type `" + t + "`");
    if (args.size() - 2 != params.size())
        throw EmitError(cat("call: signature wants ", params.size(), " argument(s), got ",
                            args.size() - 2));
    V f = ex(*args[0]);
    std::vector<std::string> pre = f.pre;
    std::string cvals;
    for (size_t i = 0; i < params.size(); i++) {
        V a = ex(*args[2 + i]);
        for (auto &p : a.pre) pre.push_back(p);
        if (!cvals.empty()) cvals += ", ";
        if (params[i] == "f64") cvals += cat("(", a.val, ").as.fval");
        else if (params[i] == "str") cvals += cat("(", a.val, ").as.str");
        else if (params[i] == "ptr") cvals += cat("(void *)(intptr_t)(", a.val, ").as.ival");
        else cvals += cat("(int64_t)(", a.val, ").as.ival");
    }
    std::string plugin_owner = new_tmp("plugin_owner");
    std::string plugin_fn = new_tmp("plugin_fn");
    pre.push_back(err_check_line());
    pre.push_back(cat("bint ", plugin_owner, " = 0;"));
    pre.push_back(cat("void *", plugin_fn, " = vplugin_acquire(", f.val, ", &",
                      plugin_owner, ");"));
    pre.push_back(err_check_line());
    std::vector<std::string> release = {
        cat("vplugin_release(", plugin_owner, ");"), err_check_line()
    };
    auto ctype = [](const std::string &t) -> std::string {
        if (t == "i64") return "int64_t";
        if (t == "f64") return "double";
        if (t == "str") return "const char *";
        if (t == "ptr") return "void *";
        return "void";
    };
    std::string c_ret = ctype(ret_t);
    std::string cparam;
    for (size_t i = 0; i < params.size(); i++) {
        if (i) cparam += ", ";
        cparam += ctype(params[i]);
    }
    std::string call_expr;
    if (!params.empty())
        call_expr = cat("(*(", c_ret, " (*)(", cparam, "))(intptr_t)", plugin_fn, ")(",
                        cvals, ")");
    else
        call_expr = cat("(*(", c_ret, " (*)(void))(intptr_t)", plugin_fn, ")()");
    std::string t_tmp = new_tmp("p");
    if (ret_t == "none") {
        pre.push_back(cat(call_expr, ";"));
        for (const auto &line : release) pre.push_back(line);
        return {pre, "vint(0)"};
    }
    if (ret_t == "f64") {
        pre.push_back(cat("double ", t_tmp, " = ", call_expr, ";"));
        for (const auto &line : release) pre.push_back(line);
        return {pre, cat("vfloat(", t_tmp, ")")};
    }
    if (ret_t == "str") {
        std::string raw = new_tmp("plugin_str");
        std::string result = new_tmp("plugin_result");
        pre.push_back(cat("const char *", raw, " = ", call_expr, ";"));
        pre.push_back(cat("Val ", result, " = vcall_str(", raw, ");"));
        for (const auto &line : release) pre.push_back(line);
        return {pre, result};
    }
    if (ret_t == "ptr") {
        pre.push_back(cat("void *", t_tmp, " = ", call_expr, ";"));
        for (const auto &line : release) pre.push_back(line);
        return {pre, cat("vint((int64_t)(intptr_t)", t_tmp, ")")};
    }
    pre.push_back(cat("int64_t ", t_tmp, " = ", call_expr, ";"));
    for (const auto &line : release) pre.push_back(line);
    return {pre, cat("vint(", t_tmp, ")")};
}

V Emitter::ex_slice(const std::string &name, const ExprList &args) {
    if (name == "slice_of") {
        need_arity(args, 3, name);
        V b = ex(*args[0]);
        V a = ex(*args[1]);
        V c = ex(*args[2]);
        std::vector<std::string> pre = b.pre;
        for (auto &p : a.pre) pre.push_back(p);
        for (auto &p : c.pre) pre.push_back(p);
        return {pre, cat("vslice_of(", b.val, ", ", a.val, ", ", c.val, ")")};
    }
    if (name == "slice_of_ptr") {
        need_arity(args, 3, name);
        const Expr &kind = *args[2];
        if (!(kind.k == EK::Str && (kind.a == "i64" || kind.a == "i32" || kind.a == "f64" ||
                                    kind.a == "f32" || kind.a == "u8")))
            throw EmitError("slice_of_ptr: kind must be the literal \"i64\", \"i32\", "
                            "\"f64\", \"f32\" or \"u8\"");
        V b = ex(*args[0]);
        V n = ex(*args[1]);
        int code = kind.a == "i64" ? 1 : kind.a == "f64" ? 2 : kind.a == "u8" ? 3
                 : kind.a == "i32" ? 4 : 5;
        std::vector<std::string> pre = b.pre;
        for (auto &p : n.pre) pre.push_back(p);
        return {pre, cat("vslice_of_ptr(", b.val, ", ", n.val, ", vint(", code, "))")};
    }
    if (name == "slice_of_struct") {
        // slice_of_struct(p, n, "T"): a window over a table of C records. The
        // stride is the record's size, so the layout stays the compiler's and
        // only the step is BRAT's.
        need_arity(args, 3, name);
        const Expr &sn = *args[2];
        if (sn.k != EK::Str) throw EmitError("slice_of_struct wants a struct name as a string literal");
        if (!structs.count(sn.a)) throw EmitError("slice_of_struct: " + sn.a + " is not an extern struct");
        V p0 = ex(*args[0]);
        V n0 = ex(*args[1]);
        std::vector<std::string> pre = p0.pre;
        for (auto &p : n0.pre) pre.push_back(p);
        return {pre, cat("vslice_of_struct(", p0.val, ", ", n0.val, ", vint(sizeof(bf_", sn.a, ")))")};
    }
    if (name == "slice_struct_of") {
        // slice_struct_of([a, b, c]): a window over elements built in the
        // program. A table of records is read-only, so this is how a window of
        // structs is written: the store reaches the element the literal made,
        // and the elements live exactly as long as the window does.
        need_arity(args, 1, name);
        const Expr &lit = *args[0];
        if (lit.k != EK::VecLit)
            throw EmitError("slice_struct_of wants a vector literal: "
                            "a window that is written cannot start empty");
        std::vector<std::string> pre, vals;
        for (const auto &kid : lit.kids) {
            V v = ex(*kid);
            for (auto &p : v.pre) pre.push_back(p);
            vals.push_back(v.val);
        }
        std::string arr = new_tmp("el");
        std::string joined;
        for (size_t k = 0; k < vals.size(); k++) {
            if (k) joined += ", ";
            joined += vals[k];
        }
        pre.push_back(cat("Val ", arr, "[] = { ", joined, " };"));
        std::string res = new_tmp("sl");
        pre.push_back(cat("Val ", res, " = vslice_elems((_Elems){ ", arr, ", ",
                          vals.size(), " });"));
        return {pre, res};
    }
    if (name == "slice_len") {
        need_arity(args, 1, name);
        V s = ex(*args[0]);
        return {s.pre, cat("vslice_len(", s.val, ")")};
    }
    if (name == "slice_get") {
        need_arity(args, 2, name);
        V s = ex(*args[0]);
        V i = ex(*args[1]);
        std::vector<std::string> pre = s.pre;
        for (auto &p : i.pre) pre.push_back(p);
        // A window over C records hands back the record, not its address: the
        // element the window declared is the layout to read it with.
        if (std::string se = slice_elem(*args[0]); structs.count(se)) {
            std::string at = cat("vslice_at(", s.val, ", ", i.val, ")");
            std::string r = new_tmp("sr");
            pre.push_back(cat("Val ", r, " = vunwind;"));
            pre.push_back(cat("if (vptr_live(", at, ")) { ", r, " = ",
                              c_struct_to_val(se, cat("(*(bf_", se, " *)(", at, ").as.ival)")),
                              "; }"));
            return {pre, r};
        }
        return {pre, cat("vslice_at(", s.val, ", ", i.val, ")")};
    }
    if (name == "slice_set") {
        need_arity(args, 3, name);
        V s = ex(*args[0]);
        V i = ex(*args[1]);
        V v = ex(*args[2]);
        std::vector<std::string> pre = s.pre;
        for (auto &p : i.pre) pre.push_back(p);
        for (auto &p : v.pre) pre.push_back(p);
        return {pre, cat("vslice_set(", s.val, ", ", i.val, ", ", v.val, ")")};
    }
    if (name == "str_from_ptr") {
        need_arity(args, 2, name);
        V p = ex(*args[0]);
        V n = ex(*args[1]);
        std::vector<std::string> pre = p.pre;
        for (auto &x : n.pre) pre.push_back(x);
        return {pre, cat("vstr_from_ptr(", p.val, ", ", n.val, ")")};
    }
    throw EmitError("unknown slice builtin " + name);
}


V Emitter::ex_vec(const ExprList &items) {
    std::string t = new_tmp("v");
    std::vector<std::string> pre = {cat("Vec *", t, " = vvec();")};
    for (const Expr *x : items) {
        V xv = ex(*x);
        for (auto &p : xv.pre) pre.push_back(p);
        pre.push_back(cat("vec_push(", t, ", ", keep(*x, xv.val), ");"));
    }
    return {pre, cat("vvecv(", t, ")")};
}

V Emitter::ex_maplit(const ExprList &kids) {
    std::string t = new_tmp("m");
    std::vector<std::string> pre = {cat("Map *", t, " = vmap(\"\");")};
    for (size_t i = 0; i + 1 < kids.size(); i += 2) {
        const Expr &ke = *kids[i], &ve = *kids[i + 1];
        V kv = ex(ke);
        V vv = ex(ve);
        for (auto &p : kv.pre) pre.push_back(p);
        for (auto &p : vv.pre) pre.push_back(p);
        pre.push_back(cat("map_set_v(", t, ", ", keep(ke, kv.val), ", ", keep(ve, vv.val), ");"));
    }
    std::string r = new_tmp("ml");
    pre.push_back(cat("Val ", r, " = vmap_has_reserved(", t, ") ? vmapkey_err() :"
                      " (vmap_bad_seen(", t, ") ? vmap_bad_key_err() : vstruct(", t, "));"));
    return {pre, r};
}

V Emitter::ex_struct(const Expr &e) {
    std::string t = new_tmp("m");
    std::vector<std::string> pre = {cat("Map *", t, " = vmap(\"", clit(e.a), "\");")};
    for (size_t i = 0; i < e.names.size(); i++) {
        const Expr &fv = *e.kids[i];
        V f = ex(fv);
        for (auto &p : f.pre) pre.push_back(p);
        pre.push_back(cat("map_set(", t, ", \"", clit(e.names[i]), "\", ", keep(fv, f.val), ");"));
    }
    return {pre, cat("vstruct(", t, ")")};
}

V Emitter::args_vec(const ExprList &args) {
    std::string t = new_tmp("a");
    std::vector<std::string> pre = {cat("Vec *", t, " = vvec();")};
    for (const Expr *a : args) {
        V v = ex(*a);
        for (auto &p : v.pre) pre.push_back(p);
        pre.push_back(cat("vec_push(", t, ", ", keep(*a, v.val), ");"));
        pre.push_back(err_check_line());
    }
    return {pre, t};
}

void Emitter::emit_loop_drops(const StmtList &body, const std::string &ind) {
    std::set<std::string> seen;
    std::function<void(const StmtList &)> walk = [&](const StmtList &stmts) {
        for (const Stmt *st : stmts) {
            if (st->k == SK::Let || st->k == SK::LetTmp) {
                auto name = head_of(st->a);
                if (relet.count(name) && !seen.count(name)) {
                    seen.insert(name);
                    ln(ind, cat("vrelease(", cname(name), ");"));
                }
            } else if (st->k == SK::If) {
                walk(view(st->body));
                if (const Stmt *els = real_else(*st)) {
                    if (els->k == SK::Block) {
                        walk(view(els->body));
                    } else {
                        walk({els});
                    }
                }
            } else if (st->k == SK::Try) {
                walk(view(st->body));
                walk(view(st->body2));
            }
        }
    };
    walk(body);
}

// Does every `return` in this body hand back a value the callee owns?
//
// The annotation is not the truth: `fn first<T>(v) -> i64 { return v[0] }` says
// i64 and hands back a string, and `safe_double(x) -> i64` in klib.brat returns
// `Ok(x * 2)`. A caller that believes the annotation treats the result as an
// alias, never releases it, and the value leaks. So the body is asked directly,
// through the same `dec_able` question the holders ask, to a fixpoint because
// one such function may call another.
static bool body_returns_owned(const StmtList &body, const std::set<std::string> &heap);
static bool block_returns_owned(const StmtList &body, const std::set<std::string> &heap, bool &saw);
static bool stmt_returns_owned(const Stmt &s, const std::set<std::string> &heap, bool &saw);

// Every `return` this statement contains has to hand back an owned value. `saw`
// records that the function returns at all; a branch or a loop with no `return`
// in it is not a counter-example, which is why the bodies share one `saw`
// instead of each answering for its own shape.
static bool stmt_returns_owned(const Stmt &s, const std::set<std::string> &heap, bool &saw) {
    switch (s.k) {
    case SK::Return:
        if (!s.e1) return true;
        // A function that answers with a number hands back nothing anybody has to
        // release; counting it as a heap return only made the caller bind a temp
        // for a value `vdrop_temp` ignores.
        if (s.e1->k == EK::Num || s.e1->k == EK::Bool) return true;
        saw = true;
        // `return x` for a binding is a transfer: the exit drops skip it, and a
        // parameter or a script-level name is counted on the way out, so the
        // caller holds a reference it may release. Without this a function that
        // answers with its accumulator was not in `heap_returns` and the caller
        // never dropped the result.
        if (s.e1->k == EK::Var) return true;
        return dec_able(*s.e1, heap);
    case SK::Block: return block_returns_owned(view(s.body), heap, saw);
    case SK::If:
        return block_returns_owned(view(s.body), heap, saw) &&
               (!s.els || stmt_returns_owned(*s.els, heap, saw));
    case SK::While: case SK::ForRange: case SK::ForIter: case SK::Spawn:
    case SK::Scope: case SK::Unsafe: case SK::Defer: case SK::Try:
        return block_returns_owned(view(s.body), heap, saw);
    default: return true;
    }
}

static bool block_returns_owned(const StmtList &body, const std::set<std::string> &heap, bool &saw) {
    for (const Stmt *s : body)
        if (!stmt_returns_owned(*s, heap, saw)) return false;
    return true;
}

static bool body_returns_owned(const StmtList &body, const std::set<std::string> &heap) {
    bool saw = false;
    return block_returns_owned(body, heap, saw) && saw;
}

std::set<std::string> Emitter::heap_returns() {
    std::set<std::string> out_;
    for (const auto &[name, ret] : fn_ret) {
        std::string r = ret.value_or("");
        if (r == "vec" || r == "map" || r == "str" || r == "tag" || r.rfind("vec[", 0) == 0) {
            out_.insert(name);
        } else if ((ret && structs.count(r)) || (!r.empty() && enums.count(r))) {
            out_.insert(name);
        }
    }
    // Bodies that prove it, whatever the annotation claims. Two passes reach
    // the usual fixpoint: a helper used by a helper.
    for (int pass = 0; pass < 3; pass++) {
        std::set<std::string> grown = out_;
        for (const auto &[name, entry] : fns) {
            if (!entry.decl || entry.ctor) continue;
            if (!body_returns_owned(view(entry.decl->body), grown)) continue;
            grown.insert(name.substr(name.rfind(':') == std::string::npos ? 0 : name.rfind(':') + 1));
            grown.insert(name);
        }
        if (grown.size() == out_.size()) break;
        out_ = grown;
    }
    // `Has(Ok(9))` builds a fresh tag in place of the call: counted as a
    // borrow, the tag and everything inside it stayed allocated.
    for (const auto &[name, entry] : fns)
        if (entry.ctor) out_.insert(name);
    // A callee that returns a parameter view hands back its own reference
    // (`vrc_inc` at the return site, because the caller frees the argument
    // vector), so the holder may release the result like any fresh value.
    return out_;
}

/* A call that only reads its argument (`len`, `str`, `tagof`, `is_err`). */
V Emitter::read_only(const Expr &arg, const std::string &fn) {
    return read_only_call(arg, [&fn](const std::string &v) { return cat(fn, "(", v, ")"); });
}

/* Evaluate `mk` over an argument that is read, never kept.
 *
 * A value the argument built has nobody else: `len(v.join("/"))` leaked the
 * joined string, `is_err("xx".parse_float())` leaked the tag. Read through a temp
 * so the value can be released as soon as the answer exists. */
V Emitter::read_only_call(const Expr &arg, const std::function<std::string(const std::string &)> &mk) {
    V a = ex(arg);
    if (!dec_able(arg, heap_returns())) return {a.pre, mk(a.val)};
    std::string t = new_tmp("a");
    a.pre.push_back("Val " + t + " = (" + a.val + ");");
    a.pre.push_back("Val " + t + "_r = " + mk(t) + ";");
    a.pre.push_back("vdrop_temp(" + t + ");");
    return {a.pre, t + "_r"};
}

/* A part read out of a tag (`x?`, `x.unwrap()`).
 *
 * The tag owns the payload, so the payload is counted once for the caller. When
 * the tag was built for this expression -- `Ok(42)?`, `f().unwrap()` -- nothing
 * else holds it and the tag is released here; that decision is made on the
 * expression, because at runtime a binding and a temporary are the same value.
 */
V Emitter::part_of_temp(const V &a, const Expr &src, const std::string &open,
                        const std::string &close) {
    const std::string expr = cat(open, a.val, close);
    std::optional<std::string> t = owned_temp(src, a.val);
    if (!dec_able(src, heap_returns()) || !t) return {a.pre, expr};
    std::string r = new_tmp("a");
    std::vector<std::string> pre = a.pre;
    pre.push_back(cat("Val ", r, " = ", expr, ";"));
    pre.push_back(cat("vdrop_temp(", *t, ");"));
    return {pre, r};
}

V Emitter::call_result(std::vector<std::string> pre, const std::string &vec, const std::string &call) {
    std::string t = new_tmp("c");
    pre.push_back(cat("Val ", t, " = (", call, ");"));
    pre.push_back(cat("vrc_dec(vvecv(", vec, "));"));
    return {pre, t};
}

void Emitter::need_arity(const ExprList &args, size_t n, const std::string &name) {
    if (args.size() != n) throw EmitError(name + " arity");
}

V Emitter::ex_tag(const std::string &tag, const ExprList &args) {
    std::string t = new_tmp("g");
    std::vector<std::string> pre = {cat("Vec *", t, " = vvec();")};
    for (const Expr *a : args) {
        V v = ex(*a);
        for (auto &p : v.pre) pre.push_back(p);
        pre.push_back(cat("vec_push(", t, ", ", keep(*a, v.val), ");"));
    }
    return {pre, cat("vtagv(\"", tag, "\", ", t, ")")};
}

V Emitter::ex_dotted(const std::string &name, const ExprList &args) {
    std::vector<std::string> parts;
    {
        std::string cur;
        for (char c : name) {
            if (c == '.') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        parts.push_back(cur);
    }
    const std::string head = parts[0], meth = parts.back();
    bool upper = !head.empty() && head[0] >= 'A' && head[0] <= 'Z';
    bool local_head = sym.count(head) || (cap && cap->second.count(head));
    if (!(upper && parts.size() == 2 && !local_head)) {
        ExprP node = mk_var(head);
        for (size_t i = 1; i + 1 < parts.size(); i++) node = mk_field(std::move(node), parts[i]);
        V o = ex(*node);
        bool direct = (VBUILTIN_DIRECT.count(meth) || (STRVEC_METH.count(meth) && MAP_METH.count(meth))) &&
                      !user_meths.count(meth);
        if (direct) {
            V a = args_vec(args);
            std::vector<std::string> pre = o.pre;
            for (auto &p : a.pre) pre.push_back(p);
            return call_result(pre, a.val,
                               cat("vbuiltin(", o.val, ", \"", clit(meth), "\", ", a.val, ")"));
        }
        if (STRVEC_METH.count(meth) && !user_meths.count(meth))
            return ex_strvec_meth(o.val, o.pre, meth, args);
        if (MAP_METH.count(meth) && !user_meths.count(meth))
            return ex_map_meth(o.val, o.pre, meth, args);
        V a = args_vec(args);
        std::vector<std::string> pre = o.pre;
        for (auto &p : a.pre) pre.push_back(p);
        return call_result(pre, a.val, cat("vcall_method(", o.val, ", \"", clit(meth), "\", ", a.val, ")"));
    }
    if (upper && parts.size() == 2) {
        auto it = fns.find(head);
        if (it != fns.end() && it->second.ctor && it->second.ctor_arity == 0) {
            const std::string &tag = it->second.ctor_tag;
            std::string base = tag == "none" ? "vnil()" : cat("vtagv(\"", clit(tag), "\", vvec())");
            V a = args_vec(args);
            return call_result(a.pre, a.val,
                               cat("vcall_method(", base, ", \"", clit(meth), "\", ", a.val, ")"));
        }
        std::string key = head + "." + meth;
        auto kt = fns.find(key);
        if (kt != fns.end() && !kt->second.ctor) {
            const std::vector<Param> &params = kt->second.decl->params;
            if (!params.empty() && params[0].name == "self")
                throw EmitError(key + " needs instance, use value.method()");
            std::vector<const Expr *> padded = pad_defaults(params, args, key);
            V a = args_vec(padded);
            return call_result(a.pre, a.val, cat(cfn("meth:" + key), "(NULL, ", a.val, ")"));
        }
    }
    ExprP node = mk_var(head);
    for (size_t i = 1; i + 1 < parts.size(); i++) node = mk_field(std::move(node), parts[i]);
    V o = ex(*node);
    V a = args_vec(args);
    std::vector<std::string> pre = o.pre;
    for (auto &p : a.pre) pre.push_back(p);
    return call_result(pre, a.val, cat("vcall_method(", o.val, ", \"", clit(meth), "\", ", a.val, ")"));
}

std::vector<const Expr *> Emitter::pad_defaults(const std::vector<Param> &params,
                                                std::vector<const Expr *> args,
                                                const std::string &name) {
    if (args.size() == params.size()) return args;
    if (args.size() < params.size()) {
        bool ok = true;
        for (size_t i = args.size(); i < params.size(); i++)
            if (!params[i].def) ok = false;
        if (ok) {
            for (size_t i = args.size(); i < params.size(); i++) args.push_back(params[i].def.get());
            return args;
        }
    }
    throw EmitError(name + " arity");
}

V Emitter::ex_direct(const std::string &name, std::vector<const Expr *> args) {
    FnEntry entry = fns[name];
    if (entry.ctor) {
        if (args.size() != static_cast<size_t>(entry.ctor_arity)) throw EmitError(name + " arity");
        if (entry.ctor_tag == "none") return {{}, "vnil()"};
        return ex_tag(entry.ctor_tag, args);
    }
    args = pad_defaults(entry.decl->params, args, name);
    V a = args_vec(args);
    return call_result(a.pre, a.val, cat(cfn("fn:" + fn_key(name)), "(NULL, ", a.val, ")"));
}

V Emitter::ex_vcall(const std::string &name, const ExprList &args) {
    V a = args_vec(args);
    return call_result(a.pre, a.val, cat("vcall_any(", var_ref(name), ", ", a.val, ")"));
}

V Emitter::ex_mcall(const Expr &e) {
    const Expr *obj = e.kids[0].get();
    const std::string &meth = e.b;
    ExprList args = ptrs(e.kids, 1);
    ExprP obj_owned;
    if (obj->k == EK::Call && obj->kids.empty() && !obj->a.empty() && obj->a.find('.') == std::string::npos &&
        obj->a[0] >= 'A' && obj->a[0] <= 'Z' &&
        (sym.count(obj->a) || (cap && cap->second.count(obj->a)))) {
        // A bare uppercase local parses as a zero-arg call; `C.len()` reads the local.
        obj_owned = mk_var(obj->a);
        obj = obj_owned.get();
    }
    if ((meth == "map" || meth == "filter") && args.size() == 1)
        return ex_mapfilter(*obj, meth, args[0]);
    if ((meth == "any" || meth == "all" || meth == "find") && args.size() == 1)
        return ex_hof(*obj, meth, args[0], nullptr);
    if (meth == "fold" && args.size() == 2) return ex_hof(*obj, meth, args[1], args[0]);
    if (meth == "sort_by" && args.size() == 1) return ex_hof(*obj, meth, args[0], nullptr);
    auto path = name_path(*obj);
    if (path && !sym.count(head_of(*path)) && !(cap && cap->second.count(head_of(*path)))) {
        // Static path: the callee is resolved by name, so no receiver storage is read.
        std::vector<ExprP> call_args;
        for (const Expr *a : args) call_args.push_back(clone(*a));
        owned.push_back(mk_call(*path + "." + meth, std::move(call_args)));
        return ex(*owned.back());
    }
    V o = ex(*obj);
    // `"a,b".split(",")` builds a container nothing else names: the receiver is
    // released once the call returns, or every chain leaks its first link.
    std::string recv;
    if ((FRESH_RESULT.count(meth) || PART_RESULT.count(meth) || user_meths.count(meth)) &&
        dec_able(*obj, heap_returns())) {
        recv = new_tmp("r");
        o.pre.push_back("Val " + recv + " = (" + o.val + ");");
        o.val = recv;
    }
    auto done = [&](V res) -> V {
        if (recv.empty()) return res;
        std::string t = new_tmp("c");
        res.pre.push_back("Val " + t + " = (" + res.val + ");");
        res.pre.push_back((PART_RESULT.count(meth) ? "vdrop_recv_part(" : "vdrop_recv(") + recv +
                          ", " + t + ");");
        res.val = t;
        return res;
    };
    bool direct = (VBUILTIN_DIRECT.count(meth) || (STRVEC_METH.count(meth) && MAP_METH.count(meth))) &&
                  !user_meths.count(meth);
    if (direct) {
        V a = args_vec(args);
        std::vector<std::string> pre = o.pre;
        for (auto &p : a.pre) pre.push_back(p);
        return done(call_result(pre, a.val, cat("vbuiltin(", o.val, ", \"", clit(meth), "\", ", a.val, ")")));
    }
    if (STRVEC_METH.count(meth) && !user_meths.count(meth))
        return done(ex_strvec_meth(o.val, o.pre, meth, args));
    if (MAP_METH.count(meth) && !user_meths.count(meth))
        return done(ex_map_meth(o.val, o.pre, meth, args));
    V a = args_vec(args);
    std::vector<std::string> pre = o.pre;
    for (auto &p : a.pre) pre.push_back(p);
    return done(call_result(pre, a.val, cat("vcall_method(", o.val, ", \"", clit(meth), "\", ", a.val, ")")));
}

V Emitter::ex_map_meth(const std::string &so, const std::vector<std::string> &po, const std::string &meth,
                       const ExprList &args) {
    if (meth == "keys") {
        need_arity(args, 0, meth);
        return {po, cat("vmap_keys(", so, ")")};
    }
    if (meth == "items") {
        need_arity(args, 0, meth);
        return {po, cat("vmap_items(", so, ")")};
    }
    if (meth == "vals") {
        need_arity(args, 0, meth);
        return {po, cat("vmap_vals(", so, ")")};
    }
    if (meth == "copy") {
        need_arity(args, 0, meth);
        return {po, cat("vcopy(", so, ")")};
    }
    if (meth == "get_or") {
        need_arity(args, 2, meth);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        std::vector<std::string> pre = po;
        for (auto &p : a.pre) pre.push_back(p);
        for (auto &p : b.pre) pre.push_back(p);
        return {pre, cat("vmap_get_or(", so, ", ", a.val, ", ", b.val, ")")};
    }
    if (meth == "get" || meth == "has" || meth == "del") {
        need_arity(args, 1, meth);
        V a = ex(*args[0]);
        std::vector<std::string> pre = po;
        for (auto &p : a.pre) pre.push_back(p);
        std::string fn = meth == "get" ? "vmap_getv" : meth == "has" ? "vmap_has" : "vmap_del";
        return {pre, cat(fn, "(", so, ", ", a.val, ")")};
    }
    need_arity(args, 2, meth);
    V a = ex(*args[0]);
    V b = ex(*args[1]);
    std::vector<std::string> pre = po;
    for (auto &p : a.pre) pre.push_back(p);
    for (auto &p : b.pre) pre.push_back(p);
    return {pre, cat("vmap_setv(", so, ", ", keep(*args[0], a.val), ", ", keep(*args[1], b.val), ")")};
}

/* `vzip(recv, ["a", "b"])`: a container literal built for an argument has nobody
 * to release it once the call has read it. Bound to a temp that holds the answer,
 * so the literal goes right after the call, never before it. */
V Emitter::release_lit_args(const std::vector<std::string> &pre, const ExprList &nodes,
                            const std::vector<std::string> &vals, const std::string &call) {
    std::vector<std::string> lits;
    for (size_t i = 0; i < nodes.size() && i < vals.size(); i++) {
        const Expr *n = nodes[i];
        if (n->k != EK::VecLit && n->k != EK::MapLit && n->k != EK::StructLit) continue;
        auto tmp = container_tmp(vals[i]);
        if (tmp) lits.push_back(*tmp);
    }
    if (lits.empty()) return {pre, call};
    V out;
    out.pre = pre;
    std::string t = new_tmp("v");
    out.pre.push_back(cat("Val ", t, " = (", call, ");"));
    for (auto &l : lits) out.pre.push_back(cat("vrc_dec(vvecv(", l, "));"));
    out.val = t;
    return out;
}

V Emitter::ex_strvec_meth(const std::string &so, const std::vector<std::string> &po, const std::string &meth,
                          const ExprList &args) {
    auto simple = SV_SIMPLE.find(meth);
    if (simple != SV_SIMPLE.end()) {
        const std::string &fn = simple->second.first;
        need_arity(args, static_cast<size_t>(simple->second.second), meth);
        std::vector<std::string> pre = po;
        std::vector<std::string> cargs;
        for (const Expr *a : args) {
            V v = ex(*a);
            for (auto &p : v.pre) pre.push_back(p);
            cargs.push_back(v.val);
        }
        if (meth == "ins") cargs[1] = keep(*args[1], cargs[1]);
        std::string joined;
        for (auto &c : cargs) joined += ", " + c;
        return release_lit_args(pre, args, cargs, cat(fn, "(", so, joined, ")"));
    }
    if (meth == "split") {
        if (args.size() > 1) throw EmitError("split arity");
        if (args.empty()) {
            std::vector<std::string> pre = po;
            pre.push_back("Vec *_sp = vvec();");
            return {pre, cat("vsplit(", so, ", vvecv(_sp))")};
        }
        V a = ex(*args[0]);
        std::vector<std::string> pre = po;
        for (auto &p : a.pre) pre.push_back(p);
        pre.push_back(cat("if ((", a.val, ").tag == V_STR && !(", a.val,
                          ").as.str[0]) { vraise(\"empty separator\"); ", err_bail(), " }"));
        return {pre, cat("vsplit(", so, ", ", a.val, ")")};
    }
    if (meth == "lines") {
        need_arity(args, 0, meth);
        return {po, cat("vlines(", so, ")")};
    }
    if (meth == "trim" || meth == "lower" || meth == "upper") {
        need_arity(args, 0, meth);
        if (meth == "trim") return {po, cat("vtrim(", so, ")")};
        return {po, cat("vcase(", so, ", ", meth == "upper" ? 1 : 0, ")")};
    }
    if (meth == "starts_with" || meth == "ends_with" || meth == "contains") {
        need_arity(args, 1, meth);
        V a = ex(*args[0]);
        std::vector<std::string> pre = po;
        for (auto &p : a.pre) pre.push_back(p);
        const std::string flags = meth == "starts_with" ? "1, 0" : meth == "ends_with" ? "0, 1" : "0, 0";
        return release_lit_args(pre, {args[0]}, {a.val},
                                cat("vbool(vaffix(", so, ", ", a.val, ", ", flags, "))"));
    }
    if (meth == "parse_int") {
        need_arity(args, 0, meth);
        return {po, cat("vparse_int(", so, ")")};
    }
    if (meth == "replace") {
        need_arity(args, 2, meth);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        std::vector<std::string> pre = po;
        for (auto &p : a.pre) pre.push_back(p);
        for (auto &p : b.pre) pre.push_back(p);
        return {pre, cat("vreplace(", so, ", ", a.val, ", ", b.val, ")")};
    }
    if (meth == "first" || meth == "last" || meth == "sort" || meth == "rev") {
        need_arity(args, 0, meth);
        std::string fn = meth == "first" ? "vfirst" : meth == "last" ? "vlast" : meth == "sort" ? "vsort" : "vrev";
        return {po, cat(fn, "(", so, ")")};
    }
    if (meth == "join") {
        need_arity(args, 1, meth);
        V a = ex(*args[0]);
        std::vector<std::string> pre = po;
        for (auto &p : a.pre) pre.push_back(p);
        return {pre, cat("vjoin(", so, ", ", a.val, ")")};
    }
    if (meth == "push") {
        need_arity(args, 1, meth);
        V a = ex(*args[0]);
        std::vector<std::string> pre = po;
        for (auto &p : a.pre) pre.push_back(p);
        return {pre, cat("vpush(", so, ", ", keep(*args[0], a.val), ")")};
    }
    if (meth == "pop") {
        need_arity(args, 0, meth);
        return {po, cat("vpop(", so, ")")};
    }
    throw EmitError("method " + meth);
}

V Emitter::ex_hof(const Expr &obj, const std::string &meth, const Expr *arg, const Expr *first) {
    V o = ex(obj);
    const FnDecl *entry = nullptr;
    if (arg->k == EK::Var) {
        auto it = fns.find(arg->a);
        if (it != fns.end() && !it->second.ctor) entry = it->second.decl;
    }
    std::vector<std::string> pre = o.pre;
    std::string fnv;
    if (entry) {
        int code = fn_code("fn:" + arg->a);
        std::string t = new_tmp("c");
        int n = tmp;
        pre.push_back(cat("Vec *_cb", n, " = vvec();"));
        pre.push_back(cat("vec_push(_cb", n, ", vint(", code, "));"));
        pre.push_back(cat("vec_push(_cb", n, ", vstruct(vmap(\"\")));"));
        pre.push_back(cat("Val ", t, " = vtagv(\"closure\", _cb", n, ");"));
        fnv = t;
    } else {
        V a = ex(*arg);
        for (auto &p : a.pre) pre.push_back(p);
        fnv = a.val;
    }
    if (first) {
        V f = ex(*first);
        for (auto &p : f.pre) pre.push_back(p);
        return {pre, cat("vfold(", o.val, ", ", keep(*first, f.val), ", ", fnv, ")")};
    }
    if (meth == "any") return {pre, cat("vpred(", o.val, ", ", fnv, ", \"any\")")};
    if (meth == "all") return {pre, cat("vpred(", o.val, ", ", fnv, ", \"all\")")};
    if (meth == "find") return {pre, cat("vpred(", o.val, ", ", fnv, ", \"find\")")};
    return {pre, cat("vsort_by(", o.val, ", ", fnv, ")")};
}

V Emitter::ex_mapfilter(const Expr &obj, const std::string &meth, const Expr *arg) {
    V o = ex(obj);
    const FnDecl *entry = nullptr;
    if (arg->k == EK::Var) {
        auto it = fns.find(arg->a);
        if (it != fns.end() && !it->second.ctor) entry = it->second.decl;
    }
    if (entry) {
        int code = fn_code("fn:" + arg->a);
        std::string t = new_tmp("c");
        int n = tmp;
        std::vector<std::string> pre = o.pre;
        pre.push_back(cat("Vec *_cb", n, " = vvec();"));
        pre.push_back(cat("vec_push(_cb", n, ", vint(", code, "));"));
        pre.push_back(cat("vec_push(_cb", n, ", vstruct(vmap(\"\")));"));
        pre.push_back(cat("Val ", t, " = vtagv(\"closure\", _cb", n, ");"));
        return {pre, cat("vmap_with(", o.val, ", ", t, ", \"", clit(meth), "\")")};
    }
    V a = ex(*arg);
    std::vector<std::string> pre = o.pre;
    for (auto &p : a.pre) pre.push_back(p);
    // vmap_with releases the closure it is given, so a binding the caller still
    // owns gets one more reference and a temporary is handed over as it is.
    return {pre, cat("vmap_with(", o.val, ", ", keep(*arg, a.val), ", \"", clit(meth), "\")")};
}

V Emitter::ex_parfor(const Expr &e) {
    V it = ex(*e.kids[0]);
    V unit = ex(*e.kids[1]);
    const FnDecl &decl = *e.fn;
    const int code = fn_code(cat("clo:", clos.size()));
    std::set<std::string> outer;
    if (cap) outer = cap->second;
    std::set<std::string> capnames;
    for (const auto &[n, s] : sym)
        if (assigned.count(n)) capnames.insert(n);
    for (const auto &n : outer) capnames.insert(n);
    capnames.erase(e.a);
    clos.push_back({&decl.params, view(decl.body), capnames});
    std::string m = new_tmp("m");
    const int body_cb = tmp;
    new_tmp("c");
    const int op_cb = tmp;
    std::string opc = new_tmp("c");
    std::vector<std::string> pre = it.pre;
    pre.insert(pre.end(), unit.pre.begin(), unit.pre.end());
    pre.push_back(cat("Map *_cm", body_cb, " = vmap(\"\");"));
    std::set<std::string> others;
    for (const auto &[n, s] : sym)
        if (assigned.count(n)) others.insert(n);
    for (const auto &n : outer) others.insert(n);
    others.erase(e.a);
    for (const auto &var : others)
        for (auto &l : capture(cat("_cm", body_cb), var)) pre.push_back(l);
    pre.push_back(cat("Vec *_cb", body_cb, " = vvec();"));
    pre.push_back(cat("vec_push(_cb", body_cb, ", vint(", code, "));"));
    pre.push_back(cat("{ Val _cv = vstruct(_cm", body_cb, "); vec_push(_cb", body_cb,
                      ", _cv); }"));
    pre.push_back(cat("Val ", m, " = vtagv(\"closure\", _cb", body_cb, ");"));
    const int opcode = fn_code("fn:" + e.b);
    pre.push_back(cat("Vec *_cb", op_cb, " = vvec();"));
    pre.push_back(cat("vec_push(_cb", op_cb, ", vint(", opcode, "));"));
    pre.push_back(cat("vec_push(_cb", op_cb, ", vstruct(vmap(\"\")));"));
    pre.push_back(cat("Val ", opc, " = vtagv(\"closure\", _cb", op_cb, ");"));
    std::string t = new_tmp("pf");
    pre.push_back(cat("Val ", t, " = vparfor(", it.val, ", ", m, ", ", unit.val,
                      ", ", opc, ");"));
    return {pre, t};
}

V Emitter::ex_closure(const Expr &e) {
    const FnDecl &decl = *e.fn;
    int code = fn_code(cat("clo:", clos.size()));
    std::set<std::string> outer;
    if (cap) outer = cap->second;
    std::set<std::string> base;
    for (const auto &[n, s] : sym)
        if (assigned.count(n)) base.insert(n);
    std::set<std::string> capnames = base;
    for (const auto &n : outer) capnames.insert(n);
    bool named = decl.name.has_value();
    if (named) capnames.insert(*decl.name);
    clos.push_back({&decl.params, view(decl.body), capnames});
    std::string t = new_tmp("m");
    int n = tmp;
    std::vector<std::string> pre = {cat("Map *_cm", n, " = vmap(\"\");")};
    std::set<std::string> others = base;
    for (const auto &x : outer) others.insert(x);
    if (named) others.erase(*decl.name);
    for (const auto &var : others)
        for (auto &l : capture(cat("_cm", n), var)) pre.push_back(l);
    pre.push_back(cat("Vec *_cb", n, " = vvec();"));
    pre.push_back(cat("vec_push(_cb", n, ", vint(", code, "));"));
    pre.push_back(cat("Val ", t, " = vtagv(\"closure\", _cb", n, ");"));
    pre.push_back(cat("{ Val _cv = vstruct(_cm", n, "); vec_push(_cb", n, ", _cv); }"));
    if (named) pre.push_back(cat("map_set(_cm", n, ", \"", *decl.name, "\", ", t, ");"));
    return {pre, t};
}


std::string Emitter::var_ref(const std::string &name) {
    auto it = sym.find(name);
    if (it != sym.end()) return it->second.cn;
    if (cap && cap->second.count(name)) return cat("(*map_get(", cap->first, ", \"", name, "\"))");
    return cname(name);
}

// The element type of `p[i]`, when `p` is a `ptr[T]` binding. Only a bare
// binding carries the type, and only the annotation declares it: a plain `ptr`
// keeps the old behaviour, so nothing that compiles today changes meaning.
// The element a window declares, for a `slice[T]` binding or for a
// `slice_of_struct(...)` written at the point of the call.
std::string Emitter::slice_elem(const Expr &e) {
    if (e.k == EK::Var) {
        auto it = sym.find(e.a);
        return it == sym.end() ? "" : it->second.elem;
    }
    if (e.k == EK::Call && e.a == "slice_of_struct" && e.kids.size() == 3 && e.kids[2]->k == EK::Str)
        return e.kids[2]->a;
    return "";
}

std::string Emitter::ptr_elem(const Expr &base) {
    if (base.k != EK::Var) return "";
    auto it = sym.find(base.a);
    return it == sym.end() ? "" : it->second.ptr;
}

// `p[i]` where `p: ptr[T]`: the element, not the address. A C record is read
// with the layout the extern struct declares; a machine word is read at the
// width the element names, so `ptr[i32]` reads four bytes and not eight.
V Emitter::ex_ptr_idx(const std::string &elem, const Expr &base, const Expr &idx) {
    V a = ex(base);
    V b = ex(idx);
    std::vector<std::string> pre = a.pre;
    for (auto &p : b.pre) pre.push_back(p);
    std::string at = cat("((char *)(", a.val, ").as.ival + (", b.val, ").as.ival * ");
    std::string read;
    if (structs.count(elem)) {
        read = c_struct_to_val(elem, cat("(*(bf_", elem, " *)", at, "sizeof(bf_", elem, ")))"));
    } else if (elem == "i64") {
        read = cat("vint(*(int64_t *)", at, "8))");
    } else if (elem == "i32") {
        read = cat("vint((int64_t)*(int32_t *)", at, "4))");
    } else if (elem == "u32") {
        read = cat("vint((int64_t)*(uint32_t *)", at, "4))");
    } else if (elem == "u8") {
        read = cat("vint((int64_t)*(unsigned char *)", at, "1))");
    } else if (elem == "f64") {
        read = cat("vfloat(*(double *)", at, "8))");
    } else if (elem == "f32") {
        read = cat("vfloat((double)*(float *)", at, "4))");
    } else {
        throw EmitError("ptr[" + elem + "]: index a pointer needs a unit");
    }
    std::string r = new_tmp("pi");
    pre.push_back(cat("Val ", r, " = vunwind;"));
    pre.push_back(cat("if (vptr_live(", a.val, ")) { ", r, " = ", read, "; }"));
    return {pre, r};
}

// `p[i] = v`: the store, at the element's width. A table of records is written
// by the function that owns the buffer (`ptr_write_struct`); a silent store
// here would be a memcpy of a layout BRAT only knows by its size.
void Emitter::st_ptr_setidx(const std::string &elem, const Stmt &s, const std::string &ind) {
    V a = ex(*s.e1);
    V i = ex(*s.e2);
    V v = ex(*s.e3);
    for (auto &p : a.pre) ln(ind, p);
    for (auto &p : i.pre) ln(ind, p);
    for (auto &p : v.pre) ln(ind, p);
    if (structs.count(elem))
        throw EmitError("a table of C structs is read-only: write the record with ptr_write_struct");
    std::string at = cat("((char *)(", a.val, ").as.ival + (", i.val, ").as.ival * ");
    std::string store;
    if (elem == "i64") store = cat("*(int64_t *)", at, "8) = (", v.val, ").as.ival");
    else if (elem == "i32") store = cat("*(int32_t *)", at, "4) = (int32_t)(", v.val, ").as.ival");
    else if (elem == "u32") store = cat("*(uint32_t *)", at, "4) = (uint32_t)(", v.val, ").as.ival");
    else if (elem == "u8") store = cat("*(unsigned char *)", at, "1) = (unsigned char)(", v.val, ").as.ival");
    else if (elem == "f64") store = cat("*(double *)", at, "8) = (", v.val, ").as.fval");
    else if (elem == "f32") store = cat("*(float *)", at, "4) = (float)(", v.val, ").as.fval");
    else throw EmitError("ptr[" + elem + "]: index a pointer needs a unit");
    ln(ind, cat("if (vptr_live(", a.val, ")) { ", store, "; }"));
    emit_err_check(ind);
    if (s.e1->k == EK::Var) assigned.insert(s.e1->a);
}

std::string Emitter::var_def(const std::string &name, Own own, const std::string &ptr,
                             const std::string &elem) {
    std::string cn = cname(name);
    sym[name] = Sym{cn, own, ptr, elem};
    return cn;
}

std::optional<std::string> Emitter::place_root(const Expr &e) {
    switch (e.k) {
    case EK::Var:
        return e.a;
    case EK::Move:
    case EK::Field:
    case EK::Idx:
        return place_root(*e.kids[0]);
    case EK::Call:
        // Reading an element: a place expression rooted at the container.
        if (e.a == "__at" && !e.kids.empty()) return place_root(*e.kids[0]);
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

std::string Emitter::keep(const Expr &e, const std::string &val) {
    if (dec_able(e, heap_returns())) return val;
    // `move x` of a live local transferred the binding's reference already:
    // ex() emitted `vmove(&x)`, which emptied it, so the scope drop will not
    // release it and the container that receives `val` is its only owner from
    // here. Retaining here left one reference with no owner, one per call with a
    // moved argument, unbounded inside a loop.
    if (e.k == EK::Move && !e.kids.empty() && e.kids[0]->k == EK::Var) {
        auto src = place_root(e);
        if (src && sym.count(*src)) return val;
    }
    return cat("vrc_inc(", val, ")");
}

std::vector<std::string> Emitter::capture(const std::string &cm, const std::string &var) {
    std::string ref = var_ref(var);
    // Always its own reference: the closure may die before the frame (a loop
    // defer) or outlive it (a returned closure), so neither side can lean on
    // the other's.
    captured.insert(var);
    return {cat("vrc_inc(", ref, ");"), cat("map_set(", cm, ", \"", var, "\", ", ref, ");")};
}

Own Emitter::own_of(const Expr &e) {
    switch (e.k) {
    case EK::Num:
    case EK::Bool:
    case EK::None:
    case EK::Str:
    case EK::RStr:
        return Own::Copy;   // scalars and string literals are immortal
    case EK::Var: {
        auto it = sym.find(e.a);
        if (it == sym.end()) return Own::Copy;   // param / capture storage is the caller's
        return it->second.own == Own::Copy ? Own::Copy : Own::Borrow;
    }
    case EK::Move:
        // A move takes the reference. A whole-value `move x` takes the one the
        // source held and invalidates it; a projection move `move p.x` is handed
        // a reference of its own by the `let`. Classified as a borrow, that
        // reference was never released and every projection move leaked a value.
        return Own::Owned;
    case EK::Closure:
        return Own::Owned;
    case EK::ParFor:
        return Own::Owned;   // the fold builds its own value
    case EK::VecLit:
    case EK::MapLit:
    case EK::StructLit:
        return Own::Owned;   // fresh heap container
    case EK::Bin:
        return Own::Owned;   // fresh result; vdrop is a no-op for scalars
    case EK::Un:
        return Own::Copy;
    case EK::Or:
        return Own::Owned;   // one counted reference, whichever side wins
    case EK::Quest:
        // `x?` takes a reference out of the tag (`vtag_payload` counts it), so the
        // binding that receives it owns that reference. Calling it a copy left the
        // payload -- a struct, a vector, a string -- allocated for the whole run.
        return Own::Owned;
    case EK::Field:
        return Own::Borrow;   // read out of a struct: aliases it
    case EK::Idx:
        return Own::Owned;    // `a[i]` hands back its own reference (vat_owned)
    case EK::Call:
    case EK::MCall:
        // `Ok(x)`/`Err(x)`/`Some(x)` build a fresh tag on the spot, so its
        // argument vector belongs to whoever holds the tag. Treating it as a
        // copy left that vector, and every container inside it, allocated.
        if (e.k == EK::Call && (e.a == "Ok" || e.a == "Err" || e.a == "Some")) return Own::Owned;
        // A callee that hands back a container or a string transfers a
        // reference: arguments were shared before the call.
        return dec_able(e, heap_returns()) ? Own::Owned : Own::Copy;
    }
    return Own::Owned;
}

// True when `e` reads a part (element, field) of the binding `root` rather than
// the binding itself.
// `a[i]` and its desugared `__at(a, i)` form: both yield an owned reference.
static bool is_index_read(const Expr &e) {
    if (e.k == EK::Idx) return true;
    return e.k == EK::Call && e.a == "__at" && e.kids.size() == 2;
}

bool Emitter::part_of_local(const Expr &e, const std::string &root) {
    auto it = sym.find(root);
    if (it == sym.end() || it->second.own != Own::Owned) return false;
    const Expr *cur = &e;
    while (cur->k == EK::Move && !cur->kids.empty()) cur = cur->kids[0].get();
    return cur->k != EK::Var;
}

bool Emitter::drop_allowed(const std::string &name) {
    auto it = sym.find(name);
    return move_rt && it != sym.end() && it->second.own == Own::Owned && !moved.count(name) &&
           !shared.count(name) && !dropped.count(name);
}

void Emitter::emit_drop(const std::string &name, const std::string &ind) {
    if (!drop_allowed(name)) return;
    dropped.insert(name);
    ln(ind, cat("vrelease(", sym[name].cn, ");"));
    ln(ind, cat(sym[name].cn, " = vnil();"));
}

// Release every owned local at the single exit point. A `return` inside a
// `match` arm jumps straight to the exit label, over the drop the end of the arm
// emits; that drop is then dead code, and the value it was written for -- a
// payload alias, a temporary tag -- leaked. Releasing here covers every path, and
// releasing twice is harmless: a binding dropped on the way out is `vnil()` first.
// `at_return` is a path that leaves through a `return`, and `skip` is the
// binding it hands to the caller. A name one `return` transfers is shared for the
// whole frame, so the exit drops skipped it on every other path too and whatever
// it still held leaked -- `path_norm` built `out`, answered `"."` on an early
// branch, and lost it.
void Emitter::emit_fn_drops(const std::string &ind, const std::string &skip,
                            bool at_return, const Names &keep) {
    for (const auto &name : sorted_sym_names()) {
        const Sym &sy = sym[name];
        if (!move_rt || sy.own != Own::Owned) continue;
        if (moved.count(name) || name == skip || keep.count(name)) continue;
        if (!at_return && shared.count(name)) continue;
        ln(ind, cat("vrelease(", sy.cn, ");"));
        ln(ind, cat(sy.cn, " = vnil();"));
    }
}

void Emitter::emit_scope_drops(const std::string &ind) {
    for (const auto &[name, s] : sym) {
        if (!drop_allowed(name)) continue;
        ln(ind, cat("vrelease(", s.cn, ");"));
        ln(ind, cat(s.cn, " = vnil();"));
        dropped.insert(name);
    }
}

std::vector<std::string> Emitter::sorted_sym_names() const {
    std::vector<std::string> out;
    for (const auto &[name, _s] : sym) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

// Every `let` name declared inside a try body, at any depth: those are released
// at the end of their block, and the catch path jumps over that code.
void try_let_names(const StmtList &body, std::set<std::string> &acc) {
    for (const Stmt *st : body) {
        if (!st) continue;
        switch (st->k) {
        case SK::Let: acc.insert(st->a.substr(0, st->a.find('.'))); break;
        case SK::If:
            try_let_names(view(st->body), acc);
            if (st->els) {
                try_let_names({st->els.get()}, acc);
            } else {
                try_let_names(view(st->body2), acc);
            }
            break;
        case SK::While: try_let_names(view(st->body), acc); break;
        case SK::ForIter: case SK::ForRange:
            acc.insert(st->a);
            try_let_names(view(st->body), acc);
            break;
        case SK::Try:
            try_let_names(view(st->body), acc);
            try_let_names(view(st->body2), acc);
            break;
        default: break;
        }
    }
}

std::set<std::string> try_let_names(const StmtList &body) {
    std::set<std::string> acc;
    try_let_names(body, acc);
    return acc;
}

void Emitter::drop_alias_temps(const StmtList &body, const std::string &ind) {
    for (const auto &name : block_let_names(body)) {
        if (alias_temps.count(name)) {
            alias_temps.erase(name);
            ln(ind, cat("vrc_dec(", cname(name), ");"));
        }
    }
}

void Emitter::emit_block(const StmtList &body, const std::string &ind) {
    if (!block_defers(body)) {
        for (const Stmt *t : body) st(*t, ind);
        if (move_rt)
            for (const auto &name : block_let_names(body)) emit_drop(name, ind);
        drop_alias_temps(body, ind);
        return;
    }
    std::string b = new_tmp("dfb");
    ln(ind, cat("volatile int64_t ", b, " = vdfsp;"));
    ln(ind, "vdfpush();");
    dfb.push_back(b);
    for (const Stmt *t : body) st(*t, ind);
    dfb.pop_back();
    ln(ind, cat("vdfreset(", b, ");"));
    if (move_rt)
        for (const auto &name : block_let_names(body)) emit_drop(name, ind);
    drop_alias_temps(body, ind);
}

void Emitter::emit_loop_exit(const std::string &k, const std::string &ind) {
    // Every lexical block between the loop boundary and this exit is left by
    // break/continue. Drain its defers, then join any scope it leaves.
    const size_t scope_depth = loop_exit_depths.empty()
        ? scopes.size() : loop_exit_depths.back().first;
    const size_t defer_depth = loop_exit_depths.empty()
        ? (dfb.empty() ? 0 : dfb.size() - 1) : loop_exit_depths.back().second;
    for (size_t i = dfb.size(); i > defer_depth; --i)
        ln(ind, cat("vdfreset(", dfb[i - 1], ");"));
    for (size_t i = scopes.size(); i > scope_depth; --i)
        ln(ind, cat("vscope_end(vint(", scopes[i - 1], "));"));
    ln(ind, k + ";");
}

void Emitter::emit_defer(const Expr &e, const std::string &ind) {
    if (fin == "__fin_top" && dfb.empty()) return;
    int code = fn_code(cat("clo:", clos.size()));
    std::set<std::string> outer;
    if (cap) outer = cap->second;
    std::set<std::string> reads, writes;
    expr_vars(e, reads);
    expr_set_vars(e, writes);
    std::set<std::string> visible;
    for (const auto &[n, s] : sym) visible.insert(n);
    for (const auto &n : dfb_locals) visible.insert(n);
    for (const auto &n : outer) visible.insert(n);
    std::set<std::string> capnames;
    for (const auto &n : reads)
        if (visible.count(n)) capnames.insert(n);
    for (const auto &n : writes)
        if (visible.count(n)) capnames.insert(n);
    for (const auto &n : outer) capnames.insert(n);
    Stmt &syn = synth.emplace_back();
    syn.k = SK::Expr;
    syn.e1 = clone(e);
    clos.push_back({&no_params(), StmtList{&syn}, capnames});
    std::string cm = new_tmp("m");
    std::string cb = new_tmp("cb");
    std::vector<std::string> pre = {cat("Map *", cm, " = vmap(\"\");")};
    for (const auto &var : capnames)
        for (auto &l : capture(cm, var)) pre.push_back(l);
    pre.push_back(cat("Vec *", cb, " = vvec();"));
    pre.push_back(cat("vec_push(", cb, ", vint(", code, "));"));
    pre.push_back(cat("{ Val _cv = vstruct(", cm, "); vec_push(", cb, ", _cv); }"));
    pre.push_back(cat("vdfadd(vtagv(\"closure\", ", cb, "));"));
    for (auto &p : pre) ln(ind, p);
}

void Emitter::st(const Stmt &s, const std::string &ind) {
    switch (s.k) {
    case SK::Block:
        emit_block(view(s.body), ind);
        return;
    case SK::Pub:
        st(*s.body[0], ind);
        return;
    case SK::Trait:
    case SK::Enum:
    case SK::Impl:
    case SK::Import:
    case SK::PubUse:
        return;
    case SK::Let:
    case SK::LetTmp: {
        const std::string &name = s.a;
        const Expr &e = *s.e1;
        if (global_lets.count(name) && fin == "__fin_top") {
            V v = ex(e);
            for (auto &p : v.pre) ln(ind, p);
            ln(ind, cat(cname(name), " = (", keep(e, v.val), ");"));
            emit_err_check(ind);
            assigned.insert(name);
            return;
        }
        V v = ex(e);
        for (auto &p : v.pre) ln(ind, p);
        if (name.find('.') != std::string::npos) {
            auto dot = name.find('.');
            std::string head = name.substr(0, dot), rest = name.substr(dot + 1);
            if (sym.count(head)) {
                ln(ind, cat("vfieldset(", sym[head].cn, ", \"", rest, "\", ", keep(e, v.val), ");"));
                emit_err_check(ind);
                return;
            }
        }
        Own own = own_of(e);
        bool fresh = dec_able(e, heap_returns());
        auto root = place_root(e);
        if (!fresh && e.k != EK::Call) v.val = cat("vrc_inc(", v.val, ")");
        bool lit = v.val.rfind("vlit(", 0) == 0;
        if ((reclaim.count(name) || relet.count(name)) && lit) {
            v.val = "vstr(" + v.val.substr(5);
            own = Own::Owned;
        }
        if (e.k == EK::Move && !e.kids.empty() && e.kids[0]->k == EK::Var) {
            // Only `move x` empties `x`. A projection move leaves the parent
            // owning its other fields, and marking the root moved here is what
            // kept the parent from ever being released.
            auto src = place_root(e);
            if (src) moved.insert(*src);
        }
        auto d = sym.find(name);
        bool is_relet = relet.count(name) && d != sym.end() && !moved.count(name) &&
                        !dropped.count(name) && place_root(e) != std::optional<std::string>(name);
        // `let p: ptr[T]` -- the element is part of the declaration, and it is
        // what `p[i]` reads through later. Without an annotation, a call that
        // returns `ptr[T]` is the declaration: `let p = node_new(...)` is the
        // normal shape of a C API and needs no second spelling.
        std::string prel;
        std::string selems;
        if (s.ann && s.ann->rfind("slice[", 0) == 0 && s.ann->back() == ']')
            selems = s.ann->substr(6, s.ann->size() - 7);
        else
            selems = slice_elem(e);
        if (s.ann && s.ann->rfind("ptr[", 0) == 0 && s.ann->back() == ']') {
            prel = s.ann->substr(4, s.ann->size() - 5);
        } else if (e.k == EK::Call) {
            for (const auto &ex0 : externs) {
                if (ex0.name != e.a || !ex0.ret) continue;
                if (ex0.ret->rfind("ptr[", 0) == 0 && ex0.ret->back() == ']')
                    prel = ex0.ret->substr(4, ex0.ret->size() - 5);
                break;
            }
        }
        std::string cn = var_def(name, own, prel, selems);
        assigned.insert(name);
        if (is_relet) {
            // After the RHS: it may still read the previous pass value.
            std::string tmp = new_tmp("v");
            ln(ind, cat("Val ", tmp, " = (", v.val, ");"));
            ln(ind, cat("vrc_dec(", d->second.cn, ");"));
            ln(ind, cat(cn, " = ", tmp, ";"));
        } else {
            ln(ind, cat(cn, " = (", v.val, ");"));
        }
        emit_err_check(ind);
        if (!name.empty() && name[0] == '@' && !fresh) alias_temps.insert(name);
        return;
    }
    case SK::Set: {
        const std::string &tgt = s.a;
        const Expr &e = *s.e1;
        auto d = sym.find(tgt);
        bool self_concat = e.k == EK::Bin && e.a == "+" && e.kids[0]->k == EK::Var && e.kids[0]->a == tgt;
        Own own;
        V v;
        std::vector<std::string> drop;
        if (self_concat) {
            v = ex(*e.kids[1]);
            for (auto &p : v.pre) ln(ind, p);
            if (auto d = owned_temp(*e.kids[1], v.val)) drop.push_back(*d);
            // `s = s + f()`: the append must not read the value a failed `f`
            // left behind; the error goes to the enclosing try.
            if (!v.pre.empty() && may_raise(*e.kids[1])) emit_err_check(ind);
            own = Own::Owned;
            std::string cn_tmp = var_def(tgt, own);
            v.val = cat("vadd_self(", cn_tmp, ", ", v.val, ")");
        } else {
            v = ex(e);
            for (auto &p : v.pre) ln(ind, p);
            if (auto d = owned_temp(e, v.val)) drop.push_back(*d);
            // `n = n + f()`: see the append case above.
            if (!v.pre.empty() && may_raise(e)) emit_err_check(ind);
            own = own_of(e);
            if (reclaim.count(tgt) && v.val.rfind("vlit(", 0) == 0) {
                v.val = "vstr(" + v.val.substr(5);
                own = Own::Owned;
            }
        }
        if (reclaim.count(tgt) && v.val.rfind("vlit(", 0) == 0 && !self_concat) {
            v.val = "vstr(" + v.val.substr(5);
            own = Own::Owned;
        }
        bool release = reclaim.count(tgt) && d != sym.end() && !moved.count(tgt) && !dropped.count(tgt) &&
                       (self_concat || place_root(e) != std::optional<std::string>(tgt));
        if (!dec_able(e, heap_returns())) v.val = cat("vrc_inc(", v.val, ")");
        std::string cn = var_def(tgt, own);
        assigned.insert(tgt);
        // A store must not land when the RHS raised: the target keeps its old
        // value and the enclosing try sees the error, as in the interpreter.
        if (self_concat || release || may_raise(e)) {
            std::string tmp = new_tmp("v");
            ln(ind, cat("Val ", tmp, " = (", v.val, ");"));
            emit_err_check(ind, drop);
            // After the RHS, so a hot store loop pays one dec per store. Never
            // for an append: `vadd_self` hands back the receiver itself.
            if (release && !self_concat) ln(ind, cat("vrc_dec(", d->second.cn, ");"));
            ln(ind, cat(cn, " = ", tmp, ";"));
            // `vadd_self` copies the right side in and keeps its own reference,
            // so the temp this statement built dies here, not only on error.
            if (self_concat)
                for (auto &t : drop) ln(ind, cat("vdrop_temp(", t, ");"));
        } else {
            ln(ind, cat(cn, " = (", v.val, ");"));
            emit_err_check(ind);
        }
        return;
    }
    case SK::SetIdx: {
        if (std::string pe = ptr_elem(*s.e1); !pe.empty()) {
            st_ptr_setidx(pe, s, ind);
            return;
        }
        V b = ex(*s.e1);
        V i = ex(*s.e2);
        V v = ex(*s.e3);
        for (auto &p : b.pre) ln(ind, p);
        for (auto &p : i.pre) ln(ind, p);
        for (auto &p : v.pre) ln(ind, p);
        // `m["a"]["b"] = 1` indexes into a temporary row; that reference is
        // nobody's once the store is done, so it is held in a temp and
        // released (`vat_owned` made every index read an owned value).
        if (dec_able(*s.e1, heap_returns())) {
            std::string r = new_tmp("r");
            ln(ind, cat("Val ", r, " = (", b.val, ");"));
            ln(ind, cat("vset_at(", r, ", ", keep(*s.e2, i.val), ", ", keep(*s.e3, v.val), ");"));
            ln(ind, cat("vdrop_temp(", r, ");"));
        } else {
            ln(ind, cat("vset_at(", b.val, ", ", keep(*s.e2, i.val), ", ", keep(*s.e3, v.val), ");"));
        }
        emit_err_check(ind);
        if (s.e1->k == EK::Var) assigned.insert(s.e1->a);
        return;
    }
    case SK::SetField: {
        V b = ex(*s.e1);
        V v = ex(*s.e2);
        for (auto &p : b.pre) ln(ind, p);
        for (auto &p : v.pre) ln(ind, p);
        // Same receiver rule as a field read: `v[0].x = 5` stores through a
        // reference nobody owns, so it is held and released like `m[k][j] = v`.
        if (dec_able(*s.e1, heap_returns())) {
            std::string r = new_tmp("r");
            ln(ind, "Val " + r + " = (" + b.val + ");");
            ln(ind, cat("vfieldset(", r, ", \"", s.a, "\", ", keep(*s.e2, v.val), ");"));
            ln(ind, "vdrop_temp(" + r + ");");
        } else {
            ln(ind, cat("vfieldset(", b.val, ", \"", s.a, "\", ", keep(*s.e2, v.val), ");"));
        }
        emit_err_check(ind);
        return;
    }
    case SK::Return: {
        V v = ex(*s.e1);
        for (auto &p : v.pre) ln(ind, p);
        emit_err_check(ind);
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            ln(ind, cat("vscope_end(vint(", *it, "));"));
        // The value leaves with the caller; the exit drops must not free it.
        // `a[i]` already hands back a reference the holder owns (vat_owned), so
        // the container can just be dropped at the exit like any other local;
        // lifting the part out and stealing it was the pre-vat_owned shape.
        const bool owned_part = is_index_read(*s.e1);
        auto root = place_root(*s.e1);
        if (root && !owned_part && sym.count(*root)) {
            if (part_of_local(*s.e1, *root)) {
                // `return v[0]` hands out a part of a local container: the part is
                // lifted into a temp, then the container goes.
                std::string t = new_tmp("c");
                ln(ind, "Val " + t + " = (" + v.val + ");");
                ln(ind, "vdrop_recv_part(" + sym[*root].cn + ", " + t + ");");
                dropped.insert(*root);
                v.val = t;
            } else {
                // A script-level binding outlives the frame that returns it, so
                // the caller gets a reference of its own; without the inc nobody
                // may release the result.
                if (global_lets.count(*root)) v.val = cat("vrc_inc(", v.val, ")");
                shared.insert(*root);
            }
        }
        if (root && cur_params.count(*root) && !owned_part) {
            // `return x` and `return v[0]` hand back storage owned by the
            // argument vector, which the caller releases: the value needs a
            // reference of its own or the caller holds freed memory.
            v.val = cat("vrc_inc(", v.val, ")");
        }
        std::string handed = (root && !owned_part && !part_of_local(*s.e1, *root)) ? *root : "";
        Names keep;
        refs_in_expr(s.e1.get(), keep);
        emit_fn_drops(ind, handed, true, keep);
        if (fin == "__fin_top") {
            ln(ind, cat("{ fprintf(stderr, \"unhandled: \"); vprint_err(", v.val,
                        "); fprintf(stderr, \"\\n\"); return 1; }"));
        } else {
            ln(ind, cat("{ __ret = (", v.val, "); goto ", fin, "; }"));
        }
        return;
    }
    case SK::Expr: {
        V v = ex(*s.e1);
        for (auto &p : v.pre) ln(ind, p);
        if (dec_able(*s.e1, heap_returns())) {
            // A statement whose value nobody keeps owns it. `vadd(vlit("a"),
            // vlit("b"))` in an `assert` builds a string, and leaving it as
            // `(void)(...)` leaked it once per evaluation; the temp is released
            // before the error check, so a raise cannot skip it.
            std::string t = new_tmp("ex");
            ln(ind, cat("Val ", t, " = (", v.val, ");"));
            ln(ind, cat("vdrop_temp(", t, ");"));
        } else {
            ln(ind, cat("(void)(", v.val, ");"));
        }
        emit_err_check(ind);
        return;
    }
    case SK::If: {
        V c = ex(*s.e1);
        for (auto &p : c.pre) ln(ind, p);
        emit_err_check(ind);
        ln(ind, cat("if (vis_true(", c.val, ")) {"));
        emit_block(view(s.body), ind + "  ");
        const Stmt *els = real_else(s);
        if (!els) {
            ln(ind, "}");
            return;
        }
        if (els->k != SK::Block) {
            // Braced even for a single statement: the statement may have pre-lines
            // of its own, and a declaration cannot follow a bare `else`
            // ("expected expression before `volatile`").
            ln(ind, "} else {");
            st(*els, ind + "  ");
            ln(ind, "}");
            return;
        }
        ln(ind, "} else {");
        emit_block(view(els->body), ind + "  ");
        ln(ind, "}");
        return;
    }
    case SK::While: {
        V c = ex(*s.e1);
        ln(ind, "while (1) {");
        for (auto &p : c.pre) ln(ind + "  ", p);
        emit_err_check(ind + "  ");
        ln(ind + "  ", cat("if (!vis_true(", c.val, ")) break;"));
        loop_exit_depths.emplace_back(scopes.size(), dfb.size());
        emit_block(view(s.body), ind + "  ");
        loop_exit_depths.pop_back();
        ln(ind, "}");
        emit_loop_drops(view(s.body), ind);
        return;
    }
    case SK::ForRange: {
        V a = ex(*s.e1);
        V b = ex(*s.e2);
        for (auto &p : a.pre) ln(ind, p);
        for (auto &p : b.pre) ln(ind, p);
        emit_err_check(ind);
        std::string lo = new_tmp("lo"), hi = new_tmp("hi"), kk = new_tmp("k");
        ln(ind, cat("int64_t ", lo, " = vlong(", a.val, ");"));
        ln(ind, cat("int64_t ", hi, " = vlong(", b.val, ");"));
        emit_err_check(ind);
        ln(ind, cat("for (int64_t ", kk, " = ", lo, "; ", kk, " < ", hi, "; ++", kk, ") {"));
        std::string cn = var_def(s.a, Own::Copy);
        ln(ind + "  ", cat(cn, " = vint(", kk, ");"));
        assigned.insert(s.a);
        loop_exit_depths.emplace_back(scopes.size(), dfb.size());
        emit_block(view(s.body), ind + "  ");
        loop_exit_depths.pop_back();
        ln(ind, "}");
        return;
    }
    case SK::ForIter: {
        V v = ex(*s.e1);
        for (auto &p : v.pre) ln(ind, p);
        emit_err_check(ind);
        std::string vv = new_tmp("v"), kk = new_tmp("k"), src = new_tmp("v");
        ln(ind, cat("Val ", src, " = (", v.val, ");"));
        ln(ind, cat("Vec *", vv, " = viter_vec(", src, ");"));
        emit_err_check(ind);
        ln(ind, cat("for (int64_t ", kk, " = 0; ", kk, " < ", vv, "->len; ++", kk, ") {"));
        std::string cn = var_def(s.a, Own::Copy);
        ln(ind + "  ", cat(cn, " = ", vv, "->items[", kk, "];"));
        assigned.insert(s.a);
        loop_exit_depths.emplace_back(scopes.size(), dfb.size());
        emit_block(view(s.body), ind + "  ");
        loop_exit_depths.pop_back();
        ln(ind, "}");
        ln(ind, cat("viter_done(", src, ", ", vv, ");"));
        // `for x in [10, 20, 30]` builds a vector nobody else holds; the loop
        // borrowed it, so the reference that built it is released here.
        if (dec_able(*s.e1, heap_returns())) ln(ind, cat("vdrop_temp(", src, ");"));
        return;
    }
    case SK::Break:
        emit_loop_exit("break", ind);
        return;
    case SK::Continue:
        emit_loop_exit("continue", ind);
        return;
    case SK::Try: {
        try_n++;
        int n = try_n;
        std::string catch_lbl = cat("__catch", n), after_lbl = cat("__after_try", n);
        auto saved_goto = err_goto;
        auto saved_base = err_base;
        std::string b = new_tmp("dfb");
        ln(ind, "{");
        ln(ind + "  ", cat("int64_t ", b, " = vdfsp;"));
        err_goto = catch_lbl;
        err_base = b;
        emit_block(view(s.body), ind + "  ");
        err_goto = saved_goto;
        err_base = saved_base;
        ln(ind + "  ", cat("goto ", after_lbl, ";"));
        ln(ind, cat(catch_lbl, ":;"));
        for (const auto &nm : try_let_names(view(s.body))) {
            // The body's own bindings cannot be live here and their drop at the
            // end of the block is unreachable from this path. Released through
            // `vrelease`: it leaves the error value alone, which the channel is
            // about to free.
            ln(ind + "  ", cat("vrelease(", cname(nm), ");"));
            ln(ind + "  ", cat(cname(nm), " = vnil();"));
        }
        if (s.opt1) {
            std::string cn = var_def(*s.opt1, Own::Copy);
            ln(ind + "  ", cat(cn, " = verr_catch_part();"));
            assigned.insert(*s.opt1);
        } else {
            ln(ind + "  ", "(void)verr_take();");
        }
        emit_block(view(s.body2), ind + "  ");
        ln(ind + "  ", "verr_catch_end();");
        if (s.opt1) {
            // The error belongs to the error channel, which frees it here. The
            // binding must not be released again: a later `let` reuses the same C
            // local, and the exit cleanup would read a tag already freed.
            ln(ind + "  ", cat(cname(*s.opt1), " = vnil();"));
        }
        if (s.opt1 && sym.count(*s.opt1)) {
            sym.erase(*s.opt1);
            assigned.erase(*s.opt1);
        }
        ln(ind, cat(after_lbl, ":;"));
        ln(ind, "}");
        return;
    }
    case SK::Scope: {
        std::string sc = new_tmp("sc");
        ln(ind, cat("int64_t ", sc, " = vscope_begin().as.ival;"));
        scopes.push_back(sc);
        emit_block(view(s.body), ind);
        scopes.pop_back();
        ln(ind, cat("vscope_end(vint(", sc, "));"));
        return;
    }
    case SK::Spawn: {
        int code = fn_code(cat("clo:", clos.size()));
        // `spawn` copies the frame: the child gets the value each name had at
        // the launch, not a window onto the parent's binding. The names to copy
        // are therefore the ones the body mentions, not the ones the parent has
        // already assigned -- the old rule missed a name the parent assigned
        // *after* the launch, so the child read the script-level global directly
        // and the two backends disagreed about a plain `spawn { print(x) }`.
        std::set<std::string> capnames;
        Names mentioned;
        refs_in(view(s.body), mentioned);
        for (const auto &[nm, sy] : sym)
            if (mentioned.count(nm)) capnames.insert(nm);
        if (cap)
            for (const auto &nm : cap->second) capnames.insert(nm);
        clos.push_back({&no_params(), view(s.body), capnames});
        new_tmp("m");
        int n = tmp;
        ln(ind, cat("Map *_cm", n, " = vmap(\"\");"));
        for (const auto &var : capnames)
            for (auto &l : capture(cat("_cm", n), var)) ln(ind, l);
        ln(ind, cat("Vec *_cb", n, " = vvec();"));
        ln(ind, cat("vec_push(_cb", n, ", vint(", code, "));"));
        ln(ind, cat("{ Val _cv = vstruct(_cm", n, "); vec_push(_cb", n, ", _cv); }"));
        ln(ind, cat("vspawn(vtagv(\"closure\", _cb", n, "));"));
        return;
    }
    case SK::Defer:
        emit_defer(*s.e1, ind);
        return;
    case SK::Fn:
    case SK::ExportFn: {
        const FnDecl &f = s.fns[0];
        std::string name = f.name.value_or("");
        if (!nesting) {
            fns[name].decl = &f;
            bool have = false;
            for (const auto &o : order)
                if (!o.meth && o.a == name) have = true;
            if (!have) order.push_back({false, name, ""});
            late.push_back({name, &f});
        } else {
            std::string mangled;
            auto it = fn_alias.find(name);
            if (it == fn_alias.end()) {
                nested_seq++;
                mangled = cat(name, "@", nested_seq);
                fns[mangled].decl = &f;
                fn_alias[name] = mangled;
                late.push_back({mangled, &f});
            } else {
                mangled = it->second;
            }
            bool have = false;
            for (const auto &l : late)
                if (l.name == mangled) have = true;
            if (!have) late.push_back({mangled, &f});
        }
        return;
    }
    case SK::Extern:
    case SK::ExternStruct:
    case SK::Unsafe:
        if (s.k == SK::Unsafe) emit_block(view(s.body), ind);
        return;
    }
}


std::pair<std::set<std::string>, std::set<std::string>> reclaim_names(
    const StmtList &body, const std::set<std::string> &params,
    const std::set<std::string> &heap) {
    std::map<std::string, bool> ok;
    std::set<std::string> has_set, loop_let;
    std::function<void(const StmtList &, bool)> walk = [&](const StmtList &stmts, bool loop) {
        for (const Stmt *st : stmts) {
            switch (st->k) {
            case SK::Let:
            case SK::LetTmp: {
                std::string n = head_of(st->a);
                ok[n] = (ok.count(n) ? ok[n] : true) && dec_able(*st->e1, heap);
                if (loop) loop_let.insert(n);
                break;
            }
            case SK::Set: {
                std::string n = head_of(st->a);
                ok[n] = (ok.count(n) ? ok[n] : true) && dec_able(*st->e1, heap);
                has_set.insert(n);
                break;
            }
            case SK::ForIter:
                ok[st->a] = false;   // elements come from the container
                walk(view(st->body), true);
                break;
            case SK::ForRange:
                ok[st->a] = false;
                walk(view(st->body), true);
                break;
            case SK::If:
                walk(view(st->body), loop);
                if (st->els) {
                    if (st->els->k == SK::Block) {
                        walk(view(st->els->body), loop);
                    } else {
                        walk({st->els.get()}, loop);
                    }
                }
                break;
            case SK::While:
                walk(view(st->body), true);
                break;
            case SK::Try:
                walk(view(st->body), loop);
                walk(view(st->body2), loop);
                if (st->opt1) ok[*st->opt1] = false;
                break;
            default:
                break;   // a spawn body is its own frame, like a closure
            }
        }
    };
    walk(body, false);
    std::set<std::string> reclaim, relet;
    for (const auto &[n, v] : ok) {
        if (v && has_set.count(n) && !params.count(n)) reclaim.insert(n);
    }
    for (const auto &n : loop_let) {
        auto it = ok.find(n);
        if (it != ok.end() && it->second && !params.count(n)) relet.insert(n);
    }
    return {reclaim, relet};
}


void Emitter::define_fn(const std::string &cn, const std::vector<Param> &params, const StmtList &body,
                        const std::set<std::string> *caps, const std::string *fastraw,
                        const std::string *fastkind, const std::string *fastname) {
    std::vector<std::string> names;
    for (const auto &p : params) names.push_back(p.name);
    auto saved_params = cur_params;
    cur_params = std::set<std::string>(names.begin(), names.end());
    auto saved_sym = sym;
    auto saved_cap = cap;
    auto saved_dfb = dfb;
    auto saved_fin = fin;
    // A top-level `let` is a file-scope Val; a function that reads it resolves
    // the name through the same table as its own locals. A name this closure
    // captured is the exception: it resolves through the capture map, because
    // the child holds the value the parent had at the launch, not a window onto
    // the global the parent may be writing.
    sym.clear();
    for (const auto &n : global_lets)
        if (!caps || !caps->count(n)) sym[n] = Sym{cname(n), Own::Copy};
    auto saved_assigned = assigned;
    assigned.clear();
    cap.reset();
    if (caps) cap = std::make_pair(std::string("cap"), *caps);
    std::string dfbname = cat("__vdf_", cn);
    dfb.clear();
    fin = cat("__fin_", cn);
    ln("", cat("static Val ", cn, "(Map *cap, Vec *args) {"));
    ln("  ", "(void)cap;");
    ln("  ", cat("if (args->len != ", names.size(), ") { vraise(\"arity\"); return vunwind; }"));
    if (fastraw) {
        const std::string &kind = *fastkind;
        // The body is raw C, so a `-> bool` annotation was lost on the way back:
        // the wrapper built vint(1) and the program printed `1` where the
        // interpreter printed `True`. The annotation is the only place that
        // knows, and it lives in fn_ret.
        bool ret_bool = false;
        if (fastname) {
            auto ri = fn_ret.find(*fastname);
            if (ri != fn_ret.end() && ri->second && *ri->second == "bool") ret_bool = true;
        }
        const std::string vret =
            ret_bool ? "vbool((__r) != 0)" : (kind == "f64" ? "vfloat(__r)" : "vint(__r)");
        if (kind == "f64") {
            std::string checks, rawargs;
            for (size_t i = 0; i < names.size(); i++) {
                if (i) {
                    checks += " && ";
                    rawargs += ", ";
                }
                checks += cat("(args->items[", i, "].tag == V_INT || args->items[", i, "].tag == V_FLOAT)");
                rawargs += cat("(args->items[", i, "].tag == V_INT ? (double)args->items[", i,
                               "].as.ival : args->items[", i, "].as.fval)");
            }
            ln("  ", cat("if (", checks.empty() ? "1" : checks, ") { double __r = ", *fastraw, "(", rawargs,
                         "); if (verr_pending()) return verr_leave(); return ", vret, "; }"));
        } else if (kind == "i64v") {
            const std::set<std::string> &vv = fastvec[*fastname];
            std::string checks, rawargs;
            for (size_t i = 0; i < names.size(); i++) {
                if (i) {
                    checks += " && ";
                    rawargs += ", ";
                }
                if (vv.count(names[i])) {
                    // The tag says vec, not vec[i64]: the raw body reads
                    // elements without a per-element check, so the wrapper scans
                    // once and falls back if one is not an int.
                    checks += cat("args->items[", i, "].tag == V_VEC && vvec_all_int(args->items[", i, "].as.vec)");
                    rawargs += cat("args->items[", i, "].as.vec");
                } else {
                    checks += cat("args->items[", i, "].tag == V_INT");
                    rawargs += cat("args->items[", i, "].as.ival");
                }
            }
            ln("  ", cat("if (", checks.empty() ? "1" : checks, ") { int64_t __r = ", *fastraw, "(", rawargs,
                         "); if (verr_pending()) return verr_leave(); return ", vret, "; }"));
        } else {
            std::string checks, rawargs;
            for (size_t i = 0; i < names.size(); i++) {
                if (i) {
                    checks += " && ";
                    rawargs += ", ";
                }
                checks += cat("args->items[", i, "].tag == V_INT");
                rawargs += cat("args->items[", i, "].as.ival");
            }
            ln("  ", cat("if (", checks.empty() ? "1" : checks, ") { int64_t __r = ", *fastraw, "(", rawargs,
                         "); if (verr_pending()) return verr_leave(); return ", vret, "; }"));
        }
    }
    ln("  ", "if (bdepth_push()) { vraise(\"recursion too deep\"); return vunwind; }");
    ln("  ", "Val __ret = vnil();");
    ln("  ", cat("volatile int64_t ", dfbname, " = vdfsp;"));
    ln("  ", "vdfpush();");
    std::set<std::string> vars(names.begin(), names.end());
    collect_vars(body, vars);
    std::set<std::string> caps_set = caps ? *caps : std::set<std::string>();
    // A script-level `let` is a file-scope Val that `sym` already points at. A
    // local of the same name would shadow it, and the assignment would land in
    // the local while the file-scope value the top level wrote stayed `vnil()`
    // -- `type error in +` on the next read. A captured name is not a global
    // here: the child holds its own copy by design.
    // Only globals the body does not bind itself: `fn bump(mut m)` next to a
    // script-level `let mut m` is a parameter that shadows the global, and
    // erasing it left the parameter undeclared -- the callee wrote through a
    // stale local and the top-level release of `m` fired on a freed value
    // (ASan, map.brat).
    {
        std::set<std::string> own;
        bound_in(body, own);
        std::set<std::string> params_of(names.begin(), names.end());
        for (const auto &g : global_lets)
            if (!caps_set.count(g) && !own.count(g) && !params_of.count(g)) vars.erase(g);
    }
    auto saved_reclaim = reclaim;
    auto saved_relet = relet;
    // A captured binding has no owner here: releasing it would drop the map's.
    auto rr = reclaim_names(body, std::set<std::string>(names.begin(), names.end()), heap_returns());
    reclaim = rr.first;
    relet = rr.second;
    for (const auto &c : caps_set) {
        reclaim.erase(c);
        relet.erase(c);
    }
    auto saved_shared = shared;
    auto saved_captured = captured;
    auto saved_dropped = dropped;
    shared.clear();
    // Per function: a name dropped in one function must still be released in
    // the next one that happens to use the same identifier.
    dropped.clear();
    captured.clear();
    auto saved_locals = dfb_locals;
    dfb_locals = vars;
    std::set<std::string> param_names(names.begin(), names.end());
    for (const auto &n : vars) {
        Own own = param_names.count(n) ? Own::Borrow : Own::Copy;
        sym[n] = Sym{cname(n), own};
        ln("  ", cat("Val BRAT_UNUSED ", cname(n), " = vnil();"));
    }
    for (const auto &n : relet)
        if (vars.count(n)) ln("  ", cat(cname(n), " = vnil();"));
    for (const auto &n : vars)
        if (caps_set.count(n) && !param_names.count(n))
            ln("  ", cat(cname(n), " = (*map_get(cap, \"", n, "\"));"));
    for (size_t i = 0; i < names.size(); i++) {
        ln("  ", cat(cname(names[i]), " = args->items[", i, "];"));
        assigned.insert(names[i]);
    }
    nesting++;
    for (const Stmt *t : body) st(*t, "  ");
    nesting--;
    ln("  ", cat(fin, ":"));
    if (move_rt) {
        // The label is the single exit point, so locals are released here and
        // not at the end of the body, which every `return` jumps over.
        emit_fn_drops("  ");
        // The same goes for the counted aliases a `match` takes of its
        // scrutinee: a `return` from inside an arm jumps over the drop at the
        // end of the block, so the alias is given back here.
        for (const auto &name : std::set<std::string>(alias_temps)) {
            auto it = sym.find(name);
            if (it == sym.end() || dropped.count(name)) continue;
            ln("  ", cat("vrc_dec(", it->second.cn, ");"));
            alias_temps.erase(name);
        }
    }
    ln("  ", "bdepth_pop();");
    ln("  ", cat("vdfreset(", dfbname, ");"));
    ln("  ", "return __ret;");
    ln("", "}");
    ln("", "");
    cur_params = saved_params;
    sym = saved_sym;
    cap = saved_cap;
    dfb = saved_dfb;
    fin = saved_fin;
    dfb_locals = saved_locals;
    assigned = saved_assigned;
    reclaim = saved_reclaim;
    relet = saved_relet;
    shared = saved_shared;
    captured = saved_captured;
    dropped = saved_dropped;
}

void Emitter::emit_fn_by_key(const std::string &key) {
    if (key.rfind("fn:", 0) == 0) {
        std::string name = key.substr(3);
        const FnDecl &f = *fns[name].decl;
        const std::string *raw = nullptr, *kind = nullptr, *fname = nullptr;
        if (fastc.count(name)) {
            raw = &fastc[name];
            kind = &fastkinds[name];
            fname = &name;
        }
        define_fn(cfn(key), f.params, view(f.body), nullptr, raw, kind, fname);
    } else if (key.rfind("meth:", 0) == 0) {
        std::string rest = key.substr(5);
        auto dot = rest.rfind('.');
        std::string tname = rest.substr(0, dot), mname = rest.substr(dot + 1);
        const FnDecl &f = *fns[tname + "." + mname].decl;
        define_fn(cfn(key), f.params, view(f.body), nullptr, nullptr, nullptr, nullptr);
    } else {
        throw EmitError("bad fn key " + key);
    }
}

void Emitter::emit_prelude_tables() {
    ln("", "static const struct { const char *type, *meth; int code; } MTAB[] = {");
    std::vector<std::pair<std::pair<std::string, std::string>, int>> mt(mtab.begin(), mtab.end());
    std::sort(mt.begin(), mt.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    for (const auto &[k, code] : mt)
        ln("  ", cat("{\"", clit(k.first), "\", \"", clit(k.second), "\", ", code, "},"));
    ln("  ", "{0, 0, 0}");
    ln("", "};");
    ln("", "static const struct { const char *tag, *type; } VTAB[] = {");
    for (const auto &[tag, tname] : vtab)
        ln("  ", cat("{\"", clit(tag), "\", \"", clit(tname), "\"},"));
    ln("  ", "{0, 0}");
    ln("", "};");
    ln("", "static Val vcall_any(Val fn, Vec *a);");
    ln("", "static Val vcall_any(Val fn, Vec *a) {");
    ln("  ", "(void)a;");
    ln("  ", "if (fn.tag != V_TAG || strcmp(fn.as.tv.tag, \"closure\") || fn.as.tv.args->len != 2) { "
            "vraise(\"not callable\"); return vunwind; }");
    ln("  ", "int64_t code = vlong(fn.as.tv.args->items[0]);");
    ln("  ", "Val cp = fn.as.tv.args->items[1];");
    ln("  ", "Map *cap = cp.tag == V_STRUCT ? cp.as.map : NULL;");
    ln("  ", "(void)cap;");
    ln("  ", "switch (code) {");
    std::vector<std::pair<std::string, int>> cd(codes.begin(), codes.end());
    std::sort(cd.begin(), cd.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    for (const auto &[key, code] : cd) ln("    ", cat("case ", code, ": return ", cfn(key), "(cap, a);"));
    ln("  ", "}");
    ln("  ", "vraise(\"bad code\"); return vunwind;");
    ln("", "}");
    ln("", "static Val __attribute__((unused)) vcall_method(Val obj, const char *meth, Vec *a) {");
    ln("  ", "(void)a;");
    ln("  ", "const char *tn = NULL;");
    ln("  ", "int64_t i;");
    ln("  ", "if (obj.tag == V_STRUCT) tn = obj.as.map->type;");
    ln("  ", "else if (obj.tag == V_TAG) { for (i = 0; VTAB[i].tag; i++) if (!strcmp(VTAB[i].tag, "
            "obj.as.tv.tag)) tn = VTAB[i].type; }");
    ln("  ", "if (tn) for (i = 0; MTAB[i].type; i++) if (!strcmp(MTAB[i].type, tn) && "
            "!strcmp(MTAB[i].meth, meth)) {");
    ln("    ", "Vec *b = vvec(); vec_push(b, obj);");
    ln("    ", "for (int64_t j = 0; j < a->len; j++) vec_push(b, a->items[j]);");
    ln("    ", "int64_t code = MTAB[i].code;");
    ln("    ", "switch (code) {");
    // `b` is this dispatcher's own argument vector. The callee copies what it
    // needs out of it, so the container goes right after the call -- emptied
    // first, because it never owned its items and freeing it would drop them.
    for (const auto &[key, code] : cd) {
        if (move_rt)
            ln("    ", cat("case ", code, ": { Val r = ", cfn(key),
                           "(NULL, b); b->len = 0; vrc_dec(vvecv(b)); return r; }"));
        else
            ln("    ", cat("case ", code, ": return ", cfn(key), "(NULL, b);"));
    }
    ln("    ", "}");
    ln("  ", "}");
    ln("  ", "return vbuiltin(obj, meth, a);");
    ln("", "}");
    ln("", "static Val __attribute__((unused)) vmap_with(Val obj, Val fn, "
           "const char *meth) {");
    ln("  ", "Vec *w = vvec_of_m(obj, meth);");
    ln("  ", "Vec *o = vvec();");
    ln("  ", "Vec *one = vvec();");
    ln("  ", "for (int64_t i = 0; i < w->len; i++) {");
    ln("    ", "one->len = 0; vec_push(one, w->items[i]);");
    ln("    ", "Val r = vcall_any(fn, one);");
    ln("    ", "if (verr_pending()) {");
    ln("      ", "one->len = 0; vrc_dec(vvecv(one)); vrc_dec(fn); vrc_dec(vvecv(o));");
    ln("      ", "return r;");
    ln("    ", "}");
    // `one` borrows its item, so it is emptied before release; `o` owns what map
    // returned and co-owns what filter kept, which is why filter takes a reference.
    ln("    ", "if (meth[0] == 'm') vec_push(o, r);");
    ln("    ", "else if (vis_true(r)) vec_push(o, vrc_inc(w->items[i]));");
    ln("  ", "}");
    ln("  ", "one->len = 0; vrc_dec(vvecv(one)); vrc_dec(fn);");
    ln("  ", "return vvecv(o);");
    ln("", "}");
}

void Emitter::run_pipeline() {
    for (const auto &o : order) {
        if (!o.meth) {
            fn_code("fn:" + o.a);
        } else {
            fn_code("meth:" + o.a + "." + o.b);
        }
    }
    for (const auto &o : order)
        if (o.meth) mtab[std::make_pair(o.a, o.b)] = fn_code("meth:" + o.a + "." + o.b);
    user_meths.clear();
    for (const auto &[k, v] : mtab) user_meths.insert(k.second);
    compute_fast();
    if (opt >= 1) run_opt();
    if (need_idiv) {
        ln("", "static int64_t bf_idiv(int64_t a, int64_t b) {");
        ln("  ", "if (!b) { vraise(\"div by zero\"); return 0; }");
        ln("  ", "if (a == INT64_MIN && b == -1) { return INT64_MIN; }");
        ln("  ", "int64_t q = a / b;");
        ln("  ", "if ((a % b) && ((a < 0) != (b < 0))) q--;");
        ln("  ", "return q;");
        ln("", "}");
    }
    if (need_imod) {
        ln("", "static int64_t bf_imod(int64_t a, int64_t b) {");
        ln("  ", "if (!b) { vraise(\"div by zero\"); return 0; }");
        ln("  ", "if (a == INT64_MIN && b == -1) { return 0; }");
        ln("  ", "int64_t r = a % b;");
        ln("  ", "if (r && ((r < 0) != (b < 0))) r += b;");
        ln("  ", "return r;");
        ln("", "}");
    }
    if (need_fdiv) {
        ln("", "static double bf_fdiv(double a, double b) {");
        ln("  ", "if (b == 0.0) { vraise(\"div by zero\"); return 0.0; }");
        ln("  ", "return a / b;");
        ln("", "}");
    }
    if (need_fmod) {
        ln("", "static double bf_fmod(double a, double b) {");
        ln("  ", "if (b == 0.0) { vraise(\"div by zero\"); return 0.0; }");
        ln("  ", "double r = fmod(a, b);");
        ln("  ", "if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;");
        ln("  ", "return r;");
        ln("", "}");
    }
    if (need_viat) {
        ln("", "static int64_t bf_viat(Vec *v, int64_t i) {");
        ln("  ", "if (i < 0 || i >= v->len) { vraise(\"index out of range\"); return 0; }");
        ln("  ", "Val x = v->items[i];");
        ln("  ", "if (x.tag != V_INT) { vraise(\"vec elem not int\"); return 0; }");
        ln("  ", "return x.as.ival;");
        ln("", "}");
    }
    if (need_vset) {
        ln("", "static void bf_vset(Vec *v, int64_t i, int64_t x) {");
        ln("  ", "if (i < 0 || i >= v->len) { vraise(\"index out of range\"); return; }");
        ln("  ", "v->items[i] = vint(x);");
        ln("", "}");
    }
    emit_forward_decls();
    for (const auto &o : order) {
        if (!o.meth) {
            emit_fn_by_key("fn:" + o.a);
        } else {
            emit_fn_by_key("meth:" + o.a + "." + o.b);
        }
    }
    for (const auto &[name, e] : fast) emit_fast_fn(name);
    emit_closures_queue();
    emit_late();
}

void Emitter::emit_late() {
    while (late_done < static_cast<int>(late.size())) {
        while (late_declared < static_cast<int>(late.size())) {
            ln("", cat("static Val ", cfn("fn:" + late[late_declared].name), "(Map *cap, Vec *args);"));
            late_declared++;
        }
        emit_fn_by_key("fn:" + late[late_done].name);
        late_done++;
    }
    emit_closures_queue();
}

void Emitter::emit_main_open() {
    sym.clear();
    cap.reset();
    dfb.clear();
    fin = "__fin_top";
    if (codes.count("fn:main")) {
        ln("", "static const char *__brat_tagname(Val v) {");
        ln("  ", "switch (v.tag) {");
        ln("    ", "case V_NIL: return \"None\";");
        ln("    ", "case V_BOOL: return \"bool\";");
        ln("    ", "case V_INT: return \"i64\";");
        ln("    ", "case V_FLOAT: return \"f64\";");
        ln("    ", "case V_STR: return \"str\";");
        ln("    ", "case V_VEC: return \"vec\";");
        ln("    ", "case V_STRUCT: return \"struct\";");
        ln("    ", "default: return v.as.tv.tag ? v.as.tv.tag : \"tag\";");
        ln("  ", "}");
        ln("", "}");
    }
    ln("", "int main(int argc, char **argv) {");
    ln("  ", "vmem_budget_apply();");
    ln("  ", "vargs_set(argc, argv);");
    ln("  ", "bdepth_init();");
    ln("  ", cat("vbase_set(\"", cstr_path(base), "\");"));
    ln("  ", "Vec *_a = vvec();");
    std::set<std::string> names;
    std::function<void(const Stmt &)> scan = [&](const Stmt &s) {
        if (s.k == SK::Pub) {
            scan(*s.body[0]);
        } else if (s.k == SK::Fn || s.k == SK::Impl || s.k == SK::Trait || s.k == SK::Enum) {
            return;
        } else {
            collect_vars({&s}, names);
        }
    };
    for (const Stmt *s : top_exe) scan(*s);
    dfb_locals = names;
    // A script's top-level `let` is a binding like any other: it owns what it
    // holds. Naming them Copy was harmless while nothing released them, but it
    // meant a script leaked every heap value it bound at exit -- `let a =
    // args()` in syslib.brat left a vector behind on every run.
    // SK::LetTmp is the same binding with a name the parser made up (`_m2`
    // becomes `@m2`), so both kinds count.
    std::set<std::string> script_lets;
    for (const Stmt *s : top_exe)
        if (s->k == SK::Let || s->k == SK::LetTmp) script_lets.insert(s->a);
    for (const auto &n : names) {
        if (global_lets.count(n)) continue;
        sym[n] = Sym{cname(n), script_lets.count(n) ? Own::Owned : Own::Copy};
        ln("  ", cat("Val BRAT_UNUSED ", cname(n), " = vnil();"));
    }
    for (const auto &n : global_lets)
        sym[n] = Sym{cname(n), script_lets.count(n) ? Own::Owned : Own::Copy};
    for (const auto &n : relet)
        if (names.count(n)) ln("  ", cat(cname(n), " = vnil();"));
}

void Emitter::emit_main_close() {
    if (codes.count("fn:main")) {
        ln("  ", cat("Val __r = ", cfn("fn:main"), "(NULL, _a);"));
        ln("  ", "vrc_dec(vvecv(_a));");
        ln("  ", "if (verr_pending()) { fprintf(stderr, \"unhandled: \"); vprint_err(verr_take()); "
                "fprintf(stderr, \"\\n\"); return 1; }");
    } else {
        // `_a` is the empty argument vector every main is given. A script has
        // no user `main` to hand it to and the release used to sit in the
        // branch above, so script mode leaked one Vec per run -- the 152 bytes
        // LeakSanitizer kept reporting for lang/examples/map.brat.
        ln("  ", "vrc_dec(vvecv(_a));");
    }
    if (move_rt) emit_scope_drops("  ");
    ln("  ", "vjoin_all();");
    ln("  ", "if (verr_pending()) { fprintf(stderr, \"unhandled: \"); vprint_err(verr_take()); "
            "fprintf(stderr, \"\\n\"); return 1; }");
    if (codes.count("fn:main")) {
        ln("  ", "if (__r.tag == V_NIL) return 0;");
        ln("  ", "if (__r.tag == V_TAG) {");
        ln("    ", "if (__r.as.tv.tag && strcmp(__r.as.tv.tag, \"err\") == 0) {");
        ln("      ", "fprintf(stderr, \"unhandled: ('err', \");");
        ln("      ", "{ Val __p = vnil();");
        ln("        ", "if (__r.as.tv.args && __r.as.tv.args->len > 0) __p = __r.as.tv.args->items[0];");
        ln("        ", "if (__p.tag == V_STR) fprintf(stderr, \"'%s'\", __p.as.str);");
        ln("        ", "else { Val __s = vstr_of(__p); fprintf(stderr, \"%s\", __s.as.str); } }");
        ln("      ", "fprintf(stderr, \")\\n\");");
        ln("      ", "return 1;");
        ln("    ", "}");
        ln("    ", "fprintf(stderr, \"TypeError: main must return i64, got %s\\n\", __r.as.tv.tag);");
        ln("    ", "return 1;");
        ln("  ", "}");
        // `main` may hand back a container or a string; the process is about to
        // exit, but a leak check still sees it. Read the code first.
        ln("  ", "if (__r.tag == V_INT) { int __code = (int)__r.as.ival; vrelease(__r); return __code; }");
        ln("  ", "fprintf(stderr, \"TypeError: main must return i64, got %s\\n\", __brat_tagname(__r));");
        ln("  ", "return 1;");
    }
    ln("  ", "return 0;");
    ln("", "}");
}

std::string Emitter::val_to_c_struct(const std::string &sname, const std::string &valexpr) {
    std::string mems;
    for (const auto &sfield : *structs[sname]) {
        const std::string &f = sfield.name;
        std::string ft = sfield.ann.value_or("");
        std::string base_expr = cat("(*map_get((", valexpr, ").as.map, \"", f, "\"))");
        const std::string ffn = ffi_c(f);
        std::string member;
        if (ft == "str") {
            member = cat(".", ffn, " = ", base_expr, ".as.str");
        } else if (ft == "ptr") {
            member = cat(".", ffn, " = (void *)", base_expr, ".as.ival");
        } else if (ft == "f64") {
            member = cat(".", ffn, " = ", base_expr, ".as.fval");
        } else if (ft == "f32") {
            member = cat(".", ffn, " = (float)", base_expr, ".as.fval");
        } else if (structs.count(ft)) {
            member = cat(".", ffn, " = ", val_to_c_struct(ft, base_expr));
        } else {
            member = cat(".", ffn, " = ", base_expr, ".as.ival");
        }
        if (!mems.empty()) mems += ", ";
        mems += member;
    }
    return cat("(", ffi_c(sname), "){ ", mems, " }");
}

std::string Emitter::c_struct_to_val(const std::string &sname, const std::string &cexpr) {
    std::string sets;
    for (const auto &sfield : *structs[sname]) {
        const std::string &f = sfield.name;
        std::string ft = sfield.ann.value_or("");
        const std::string fexpr = cat(cexpr, ".", ffi_c(f));
        std::string val;
        if (ft == "str") {
            val = cat("vstr(", fexpr, ")");
        } else if (ft == "ptr") {
            val = cat("vint((int64_t)", fexpr, ")");
        } else if (ft == "f64") {
            val = cat("vfloat(", fexpr, ")");
        } else if (ft == "f32") {
            val = cat("vfloat((double)", fexpr, ")");
        } else if (structs.count(ft)) {
            val = c_struct_to_val(ft, fexpr);
        } else {
            val = cat("vint(", fexpr, ")");
        }
        if (!sets.empty()) sets += " ";
        sets += cat("map_set(_v.as.map, \"", f, "\", ", val, ");");
    }
    return cat("({ ", ffi_c(sname), " _r = ", cexpr, "; Val _v = vstruct(vmap(\"", sname, "\")); ", sets,
               "; _v; })");
}

void Emitter::emit_forward_decls() {
    ln("", "static Val vcall_any(Val fn, Vec *a);");
    ln("", "static Val vcall_method(Val obj, const char *meth, Vec *a);");
    ln("", "static Val vmap_with(Val obj, Val fn, const char *meth);");
    std::vector<std::pair<std::string, int>> cd(codes.begin(), codes.end());
    std::sort(cd.begin(), cd.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    for (const auto &[key, code] : cd)
        ln("", cat("static Val ", cfn(key), "(Map *cap, Vec *args);"));
    for (const auto &[name, e] : fast) {
        const std::vector<Param> &params = e.decl->params;
        std::string ct = fastkinds[name] == "f64" ? "double" : "int64_t";
        std::string types;
        if (fastkinds[name] == "i64v") {
            const std::set<std::string> &vv = fastvec[name];
            for (const auto &p : params) {
                if (!types.empty()) types += ", ";
                types += vv.count(p.name) ? "Vec *" : "int64_t";
            }
        } else {
            for (size_t i = 0; i < params.size(); i++) {
                if (i) types += ", ";
                types += ct;
            }
        }
        ln("", cat("static ", ct, " ", fastc[name], "(", types.empty() ? "void" : types, ");"));
    }
    ln("", "");
}

void Emitter::emit_closure_protos() {
    while (clos_declared < static_cast<int>(clos.size())) {
        ln("", cat("static Val ", cfn(cat("clo:", clos_declared)), "(Map *cap, Vec *args);"));
        clos_declared++;
    }
}

void Emitter::emit_closures_queue() {
    while (clos_done < static_cast<int>(clos.size())) {
        emit_closure_protos();
        const Closure &c = clos[clos_done];
        define_fn(cfn(cat("clo:", clos_done)), *c.params, c.body, &c.caps, nullptr, nullptr, nullptr);
        clos_done++;
        emit_closure_protos();
    }
}

void Emitter::emit_prelude_headers() {
    const char *case_version = std::getenv("BRAT_CASE_MAP_VERSION");
    ln("", case_version && std::strcmp(case_version, "1600") == 0
              ? "#define BRAT_CASE_MAP_VERSION 1600"
              : "#define BRAT_CASE_MAP_VERSION 1500");
    ln("", "#include \"crt.h\"");
    ln("", "");
}

std::string Emitter::emit_program() {
    // Two passes. A `fn` nested in a body, a closure inside a closure and a
    // spawn inside a nested fn are all discovered *while* emitting the body,
    // which is after the point where C needs their prototypes. Pass one emits
    // into a throwaway buffer purely to learn the complete set; pass two emits
    // the real text with every prototype up front. `codes` survives, so both
    // passes number the functions identically.
    out.clear();
    run_pipeline();
    cb_wrappers.clear();
    for (const auto &[cbname, info] : fns) {
        if (info.ctor || info.decl == nullptr) continue;
        bool ident = !cbname.empty() && (std::isalpha(static_cast<unsigned char>(cbname[0])) || cbname[0] == '_');
        for (char c : cbname)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) ident = false;
        if (!ident) continue;
        const std::vector<Param> &params = info.decl->params;
        std::optional<std::string> ret;
        auto rit = fn_ret.find(cbname);
        if (rit != fn_ret.end()) ret = rit->second;
        bool ok = true;
        std::vector<std::string> cparams;
        for (size_t i = 0; i < params.size(); i++) {
            const auto &ann = params[i].ann;
            std::string arg = cat("a", i);
            std::string t;
            if (!ann || *ann == "none") {
                // No annotation, no C width: the wrapper cannot be declared.
                ok = false;
                break;
            }
            if (structs.count(*ann)) {
                t = ffi_c(*ann);
            } else if (ann->rfind("ptr", 0) == 0 || ann->rfind("fn", 0) == 0) {
                t = "void *";
            } else if (ffi_scalar_c().count(*ann)) {
                t = ffi_scalar_c().at(*ann);
            } else {
                ok = false;
                break;
            }
            cparams.push_back(cat(t, " ", arg));
        }
        if (!ok) continue;
        std::string cret;
        if (!ret || *ret == "none") {
            cret = "void";
        } else if (structs.count(*ret)) {
            cret = ffi_c(*ret);
        } else if (ret->rfind("ptr", 0) == 0) {
            cret = "void *";
        } else if (ffi_scalar_c().count(*ret)) {
            cret = ffi_scalar_c().at(*ret);
        } else {
            continue;
        }
        cb_wrappers.push_back(Cb{cbname, cret, cparams});
    }
    out.clear();
    nested_seq = 0;
    clos_done = clos_declared = late_done = late_declared = 0;
    tmp = 0;
    // Ownership is re-derived in pass two; a stale `dropped` would suppress it.
    moved.clear();
    captured.clear();
    shared.clear();
    dropped.clear();
    scopes.clear();
    emit_prelude_headers();
    ln("", "#include <stdlib.h>");
    ln("", "#include <string.h>");
    ln("", "#include <errno.h>");
    // `offsetof` in the extern-struct layout assertions below.
    ln("", "#include <stddef.h>");
    // C needs a struct defined before the one that holds it by value, and the
    // declaration order in the file is whatever the author wrote.
    std::vector<std::string> sorted_structs;
    {
        std::set<std::string> seen;
        std::function<void(const std::string &)> visit = [&](const std::string &n) {
            if (seen.count(n) || !structs.count(n)) return;
            seen.insert(n);
            for (const auto &fld : *structs[n])
                if (fld.ann && structs.count(*fld.ann) && *fld.ann != n) visit(*fld.ann);
            sorted_structs.push_back(n);
        };
        for (const auto &n : struct_order) visit(n);
        for (const auto &kv : structs) visit(kv.first);
    }
    // Size, alignment and field offsets of every extern struct, computed here.
    // The C compiler is the only authority on layout, so this is not a second
    // opinion that can win: the numbers are asserted against it with
    // `_Static_assert` in the emitted C. What the computation buys is a floor on
    // the target's rules -- a field spelled at the wrong width, or a target whose
    // `long` is 4 bytes while `i64` claims 8, fails the build instead of
    // corrupting the struct at run time. Same numbers, same text as codegen_c.py.
    // The target's widths, not the host's: the Python emitter computes them
    // (`ffi.field_shape`) and passes the two numbers, so the triple is parsed
    // in one place and both emitters cannot drift.
    int ffi_pw = 8, ffi_a8 = 8;
    if (const char *e = std::getenv("BRAT_FFI_PTR")) ffi_pw = std::atoi(e);
    if (const char *e = std::getenv("BRAT_FFI_A8")) ffi_a8 = std::atoi(e);
    auto ffi_field_shape = [ffi_pw, ffi_a8](const std::string &t) -> std::optional<std::pair<int, int>> {
        static const std::map<std::string, std::pair<int, int>> shape = {
            {"i8", {1, 1}}, {"u8", {1, 1}}, {"i16", {2, 2}}, {"u16", {2, 2}},
            {"i32", {4, 4}}, {"u32", {4, 4}}, {"f32", {4, 4}},
            {"i64", {8, 8}}, {"u64", {8, 8}}, {"f64", {8, 8}},
        };
        if (t.rfind("ptr", 0) == 0 || t.rfind("fn", 0) == 0) return std::make_pair(ffi_pw, ffi_pw);
        if (t == "usize" || t == "str" || t == "ptr") return std::make_pair(ffi_pw, ffi_pw);
        auto it = shape.find(t);
        if (it == shape.end()) return std::nullopt;
        if (it->second.first == 8) return std::make_pair(8, ffi_a8);
        return it->second;
    };
    struct FFILayout {
        int size = 0;
        int align = 1;
        std::vector<std::pair<std::string, int>> slots;
    };
    std::map<std::string, FFILayout> ffi_layout;
    std::function<const FFILayout *(const std::string &)> ffi_of =
        [&](const std::string &n) -> const FFILayout * {
        auto done = ffi_layout.find(n);
        if (done != ffi_layout.end()) return &done->second;
        if (!structs.count(n)) return nullptr;
        const bool pk = packed.count(n) > 0;
        FFILayout lay;
        int off = 0, align = 1;
        for (const auto &sfld : *structs[n]) {
            const std::string sty = sfld.ann.value_or("");
            int size = 0, falign = 1;
            if (structs.count(sty)) {
                const FFILayout *sub = ffi_of(sty);
                if (!sub) return nullptr;
                size = sub->size;
                falign = sub->align;
            } else {
                auto sh = ffi_field_shape(sty);
                if (!sh) return nullptr;
                size = sh->first;
                falign = sh->second;
            }
            if (pk) falign = 1;
            off = (off + falign - 1) / falign * falign;
            lay.slots.push_back({sfld.name, off});
            off += size;
            align = std::max(align, falign);
        }
        if (pk) align = 1;
        else off = (off + align - 1) / align * align;
        lay.size = off;
        lay.align = align;
        auto ins = ffi_layout.emplace(n, std::move(lay));
        return &ins.first->second;
    };
    for (const auto &sname : sorted_structs) ffi_of(sname);

    for (const auto &sname : sorted_structs) {
        std::string mems;
        for (const auto &sfld : *structs[sname]) {
            const std::string &sf = sfld.name;
            const std::string sty = sfld.ann.value_or("");
            std::string m;
            const std::string sfn = ffi_c(sf);
            if (sty == "i64") {
                m = cat("int64_t ", sfn, ";");
            } else if (sty == "i32") {
                m = cat("int ", sfn, ";");
            } else if (sty == "u8") {
                // A byte field is one byte: falling through to `int64_t` put
                // every field after it at the wrong offset, which is exactly
                // what a packed record cannot survive.
                m = cat("unsigned char ", sfn, ";");
            } else if (sty == "i8") {
                m = cat("signed char ", sfn, ";");
            } else if (sty == "u16") {
                m = cat("unsigned short ", sfn, ";");
            } else if (sty == "i16") {
                m = cat("short ", sfn, ";");
            } else if (sty == "u32") {
                m = cat("unsigned ", sfn, ";");
            } else if (sty == "u64") {
                m = cat("uint64_t ", sfn, ";");
            } else if (sty == "usize") {
                m = cat("size_t ", sfn, ";");
            } else if (sty == "f64") {
                m = cat("double ", sfn, ";");
            } else if (sty == "f32") {
                m = cat("float ", sfn, ";");
            } else if (sty == "str") {
                m = cat("const char *", sfn, ";");
            } else if (sty == "ptr") {
                m = cat("void *", sfn, ";");
            } else if (structs.count(sty)) {
                m = cat(ffi_c(sty), " ", sfn, ";");
            } else {
                m = cat("int64_t ", sfn, ";");
            }
            if (!mems.empty()) mems += " ";
            mems += m;
        }
        // The attribute is what makes the offsets the header's, not the
        // compiler's: a packed record has no padding between its fields.
        ln("", cat(packed.count(sname) ? "typedef struct __attribute__((packed)) { "
                                      : "typedef struct { ",
                   mems, " } ", ffi_c(sname), ";"));
        // Compile-time only: no code, no data, no run-time cost. A struct whose
        // C layout disagrees with the one the emitter generated code for is an
        // ABI bug that would otherwise surface as a corrupted value in
        // production, or not at all.
        const FFILayout *lay = ffi_of(sname);
        if (lay) {
            ln("", cat("_Static_assert(sizeof(", ffi_c(sname), ") == ", std::to_string(lay->size),
                       ", \"extern struct ", sname,
                       ": size differs from the declared layout\");"));
            for (const auto &slot : lay->slots) {
                ln("", cat("_Static_assert(offsetof(", ffi_c(sname), ", ", ffi_c(slot.first),
                           ") == ", std::to_string(slot.second), ", \"extern struct ", sname,
                           ".", slot.first, ": offset differs from the declared layout\");"));
            }
        }
    }
    ln("", "");
    // C spelling of one declaration slot. A width that is not in the table is
    // refused here rather than spelled as a machine word: an extern the emitter
    // cannot spell is an ABI the program must not call.
    auto ffi_decl_type = [&](const std::string &ann) -> std::string {
        if (ann.empty() || ann == "none") return "void";
        if (structs.count(ann)) return ffi_c(ann);
        if (ann.rfind("ptr", 0) == 0 || ann.rfind("fn", 0) == 0) return "void *";
        auto it = ffi_scalar_c().find(ann);
        if (it == ffi_scalar_c().end())
            throw std::runtime_error("unsupported FFI type `" + ann + "` in an extern declaration");
        return it->second;
    };
    for (const auto &e : externs) {
        if (e.lib.empty()) continue;
        if (e.lib.find("libc.so") != std::string::npos || e.lib == "libc") continue;
        const std::string cret = ffi_decl_type(e.ret ? *e.ret : std::string());
        std::string cparams;
        for (const auto &p : *e.params) {
            const std::string t = ffi_decl_type(p.ann ? *p.ann : std::string());
            if (!cparams.empty()) cparams += ", ";
            cparams += t;
        }
        if (e.varargs) {
            if (!cparams.empty()) cparams += ", ";
            cparams += "...";
        }
        ln("", cat("extern ", cret, " ", e.name, "(", cparams.empty() ? "void" : cparams, ");"));
    }
    ln("", "");
    for (const auto &cb : cb_wrappers) {
        std::string joined;
        for (const auto &p : cb.params) {
            if (!joined.empty()) joined += ", ";
            joined += p;
        }
        ln("", cat(cb.ret, " brat_cb_", cb.name, "(", joined, ");"));
    }
    ln("", "");
    if (!global_lets.empty()) {
        for (const auto &gn : global_lets) ln("", cat("Val ", cname(gn), ";"));
        ln("", "");
    }
    run_pipeline();
    auto rr = reclaim_names(top_exe, {}, heap_returns());
    reclaim = rr.first;
    relet = rr.second;
    // C-callable wrappers for BRAT functions used as callbacks.
    for (const auto &cb : cb_wrappers) {
        const FnDecl &f = *fns[cb.name].decl;
        std::optional<std::string> ret;
        auto rit = fn_ret.find(cb.name);
        if (rit != fn_ret.end()) ret = rit->second;
        std::vector<std::string> pushes;
        for (size_t i = 0; i < f.params.size(); i++) {
            const auto &ann = f.params[i].ann;
            std::string arg = cat("a", i);
            if (ann && structs.count(*ann)) {
                pushes.push_back(cat("vec_push(_a, ", c_struct_to_val(*ann, arg), ");"));
            } else if (ann && *ann == "str") {
                pushes.push_back(cat("vec_push(_a, vstr(", arg, "));"));
            } else if (ann && (*ann == "f32" || *ann == "f64")) {
                // `f32` arrives as a C float: widen once, here, because BRAT has
                // a single float type and the callee expects one.
                pushes.push_back(cat("vec_push(_a, vfloat((double)", arg, "));"));
            } else if (ann && (ann->rfind("ptr", 0) == 0 || ann->rfind("fn", 0) == 0)) {
                pushes.push_back(cat("vec_push(_a, vint((int64_t)", arg, "));"));
            } else if (ann) {
                // Every integer width, sign-extended: a `short -1` is -1.
                pushes.push_back(cat("vec_push(_a, vint((int64_t)", arg, "));"));
            }
        }
        std::string joined;
        for (const auto &p : cb.params) {
            if (!joined.empty()) joined += ", ";
            joined += p;
        }
        ln("", cat(cb.ret, " brat_cb_", cb.name, "(", joined, ") {"));
        ln("  ", "Vec *_a = vvec();");
        for (const auto &p : pushes) ln("  ", p);
        ln("  ", cat("Val _r = ", cfn("fn:" + cb.name), "(NULL, _a);"));
        // The packed arguments belong to this call: C holds nothing from them,
        // so a wrapper that skips this leaks a Vec per callback invocation.
        ln("  ", "vrc_dec(vvecv(_a));");
        ln("  ", "vcb_check(_r);");
        if (!ret || *ret == "none") {
            ln("  ", "(void)_r;");
        } else if (*ret == "str") {
            ln("  ", "return _r.as.str;");
        } else if (*ret == "ptr") {
            ln("  ", "return (void *)_r.as.ival;");
        } else if (*ret == "f32" || *ret == "f64") {
            // BRAT has one float type; a `float` return read out of `ival` is
            // the float's bit pattern, which is how 3.5 became 4.6e18.
            ln("  ", "return _r.as.fval;");
        } else if (structs.count(*ret)) {
            ln("  ", cat("return ", val_to_c_struct(*ret, "_r"), ";"));
        } else {
            ln("  ", "return _r.as.ival;");
        }
        ln("", "}");
        ln("", "");
    }
    emit_main_open();
    for (const Stmt *s : top_exe) st(*s, "  ");
    emit_main_close();
    emit_closures_queue();
    emit_prelude_tables();
    for (const auto &exname : exported) {
        auto it = fns.find(exname);
        if (it == fns.end() || it->second.ctor) continue;
        const FnDecl &f = *it->second.decl;
        std::optional<std::string> ret;
        auto rit = fn_ret.find(exname);
        if (rit != fn_ret.end()) ret = rit->second;
        std::vector<std::string> cparams, pushes;
        bool ok = true;
        for (size_t i = 0; i < f.params.size(); i++) {
            const auto &ann = f.params[i].ann;
            std::string arg = cat("a", i);
            if (ann && (*ann == "i64" || *ann == "i32")) {
                cparams.push_back(cat(*ann == "i64" ? "int64_t" : "int", " ", arg));
                pushes.push_back(cat("vec_push(_a, vint(", arg, "));"));
            } else if (ann && *ann == "u8") {
                cparams.push_back(cat("unsigned char ", arg));
                pushes.push_back(cat("vec_push(_a, vint(", arg, "));"));
            } else if (ann && *ann == "u16") {
                cparams.push_back(cat("unsigned short ", arg));
                pushes.push_back(cat("vec_push(_a, vint(", arg, "));"));
            } else if (ann && (*ann == "u32" || *ann == "u64" || *ann == "usize")) {
                cparams.push_back(cat(*ann == "u32" ? "unsigned" : (*ann == "u64" ? "uint64_t"
                                                  : "size_t"), " ", arg));
                pushes.push_back(cat("vec_push(_a, vint(", arg, "));"));
            } else if (ann && *ann == "f64") {
                cparams.push_back(cat("double ", arg));
                pushes.push_back(cat("vec_push(_a, vfloat(", arg, "));"));
            } else if (ann && *ann == "f32") {
                cparams.push_back(cat("float ", arg));
                pushes.push_back(cat("vec_push(_a, vfloat((double)", arg, "));"));
            } else if (ann && *ann == "str") {
                cparams.push_back(cat("const char * ", arg));
                pushes.push_back(cat("vec_push(_a, vstr(", arg, "));"));
            } else if (ann && (*ann == "ptr" || ann->rfind("ptr[", 0) == 0)) {
                cparams.push_back(cat("void * ", arg));
                pushes.push_back(cat("vec_push(_a, vint((int64_t)", arg, "));"));
            } else if (ann && structs.count(*ann)) {
                cparams.push_back(cat(ffi_c(*ann), " ", arg));
                pushes.push_back(cat("vec_push(_a, ", c_struct_to_val(*ann, arg), ");"));
            } else {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        std::string cret;
        if (!ret || *ret == "none") {
            cret = "void";
        } else if (structs.count(*ret)) {
            cret = ffi_c(*ret);
        } else if (*ret == "i64" || *ret == "i32") {
            cret = *ret == "i64" ? "int64_t" : "int";
        } else if (*ret == "f64") {
            cret = "double";
        } else if (*ret == "f32") {
            cret = "float";
        } else if (*ret == "str") {
            cret = "const char *";
        } else if (*ret == "ptr") {
            cret = "void *";
        } else {
            continue;
        }
        std::string joined;
        for (const auto &p : cparams) {
            if (!joined.empty()) joined += ", ";
            joined += p;
        }
        ln("", "");
        ln("", cat(cret, " ", exname, "(", joined, ") {"));
        ln("  ", "Vec *_a = vvec();");
        for (const auto &p : pushes) ln("  ", p);
        ln("  ", cat("Val _r = ", cfn("fn:" + exname), "(NULL, _a);"));
        if (!ret || *ret == "none") {
            ln("  ", "(void)_r;");
        } else if (*ret == "str") {
            ln("  ", "return _r.as.str;");
        } else if (*ret == "ptr") {
            ln("  ", "return (void *)_r.as.ival;");
        } else if (*ret == "f64") {
            ln("  ", "return _r.as.fval;");
        } else if (structs.count(*ret)) {
            ln("  ", cat("return ", val_to_c_struct(*ret, "_r"), ";"));
        } else {
            ln("  ", "return _r.as.ival;");
        }
        ln("", "}");
    }
    std::string text;
    for (size_t i = 0; i < out.size(); i++) {
        if (i) text.push_back('\n');
        text += out[i];
    }
    text.push_back('\n');
    return text;
}


bool Emitter::booly(const Expr &e) {
    return e.k == EK::Bool || (e.k == EK::Un && e.a == "!") ||
           (e.k == EK::Bin && (e.a == "==" || e.a == "!=" || e.a == "<" || e.a == ">" || e.a == "<=" ||
                               e.a == ">=" || e.a == "&&" || e.a == "||"));
}

std::string Emitter::err_bail() {
    if (err_goto)
        return cat("{ ", err_base ? cat("vdfreset(", *err_base, "); ") : std::string(), "goto ",
                   *err_goto, "; }");
    if (fin == "__fin_top")
        return "{ fprintf(stderr, \"unhandled: \"); vprint_err(verr_take()); fprintf(stderr, "
               "\"\\n\"); return 1; }";
    return cat("{ __ret = verr_leave(); goto ", fin, "; }");
}

std::string Emitter::err_check_line(const std::vector<std::string> &drops) {
    // `drops` are temps this statement built and handed to nothing: on the way to
    // the catch their owner is gone, so they are released here.
    std::string rel;
    for (auto &d : drops) rel += cat("vdrop_temp(", d, "); ");
    if (err_goto) {
        if (err_base)
            return cat("if (verr_pending()) { ", rel, "vdfreset(", *err_base, "); goto ", *err_goto, "; }");
        return cat("if (verr_pending()) { ", rel, "goto ", *err_goto, "; }");
    }
    if (fin == "__fin_top")
        return cat("if (verr_pending()) { ", rel,
                   "fprintf(stderr, \"unhandled: \"); vprint_err(verr_take()); "
                   "fprintf(stderr, \"\\n\"); return 1; }");
    return cat("if (verr_pending()) { ", rel, "__ret = verr_leave(); goto ", fin, "; }");
}

void Emitter::emit_err_check(const std::string &ind, const std::vector<std::string> &drops) {
    ln(ind, err_check_line(drops));
}

// `&x` -- the address of a binding, as a C pointer the callee writes through.
//
// Only a name can be written back through, so `&` takes a name, and the width
// comes from the extern's `ptr[T]`: an `i32 *` out-parameter is four bytes. The
// binding's value goes into a scratch of that width; the callee writes the
// scratch; `out_flush` puts the value back the moment the call returns.
std::pair<std::vector<std::string>, std::string> Emitter::out_addr(const Expr &e,
                                                                  const std::string *ann) {
    if (e.k != EK::Var) throw EmitError("`&` takes a binding: `let mut p = &x`");
    if (!in_out_arg)
        throw EmitError("`&` is an out-parameter: write it where the extern takes a "
                        "`ptr[T]`, as in `fill(&n, 21)`");
    std::string el = (ann && ann->rfind("ptr[", 0) == 0) ? ann->substr(4, ann->size() - 5) : "";
    int width = el == "u8" || el == "i8" ? 1 : el == "u16" || el == "i16" ? 2
              : el == "i32" || el == "u32" || el == "f32" ? 4 : 8;
    std::string ctype = width == 1 ? "unsigned char" : width == 2 ? "short"
                      : width == 4 ? "int" : "int64_t";
    std::string cn = var_ref(e.a);
    std::string scratch = new_tmp("out");
    out_binds.push_back({e.a, cn, scratch, width});
    return {{cat(ctype, " ", scratch, " = (", ctype, ")(", cn, ").as.ival;")},
            cat("(void *)&", scratch)};
}

std::vector<std::string> Emitter::out_flush() {
    std::vector<std::string> out;
    for (const auto &b : out_binds) {
        out.push_back(cat("vrc_dec(", b.cn, ");"));
        std::string c = b.width == 1 ? cat("(int64_t)(unsigned char)", b.scratch)
                      : b.width == 2 ? cat("(int64_t)(short)", b.scratch)
                      : cat("(int64_t)", b.scratch);
        out.push_back(cat(b.cn, " = vint(", c, ");"));
    }
    return out;
}

V Emitter::ex_call(const Expr &e) {
    const std::string &name = e.a;
    ExprList args = ptrs(e.kids);
    if (name == "print") {
        if (args.empty())
            return {{}, "(BRAT_PRINT_LOCK(), vprint_nl(), BRAT_PRINT_UNLOCK(), vint(0))"};
        std::vector<std::string> pre_all, parts;
        for (const Expr *a : args) {
            V v = ex(*a);
            for (auto &p : v.pre) pre_all.push_back(p);
            std::string t = new_tmp("p");
            pre_all.push_back(cat("Val ", t, " = (", v.val, ");"));
            pre_all.push_back(err_check_line());
            parts.push_back(t);
        }
        pre_all.push_back("BRAT_PRINT_LOCK();");
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) pre_all.push_back("printf(\" \");");
            pre_all.push_back(cat("vprint(", parts[i], ");"));
        }
        pre_all.push_back("vprint_nl();");
        pre_all.push_back("BRAT_PRINT_UNLOCK();");
        // vprint reads its argument; a value this statement built is nobody's
        // afterwards, and leaving it was a leak of one container per print.
        for (size_t i = 0; i < parts.size(); i++)
            if (dec_able(*args[i], heap_returns())) pre_all.push_back(cat("vdrop_temp(", parts[i], ");"));
        return {pre_all, "vint(0)"};
    }
    if (name == "log.info" || name == "log.warn" || name == "log.error") {
        if (args.empty()) throw EmitError("log arity");
        V a = args_vec(args);
        std::string lvl;
        for (char c : name.substr(4))
            lvl.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c));
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("vlogv(\"", lvl, "\", ", a.val, ");"));
        pre.push_back(cat("vrc_dec(vvecv(", a.val, "));"));
        return {pre, "vint(0)"};
    }
    if (name == "trace.span") {
        if (args.empty()) throw EmitError("span arity");
        ExprList rest = args;
        const Expr *body = nullptr;
        if (rest.back()->k == EK::Closure) {
            body = rest.back();
            rest.pop_back();
        }
        V a = args_vec(rest);
        std::vector<std::string> out = a.pre;
        out.push_back(cat("vspan(vvecv(", a.val, "));"));
        out.push_back(cat("vrc_dec(vvecv(", a.val, "));"));
        if (body) {
            V b = ex(*body);
            for (auto &p : b.pre) out.push_back(p);
            std::string t = new_tmp("sp"), sv = new_tmp("sv");
            out.push_back(cat("Vec *", sv, " = vvec();"));
            out.push_back(cat("Val ", t, " = vcall_any(", b.val, ", ", sv, ");"));
            out.push_back(cat("vrc_dec(vvecv(", sv, "));"));
            return {out, t};
        }
        return {out, "vint(0)"};
    }
    if (name == "len") {
        need_arity(args, 1, name);
        return read_only(*args[0], "vlen");
    }
    if (name == "__at") {
        need_arity(args, 2, name);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        return {pre, cat("vat_owned(", a.val, ", ", b.val, ")")};
    }
    if (name == "Ok" || name == "Err" || name == "Some") {
        need_arity(args, 1, name);
        return ex_tag(lower(name), args);
    }
    if (name == "unwrap" || name == "is_err" || name == "is_none" || name == "is_vec" ||
        name == "is_str" || name == "is_map" || name == "tagof" || name == "str") {
        need_arity(args, 1, name);
        if (name == "str") return read_only(*args[0], "vstr_of");
        if (name == "tagof") return read_only(*args[0], "vtagof");
        if (name == "is_err")
            return read_only_call(*args[0], [](const std::string &v) { return cat("vbool(vis_err(", v, "))"); });
        if (name == "is_none")
            return read_only_call(*args[0], [](const std::string &v) { return cat("vbool(vis_none(", v, "))"); });
        if (name == "is_vec")
            return read_only_call(*args[0], [](const std::string &v) { return cat("vbool((", v, ").tag == V_VEC)"); });
        if (name == "is_str")
            return read_only_call(*args[0], [](const std::string &v) { return cat("vbool((", v, ").tag == V_STR)"); });
        if (name == "is_map")
            return read_only_call(*args[0], [](const std::string &v) { return cat("vbool((", v, ").tag == V_STRUCT)"); });
        V a = ex(*args[0]);
        if (name == "unwrap") return part_of_temp(a, *args[0], "vunwrap(", ")");
        return {a.pre, cat("vstr_of(", a.val, ")")};
    }
    if (name == "assert") {
        if (args.empty() || args.size() > 2) throw EmitError("assert arity");
        V a = ex(*args[0]);
        std::string bail = err_bail();
        if (args.size() == 2) {
            V b = ex(*args[1]);
            std::vector<std::string> pre = a.pre;
            for (auto &p : b.pre) pre.push_back(p);
            pre.push_back(cat("if (!vis_true(", a.val, ")) { Vec *_ae = vvec(); vec_push(_ae, ", b.val,
                              "); vunwind = vtagv(\"err\", _ae); vunwind_do(); ", bail, " }"));
            return {pre, "vbool(1)"};
        }
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("if (!vis_true(", a.val, ")) { vraise(\"assert failed\"); ", bail, " }"));
        return {pre, "vbool(1)"};
    }
    if (name == "read_file" || name == "write_file" || name == "tcp_listen" || name == "tcp_connect" ||
        name == "tcp_accept" || name == "tcp_close" || name == "sbuf" || name == "sbuf_add" ||
        name == "sbuf_len" || name == "sbuf_str" || name == "tcp_recv" || name == "tcp_send" ||
        name == "chan" || name == "join_all" || name == "args" || name == "exit" || name == "eprint" ||
        name == "raise" || name == "own" || name == "__arena_enter" || name == "__arena_leave" ||
        name == "time_ms" || name == "sleep_ms") {
        if (name == "read_file") {
            need_arity(args, 1, name);
            V a = ex(*args[0]);
            return {a.pre, cat("vreadfile(", a.val, ")")};
        }
        if (name == "write_file") {
            need_arity(args, 2, name);
            V a = ex(*args[0]);
            V b = ex(*args[1]);
            std::vector<std::string> pre = a.pre;
            for (auto &p : b.pre) pre.push_back(p);
            return {pre, cat("vwritefile(", a.val, ", ", b.val, ")")};
        }
        if (name == "tcp_listen" || name == "tcp_connect" || name == "tcp_accept" || name == "tcp_close" ||
            name == "sbuf_len" || name == "sbuf_str") {
            need_arity(args, 1, name);
            V a = ex(*args[0]);
            return {a.pre, cat("v", name, "(", a.val, ")")};
        }
        if (name == "sbuf") {
            need_arity(args, 0, name);
            return {{}, "vsbuf()"};
        }
        if (name == "sbuf_add" || name == "tcp_recv" || name == "tcp_send") {
            need_arity(args, 2, name);
            V a = ex(*args[0]);
            V b = ex(*args[1]);
            std::vector<std::string> pre = a.pre;
            for (auto &p : b.pre) pre.push_back(p);
            return {pre, cat("v", name, "(", a.val, ", ", b.val, ")")};
        }
        if (name == "chan") {
            need_arity(args, 1, name);
            V a = ex(*args[0]);
            return {a.pre, cat("vchan_new(", a.val, ")")};
        }
        if (name == "join_all") {
            need_arity(args, 0, name);
            return {{}, "(vjoin_all(), vint(0))"};
        }
        if (name == "args") {
            need_arity(args, 0, name);
            return {{}, "vargs()"};
        }
        if (name == "exit") {
            need_arity(args, 1, name);
            V a = ex(*args[0]);
            return {a.pre, cat("(vexit(", a.val, "), vint(0))")};
        }
        if (name == "eprint") {
            V a = args_vec(args);
            std::vector<std::string> pre = a.pre;
            pre.push_back(cat("veprint(", a.val, ");"));
            pre.push_back(cat("vrc_dec(vvecv(", a.val, "));"));
            return {pre, "vint(0)"};
        }
        if (name == "raise") {
            need_arity(args, 1, name);
            V a = ex(*args[0]);
            std::vector<std::string> pre = a.pre;
            pre.push_back(cat("vraisev(", keep(*args[0], a.val), ");"));
            pre.push_back(err_bail());
            return {pre, "vnil()"};
        }
        if (name == "own") {
            need_arity(args, 1, name);
            V a = ex(*args[0]);
            return {a.pre, cat("vown(", a.val, ")")};
        }
        if (name == "__arena_enter" || name == "__arena_leave") {
            need_arity(args, 0, name);
            std::string fn = name == "__arena_enter" ? "varena_enter" : "varena_leave";
            return {{cat(fn, "();")}, "vint(0)"};
        }
        if (name == "time_ms") {
            need_arity(args, 0, name);
            return {{}, "vtime_ms()"};
        }
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return {a.pre, cat("(vsleep_ms(", a.val, "), vint(0))")};
    }
    if (name == "slice_of" || name == "slice_of_ptr" || name == "slice_len" || name == "slice_get" ||
        name == "slice_set" || name == "str_from_ptr" || name == "slice_of_struct" ||
        name == "slice_struct_of")
        return ex_slice(name, args);
    if (name == "lib_open" || name == "lib_sym" || name == "lib_close" || name == "call")
        return ex_plugin(name, args);
    if (name == "ptr_null") {
        need_arity(args, 0, name);
        return {{}, "vint(0)"};
    }
    if (name == "ptr_is_null") {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return {a.pre, cat("vbool((", a.val, ").as.ival == 0)")};
    }
    if (name == "ptr_offset") {
        need_arity(args, 2, name);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        return {pre, cat("vint((int64_t)((char *)(", a.val, ").as.ival + (", b.val, ").as.ival))")};
    }
    if (name == "ptr_read_f32") {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return {a.pre, cat("((vptr_live(", a.val, ")) ? vfloat((double)*(float *)(",
                           a.val, ").as.ival) : vunwind)")};
    }
    if (name == "ptr_write_f32") {
        need_arity(args, 2, name);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        return {pre, cat("((vptr_live(", a.val, ")) ? ((*(float *)(", a.val,
                         ").as.ival = (float)(", b.val, ").as.fval), vint(0)) : vunwind)")};
    }
    if (name == "ptr_read_struct") {
        need_arity(args, 2, name);
        V a = ex(*args[0]);
        const Expr &tn = *args[1];
        if (tn.k != EK::Str) throw EmitError("ptr_read_struct wants a struct name as a string literal");
        if (!structs.count(tn.a)) throw EmitError("ptr_read_struct: " + tn.a + " is not an extern struct");
        std::string r = new_tmp("pr");
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("Val ", r, " = vunwind;"));
        pre.push_back(cat("if (vptr_live(", a.val, ")) { ", r, " = ",
                          c_struct_to_val(tn.a, cat("(*(bf_", tn.a, " *)(", a.val, ").as.ival)")), "; }"));
        return {pre, r};
    }
    if (name == "ptr_write_struct") {
        need_arity(args, 3, name);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        const Expr &tn = *args[2];
        if (tn.k != EK::Str) throw EmitError("ptr_write_struct wants a struct name as a string literal");
        if (!structs.count(tn.a)) throw EmitError("ptr_write_struct: " + tn.a + " is not an extern struct");
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        std::string stmt = cat("*(", "bf_", tn.a, " *)(", a.val, ").as.ival = ", val_to_c_struct(tn.a, b.val));
        return {pre, cat("((vptr_live(", a.val, ")) ? ((", stmt, "), vint(0)) : vunwind)")};
    }
    if (name == "ptr_read_i64" || name == "ptr_read_f64" || name == "ptr_read_ptr" ||
        name == "ptr_read_str") {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        std::string deref;
        if (name == "ptr_read_i64") deref = cat("vint(*(int64_t *)(", a.val, ").as.ival)");
        else if (name == "ptr_read_f64") deref = cat("vfloat(*(double *)(", a.val, ").as.ival)");
        else if (name == "ptr_read_ptr") deref = cat("vint((int64_t)*(void **)(", a.val, ").as.ival)");
        else deref = cat("vstr((const char *)(", a.val, ").as.ival)");
        return {a.pre, cat("((vptr_live(", a.val, ")) ? ", deref, " : vunwind)")};
    }
    if (name == "ptr_write_i64" || name == "ptr_write_f64" || name == "ptr_write_ptr" ||
        name == "ptr_write_str") {
        need_arity(args, 2, name);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        std::string store;
        if (name == "ptr_write_i64")
            store = cat("(*(int64_t *)(", a.val, ").as.ival = (", b.val, ").as.ival)");
        else if (name == "ptr_write_f64")
            store = cat("(*(double *)(", a.val, ").as.ival = (", b.val, ").as.fval)");
        else if (name == "ptr_write_ptr")
            store = cat("(*(void **)(", a.val, ").as.ival = (void *)(", b.val, ").as.ival)");
        else
            store = cat("(*(const char **)(", a.val, ").as.ival = (const char *)(", b.val, ").as.str)");
        return {pre, cat("((vptr_live(", a.val, ")) ? (", store, ", vint(0)) : vunwind)")};
    }
    if (name == "ptr_of_str") {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return {a.pre, cat("vint((int64_t)(", a.val, ").as.str)")};
    }
    if (name == "errno") {
        need_arity(args, 0, name);
        return {{}, "vint(errno)"};
    }
    if (name == "strerror") {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return {a.pre, cat("vstr(strerror((int)(", a.val, ").as.ival))")};
    }
    if (name == "env_get") {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return {a.pre, cat("venv_get(", a.val, ")")};
    }
    if (name == "re_match") {
        need_arity(args, 2, name);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        return {pre, cat("vbool(vre_match(", a.val, ", ", b.val, "))")};
    }
    if (fns.count(fn_key(name))) return ex_direct(fn_key(name), args);
    if (name.find('.') != std::string::npos) return ex_dotted(name, args);
    if (args.empty() && !name.empty() && name[0] >= 'A' && name[0] <= 'Z' &&
        (sym.count(name) || (cap && cap->second.count(name)))) {
        // A bare uppercase name parses as a zero-arg call; when it names a
        // local binding it reads the binding, as the interpreter does.
        ExprP var = mk_var(name);
        return ex(*var);
    }
    auto ext = ext_ix.find(name);
    if (ext != ext_ix.end()) {
        const Ext &x = externs[ext->second];
        std::vector<std::string> pa_all;
        std::string cargs;
        for (size_t i = 0; i < args.size(); i++) {
            std::optional<std::string> ann;
            if (i < x.params->size()) {
                ann = (*x.params)[i].ann;
            } else {
                ann = x.varargs;
            }
            const Expr &a = *args[i];
            if (ann && ann->rfind("fn", 0) == 0) {
                check_cb_arg(*ann, a, name, static_cast<int>(i) + 1);
                cargs += cat(cargs.empty() ? "" : ", ", "brat_cb_", a.a);
                continue;
            }
            if (ann && !FFI_TYPES.count(*ann) && !structs.count(*ann) && ann->rfind("fn", 0) != 0 &&
                ann->rfind("ptr[", 0) != 0)
                throw EmitError(cat("unsupported FFI argument type `", *ann, "` in extern fn ", name));
            if (ann && ann->rfind("ptr", 0) == 0 && a.k == EK::Un && a.a == "&") {
                // `&x` is the out-parameter: the callee gets the address of the
                // binding's storage, and the binding takes the value back as
                // soon as the call returns.
                in_out_arg = true;
                auto o = out_addr(*a.kids[0], ann ? &*ann : nullptr);
                in_out_arg = false;
                for (auto &p : o.first) pa_all.push_back(p);
                cargs += cat(cargs.empty() ? "" : ", ", o.second);
                continue;
            }
            V v = ex(a);
            for (auto &p : v.pre) pa_all.push_back(p);
            if (ann && structs.count(*ann)) {
                cargs += cat(cargs.empty() ? "" : ", ", val_to_c_struct(*ann, v.val));
                continue;
            }
            // FFI misuse is a BRAT error, never a segfault: check the tag once
            // into a temp, then read the payload.
            std::string t = new_tmp("fa");
            pa_all.push_back(cat("Val ", t, " = ", v.val, ";"));
            std::string ok;
            bool var_slot = i >= x.params->size() && ann && *ann == "ptr";
            if (ann && *ann == "str") {
                ok = cat(t, ".tag == V_STR");
            } else if (var_slot) {
                // A `str` fills a variadic `..ptr` slot as `const char *`,
                // which is what `%s` reads.
                ok = cat("((", t, ".tag == V_INT && ", t, ".as.ival >= 0) || ", t, ".tag == V_STR)");
            } else if (ann && (*ann == "f64" || *ann == "f32")) {
                ok = cat("(", t, ".tag == V_FLOAT || ", t, ".tag == V_INT)");
            } else {
                ok = cat(t, ".tag == V_INT");
                if (ann && FFI_RANGE.count(*ann)) {
                    const auto &r = FFI_RANGE.at(*ann);
                    ok += cat(" && ", t, ".as.ival >= ", r.first, " && ", t, ".as.ival <= ", r.second);
                } else if (ann && *ann == "ptr") {
                    ok += cat(" && ", t, ".as.ival >= 0");   // negative pointers never reach C
                }
            }
            pa_all.push_back(cat("if (!(", ok, ")) { vraise(\"ffi: arg ", i + 1, " of ", name, " wants ",
                                 ann.value_or("value"), "\"); ", err_bail(), " }"));
            if (ann && *ann == "str") {
                cargs += cat(cargs.empty() ? "" : ", ", t, ".as.str");
            } else if (var_slot) {
                cargs += cat(cargs.empty() ? "" : ", ", "(", t, ".tag == V_STR ? (void *)", t,
                             ".as.str : (void *)", t, ".as.ival)");
            } else if (ann && (*ann == "ptr" || ann->rfind("ptr[", 0) == 0)) {
                cargs += cat(cargs.empty() ? "" : ", ", "(void *)", t, ".as.ival");
            } else if (ann && *ann == "f64") {
                cargs += cat(cargs.empty() ? "" : ", ", "(", t, ".tag == V_INT ? (double)", t,
                             ".as.ival : ", t, ".as.fval)");
            } else if (ann && *ann == "f32") {
                // A CUDA/OpenGL kernel takes floats: the value narrows here, once.
                cargs += cat(cargs.empty() ? "" : ", ", "(float)(", t, ".tag == V_INT ? (double)", t,
                             ".as.ival : ", t, ".as.fval)");
            } else {
                cargs += cat(cargs.empty() ? "" : ", ", t, ".as.ival");
            }
        }
        std::string call = cat(name, "(", cargs, ")");
        std::string ret = x.ret.value_or("");
        // The values the callee wrote come back here: after the call, before
        // anything a raise could skip.
        std::vector<std::string> fl = out_flush();
        out_binds.clear();
        if (errno_externs.count(name)) {
            // Failure is NULL (ptr) or -1 (integers); errno is read right after.
            std::string r = new_tmp("er");
            std::string sentinel = ret == "ptr" ? "0" : "-1";
            pa_all.push_back("errno = 0;");
            pa_all.push_back(cat("int64_t ", r, " = (int64_t)", call, ";"));
            pa_all.push_back(cat("if (", r, " == ", sentinel, ") { vraise_errno(\"", name, "\"); ",
                                 err_bail(), " }"));
            return {pa_all, cat("vint(", r, ")")};
        }
        if (ffi_int_result(ret)) {
            // The cast widens the width the header declared, so a `short` that
            // holds -1 arrives as -1 and not as 65535.
            std::string r = new_tmp("ir");
            pa_all.push_back(cat("int64_t ", r, " = (int64_t)", call, ";"));
            for (auto &f : fl) pa_all.push_back(f);
            return {pa_all, cat("vint(", r, ")")};
        }
        if (ret == "u64" || ret == "usize") {
            std::string r = new_tmp("ur");
            pa_all.push_back(cat("uint64_t ", r, " = ", call, ";"));
            pa_all.push_back(cat("if (", r, " > 9223372036854775807ULL) { vraise(\"ffi: result of ", name,
                                 " does not fit i64\"); ", err_bail(), " }"));
            return {pa_all, cat("vint((int64_t)", r, ")")};
        }
        if (ret == "str") {
            std::string t = new_tmp("fr");
            pa_all.push_back(cat("Val ", t, " = vstr_ffi(", call, ");"));
            pa_all.push_back(cat("if (verr_pending()) ", err_bail()));
            return {pa_all, t};
        }
        if (ret == "ptr" || ret.rfind("ptr[", 0) == 0)
            return {pa_all, cat("vint((int64_t)(", call, "))")};
        if (ret == "f64") return {pa_all, cat("vfloat(", call, ")")};
        if (ret == "f32") return {pa_all, cat("vfloat((double)", call, ")")};
        if (structs.count(ret)) {
            // The record is read field by field, so the call goes into a temp
            // first: inlined into every field access it would be called once per
            // field -- three calls, three side effects, three different records.
            std::string rt = new_tmp("cs");
            pa_all.push_back(cat(ffi_c(ret), " ", rt, " = ", call, ";"));
            return {pa_all, c_struct_to_val(ret, rt)};
        }
        // `-> none` is a spelling of "no result", exactly like an omitted
        // arrow: the checker already reads them as one (`ffi.check`), and the
        // emitter must not be the place where the two spellings differ.
        if (!x.ret || *x.ret == "none") {
            pa_all.push_back(cat(call, ";"));
            for (auto &f : fl) pa_all.push_back(f);
            return {pa_all, "vnil()"};
        }
        throw EmitError(cat("unsupported FFI return type `", ret, "` in extern fn ", name));
    }
    if (sym.count(name)) return ex_vcall(name, args);
    if (cap && cap->second.count(name)) {
        V a = args_vec(args);
        return call_result(a.pre, a.val,
                           cat("vcall_any((*map_get(", cap->first, ", \"", name, "\")), ", a.val, ")"));
    }
    throw EmitError("unresolved call " + name);
}

}  // namespace

std::string emit_program(const std::vector<StmtP> &prog, const std::string &base_, int opt,
                         std::vector<ir::Proof> *proofs) {
    Emitter em;
    em.base = base_;
    em.opt = opt;
    em.proofs = proofs;
    em.collect_prog(prog);
    return em.emit_program();
}

std::vector<std::string> extern_libs(const std::vector<StmtP> &prog) {
    Emitter em;
    em.collect_prog(prog);
    std::set<std::string> libs;
    for (const auto &e : em.externs)
        if (!e.lib.empty()) libs.insert(e.lib);
    return std::vector<std::string>(libs.begin(), libs.end());
}

}  // namespace codegen
