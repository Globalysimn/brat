// The C emitter: BRAT's tree becomes C that links against crt.h.
//
// This is BRAT's only C emitter. Its output is a compatibility surface for
// generated programs and tests, so construction order is deliberately stable.

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


[[maybe_unused]] inline void app(std::string &o, const std::string &s) { o += s; }
[[maybe_unused]] inline void app(std::string &o, const char *s) { o += s; }
[[maybe_unused]] inline void app(std::string &o, int v) { o += std::to_string(v); }
[[maybe_unused]] inline void app(std::string &o, unsigned int v) { o += std::to_string(v); }
[[maybe_unused]] inline void app(std::string &o, long v) { o += std::to_string(v); }
[[maybe_unused]] inline void app(std::string &o, unsigned long v) { o += std::to_string(v); }
[[maybe_unused]] inline void app(std::string &o, long long v) { o += std::to_string(v); }
[[maybe_unused]] inline void app(std::string &o, unsigned long long v) { o += std::to_string(v); }

// Variadic string concatenation helper for C emission.
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

// Normalize empty `else {}` blocks.
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
    c->from_match = s.from_match;
    c->pat_uncond = s.pat_uncond;
    c->pat_arity = s.pat_arity;
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
    c->ext.effects = s.ext.effects;
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

// Single mapping from BRAT FFI scalar names to C types; unknown widths are rejected.
static const std::map<std::string, std::string> &ffi_scalar_c() {
    static const std::map<std::string, std::string> t = {
        {"bool", "_Bool"},
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
            // the pair from forming.
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
    if (e.k == EK::Call || e.k == EK::MCall || e.k == EK::Closure ||
        e.k == EK::Idx || e.k == EK::Field || e.k == EK::Quest)
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
        if (s.opt1) acc.insert("@catch_" + *s.opt1);
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
    const std::vector<std::string> *effects = nullptr;
    std::optional<std::string> ret, varargs;
    bool checked = false;
};

struct Closure {
    const std::vector<Param> *params = nullptr;
    StmtList body;
    std::set<std::string> caps;
    bool ret_result = false;
    std::string src_file;
};

static bool is_result_ret_ann(const std::optional<std::string> &ret) {
    if (!ret) return false;
    size_t i = 0;
    while (i < ret->size() && (*ret)[i] == ' ') i++;
    return ret->compare(i, 6, "Result") == 0 &&
           (i + 6 == ret->size() || (*ret)[i + 6] == '[' || (*ret)[i + 6] == ' ');
}

struct Late {
    std::string name;
    const FnDecl *decl = nullptr;
    std::string src_file;
};

struct Cb {
    std::string name, ret, ret_ann;
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
    "items", "entries", "values", "recv", "send", "parse_int", "parse_float",
    "rev", "pop", "del", "zip", "chunks", "enumerate",
    "min", "max", "first", "last", "get", "find", "set", "sort_by",
};

// Builtins that hand back a fresh value the caller owns: a new vector, a new
// string, or an Ok/Err tag built on the spot. Missing one of these is not a
// type error anywhere -- it just means the binding is classified as an alias,
// never released, and LeakSanitizer reports it once per call. `args()` did.
const std::string LOCAL_BINDING_PREFIX = "\x01local:";

const std::set<std::string> HEAP_CALLS = {
    "str", "own", "str_from_ptr", "args", "args_bytes", "read_file", "write_file",
    "append_file", "read_bytes", "write_bytes", "proc_run", "env_get", "env_get_bytes",
    "utf8_bytes", "utf8_str", "json_parse", "json_stringify",
    "fs_file_size", "fs_mkdir_all", "fs_list_dir", "fs_remove_dir",
    "fs_open", "fs_read_line", "fs_close", "stdin_read_line",
    "fs_rename", "fs_remove",
    "sha256_bytes", "random_bytes", "https_request_raw",
    "sql_open", "sql_close", "sql_exec", "sql_query",
    "re_find", "re_captures", "re_replace",
    "strerror", "ptr_read_str", "ptr_read_struct",
    "tcp_listen", "tcp_connect", "tcp_accept", "tcp_close",
    "tcp_recv", "tcp_send", "sbuf", "sbuf_str", "chan", "tagof",
    "slice_of", "slice_of_ptr", "slice_of_struct", "slice_struct_of", "slice_get", "call"};

bool must_return(const StmtList &body) {
    if (body.empty() || body.back() == nullptr) return false;
    const Stmt &last = *body.back();
    if (last.k == SK::Return) return true;
    if (last.k == SK::If) {
        const Stmt *els = real_else(last);
        if (!els) return false;
        StmtList eb = els->k == SK::Block ? view(els->body) : StmtList{els};
        return must_return(view(last.body)) && must_return(eb);
    }
    return false;
}

struct Emitter {
    int tmp = 0;
    std::map<std::string, FnEntry> fns;
    std::vector<OrderItem> order;
    std::map<std::pair<std::string, std::string>, int> mtab;
    std::map<std::string, std::string> vtab;
    std::map<std::string, std::string> vdisp;
    std::map<std::string, std::vector<std::string>> traits;
    std::vector<Ext> externs;
    std::map<std::string, size_t> ext_ix;
    std::set<std::string> errno_externs;
    std::map<std::string, const std::vector<StructField> *> structs;
    std::map<std::string, const std::vector<StructField> *> native_structs;
    std::set<std::string> packed;
    std::set<std::string> unions;
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
    std::map<std::string, int64_t> cap_slots;
    std::vector<std::string> dfb;
    std::vector<std::vector<std::string>> dfb_vars;
    std::set<std::string> dfb_locals;
    int try_n = 0;
    std::set<std::string> alias_ret;
    std::set<std::string> cur_params;  // parameters of the function being emitted
    bool cur_ret_result = false;       // true when current fn declares `-> Result[...]`
    std::vector<std::string> scopes;
    std::vector<std::string> iter_cleanups;
    // Lexical boundaries for each loop body: early exits must drain every
    // defer frame and task scope entered since the loop began.
    std::vector<std::pair<size_t, size_t>> loop_exit_depths;
    std::string fin = "__fin_top";
    std::optional<std::string> err_goto;
    size_t err_scope_depth = 0;
    size_t err_iter_depth = 0;
    std::map<std::string, FnEntry> fast;
    std::map<std::string, std::string> fastc, fastkinds;
    std::set<std::string> rec_fast;
    std::set<std::string> fails;
    bool frec = false;
    size_t frame_arr_n = 0;
    std::map<std::string, int> frame_idx;
    // -O1: the nodes ir::analyze proved need no check, and the proofs to print.
    int opt = 0;
    std::string target_triple;
    bool trace_lines = false;
    bool debug_lines = false;
    std::string root_src_file;
    std::string cur_src_file;
    const std::map<const Stmt *, std::string> *stmt_files = nullptr;
    const std::map<std::string, std::string> *fn_files = nullptr;
    std::string last_dbg_file;
    int last_dbg_line = -1;
    size_t last_dbg_next_idx = 0;
    bool opt_done = false;
    std::set<const Expr *> unchecked;
    std::set<const Expr *> nonzero_div;
    std::set<const Expr *> unchecked_idx;
    std::set<const Stmt *> unchecked_setidx;
    std::vector<ir::Proof> *proofs = nullptr;
    std::map<std::string, std::set<std::string>> fastvec;
    std::map<std::string, std::set<std::string>> fastbool;
    std::map<std::string, std::set<std::string>> fastint;
    std::string fkind = "int64_t";
    std::map<std::string, std::string> fsym;
    bool need_idiv = false, need_imod = false, need_idiv_nz = false, need_imod_nz = false;
    bool need_fdiv = false, need_fmod = false;
    bool need_viat = false, need_viat_nb = false, need_vset = false;
    bool need_vfat = false, need_vset_f = false;
    std::set<std::string> fvecvars;
    std::set<std::string> fboolvars;
    std::set<std::string> fintvars;
    std::string fself_rec;
    int ic_n = 0, fic_n = 0, mic_slot = -1;
    std::map<std::pair<std::string, std::vector<std::string>>, int> shapes;
    std::vector<std::pair<std::string, std::vector<std::string>>> shape_defs;
    std::set<std::string> ub_ints;
    std::set<std::string> ub_floats;
    int nesting = 0;
    int nested_seq = 0;
    std::map<std::string, std::string> fn_alias;
    std::set<std::string> assigned;
    std::set<std::string> reclaim, relet;
    // BRAT_MOVE_RT=0 turns the drops off. It is a bisection knob: a program
    // emitted that way leaks on purpose so drop changes can be isolated.
    bool move_rt = std::getenv("BRAT_MOVE_RT") == nullptr ||
                   std::strcmp(std::getenv("BRAT_MOVE_RT"), "0") != 0;
    std::set<std::string> moved, captured, shared, dropped, alias_temps, catch_cns;
    std::vector<std::string> call_cleanup;
    std::set<std::string> scalar_vars;
    std::map<std::string, std::string> var_structs;
    std::deque<FnDecl> sra_fns;
    std::deque<StmtP> shadow_top_stmts;
    std::map<const Stmt *, std::string> local_stmt_files;
    bool shadows_done = false;
    bool sra_done = false;
    int sra_seq = 0;
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
    std::string lookup_stmt_file(const Stmt *s) const {
        if (s) {
            auto lit = local_stmt_files.find(s);
            if (lit != local_stmt_files.end() && !lit->second.empty()) return lit->second;
            if (stmt_files) {
                auto it = stmt_files->find(s);
                if (it != stmt_files->end() && !it->second.empty()) return it->second;
            }
        }
        return !cur_src_file.empty() ? cur_src_file : root_src_file;
    }
    std::string lookup_fn_file(const std::string &name, const FnDecl *f = nullptr) const {
        if (fn_files) {
            auto it = fn_files->find(name);
            if (it != fn_files->end() && !it->second.empty()) return it->second;
        }
        if (f && !f->body.empty() && f->body[0]) {
            std::string sf = lookup_stmt_file(f->body[0].get());
            if (!sf.empty()) return sf;
        }
        return !cur_src_file.empty() ? cur_src_file : root_src_file;
    }
    void emit_dbg_loc(int line, const std::string &file) {
        if (!debug_lines || line <= 0 || file.empty()) return;
        if (last_dbg_file == file && last_dbg_line >= 0 && out.size() >= last_dbg_next_idx) {
            int expected = last_dbg_line + static_cast<int>(out.size() - last_dbg_next_idx);
            if (expected == line) return;
            out.push_back(cat("#line ", line));
            last_dbg_line = line;
            last_dbg_next_idx = out.size();
            return;
        }
        out.push_back(cat("#line ", line, " \"", clit(file), "\""));
        last_dbg_file = file;
        last_dbg_line = line;
        last_dbg_next_idx = out.size();
    }
    void emit_dbg_stmt(const Stmt &s) {
        if (!debug_lines || s.line <= 0) return;
        emit_dbg_loc(s.line, lookup_stmt_file(&s));
    }
    int fn_code(const std::string &key) {
        auto it = codes.find(key);
        if (it != codes.end()) return it->second;
        int c = static_cast<int>(codes.size());
        codes[key] = c;
        return c;
    }
    std::string cfn(const std::string &key) { return cat("bf_c", fn_code(key)); }
    std::string direct_cfn(const std::string &key) { return cat(cfn(key), "_p"); }
    std::string fn_key(const std::string &name) const {
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
    std::string err_scope_unwind();
    std::string err_bail();
    std::string err_check_line(const std::vector<std::string> &drops = {});
    void emit_err_check(const std::string &ind, const std::vector<std::string> &drops = {});

    bool int_expr(const Expr &e, const std::map<std::string, FnEntry> &cand, const std::string &kind,
                  const std::set<std::string> &intvars, const std::set<std::string> &vecvars);
    bool intish(const Expr &e, const std::string &kind, const std::set<std::string> &intvars);
    bool fint(const Expr &e);
    bool booly(const Expr &e);
    bool all_bool(const Expr &e);
    bool print_ok(const Expr &e, const std::map<std::string, FnEntry> &cand, const std::string &kind,
                  const std::set<std::string> &intvars, const std::set<std::string> &vecvars = {});
    // `returned`, when non-null, records whether each `return` expression is a
    // float so `f64` fast paths do not alter integer return tags when unannotated.
    bool int_body(const StmtList &body, const std::map<std::string, FnEntry> &cand, const std::string &kind,
                  std::set<std::string> intvars, const std::set<std::string> &vecvars,
                  std::vector<bool> *returned = nullptr);
    bool pure_int_returns(const StmtList &body, const std::map<std::string, FnEntry> &cand,
                          const std::set<std::string> &param_names,
                          const std::set<std::string> &vecvars = {});
    bool pure_float_returns(const StmtList &body, const std::map<std::string, FnEntry> &cand,
                            const std::set<std::string> &param_names);
    bool has_float_lit(const StmtList &body) const;
    std::set<std::string> vec_params_in(const StmtList &body, const std::set<std::string> &pnames) const;
    bool ub_expr_ok(const Expr &e, const std::set<std::string> &cand) const;
    bool ub_expr_ok(const Expr &e) const { return ub_expr_ok(e, ub_ints); }
    bool ub_may_raise(const Expr &e) const;
    std::string ub_ex(const Expr &e) const;
    void ub_assign(const std::string &name, const Expr &rhs, const std::string &ind);
    bool ub_int_sub(const Expr &e) const;
    bool ubf_expr_ok(const Expr &e, const std::set<std::string> &cand) const;
    bool ubf_expr_ok(const Expr &e) const { return ubf_expr_ok(e, ub_floats); }
    bool ubf_may_raise(const Expr &e) const;
    std::string ubf_ex(const Expr &e) const;
    std::string ubf_cmp_c(const std::string &op, const std::string &a, const std::string &b) const;
    void ubf_assign(const std::string &name, const Expr &rhs, const std::string &ind);
    std::set<std::string> closure_free_vars(const FnDecl &decl) const;
    void closure_walk_expr(const Expr *e, const std::set<std::string> &bound,
                           std::set<std::string> &free) const;
    void closure_walk_stmts(const StmtList &stmts, const std::set<std::string> &bound,
                            std::set<std::string> &free) const;
    std::set<std::string> compute_ub_vars(const StmtList &body, const std::set<std::string> &vars_,
                                          const std::set<std::string> &param_names, bool is_float);
    std::set<std::string> compute_ub_ints(const StmtList &body, const std::set<std::string> &vars_,
                                          const std::set<std::string> &param_names);
    std::set<std::string> compute_ub_floats(const StmtList &body, const std::set<std::string> &vars_,
                                            const std::set<std::string> &param_names);
    std::string next_ic() { return cat("&_mic[", ic_n++, "]"); }
    std::string next_fic() { return cat("&_fic[", fic_n++, "]"); }
    int shape_id(const std::string &tname, const std::vector<std::string> &fnames) {
        auto key = std::make_pair(tname, fnames);
        auto it = shapes.find(key);
        if (it != shapes.end()) return it->second;
        int sid = static_cast<int>(shape_defs.size());
        shapes[key] = sid;
        shape_defs.push_back(std::move(key));
        return sid;
    }
    void compute_fast();
    void run_opt();
    bool has_setidx(const StmtList &body);
    std::set<std::string> ops_in(const StmtList &body);
    std::string fast_un_c(const Expr &e, const std::string &v);
    std::string fast_idx_c(const Expr &e, const std::string &idx);
    std::string fast_call_c(const Expr &e, std::string args);
    std::string fast_bin_c(const Expr &e, const std::string &a, const std::string &b,
                           bool *div_can_fail = nullptr);
    std::string fx(const Expr &e);
    // A fast-path expression that can panic must not feed a value on: fchk
    // evaluates it into a temp and bails while the error is still pending.
    bool fraise(const Expr &e);
    // Names whose body can leave an error pending (raise, ?, div, index, unwrap).
    // A fast-path call to one of them needs a verr_pending() check; a call to a
    // plain arithmetic helper does not.
    std::set<std::string> may_fail();
    bool has_call(const Expr &e) const;
    std::string fret_bail() const;
    std::string fchk(const Expr &e, const std::string &ind);
    std::string slot_cn(const std::string &name) const {
        auto it = sym.find(name);
        return (it != sym.end() && !it->second.cn.empty()) ? it->second.cn : cname(name);
    }
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
    V direct_call_result(const ExprList &args,
                         const std::function<std::string(const std::vector<std::string> &)> &call);
    bool raw_i64_arg(const Expr &e, std::string &out) const;
    bool direct_fast_call(const std::string &name, const ExprList &args, V &out);
    V read_only(const Expr &arg, const std::string &fn);
    V read_only_call(const Expr &arg, const std::function<std::string(const std::string &)> &mk);
    V borrow_builtin(const std::string &c_name, const ExprList &args);
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
    V ex_hof(const Expr &obj, const std::string &meth, const Expr *arg, const Expr *first,
             const V *recv = nullptr);
    V ex_mapfilter(const Expr &obj, const std::string &meth, const Expr *arg,
                   const V *recv = nullptr);
    bool fusible_expr(const Expr &e, const std::set<std::string> &pset) const;
    bool fusible_fnlit(const Expr *fn_node, size_t arity) const;
    V ex_fused_stream(const Expr &obj, const std::string &meth, const ExprList &args);
    V ex_closure(const Expr &e);
    V ex_parfor(const Expr &e);

    void st(const Stmt &s, const std::string &ind);
    std::string var_def(const std::string &name, Own own, const std::string &ptr = "",
                        const std::string &elem = "");
    std::string slice_elem(const Expr &e);
    // `&x` inside the call being built: (name, c name, scratch, width)
    struct OutBind { std::string name, cn, scratch, elem; };
    std::vector<OutBind> out_binds;
    bool in_out_arg = false;
    std::pair<std::vector<std::string>, std::string> out_addr(const Expr &e, const std::string *ann);
    std::vector<std::string> out_flush();
    std::string ptr_elem(const Expr &base);
    V ex_ptr_idx(const std::string &elem, const Expr &base, const Expr &idx);
    void st_ptr_setidx(const std::string &elem, const Stmt &s, const std::string &ind);
    void emit_block(const StmtList &body, const std::string &ind);
    void drop_alias_temps(const StmtList &body, const std::string &ind);
    void emit_defer(const Expr &e, const std::string &ind, int line = 0);
    void emit_loop_exit(const std::string &k, const std::string &ind);
    std::string var_ref(const std::string &name);
    std::optional<std::string> place_root(const Expr &e);
    bool moves_owned_local(const Expr &e) const;
    std::string keep(const Expr &e, const std::string &val);
    std::string own_operand(const Expr &e, const std::string &val);
    std::string truth(const Expr &e, const std::string &val, std::vector<std::string> &lines);
    std::vector<std::string> capture(const std::string &cm, const std::string &var, int64_t slot);
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
                   const std::string *fastkind, const std::string *fastname,
                   bool positional = false, bool ret_result = false,
                   const std::string *self_struct = nullptr);
    int struct_field_slot(const std::string &sname, const std::string &fname) const;
    std::optional<std::string> struct_field_ann(const std::string &sname, const std::string &fname) const;
    int canonical_struct_shape(const std::string &sname);
    std::string infer_expr_struct(const Expr *e, const std::map<std::string, std::string> &known) const;
    std::map<std::string, std::string> compute_var_structs(const std::vector<Param> &params,
                                                           const StmtList &body,
                                                           const std::string *self_struct) const;
    bool sra_round(const std::string &fn_name, FnDecl &decl);
    bool sra_rewrite_decl(const std::string &fn_name, FnDecl &decl);
    void run_shadow_disambig();
    void run_sra();
    void emit_fn_by_key(const std::string &key);
    void emit_prelude_tables();
    void run_pipeline();
    void emit_late();
    void emit_main_open();
    void emit_main_close();
    std::string emit_program();
    std::string val_to_c_struct(const std::string &sname, const std::string &valexpr, bool pin_ret = false);
    std::vector<std::string> emit_struct_conv(const std::string &sname);
    std::string c_struct_to_val(const std::string &sname, const std::string &cexpr);
    void emit_forward_decls();
    void emit_closure_protos();
    void emit_closures_queue();
    void emit_prelude_headers();
    unsigned crt_mask = 0xFFFFu;
};


static bool may_raise(const Expr &e, bool typed = false);

bool Emitter::int_expr(const Expr &e, const std::map<std::string, FnEntry> &cand,
                       const std::string &kind, const std::set<std::string> &intvars,
                       const std::set<std::string> &vecvars) {
    const bool fk_float = (kind == "f64" || kind == "f64_dyn" || kind == "f64v");
    const bool fk_vec = (kind == "i64v" || kind == "i64v_dyn" || kind == "f64v");
    switch (e.k) {
    case EK::Num:
        if (!e.is_float) return true;
        return fk_float;
    case EK::Bool:
        return true;
    case EK::Var:
        return !vecvars.count(e.a);
    case EK::Un:
        if (e.a == "~" && fk_float) return false;
        return (e.a == "!" || e.a == "-" || e.a == "~") &&
               int_expr(*e.kids[0], cand, kind, intvars, vecvars);
    case EK::Move:
        return int_expr(*e.kids[0], cand, kind, intvars, vecvars);
    case EK::Idx:
        return fk_vec && e.kids[0]->k == EK::Var &&
               vecvars.count(e.kids[0]->a) && intish(*e.kids[1], kind, intvars);
    case EK::Bin: {
        static const std::set<std::string> ops = {"+", "-", "*", "/", "%", "&", "|", "^",
                                                  "<<", ">>", "==", "!=", "<", ">", "<=",
                                                  ">=", "&&", "||"};
        if (!ops.count(e.a)) return false;
        if ((e.a == "&" || e.a == "|" || e.a == "^" || e.a == "<<" || e.a == ">>") && fk_float)
            return false;
        if (!int_expr(*e.kids[0], cand, kind, intvars, vecvars)) return false;
        if (!int_expr(*e.kids[1], cand, kind, intvars, vecvars)) return false;
        if ((e.a == "/" || e.a == "%") && fk_float &&
            intish(*e.kids[0], kind, intvars) && intish(*e.kids[1], kind, intvars))
            return false;
        if (e.a == "&&" || e.a == "||") {
            if (may_raise(*e.kids[0], true) || may_raise(*e.kids[1], true)) return false;
            if (!all_bool(*e.kids[0]) &&
                e.kids[0]->k != EK::Var && e.kids[0]->k != EK::Num && e.kids[0]->k != EK::Bool)
                return false;
        }
        return true;
    }
    case EK::Call:
        if (e.a == "len" && fk_vec && e.kids.size() == 1) {
            const Expr &a = *e.kids[0];
            if (a.k == EK::Var && vecvars.count(a.a)) return true;
        }
        {
            auto it = cand.find(e.a);
            if (it == cand.end()) return false;
            if (fastvec.count(e.a) && !fastvec[e.a].empty()) return false;
            const std::string &ck = fastkinds[e.a];
            auto ri = fn_ret.find(e.a);
            bool not_bool = (ri == fn_ret.end() || !ri->second || *ri->second != "bool");
            bool same_k = ((ck == kind) && not_bool) ||
                          ((kind == "i64" || kind == "i64_dyn" || kind == "i64v" || kind == "i64v_dyn") &&
                           (ck == "i64" || ck == "i64_dyn") && not_bool) ||
                          (fk_float && (ck == "f64" || ck == "f64_dyn") && not_bool);
            if (!same_k) return false;
            if (fastbool.count(e.a)) {
                const auto &callee_params = it->second.decl->params;
                if (e.kids.size() != callee_params.size()) return false;
                for (size_t i = 0; i < e.kids.size(); i++) {
                    if (fastbool[e.a].count(callee_params[i].name) && !all_bool(*e.kids[i]))
                        return false;
                }
            }
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
        if (e.a == "!") return true;
        return intish(*e.kids[0], kind, intvars);
    case EK::Move:
        return intish(*e.kids[0], kind, intvars);
    case EK::Idx:
        return kind == "i64v" || kind == "i64v_dyn";
    case EK::Bin:
        if (e.a == "==" || e.a == "!=" || e.a == "<" || e.a == ">" || e.a == "<=" ||
            e.a == ">=")
            return true;
        if (e.a == "&&" || e.a == "||")
            return all_bool(e) ||
                   (intish(*e.kids[0], kind, intvars) && intish(*e.kids[1], kind, intvars));
        return intish(*e.kids[0], kind, intvars) && intish(*e.kids[1], kind, intvars);
    case EK::Call: {
        if (e.a == "len" && (kind == "i64v" || kind == "i64v_dyn" || kind == "f64v") && e.kids.size() == 1)
            return true;
        auto it = fastkinds.find(e.a);
        return (kind == "i64" || kind == "i64_dyn" || kind == "i64v" || kind == "i64v_dyn") &&
               it != fastkinds.end() && (it->second == "i64" || it->second == "i64_dyn");
    }
    default:
        return false;
    }
}

bool Emitter::fint(const Expr &e) {
    return fkind != "f64" || intish(e, "f64v", fintvars);
}

bool Emitter::all_bool(const Expr &e) {
    if (e.k == EK::Bool || (e.k == EK::Un && e.a == "!")) return true;
    if (e.k == EK::Var && fboolvars.count(e.a)) return true;
    if (e.k == EK::Call) {
        auto ri = fn_ret.find(e.a);
        if (ri != fn_ret.end() && ri->second && *ri->second == "bool") return true;
    }
    if (e.k == EK::Bin) {
        if (e.a == "==" || e.a == "!=" || e.a == "<" || e.a == ">" || e.a == "<=" || e.a == ">=")
            return true;
        if ((e.a == "&&" || e.a == "||") && e.kids.size() == 2)
            return all_bool(*e.kids[0]) && all_bool(*e.kids[1]);
    }
    return false;
}

bool Emitter::print_ok(const Expr &e, const std::map<std::string, FnEntry> &cand,
                       const std::string &kind, const std::set<std::string> &intvars,
                       const std::set<std::string> &vecvars) {
    if (e.k != EK::Call || e.a != "print") return false;
    bool dbl = (kind == "f64" || kind == "f64_dyn" || kind == "f64v");
    int fslots = 0;
    for (const auto &ap : e.kids) {
        const Expr &a = *ap;
        if (a.k == EK::Str) {
            for (const auto &p : split_interp(a.a)) {
                if (p.first == "var") {
                    if (p.second.find('.') != std::string::npos || vecvars.count(p.second) ||
                        fboolvars.count(p.second) || global_lets.count(p.second))
                        return false;
                    if (dbl && !intvars.count(p.second)) fslots++;
                }
            }
            continue;
        }
        if (booly(a) || (a.k == EK::Var && fboolvars.count(a.a))) return false;
        if (!int_expr(a, cand, kind, intvars, vecvars)) return false;
        if (a.k == EK::Num && a.is_float) fslots++;
        else if (dbl && !intish(a, kind, intvars)) fslots++;
    }
    return fslots <= 8;
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
            if (kind != "i64v" && kind != "i64v_dyn" && kind != "f64v") return false;
            if (!(s.e1->k == EK::Var && vecvars.count(s.e1->a))) return false;
            if (!intish(*s.e2, kind, intvars)) return false;
            if (kind == "f64v") {
                if (!int_expr(*s.e3, cand, kind, intvars, vecvars) || intish(*s.e3, kind, intvars))
                    return false;
            } else {
                if (!intish(*s.e3, kind, intvars)) return false;
            }
            break;
        case SK::Return:
            if (!int_expr(*s.e1, cand, kind, intvars, vecvars)) return false;
            if (returned) returned->push_back(intish(*s.e1, kind, intvars));
            break;
        case SK::Expr:
            if (print_ok(*s.e1, cand, kind, intvars, vecvars)) break;
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
            if (kind != "i64v" && kind != "i64v_dyn" && kind != "f64v") return false;
            if (!(s.e1->k == EK::Var && vecvars.count(s.e1->a))) return false;
            std::set<std::string> sub = intvars;
            if (kind != "f64v") sub.insert(s.a);
            else sub.erase(s.a);
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

static bool pure_int_arith_expr(const Expr &e, const std::set<std::string> &vars) {
    switch (e.k) {
    case EK::Num: return !e.is_float;
    case EK::Var: return vars.count(e.a) > 0;
    case EK::Un: return (e.a == "-" || e.a == "~") && pure_int_arith_expr(*e.kids[0], vars);
    case EK::Bin:
        return (e.a == "+" || e.a == "-" || e.a == "*" || e.a == "/" || e.a == "%" ||
                e.a == "&" || e.a == "|" || e.a == "^" || e.a == "<<" || e.a == ">>") &&
               pure_int_arith_expr(*e.kids[0], vars) && pure_int_arith_expr(*e.kids[1], vars);
    default: return false;
    }
}

static bool all_returns_match(const StmtList &stmts,
                              const std::function<bool(const Expr &)> &pred,
                              bool allow_for_iter) {
    for (const Stmt *sp : stmts) {
        if (!sp) continue;
        const Stmt &s = *sp;
        if (s.k == SK::Return) {
            if (!pred(*s.e1)) return false;
        } else if (s.k == SK::If) {
            if (!all_returns_match(view(s.body), pred, allow_for_iter)) return false;
            if (const Stmt *els = real_else(s)) {
                StmtList eb = els->k == SK::Block ? view(els->body) : StmtList{els};
                if (!all_returns_match(eb, pred, allow_for_iter)) return false;
            }
        } else if (s.k == SK::While || s.k == SK::ForRange ||
                   (allow_for_iter && s.k == SK::ForIter)) {
            if (!all_returns_match(view(s.body), pred, allow_for_iter)) return false;
        }
    }
    return true;
}

bool Emitter::pure_int_returns(const StmtList &body, const std::map<std::string, FnEntry> &cand,
                               const std::set<std::string> &param_names,
                               const std::set<std::string> &vecvars) {
    std::set<std::string> pure, vars_all;
    for (const auto &p : param_names)
        if (!vecvars.count(p)) pure.insert(p);
    collect_vars(body, vars_all);
    for (const auto &v : vars_all)
        if (!vecvars.count(v)) pure.insert(v);
    std::function<bool(const Expr &)> is_pi = [&](const Expr &e) -> bool {
        switch (e.k) {
        case EK::Num: return !e.is_float;
        case EK::Var: return pure.count(e.a) > 0;
        case EK::Idx:
            return e.kids[0]->k == EK::Var && vecvars.count(e.kids[0]->a) && is_pi(*e.kids[1]);
        case EK::Un: return (e.a == "-" || e.a == "~") && is_pi(*e.kids[0]);
        case EK::Move: return is_pi(*e.kids[0]);
        case EK::Bin:
            return (e.a == "+" || e.a == "-" || e.a == "*" || e.a == "/" || e.a == "%" ||
                    e.a == "&" || e.a == "|" || e.a == "^" || e.a == "<<" || e.a == ">>") &&
                   is_pi(*e.kids[0]) && is_pi(*e.kids[1]);
        case EK::Call: {
            if (e.a == "len" && e.kids.size() == 1 && e.kids[0]->k == EK::Var &&
                vecvars.count(e.kids[0]->a))
                return true;
            if (!cand.count(e.a)) return false;
            auto fk = fastkinds.find(e.a);
            if (fk == fastkinds.end() || (fk->second != "i64" && fk->second != "i64_dyn")) return false;
            auto ri = fn_ret.find(e.a);
            return ri == fn_ret.end() || !ri->second || *ri->second != "bool";
        }
        default: return false;
        }
    };
    std::function<bool(const StmtList &)> scan_binds = [&](const StmtList &stmts) -> bool {
        bool dropped = false;
        for (const Stmt *sp : stmts) {
            if (!sp) continue;
            const Stmt &s = *sp;
            if (s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::Set) {
                std::string nm = s.a.substr(0, s.a.find('.'));
                if (pure.count(nm) && !is_pi(*s.e1)) {
                    pure.erase(nm);
                    dropped = true;
                }
            } else if (s.k == SK::If) {
                if (scan_binds(view(s.body))) dropped = true;
                if (const Stmt *els = real_else(s)) {
                    StmtList eb = els->k == SK::Block ? view(els->body) : StmtList{els};
                    if (scan_binds(eb)) dropped = true;
                }
            } else if (s.k == SK::While || s.k == SK::ForRange) {
                if (scan_binds(view(s.body))) dropped = true;
            } else if (s.k == SK::ForIter) {
                if (!(s.e1->k == EK::Var && vecvars.count(s.e1->a)) && pure.erase(s.a) > 0)
                    dropped = true;
                if (scan_binds(view(s.body))) dropped = true;
            }
        }
        return dropped;
    };
    while (scan_binds(body)) {}
    return all_returns_match(body, is_pi, true);
}

bool Emitter::pure_float_returns(const StmtList &body, const std::map<std::string, FnEntry> &cand,
                                 const std::set<std::string> &param_names) {
    std::set<std::string> fpure = param_names, ipure, vars_all;
    collect_vars(body, vars_all);
    for (const auto &v : vars_all)
        if (!param_names.count(v)) fpure.insert(v);
    std::function<bool(const Expr &)> is_pf = [&](const Expr &e) -> bool {
        switch (e.k) {
        case EK::Num: return e.is_float;
        case EK::Var: return fpure.count(e.a) > 0;
        case EK::Un: return e.a == "-" && is_pf(*e.kids[0]);
        case EK::Move: return is_pf(*e.kids[0]);
        case EK::Bin: {
            if (e.a != "+" && e.a != "-" && e.a != "*" && e.a != "/" && e.a != "%") return false;
            bool lf = is_pf(*e.kids[0]), rf = is_pf(*e.kids[1]);
            bool li = pure_int_arith_expr(*e.kids[0], ipure), ri = pure_int_arith_expr(*e.kids[1], ipure);
            return (lf && (rf || ri)) || (rf && li);
        }
        case EK::Call: {
            if (!cand.count(e.a)) return false;
            auto fk = fastkinds.find(e.a);
            if (fk == fastkinds.end() || (fk->second != "f64" && fk->second != "f64_dyn")) return false;
            auto ri = fn_ret.find(e.a);
            return ri == fn_ret.end() || !ri->second || *ri->second != "bool";
        }
        default: return false;
        }
    };
    std::function<bool(const StmtList &)> scan_binds = [&](const StmtList &stmts) -> bool {
        bool dropped = false;
        for (const Stmt *sp : stmts) {
            if (!sp) continue;
            const Stmt &s = *sp;
            if (s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::Set) {
                std::string nm = s.a.substr(0, s.a.find('.'));
                if (fpure.count(nm) && !is_pf(*s.e1)) {
                    fpure.erase(nm);
                    if (pure_int_arith_expr(*s.e1, ipure)) ipure.insert(nm);
                    dropped = true;
                } else if (ipure.count(nm) && !pure_int_arith_expr(*s.e1, ipure)) {
                    ipure.erase(nm);
                    dropped = true;
                }
            } else if (s.k == SK::If) {
                if (scan_binds(view(s.body))) dropped = true;
                if (const Stmt *els = real_else(s)) {
                    StmtList eb = els->k == SK::Block ? view(els->body) : StmtList{els};
                    if (scan_binds(eb)) dropped = true;
                }
            } else if (s.k == SK::While) {
                if (scan_binds(view(s.body))) dropped = true;
            } else if (s.k == SK::ForRange) {
                if (fpure.erase(s.a) > 0) {
                    ipure.insert(s.a);
                    dropped = true;
                }
                if (scan_binds(view(s.body))) dropped = true;
            }
        }
        return dropped;
    };
    while (scan_binds(body)) {}
    return all_returns_match(body, is_pf, false);
}

bool Emitter::has_float_lit(const StmtList &body) const {
    std::function<bool(const Expr &)> we = [&](const Expr &e) -> bool {
        if (e.k == EK::Num && e.is_float) return true;
        for (const auto &k : e.kids)
            if (k && we(*k)) return true;
        return false;
    };
    for (const Stmt *sp : body) {
        if (!sp) continue;
        const Stmt &s = *sp;
        if (s.e1 && we(*s.e1)) return true;
        if (s.e2 && we(*s.e2)) return true;
        if (s.e3 && we(*s.e3)) return true;
        if (has_float_lit(view(s.body)) || has_float_lit(view(s.body2))) return true;
        if (const Stmt *els = real_else(s)) {
            StmtList eb = els->k == SK::Block ? view(els->body) : StmtList{els};
            if (has_float_lit(eb)) return true;
        }
    }
    return false;
}

std::set<std::string> Emitter::vec_params_in(const StmtList &body,
                                             const std::set<std::string> &pnames) const {
    std::set<std::string> out;
    std::function<void(const Expr &)> we = [&](const Expr &e) {
        if (e.k == EK::Idx && !e.kids.empty() && e.kids[0]->k == EK::Var &&
            pnames.count(e.kids[0]->a))
            out.insert(e.kids[0]->a);
        else if (e.k == EK::Call && e.a == "len" && e.kids.size() == 1 &&
                 e.kids[0]->k == EK::Var && pnames.count(e.kids[0]->a))
            out.insert(e.kids[0]->a);
        for (const auto &k : e.kids)
            if (k) we(*k);
    };
    std::function<void(const StmtList &)> ws = [&](const StmtList &stmts) {
        for (const Stmt *sp : stmts) {
            if (!sp) continue;
            const Stmt &s = *sp;
            if (s.k == SK::ForIter && s.e1 && s.e1->k == EK::Var && pnames.count(s.e1->a))
                out.insert(s.e1->a);
            else if (s.k == SK::SetIdx && s.e1 && s.e1->k == EK::Var && pnames.count(s.e1->a))
                out.insert(s.e1->a);
            if (s.e1) we(*s.e1);
            if (s.e2) we(*s.e2);
            if (s.e3) we(*s.e3);
            ws(view(s.body));
            ws(view(s.body2));
            if (const Stmt *els = real_else(s)) {
                StmtList eb = els->k == SK::Block ? view(els->body) : StmtList{els};
                ws(eb);
            }
        }
    };
    ws(body);
    return out;
}

void Emitter::compute_fast() {
    std::map<std::string, std::string> kinds;
    fastvec.clear();
    fastbool.clear();
    fastint.clear();
    for (const auto &[name, entry] : fns) {
        if (entry.ctor || entry.decl == nullptr) continue;
        const std::vector<Param> &params = entry.decl->params;
        std::vector<std::string> anns;
        bool ok = true;
        auto ri = fn_ret.find(name);
        std::string ret_ann = (ri != fn_ret.end() && ri->second) ? *ri->second : "";
        bool ret_dyn_ok = (ret_ann.empty() || ret_ann == "i64" || ret_ann == "bool");
        bool ret_fdyn_ok = (ret_ann.empty() || ret_ann == "f64" || ret_ann == "bool");
        bool dyn_ok = (name.find('.') == std::string::npos) && ret_dyn_ok;
        bool fdyn_ok = (name.find('.') == std::string::npos) && ret_fdyn_ok;
        std::set<std::string> unann_pnames, dfl_pnames;
        for (const auto &p : params) {
            if (!p.ann) unann_pnames.insert(p.name);
            if (p.def) dfl_pnames.insert(p.name);
            if (p.ann && (*p.ann == "i64" || *p.ann == "bool" || *p.ann == "f64" ||
                          *p.ann == "vec[i64]" || *p.ann == "vec[f64]")) {
                anns.push_back(*p.ann);
                if (*p.ann != "i64") dyn_ok = false;
                if (*p.ann != "f64") fdyn_ok = false;
            } else {
                ok = false;
                if (p.ann) {
                    dyn_ok = false;
                    fdyn_ok = false;
                }
            }
            if (p.def && !(p.def->k == EK::Num && !p.def->is_float)) dyn_ok = false;
            if (p.def && p.def->k != EK::Num) fdyn_ok = false;
        }
        std::set<std::string> body_names, own_names;
        refs_in(view(entry.decl->body), body_names);
        bound_in(view(entry.decl->body), own_names);
        bool names_global = false;
        for (const auto &n : body_names)
            if (global_lets.count(n) && !own_names.count(n)) names_global = true;
        if (names_global) continue;
        bool all_i64_family = true;
        bool all_f64_family = true;
        for (const auto &a : anns) {
            if (a != "i64" && a != "bool" && a != "vec[i64]") all_i64_family = false;
            if (a != "f64" && a != "vec[f64]") all_f64_family = false;
        }
        bool has_ivec = std::find(anns.begin(), anns.end(), "vec[i64]") != anns.end();
        bool has_fvec = std::find(anns.begin(), anns.end(), "vec[f64]") != anns.end();
        bool has_bool = std::find(anns.begin(), anns.end(), "bool") != anns.end();
        if (ok && has_bool) {
            std::set<std::string> bs;
            for (const auto &p : params)
                if (p.ann && *p.ann == "bool") bs.insert(p.name);
            fastbool[name] = std::move(bs);
        }
        if (ok && !anns.empty() && has_ivec && all_i64_family) {
            kinds[name] = "i64v";
            std::set<std::string> vs;
            for (const auto &p : params)
                if (p.ann && *p.ann == "vec[i64]") vs.insert(p.name);
            fastvec[name] = vs;
        } else if (ok && !anns.empty() && has_fvec && all_f64_family) {
            kinds[name] = "f64v";
            std::set<std::string> vs;
            for (const auto &p : params)
                if (p.ann && *p.ann == "vec[f64]") vs.insert(p.name);
            fastvec[name] = vs;
        } else if (ok && all_i64_family) {
            kinds[name] = "i64";
        } else if (ok && all_f64_family) {
            kinds[name] = "f64";
        } else if (!ok && !params.empty() && must_return(view(entry.decl->body))) {
            std::set<std::string> vparams =
                dyn_ok ? vec_params_in(view(entry.decl->body), unann_pnames) : std::set<std::string>();
            bool vp_has_dfl = false;
            for (const auto &vp : vparams)
                if (dfl_pnames.count(vp)) vp_has_dfl = true;
            if (dyn_ok && !vparams.empty() && !vp_has_dfl) {
                kinds[name] = "i64v_dyn";
                fastvec[name] = vparams;
            } else if (fdyn_ok && (ret_ann == "f64" || has_float_lit(view(entry.decl->body)))) {
                kinds[name] = "f64_dyn";
            } else if (dyn_ok) {
                kinds[name] = "i64_dyn";
            } else {
                continue;
            }
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
            std::set<std::string> pnames;
            for (const auto &p : params) pnames.insert(p.name);
            const std::set<std::string> &vvars = fastvec[name];
            fboolvars = fastbool.count(name) ? fastbool[name] : std::set<std::string>();
            std::set<std::string> intvars;
            bool mixed_fvar = false;
            if (kind == "i64" || kind == "i64_dyn" || kind == "i64v" || kind == "i64v_dyn") {
                for (const auto &pn : pnames)
                    if (!vvars.count(pn)) intvars.insert(pn);
            } else {
                std::set<std::string> lvars;
                collect_vars(view(fns[name].decl->body), lvars);
                for (const auto &pn : pnames) lvars.erase(pn);
                for (const auto &vv : vvars) lvars.erase(vv);
                intvars = lvars;
                std::function<void(const StmtList &, bool)> walk_fvars = [&](const StmtList & sl,
                                                                             bool verify) {
                    for (const Stmt *sp : sl) {
                        const Stmt &s = *sp;
                        if (s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::Set) {
                            if (s.e1 && s.a.find('.') == std::string::npos) {
                                bool rhs_i = intish(*s.e1, kind, intvars);
                                if (!verify) {
                                    if (!rhs_i && intvars.erase(s.a)) mixed_fvar = true;
                                } else if (rhs_i != (intvars.count(s.a) > 0)) {
                                    mixed_fvar = true;
                                }
                            }
                        } else if (s.k == SK::ForRange) {
                            if (verify && !intvars.count(s.a)) mixed_fvar = true;
                        } else if (s.k == SK::ForIter) {
                            if (!verify) {
                                if (intvars.erase(s.a)) mixed_fvar = true;
                            } else if (intvars.count(s.a)) {
                                mixed_fvar = true;
                            }
                        }
                        walk_fvars(view(s.body), verify);
                        walk_fvars(view(s.body2), verify);
                        if (const Stmt *els = real_else(s)) {
                            StmtList one = els->k == SK::Block ? view(els->body) : StmtList{els};
                            walk_fvars(one, verify);
                        }
                    }
                };
                do {
                    mixed_fvar = false;
                    walk_fvars(view(fns[name].decl->body), false);
                } while (mixed_fvar);
                mixed_fvar = false;
                walk_fvars(view(fns[name].decl->body), true);
            }
            auto snapshot = cand;   // int_body mutates a copy of candidate vars
            std::vector<bool> returned;
            const bool ok = !mixed_fvar &&
                            int_body(view(fns[name].decl->body), snapshot, kind, intvars,
                                     vvars, &returned);
            bool any_int = false;
            for (bool r : returned) any_int = any_int || r;
            auto ri = fn_ret.find(name);
            std::string ret_ann = (ri != fn_ret.end() && ri->second) ? *ri->second : "";
            bool has_ret_ann = (ret_ann == "i64" || ret_ann == "bool");
            bool bad_bool_ret = !fboolvars.empty() && !has_ret_ann;
            if (bad_bool_ret) {
                std::set<std::string> non_bool_pnames;
                for (const auto &pn : pnames)
                    if (!fboolvars.count(pn)) non_bool_pnames.insert(pn);
                if (pure_int_returns(view(fns[name].decl->body), snapshot, non_bool_pnames, vvars))
                    bad_bool_ret = false;
            }
            bool bad_fdyn = (kind == "f64_dyn") &&
                            ((ret_ann != "bool" && any_int) ||
                             (ret_ann != "f64" && ret_ann != "bool" &&
                              !pure_float_returns(view(fns[name].decl->body), snapshot, pnames)));
            if (!ok || ((kind == "f64" || kind == "f64v") && ret_ann != "bool" && any_int) ||
                bad_bool_ret || bad_fdyn ||
                ((kind == "i64_dyn" || kind == "i64v_dyn") && !has_ret_ann &&
                 !pure_int_returns(view(fns[name].decl->body), snapshot, pnames, vvars))) {
                fastint.erase(name);
                it = cand.erase(it);
                changed = true;
            } else {
                if (kind == "f64" || kind == "f64_dyn" || kind == "f64v")
                    fastint[name] = std::move(intvars);
                ++it;
            }
        }
    }
    fboolvars.clear();
    rec_fast = drop_recursive_fast(cand);
    for (const auto &n : rec_fast) fails.insert(n);
    fast = cand;
    fastkinds.clear();
    for (const auto &[n, e] : cand) fastkinds[n] = kinds[n];
    int i = 0;
    for (const auto &[n, e] : fast) fastc[n] = cat("bf_f", i++);
    for (const auto &[name, entry] : fast) {
        std::string kind = fastkinds[name];
        for (const auto &op : ops_in(view(entry.decl->body))) {
            if (op == "/") {
                if (kind == "i64" || kind == "i64_dyn" || kind == "i64v" || kind == "i64v_dyn") {
                    need_idiv = true;
                } else {
                    need_fdiv = true;
                }
            }
            if (op == "%") {
                if (kind == "i64" || kind == "i64_dyn" || kind == "i64v" || kind == "i64v_dyn") {
                    need_imod = true;
                } else {
                    need_fmod = true;
                }
            }
        }
        if (kind == "i64v" || kind == "i64v_dyn") {
            need_viat = true;
            if (has_setidx(view(entry.decl->body))) need_vset = true;
        } else if (kind == "f64v") {
            need_vfat = true;
            if (has_setidx(view(entry.decl->body))) need_vset_f = true;
        }
    }
}

void Emitter::run_opt() {
    if (opt_done) return;   // run_pipeline runs twice; the pass reads the same tree
    opt_done = true;
    if (opt == 1) {
        // `-O1`: exact original contract on `"i64"` fast functions.
        for (const auto &[name, entry] : fast) {
            if (fastkinds[name] != "i64") continue;
            ir::Result r;
            ir::analyze(name, entry.decl->params, entry.decl->body, r);
            unchecked.insert(r.unchecked.begin(), r.unchecked.end());
            if (proofs) proofs->insert(proofs->end(), r.proofs.begin(), r.proofs.end());
        }
        return;
    }
    // `-O2`: bottom-up interprocedural return summaries + relational `IV × Cong × DBM`
    // across `"i64"`, `"i64_dyn"`, `"i64v"` and `"i64v_dyn"` fast functions.
    std::map<std::string, ir::FnSummary> summaries;
    auto is_int_fast = [&](const std::string &rk) {
        return rk == "i64" || rk == "i64_dyn" || rk == "i64v" || rk == "i64v_dyn";
    };
    const size_t max_rounds = std::min<size_t>(fast.size() + 1, 16);
    for (size_t round = 0; round < max_rounds; ++round) {
        bool changed = false;
        for (const auto &[name, entry] : fast) {
            if (!is_int_fast(fastkinds[name])) continue;
            const std::set<std::string> empty_vs;
            const auto &vs = fastvec.count(name) ? fastvec[name] : empty_vs;
            ir::Result dummy;
            ir::FnSummary next_s;
            if (!ir::analyze_o2(name, entry.decl->params, vs, entry.decl->body,
                                summaries, dummy, &next_s))
                continue;
            const ir::FnSummary &prev_s = summaries[name];
            if (prev_s.valid != next_s.valid || prev_s.top != next_s.top ||
                prev_s.lo != next_s.lo || prev_s.hi != next_s.hi ||
                prev_s.mod != next_s.mod || prev_s.rem != next_s.rem) {
                summaries[name] = next_s;
                changed = true;
            }
        }
        if (!changed) break;
    }
    for (const auto &[name, entry] : fast) {
        if (!is_int_fast(fastkinds[name])) continue;
        const std::set<std::string> empty_vs;
        const auto &vs = fastvec.count(name) ? fastvec[name] : empty_vs;
        ir::Result r;
        ir::analyze_o2(name, entry.decl->params, vs, entry.decl->body, summaries, r, nullptr);
        unchecked.insert(r.unchecked.begin(), r.unchecked.end());
        nonzero_div.insert(r.nonzero_div.begin(), r.nonzero_div.end());
        unchecked_idx.insert(r.unchecked_idx.begin(), r.unchecked_idx.end());
        unchecked_setidx.insert(r.unchecked_setidx.begin(), r.unchecked_setidx.end());
        for (const Expr *nd : r.nonzero_div) {
            if (nd->a == "/") need_idiv_nz = true;
            else need_imod_nz = true;
        }
        if (!r.unchecked_idx.empty()) need_viat_nb = true;
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

static std::string fcmp_di_expr(const std::string &op, bool left_int,
                                const std::string &a, const std::string &b) {
    const std::string &d = left_int ? b : a;
    const std::string &i = left_int ? a : b;
    if (op == "==") return cat("vcmp_di_eq(", d, ", ", i, ")");
    if (op == "!=") return cat("(!vcmp_di_eq(", d, ", ", i, "))");
    if (op == "<") return cat(left_int ? "vcmp_di_gt(" : "vcmp_di_lt(", d, ", ", i, ")");
    if (op == "<=") return cat(left_int ? "vcmp_di_ge(" : "vcmp_di_le(", d, ", ", i, ")");
    if (op == ">") return cat(left_int ? "vcmp_di_lt(" : "vcmp_di_gt(", d, ", ", i, ")");
    return cat(left_int ? "vcmp_di_le(" : "vcmp_di_ge(", d, ", ", i, ")");
}

static std::string strip_outer_parens(const std::string &s) {
    if (s.size() < 2 || s.front() != '(' || s.back() != ')') return s;
    int depth = 0;
    for (size_t i = 0; i + 1 < s.size(); i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')' && --depth == 0) return s;
    }
    return depth == 1 ? s.substr(1, s.size() - 2) : s;
}

std::string Emitter::fast_un_c(const Expr &e, const std::string &v) {
    if (e.a == "!") return cat("(!(", v, "))");
    if (e.a == "~") return cat("((int64_t)(~(uint64_t)(", v, ")))");
    return (fkind == "f64" && !fint(*e.kids[0]))
               ? cat("(-(", v, "))")
               : cat("((int64_t)(0ULL - (uint64_t)(", v, ")))");
}

std::string Emitter::fast_idx_c(const Expr &e, const std::string &idx) {
    const char *fn = (fkind == "f64")
                         ? "bf_vfat"
                         : ((opt >= 2 && unchecked_idx.count(&e)) ? "bf_viat_nb" : "bf_viat");
    return cat(fn, "(", fsym[e.kids[0]->a], ", ", idx, ")");
}

std::string Emitter::fast_call_c(const Expr &e, std::string args) {
    if (!fself_rec.empty() && e.a == fself_rec) {
        if (!args.empty()) args += ", ";
        args += "_rem - 1";
        return cat(fastc[e.a], "_r(", args, ")");
    }
    return cat(fastc[e.a], "(", args, ")");
}

std::string Emitter::fast_bin_c(const Expr &e, const std::string &a, const std::string &b,
                                bool *div_can_fail) {
    const bool dbl = fkind == "f64";
    const std::string &op = e.a;
    const bool ia = fint(*e.kids[0]), ib = fint(*e.kids[1]);
    if (div_can_fail) *div_can_fail = false;
    if (op == "/" || op == "%") {
        if (!dbl && opt >= 1 && unchecked.count(&e)) {
            if (div_can_fail) *div_can_fail = (opt < 2);
            return cat("(", a, " ", op, " ", b, ")");
        }
        if (!dbl && opt >= 2 && nonzero_div.count(&e))
            return cat(op == "/" ? "bf_idiv_nz" : "bf_imod_nz", "(", a, ", ", b, ")");
        if (div_can_fail) *div_can_fail = true;
        if (dbl)
            return cat(op == "/" ? "bf_fdiv" : "bf_fmod", "((double)(", a, "), (double)(", b, "))");
        return cat(op == "/" ? "bf_idiv" : "bf_imod", "(", a, ", ", b, ")");
    }
    if (ia && ib && (op == "+" || op == "-" || op == "*" || op == "&" || op == "|" || op == "^"))
        return cat("((int64_t)((uint64_t)(", a, ") ", op, " (uint64_t)(", b, ")))");
    if (ia && ib && op == "<<") return cat("bshl64(", a, ", ", b, ")");
    if (ia && ib && op == ">>") return cat("bshr64(", a, ", ", b, ")");
    if (dbl && (op == "+" || op == "-" || op == "*"))
        return cat("((double)(", a, ") ", op, " (double)(", b, "))");
    if (dbl && ia != ib &&
        (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">="))
        return fcmp_di_expr(op, ia, a, b);
    return cat("(", a, " ", op, " ", b, ")");
}

std::string Emitter::fx(const Expr &e) {
    const bool dbl = fkind == "f64";
    switch (e.k) {
    case EK::Num:
        return (dbl && e.is_float) ? cat("(", repr_f64(e.f), ")") : clit64(e.i);
    case EK::Bool:
        return cat("(", e.flag ? 1 : 0, "LL)");
    case EK::Var:
        return fsym[e.a];
    case EK::Un:
        return fast_un_c(e, fx(*e.kids[0]));
    case EK::Move:
        return fx(*e.kids[0]);
    case EK::Idx:
        return fast_idx_c(e, fx(*e.kids[1]));
    case EK::Bin: {
        std::string a = fx(*e.kids[0]), b = fx(*e.kids[1]);
        if (e.a == "&&" || e.a == "||") {
            const bool is_and = (e.a == "&&");
            if (all_bool(e)) return cat("(", a, is_and ? " && " : " || ", b, ")");
            const bool ia = fint(*e.kids[0]), ib = fint(*e.kids[1]);
            const std::string &tbr = is_and ? b : a, &fbr = is_and ? a : b;
            if (ia == ib) return cat("(", a, " ? ", tbr, " : ", fbr, ")");
            return cat("((", a, ") ? (double)(", tbr, ") : (double)(", fbr, "))");
        }
        return fast_bin_c(e, a, b);
    }
    case EK::Call: {
        if (e.a == "len") return cat("((", fsym[e.kids[0]->a], ")->len)");
        std::string args;
        for (size_t i = 0; i < e.kids.size(); i++) {
            if (i) args += ", ";
            std::string ai = fx(*e.kids[i]);
            args += (dbl && fint(*e.kids[i])) ? cat("(double)(", ai, ")") : ai;
        }
        return fast_call_c(e, std::move(args));
    }
    default:
        throw EmitError("fast node");
    }
}

std::set<std::string> Emitter::may_fail() {
    std::function<bool(const Expr &)> touches = [&](const Expr &e) {
        if (e.k == EK::Quest || e.k == EK::Idx || e.k == EK::Field) return true;
        if (e.k == EK::Bin && (e.a == "/" || e.a == "%")) return true;
        if (e.k == EK::Call && (e.a == "raise" || e.a == "unwrap" || e.a == "unwrap_or" ||
                                e.a == "assert" || e.a == "exit"))
            return true;
        if (e.k == EK::MCall && (e.b == "unwrap" || e.b == "unwrap_or")) return true;
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
            if (s.k == SK::SetIdx) fail.insert(name);
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
    std::function<bool(const std::string &, const std::string &, std::set<std::string> &)> reaches =
        [&](const std::string &src, const std::string &dst, std::set<std::string> &seen) {
            if (src == dst) return true;
            if (seen.count(src)) return false;
            seen.insert(src);
            auto eit = edges.find(src);
            if (eit == edges.end()) return false;
            for (const auto &c : eit->second)
                if (reaches(c, dst, seen)) return true;
            return false;
        };
    for (const auto &[name, callees] : edges) {
        for (const auto &c : callees) {
            std::set<std::string> seen;
            if (reaches(c, name, seen)) {
                fail.insert(name);
                break;
            }
        }
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

std::string Emitter::fret_bail() const {
    const char *z = (fkind == "f64") ? "0.0" : "0";
    return (frec && fself_rec.empty()) ? cat("{ bdepth_pop(); return ", z, "; }")
                                       : cat("return ", z, ";");
}

bool Emitter::fraise(const Expr &e) {
    if (e.k == EK::Bin && (e.a == "/" || e.a == "%")) {
        if (opt >= 2 && fkind != "f64" && (unchecked.count(&e) || nonzero_div.count(&e))) {
            return fraise(*e.kids[0]) || fraise(*e.kids[1]);
        }
        return true;
    }
    if (e.k == EK::Idx) return true;
    if (e.k == EK::Call) {
        if (e.a == "len") return false;   // a length is a field read, it cannot fail
        if (!fastc.count(e.a) || fails.count(e.a) > 0) return true;
    }
    for (const auto &k : e.kids)
        if (fraise(*k)) return true;
    return false;
}

// A call must not run once an operand before it has failed, and a failed call
// returns 0 whatever range its summary promised, so an expression holding one
// is checked operand by operand instead of once at the end.
bool Emitter::has_call(const Expr &e) const {
    if (e.k == EK::Call && e.a != "len") return true;
    for (const auto &k : e.kids)
        if (has_call(*k)) return true;
    return false;
}

std::string Emitter::fchk(const Expr &e, const std::string &ind) {
    if (!fraise(e)) return fx(e);
    const bool dbl = fkind == "f64";
    const char *ect = (dbl && !fint(e)) ? "double" : "int64_t";
    if (!has_call(e)) {
        std::string t = new_tmp("t");
        ln(ind, cat(ect, " ", t, " = ", fx(e), ";"));
        ln(ind, cat("if (bverr_unlikely()) ", fret_bail()));
        return t;
    }
    switch (e.k) {
    case EK::Un:
        return fast_un_c(e, fchk(*e.kids[0], ind));
    case EK::Move:
        return fchk(*e.kids[0], ind);
    case EK::Idx: {
        std::string idx = fchk(*e.kids[1], ind), t = new_tmp("t");
        ln(ind, cat(ect, " ", t, " = ", fast_idx_c(e, idx), ";"));
        ln(ind, cat("if (bverr_unlikely()) ", fret_bail()));
        return t;
    }
    case EK::Call: {
        if (e.a == "len") return fx(e);
        std::string args;
        for (size_t i = 0; i < e.kids.size(); i++) {
            std::string ai = fchk(*e.kids[i], ind);
            if (i) args += ", ";
            args += (dbl && fint(*e.kids[i])) ? cat("(double)(", ai, ")") : ai;
        }
        std::string t = new_tmp("t");
        ln(ind, cat(ect, " ", t, " = ", fast_call_c(e, std::move(args)), ";"));
        if (!fastc.count(e.a) || fails.count(e.a))
            ln(ind, cat("if (bverr_unlikely()) ", fret_bail()));
        return t;
    }
    case EK::Bin: {
        const std::string &op = e.a;
        const bool ia = fint(*e.kids[0]), ib = fint(*e.kids[1]);
        if (op == "&&" || op == "||") {
            std::string a = fchk(*e.kids[0], ind), res = new_tmp("t");
            const bool ab = all_bool(e);
            auto cast_op = [&](const std::string &v, bool is_i) {
                return ab ? cat("((", v, ") ? 1LL : 0LL)")
                          : ((dbl && !fint(e) && is_i) ? cat("(double)(", v, ")") : v);
            };
            ln(ind, cat(ect, " ", res, " = ", cast_op(a, ia), ";"));
            ln(ind, cat("if (", op == "&&" ? res : cat("!(", res, ")"), ") {"));
            std::string b = fchk(*e.kids[1], ind + "  ");
            ln(ind + "  ", cat(res, " = ", cast_op(b, ib), ";"));
            ln(ind, "}");
            return res;
        }
        std::string a = fchk(*e.kids[0], ind), b = fchk(*e.kids[1], ind);
        bool div_can_fail = false;
        std::string comb = fast_bin_c(e, a, b, &div_can_fail);
        if (div_can_fail) {
            std::string t = new_tmp("t");
            ln(ind, cat(ect, " ", t, " = ", comb, ";"));
            ln(ind, cat("if (verr_pending()) ", fret_bail()));
            return t;
        }
        return comb;
    }
    default: {
        std::string t = new_tmp("t");
        ln(ind, cat(ect, " ", t, " = ", fx(e), ";"));
        ln(ind, cat("if (verr_pending()) ", fret_bail()));
        return t;
    }
    }
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
                    const bool vi = !dbl || fintvars.count(val) > 0;
                    fmt += vi ? "%lld" : "%s";
                    vals += vi ? cat(", (long long)(", fsym[val], ")")
                               : cat(", vfmt_double(", fsym[val], ")");
                }
            }
        } else if (a.k == EK::Num && a.is_float) {
            fmt += "%s";
            vals += cat(", vfmt_double(", repr_f64(a.f), ")");
        } else if (a.k == EK::Num) {
            fmt += "%lld";
            vals += cat(", (long long)(", clit64(a.i), ")");
        } else if (dbl && !fint(a)) {
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
    emit_dbg_stmt(s);
    if (trace_lines && s.line > 0) {
        ln(ind, cat("btrace_line(", s.line, ");"));
    }
    switch (s.k) {
    case SK::Let:
    case SK::LetTmp:
    case SK::Set: {
        std::string rhs = fchk(*s.e1, ind);
        if (rhs != fsym[s.a]) ln(ind, cat(fsym[s.a], " = ", rhs, ";"));
        break;
    }
    case SK::Return: {
        std::string rv = fchk(*s.e1, ind);
        if (frec && fself_rec.empty()) ln(ind, "bdepth_pop();");
        if (fkind == "f64" && fint(*s.e1))
            ln(ind, cat("return (double)(", rv, ");"));
        else
            ln(ind, cat("return ", rv, ";"));
        break;
    }
    case SK::Expr:
        if (s.e1->k == EK::Call && s.e1->a == "print") {
            fprint(ptrs(s.e1->kids), ind);
        } else {
            ln(ind, cat("(void)(", fchk(*s.e1, ind), ");"));
        }
        break;
    case SK::If: {
        ln(ind, cat("if (", strip_outer_parens(fx(*s.e1)), ") {"));
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
        ln(ind, cat("while (", strip_outer_parens(fx(*s.e1)), ") {"));
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
        ln(ind + "  ", cat(fsym[s.a], " = ", fkind == "f64" ? "bf_vfat(" : "bf_viat(", vv, ", ", kk, ");"));
        ln(ind + "  ", cat("if (bverr_unlikely()) ", fret_bail()));
        for (const Stmt *t : view(s.body)) fst(*t, ind + "  ");
        ln(ind, "}");
        break;
    }
    case SK::SetIdx: {
        std::string idx_s = fchk(*s.e2, ind), val_s = fchk(*s.e3, ind);
        if (fkind != "f64" && opt >= 2 && unchecked_setidx.count(&s)) {
            ln(ind, cat(fsym[s.e1->a], "->items[", idx_s, "] = vint(", val_s, ");"));
        } else {
            ln(ind, cat(fkind == "f64" ? "bf_vset_f(" : "bf_vset(", fsym[s.e1->a], ", ", idx_s, ", ", val_s, ");"));
            ln(ind, cat("if (bverr_unlikely()) ", fret_bail()));
        }
        break;
    }
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
    std::string saved_src_file = cur_src_file;
    cur_src_file = lookup_fn_file(name, &decl);
    const std::string &rk = fastkinds[name];
    fkind = (rk == "f64" || rk == "f64_dyn" || rk == "f64v") ? "f64"
                                                              : ((rk == "i64v" || rk == "i64v_dyn") ? "i64v" : rk);
    std::string ct = fkind == "f64" ? "double" : "int64_t";
    fvecvars = fastvec.count(name) ? fastvec[name] : std::set<std::string>();
    fboolvars = fastbool.count(name) ? fastbool[name] : std::set<std::string>();
    fintvars = fastint.count(name) ? fastint[name] : std::set<std::string>();
    std::vector<std::string> names;
    for (const auto &p : decl.params) names.push_back(p.name);
    fsym.clear();
    std::string args, call_args;
    for (const auto &n : names) {
        std::string cn = cname(n);
        fsym[n] = cn;
        if (!args.empty()) {
            args += ", ";
            call_args += ", ";
        }
        args += fvecvars.count(n) ? cat("Vec *", cn) : cat(ct, " ", cn);
        call_args += cn;
    }
    frec = rec_fast.count(name) > 0;
    fself_rec.clear();
    if (frec && !trace_lines) {
        std::set<std::string> calls;
        for (const Stmt *s : view(decl.body)) collect_calls(*s, calls);
        bool only_self = calls.count(name) > 0;
        for (const auto &c : calls) {
            if (c != name && c != "len" && c != "print") {
                only_self = false;
                break;
            }
        }
        if (only_self) fself_rec = name;
    }
    const int fn_line = (!decl.body.empty() && decl.body[0]->line > 0) ? decl.body[0]->line : 1;
    emit_dbg_loc(fn_line, cur_src_file);
    if (!fself_rec.empty()) {
        std::string rargs = args.empty() ? "bint _rem" : cat(args, ", bint _rem");
        ln("", cat("static ", ct, " ", fastc[name], "_r(", rargs, ") {"));
    } else {
        ln("", cat("static ", ct, " ", fastc[name], "(", args.empty() ? "void" : args, ") {"));
    }
    std::set<std::string> param_set(names.begin(), names.end());
    std::function<bool(const Expr &)> pure_int_leaf = [&](const Expr &e) -> bool {
        if (e.is_float) return false;
        switch (e.k) {
        case EK::Num:
        case EK::Bool:
            return true;
        case EK::Var:
            return param_set.count(e.a) > 0 && !fvecvars.count(e.a);
        case EK::Un:
            return (e.a == "-" || e.a == "!" || e.a == "~") &&
                   e.kids.size() == 1 && pure_int_leaf(*e.kids[0]);
        case EK::Bin:
            if (e.a == "<" || e.a == "<=" || e.a == ">" || e.a == ">=" ||
                e.a == "==" || e.a == "!=" || e.a == "+" || e.a == "-" ||
                e.a == "*" || e.a == "&" || e.a == "|" || e.a == "^") {
                return e.kids.size() == 2 && pure_int_leaf(*e.kids[0]) && pure_int_leaf(*e.kids[1]);
            }
            return false;
        default:
            return false;
        }
    };
    size_t body_start = 0;
    if (frec && opt >= 2 && !trace_lines && fkind != "f64") {
        while (body_start < decl.body.size()) {
            const Stmt &st = *decl.body[body_start];
            if (st.k != SK::If || real_else(st) != nullptr || st.body.size() != 1) break;
            const Stmt &ret_st = *st.body[0];
            if (ret_st.k != SK::Return || !st.e1 || !ret_st.e1) break;
            if (!pure_int_leaf(*st.e1) || !pure_int_leaf(*ret_st.e1)) break;
            emit_dbg_stmt(st);
            ln("  ", cat("if (", fx(*st.e1), ") {"));
            if (!fself_rec.empty()) {
                ln("    ", "if (bdepth_rem_leaf(_rem)) { vraise(\"recursion too deep\"); return 0; }");
            } else {
                ln("    ", "if (bdepth_leaf_overflow()) { vraise(\"recursion too deep\"); return 0; }");
            }
            emit_dbg_stmt(ret_st);
            ln("    ", cat("return ", fchk(*ret_st.e1, "    "), ";"));
            ln("  ", "}");
            body_start++;
        }
    }
    if (frec) {
        if (!fself_rec.empty()) {
            ln("  ", cat("if (bdepth_probe_rem(_rem)) { vraise(\"recursion too deep\"); return ",
                         fkind == "f64" ? "0.0" : "0", "; }"));
        } else {
            ln("  ", cat("if (bdepth_push()) { vraise(\"recursion too deep\"); return ",
                         fkind == "f64" ? "0.0" : "0", "; }"));
        }
    }
    std::set<std::string> vars(names.begin(), names.end());
    collect_vars(view(decl.body), vars);
    for (const auto &n : vars) {
        if (std::find(names.begin(), names.end(), n) != names.end()) continue;
        fsym[n] = cname(n);
        const std::string vct = (fkind == "f64" && fintvars.count(n)) ? "int64_t" : ct;
        ln("  ", cat(vct, " BRAT_UNUSED ", cname(n), ";"));
    }
    for (size_t bi = body_start; bi < decl.body.size(); bi++) fst(*decl.body[bi], "  ");
    if (frec && fself_rec.empty()) ln("  ", "bdepth_pop();");
    ln("  ", cat("return ", fkind == "f64" ? "0.0" : "0", ";"));
    ln("", "}");
    if (!fself_rec.empty()) {
        std::string rcall = call_args.empty() ? "bdepth_rem()" : cat(call_args, ", bdepth_rem()");
        ln("", cat("static ", ct, " ", fastc[name], "(", args.empty() ? "void" : args, ") {"));
        ln("  ", cat("return ", fastc[name], "_r(", rcall, ");"));
        ln("", "}");
    }
    ln("", "");
    fsym.clear();
    fkind = "int64_t";
    fvecvars.clear();
    fboolvars.clear();
    fintvars.clear();
    fself_rec.clear();
    frec = false;
    cur_src_file = saved_src_file;
}


static bool is_top_kind(SK k) {
    switch (k) {
    case SK::Fn: case SK::Impl: case SK::Trait: case SK::Enum: case SK::Struct: case SK::Pub:
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
    for (const auto &sp : prog) {
        const Stmt &u = (sp->k == SK::Pub && !sp->body.empty()) ? *sp->body[0] : *sp;
        if (is_top_kind(u.k)) collect_top(*sp);
    }
    for (const auto &sp : prog) {
        const Stmt &u = (sp->k == SK::Pub && !sp->body.empty()) ? *sp->body[0] : *sp;
        if (is_top_kind(u.k)) continue;
        top_exe.push_back(&u);
        if (u.k == SK::Let || u.k == SK::LetTmp) global_lets.insert(u.a);
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
        if (s.opt1 && *s.opt1 == "union") unions.insert(s.a);
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
        e.effects = &s.ext.effects;
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
                fn_ret[key] = f.ret;
                order.push_back({true, s.a, *f.name});
            }
        }
        return;
    }
    case SK::Struct:
        native_structs[s.a] = &s.fields;
        return;
    case SK::Enum: {
        enums.insert(s.a);
        for (const auto &v : s.variants) {
            std::string tag = (v.name == "None" && v.fields.empty()) ? "none" : lower(v.name);
            vtab[tag] = s.a;
            vdisp[tag] = v.name;
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
        late.push_back({mangled, &f, lookup_fn_file(name, &f)});
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
    {"lines", {"vlines", 0}}, {"trim", {"vtrim", 0}},
    {"parse_int", {"vparse_int", 0}}, {"first", {"vfirst", 0}},
    {"last", {"vlast", 0}}, {"sort", {"vsort", 0}},
    {"rev", {"vrev", 0}}, {"pop", {"vpop", 0}},
    {"join", {"vjoin", 1}}, {"replace", {"vreplace", 2}},
};

const std::set<std::string> VBUILTIN_DIRECT = {"del", "copy", "slice"};

const std::set<std::string> MAP_METH = {"get", "has", "keys", "del", "set",
                                        "items", "vals", "copy", "get_or"};

const std::set<std::string> FFI_TYPES = {"bool", "i8", "u8", "i16", "u16", "i32", "u32",
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

static std::string norm_plugin_slot(const std::string &raw) {
    std::string s;
    for (unsigned char c : raw)
        if (!std::isspace(c)) s.push_back(static_cast<char>(c));
    if (s == "ownptr" || s == "moveptr") return "own ptr";
    if (s == "outptr") return "out ptr";
    if (s == "&ptr" || s == "refptr") return "&ptr";
    if (s.rfind("fn(", 0) == 0) {
        while (true) {
            std::string hit;
            const std::pair<std::string, std::vector<std::string>> tbl[] = {
                {"thread", {"same", "any"}}, {"reenter", {"yes", "no"}}
            };
            for (const auto &ent : tbl) {
                for (const auto &v : ent.second) {
                    std::string suf = ent.first + "(" + v + ")";
                    if (s.size() >= suf.size() &&
                        s.compare(s.size() - suf.size(), suf.size(), suf) == 0) {
                        hit = suf;
                        break;
                    }
                }
                if (!hit.empty()) break;
            }
            if (hit.empty()) break;
            s.erase(s.size() - hit.size());
        }
    }
    return s;
}

// `"own ptr(&ptr,out ptr)"` -> ("own ptr", {"&ptr", "out ptr"}); throws when malformed.
static std::pair<std::string, std::vector<std::string>> parse_sig(const Expr &node) {
    if (node.k != EK::Str) throw EmitError("call: signature must be a string literal");
    size_t lo = 0, hi = node.a.size();
    while (lo < hi && std::isspace(static_cast<unsigned char>(node.a[lo]))) lo++;
    while (hi > lo && std::isspace(static_cast<unsigned char>(node.a[hi - 1]))) hi--;
    std::string raw = node.a.substr(lo, hi - lo);
    auto bad = [&]() -> std::pair<std::string, std::vector<std::string>> {
        std::string compact;
        for (char c : node.a)
            if (c != ' ') compact.push_back(c);
        throw EmitError("call: bad signature `" + compact + "`");
    };
    size_t p = raw.find('(');
    if (p == 0 || p == std::string::npos || raw.empty() || raw.back() != ')') return bad();
    int pdepth = 0;
    for (size_t i = p; i < raw.size(); i++) {
        if (raw[i] == '(') pdepth++;
        else if (raw[i] == ')') {
            pdepth--;
            if (pdepth == 0 && i + 1 != raw.size()) return bad();
        }
    }
    if (pdepth != 0) return bad();
    std::string ret = norm_plugin_slot(raw.substr(0, p));
    std::string rest = raw.substr(p + 1, raw.size() - p - 2);
    std::vector<std::string> params;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < rest.size(); i++) {
        if (rest[i] == '(') depth++;
        else if (rest[i] == ')') depth--;
        else if (rest[i] == ',' && depth == 0) {
            std::string piece = norm_plugin_slot(rest.substr(start, i - start));
            if (!piece.empty()) params.push_back(piece);
            start = i + 1;
        }
    }
    std::string tail = norm_plugin_slot(rest.substr(start));
    if (!tail.empty()) params.push_back(tail);
    return {ret, params};
}

// Builtin element-returning methods increment the element's reference count in
// the runtime (`vfirst`/`vlast`/`vmin`/`vmax`/`vmap_get*`/`vpred`), so callers
// receive an owned value and never borrow into a container slot.
const std::set<std::string> PART_RESULT = {};

// Methods that mutate the receiver and hand it back: the result is owned exactly
// when the receiver was. `m.keys().sort()` is a container with no other name,
// `s.sort()` is `s`, and dropping that freed a live vector.
const std::set<std::string> SELF_RESULT = {"sort"};

const std::set<std::string> FRESH_RESULT = {
    "split", "split_once", "join", "upper", "lower", "trim", "replace", "repeat",
    "chars", "lines", "zip", "chunks", "enumerate", "copy", "keys", "items",
    "vals", "sum", "index_of", "starts_with", "ends_with", "contains", "has",
    "get_or", "parse_int", "parse_float", "rev", "fold", "any", "all", "map",
    "filter", "pop", "del", "slice", "unwrap", "unwrap_or", "is_ok", "is_err",
    "is_some", "is_none", "min", "max", "first", "last", "get", "find", "set", "sort_by"};
// True when evaluating `e` can raise: it calls something, indexes/projects,
// unwraps `?`, divides, or hands an operator a value it has no meaning for
// (`"a" - 1`). `typed` leaves the last one out: the operands are numbers.
static bool may_raise(const Expr &e, bool typed) {
    if (e.k == EK::Call || e.k == EK::MCall ||
        e.k == EK::Idx || e.k == EK::Field || e.k == EK::Quest)
        return true;
    if (e.k == EK::Bin && (e.a == "/" || e.a == "%")) return true;
    if (!typed && e.k == EK::Bin && e.a != "&&" && e.a != "||" && e.a != "==" && e.a != "!=")
        return true;
    if (!typed && e.k == EK::Un && e.a != "!") return true;
    for (const auto &kid : e.kids)
        if (kid && may_raise(*kid, typed)) return true;
    return false;
}

static bool dec_able(const Expr &e, const std::set<std::string> &heap) {
    switch (e.k) {
    case EK::Str: case EK::RStr: case EK::Num: case EK::Bool: case EK::None:
    case EK::Bin: case EK::VecLit: case EK::MapLit: case EK::StructLit: case EK::Closure:
    case EK::Or: case EK::ParFor:
        return true;
    case EK::Call:
        // `unwrap`/`unwrap_or` return a reference the caller owns (see vtag_payload), and
        // `Ok(x)`/`Err(x)`/`Some(x)` build a fresh tag on the spot.
        if (e.a == "unwrap" || e.a == "unwrap_or" || e.a == "Ok" || e.a == "Err" ||
            e.a == "Some")
            return true;
        // `__at` is the desugared `a[i]`, which hands back an owned reference.
        if (e.a == "__at") return true;
        if (e.kids.empty() && !e.a.empty() && e.a[0] >= 'A' && e.a[0] <= 'Z' &&
            heap.count(LOCAL_BINDING_PREFIX + e.a)) return false;
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
    case EK::Field:
        return true;
    case EK::Idx:
        // `a[i]` hands back a reference the holder owns (see vat_owned).
        return true;
    case EK::Move:
        return dec_able(*e.kids[0], heap);
    default:
        return false;
    }
}

// Only ever a bool or a number: nothing to retain or release.
static bool is_imm_expr(const Expr &e) {
    switch (e.k) {
    case EK::Num: case EK::Bool:
        return true;
    case EK::Un:
        return e.a == "!" || e.a == "~";
    case EK::Bin:
        if (e.a == "&&" || e.a == "||") return is_imm_expr(*e.kids[0]) && is_imm_expr(*e.kids[1]);
        return e.a == "==" || e.a == "!=" || e.a == "<" || e.a == "<=" || e.a == ">" || e.a == ">=" ||
               e.a == "&" || e.a == "|" || e.a == "^" || e.a == "<<" || e.a == ">>";
    default:
        return false;
    }
}

// `a && b`, `a || b` and `x or y` evaluate to one of their operands.
static bool yields_operand(const Expr &e) {
    return e.k == EK::Or || (e.k == EK::Bin && (e.a == "&&" || e.a == "||") && !is_imm_expr(e));
}

static bool plain_cstr_key(const Expr &e, std::string &out);

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
        if (!ub_ints.empty() && (e.a == "-" || e.a == "~") && ub_expr_ok(e)) {
            if (ub_may_raise(e)) {
                std::string t = new_tmp("ub");
                return {{cat("int64_t ", t, " = ", ub_ex(e), ";"), err_check_line()}, cat("vint(", t, ")")};
            }
            return {{}, cat("vint(", ub_ex(e), ")")};
        }
        if (!ub_floats.empty() && e.a == "-" && ubf_expr_ok(e)) {
            if (ubf_may_raise(e)) {
                std::string t = new_tmp("ubf");
                return {{cat("double ", t, " = ", ubf_ex(e), ";"), err_check_line()}, cat("vfloat(", t, ")")};
            }
            return {{}, cat("vfloat(", ubf_ex(e), ")")};
        }
        V x = ex(*e.kids[0]);
        if (e.a == "!") {
            const std::string test = truth(*e.kids[0], x.val, x.pre);
            return {x.pre, cat("vbool(!", test, ")")};
        }
        if (e.a == "~") return {x.pre, cat("vbnot(", x.val, ")")};
        return {x.pre, cat("vneg(", x.val, ")")};
    }
    case EK::Bin:
        return ex_bin(e);
    case EK::Or: {
        V a = ex(*e.kids[0]);
        auto saved_moved = moved;
        V b = ex(*e.kids[1]);
        moved = saved_moved;
        std::string t = new_tmp();
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("Val ", t, " = (", own_operand(*e.kids[0], a.val), ");"));
        pre.push_back(cat("if (vor_fb(", t, ")) {"));
        pre.push_back(cat("  vdrop_temp(", t, "); ", t, " = vnil();"));
        for (auto &p : b.pre) pre.push_back("  " + p);
        pre.push_back(cat("  ", t, " = (", own_operand(*e.kids[1], b.val), ");"));
        pre.push_back("}");
        return {pre, t};
    }
    case EK::Quest: {
        V a = ex(*e.kids[0]);
        return part_of_temp(a, *e.kids[0],
                            cur_ret_result ? "vquest_ok(vquest_ret(" : "vquest_ok(vquest(", "))");
    }
    case EK::VecLit:
        return ex_vec(ptrs(e.kids));
    case EK::MapLit:
        return ex_maplit(ptrs(e.kids));
    case EK::StructLit:
        return ex_struct(e);
    case EK::Field: {
        V a = ex(*e.kids[0]);
        std::string fname = clit(e.b);
        std::string bail = err_bail();
        const bool owned = dec_able(*e.kids[0], heap_returns());
        std::string recv = a.val;
        if (owned) {
            recv = new_tmp("r");
            a.pre.push_back("Val " + recv + " = (" + a.val + ");");
        }
        std::string st = infer_expr_struct(e.kids[0].get(), var_structs);
        int slot = !st.empty() ? struct_field_slot(st, e.b) : -1;
        std::string drop_recv = owned ? cat("vdrop_temp(", recv, "); ") : std::string();
        std::string v = new_tmp("v");
        if (slot >= 0) {
            std::string t = new_tmp("f");
            int sid = canonical_struct_shape(st);
            a.pre.push_back(cat("Val *", t, " = vstruct_slot_ptr(", recv, ", &_shp[", sid, "], ",
                                slot, ", \"", fname, "\");"));
            a.pre.push_back(cat("if (!", t, ") { ", drop_recv, bail, " }"));
            a.pre.push_back(cat("Val ", v, " = vstruct_slot_read(", recv, ", ", t, ");"));
        } else {
            a.pre.push_back(cat("Val ", v, " = vfield_map_get_ic(", recv, ", \"", fname, "\", ",
                                next_fic(), ");"));
            a.pre.push_back(cat("if (bverr_unlikely()) { ", drop_recv, bail, " }"));
        }
        if (owned) a.pre.push_back("vdrop_temp(" + recv + ");");
        return {a.pre, v};
    }
    case EK::Idx: {
        if (std::string pe = ptr_elem(*e.kids[0]); !pe.empty())
            return ex_ptr_idx(pe, *e.kids[0], *e.kids[1]);
        std::string klit;
        if (plain_cstr_key(*e.kids[1], klit)) {
            V a = ex(*e.kids[0]);
            std::vector<std::string> pre = a.pre;
            std::string fic = next_fic();
            std::string kl = clit(klit);
            if (!dec_able(*e.kids[0], heap_returns()))
                return {pre, cat("vat_cstr_ic(", a.val, ", \"", kl, "\", ", klit.size(), "u, ", fic, ")")};
            std::string recv = new_tmp("r");
            std::string t = new_tmp("c");
            pre.push_back("Val " + recv + " = (" + a.val + ");");
            pre.push_back(cat("Val ", t, " = vat_cstr_ic(", recv, ", \"", kl, "\", ", klit.size(), "u, ", fic, ");"));
            pre.push_back("vdrop_temp(" + recv + ");");
            return {pre, t};
        }
        const auto hr_idx = heap_returns();
        const bool drop_a = dec_able(*e.kids[0], hr_idx);
        const bool drop_b = dec_able(*e.kids[1], hr_idx);
        V a = ex(*e.kids[0]);
        std::vector<std::string> pre = a.pre;
        std::string recv = a.val;
        const size_t idx_cstart = call_cleanup.size();
        if (drop_a) {
            recv = new_tmp("r");
            pre.push_back("Val " + recv + " = (" + a.val + ");");
            call_cleanup.push_back(recv);
        }
        V b;
        try {
            b = ex(*e.kids[1]);
        } catch (...) {
            call_cleanup.resize(idx_cstart);
            throw;
        }
        call_cleanup.resize(idx_cstart);
        for (auto &p : b.pre) pre.push_back(p);
        if (!drop_a && !drop_b)
            return {pre, cat("vat_owned(", recv, ", ", b.val, ")")};
        std::string idxv = b.val;
        if (drop_b) {
            idxv = new_tmp("r");
            pre.push_back("Val " + idxv + " = (" + b.val + ");");
        }
        std::string t = new_tmp("c");
        pre.push_back("Val " + t + " = vat_owned(" + recv + ", " + idxv + ");");
        if (drop_b) pre.push_back("vdrop_temp(" + idxv + ");");
        if (drop_a) pre.push_back("vdrop_temp(" + recv + ");");
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
            std::string mv = new_tmp("mv");
            x.pre.push_back(cat("Val ", mv, " = vmove(&", sym[*src].cn, ");"));
            x.pre.push_back(err_check_line());
            x.val = mv;
        }
        return x;
    }
    }
    throw EmitError("node");
}

struct CleanupGuard {
    std::vector<std::string> &vec;
    size_t base;
    ~CleanupGuard() {
        if (vec.size() > base) vec.resize(base);
    }
};

V Emitter::ex_str(const std::string &s) {
    auto parts = split_interp(s);
    if (parts.empty()) return {{}, "vlit(\"\")"};
    if (parts.size() == 1 && parts[0].first == "lit")
        return {{}, cat("vlit(\"", clit(parts[0].second), "\")")};
    std::string t = new_tmp("s");
    std::vector<std::string> pre = {cat("Val ", t, " = vlit(\"\");")};
    CleanupGuard guard{call_cleanup, call_cleanup.size()};
    call_cleanup.push_back(t);
    for (const auto &[kind, val] : parts) {
        if (kind == "lit") {
            pre.push_back(cat(t, " = vadd_self(", t, ", vlit(\"", clit(val), "\"));"));
        } else {
            std::string sv = new_tmp("sv");
            pre.push_back(cat("Val ", sv, " = vstr_of(", path_ref(val), ");"));
            pre.push_back(err_check_line({sv}));
            pre.push_back(cat(t, " = vadd_self(", t, ", ", sv, ");"));
            pre.push_back(cat("vdrop_temp(", sv, ");"));
        }
        pre.push_back(err_check_line());
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
    std::string cur_st;
    auto vit = var_structs.find(parts[0]);
    if (vit != var_structs.end()) cur_st = vit->second;
    for (size_t i = 1; i < parts.size(); i++) {
        int slot = !cur_st.empty() ? struct_field_slot(cur_st, parts[i]) : -1;
        if (slot >= 0) {
            int sid = canonical_struct_shape(cur_st);
            e = cat("vstruct_slot_val(", e, ", &_shp[", sid, "], ", slot, ", \"",
                    clit(parts[i]), "\")");
            auto fann = struct_field_ann(cur_st, parts[i]);
            cur_st = (fann && native_structs.count(*fann)) ? *fann : "";
        } else {
            e = cat("vfield_ic(", e, ", \"", clit(parts[i]), "\", ", next_fic(), ")");
            cur_st.clear();
        }
    }
    return e;
}

bool Emitter::ub_expr_ok(const Expr &e, const std::set<std::string> &cand) const {
    return !cand.empty() && pure_int_arith_expr(e, cand);
}

bool Emitter::ub_may_raise(const Expr &e) const {
    if (e.k == EK::Bin) {
        if (e.a == "/" || e.a == "%") return true;
        return ub_may_raise(*e.kids[0]) || ub_may_raise(*e.kids[1]);
    }
    if (e.k == EK::Un) return ub_may_raise(*e.kids[0]);
    return false;
}

std::string Emitter::ub_ex(const Expr &e) const {
    switch (e.k) {
    case EK::Num: return clit64(e.i);
    case EK::Var: return cat("ub_", cname(e.a));
    case EK::Un:
        if (e.a == "~") return cat("((int64_t)(~(uint64_t)(", ub_ex(*e.kids[0]), ")))");
        return cat("((int64_t)(0ULL - (uint64_t)(", ub_ex(*e.kids[0]), ")))");
    case EK::Bin: {
        std::string a = ub_ex(*e.kids[0]);
        std::string b = ub_ex(*e.kids[1]);
        if (e.a == "/") return cat("vub_div(", a, ", ", b, ")");
        if (e.a == "%") return cat("vub_mod(", a, ", ", b, ")");
        if (e.a == "<<") return cat("bshl64(", a, ", ", b, ")");
        if (e.a == ">>") return cat("bshr64(", a, ", ", b, ")");
        return cat("((int64_t)((uint64_t)(", a, ") ", e.a, " (uint64_t)(", b, ")))");
    }
    default: throw EmitError("ub node");
    }
}

bool Emitter::ub_int_sub(const Expr &e) const {
    return pure_int_arith_expr(e, ub_ints);
}

bool Emitter::ubf_expr_ok(const Expr &e, const std::set<std::string> &cand) const {
    if (cand.empty()) return false;
    switch (e.k) {
    case EK::Num: return e.is_float;
    case EK::Var: return cand.count(e.a) > 0;
    case EK::Un: return e.a == "-" && ubf_expr_ok(*e.kids[0], cand);
    case EK::Bin: {
        if (e.a != "+" && e.a != "-" && e.a != "*" && e.a != "/" && e.a != "%") return false;
        bool lf = ubf_expr_ok(*e.kids[0], cand);
        bool rf = ubf_expr_ok(*e.kids[1], cand);
        bool li = ub_int_sub(*e.kids[0]);
        bool ri = ub_int_sub(*e.kids[1]);
        return (lf && (rf || ri)) || (rf && li);
    }
    default: return false;
    }
}

bool Emitter::ubf_may_raise(const Expr &e) const {
    if (ub_int_sub(e)) return ub_may_raise(e);
    if (e.k == EK::Bin) {
        if (e.a == "/" || e.a == "%") return true;
        return ubf_may_raise(*e.kids[0]) || ubf_may_raise(*e.kids[1]);
    }
    if (e.k == EK::Un) return ubf_may_raise(*e.kids[0]);
    return false;
}

std::string Emitter::ubf_ex(const Expr &e) const {
    if (ub_int_sub(e)) return cat("((double)(", ub_ex(e), "))");
    switch (e.k) {
    case EK::Num: return repr_f64(e.f);
    case EK::Var: return cat("ubf_", cname(e.a));
    case EK::Un: return cat("(-(", ubf_ex(*e.kids[0]), "))");
    case EK::Bin: {
        std::string a = ubf_ex(*e.kids[0]);
        std::string b = ubf_ex(*e.kids[1]);
        if (e.a == "/") return cat("vub_fdiv(", a, ", ", b, ")");
        if (e.a == "%") return cat("vub_fmod(", a, ", ", b, ")");
        return cat("((", a, ") ", e.a, " (", b, "))");
    }
    default: throw EmitError("ubf node");
    }
}

std::string Emitter::ubf_cmp_c(const std::string &op, const std::string &a, const std::string &b) const {
    if (op == "==" || op == "!=") return cat("(", a, ") ", op, " (", b, ")");
    int code = op == "<" ? 0 : (op == "<=" ? 1 : (op == ">" ? 2 : 3));
    return cat("vub_fcmp(", a, ", ", b, ", ", code, ")");
}

V Emitter::ex_bin(const Expr &e) {
    const std::string &op = e.a;
    if (op == "&&" || op == "||") {
        // The result is one operand, chosen at run time, and whoever receives it owns it
        // either way: a variable is retained, and a fresh left value is released when the
        // right one replaces it. Taking the operand as an alias freed it with the result.
        V a = ex(*e.kids[0]);
        auto saved_moved = moved;
        V b = ex(*e.kids[1]);
        moved = saved_moved;
        std::string t = new_tmp();
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("Val ", t, " = (", own_operand(*e.kids[0], a.val), ");"));
        pre.push_back(op == "&&" ? cat("if (vis_true(", t, ")) {")
                                 : cat("if (!vis_true(", t, ")) {"));
        if (!is_imm_expr(*e.kids[0])) {
            pre.push_back(cat("  vdrop_temp(", t, ");"));
            pre.push_back(cat("  ", t, " = vnil();"));
        }
        for (auto &p : b.pre) pre.push_back("  " + p);
        pre.push_back(cat("  ", t, " = (", own_operand(*e.kids[1], b.val), ");"));
        pre.push_back("}");
        return {pre, t};
    }
    if ((op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") &&
        e.kids.size() == 2 && e.kids[0]->k == EK::Idx && e.kids[0]->kids.size() == 2 &&
        e.kids[0]->kids[0]->k == EK::Var &&
        (e.kids[0]->kids[1]->k == EK::Var || e.kids[0]->kids[1]->k == EK::Num) &&
        e.kids[1]->k == EK::Str && e.kids[1]->a.size() == 1 &&
        static_cast<unsigned char>(e.kids[1]->a[0]) < 0x80) {
        V base = ex(*e.kids[0]->kids[0]);
        V index = ex(*e.kids[0]->kids[1]);
        V ch = ex(*e.kids[1]);
        if (base.pre.empty() && index.pre.empty() && ch.pre.empty()) {
            const std::string fast = new_tmp("ascii_fast");
            const std::string cmp = new_tmp("ascii_cmp");
            const std::string result = new_tmp("ascii_result");
            const std::string picked = new_tmp("ascii_char");
            std::vector<std::string> pre{
                cat("int ", fast, " = 0;"),
                cat("int ", cmp, " = vstr_ascii_cmp(", base.val, ", ", index.val, ", ",
                    static_cast<int>(static_cast<unsigned char>(e.kids[1]->a[0])), ", &", fast, ");"),
                err_check_line(),
                cat("Val ", result, ";"),
                cat("if (", fast, ") {"),
                cat("  ", result, " = vbool((", cmp, ") ", op, " 0);"),
                "} else {",
                cat("  Val ", picked, " = vat(", base.val, ", ", index.val, ");"),
                cat("  ", err_check_line())
            };
            std::string fallback;
            if (op == "==") fallback = cat("veq(", picked, ", ", ch.val, ")");
            else if (op == "!=") fallback = cat("!veq(", picked, ", ", ch.val, ")");
            else fallback = cat("vcmpt(", picked, ", ", ch.val, ", \"", op, "\") ", op, " 0");
            pre.push_back(cat("  ", result, " = vbool(", fallback, ");"));
            pre.push_back("}");
            return {pre, result};
        }
    }
    if (!ub_ints.empty() && ub_expr_ok(*e.kids[0]) && ub_expr_ok(*e.kids[1])) {
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%" ||
            op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
            if (ub_may_raise(e)) {
                std::string t = new_tmp("ub");
                return {{cat("int64_t ", t, " = ", ub_ex(e), ";"), err_check_line()}, cat("vint(", t, ")")};
            }
            return {{}, cat("vint(", ub_ex(e), ")")};
        }
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
            if (ub_may_raise(*e.kids[0]) || ub_may_raise(*e.kids[1])) {
                std::string ta = new_tmp("ub"), tb = new_tmp("ub");
                return {{cat("int64_t ", ta, " = ", ub_ex(*e.kids[0]), ";"), err_check_line(),
                         cat("int64_t ", tb, " = ", ub_ex(*e.kids[1]), ";"), err_check_line()},
                        cat("vbool((", ta, ") ", op, " (", tb, "))")};
            }
            return {{}, cat("vbool((", ub_ex(*e.kids[0]), ") ", op, " (", ub_ex(*e.kids[1]), "))")};
        }
    }
    if (!ub_floats.empty()) {
        bool lf = ubf_expr_ok(*e.kids[0]);
        bool rf = ubf_expr_ok(*e.kids[1]);
        if ((op == "+" || op == "-" || op == "*" || op == "/" || op == "%") && ubf_expr_ok(e)) {
            if (ubf_may_raise(e)) {
                std::string t = new_tmp("ubf");
                return {{cat("double ", t, " = ", ubf_ex(e), ";"), err_check_line()}, cat("vfloat(", t, ")")};
            }
            return {{}, cat("vfloat(", ubf_ex(e), ")")};
        }
        if ((op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") && lf && rf) {
            if (ubf_may_raise(*e.kids[0]) || ubf_may_raise(*e.kids[1])) {
                std::string ta = new_tmp("ubf"), tb = new_tmp("ubf");
                return {{cat("double ", ta, " = ", ubf_ex(*e.kids[0]), ";"), err_check_line(),
                         cat("double ", tb, " = ", ubf_ex(*e.kids[1]), ";"), err_check_line()},
                        cat("vbool(", ubf_cmp_c(op, ta, tb), ")")};
            }
            return {{}, cat("vbool(", ubf_cmp_c(op, ubf_ex(*e.kids[0]), ubf_ex(*e.kids[1])), ")")};
        }
    }
    V a = ex(*e.kids[0]);
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
    auto is_lit_ctr = [](const Expr &node, const std::string &val) {
        const auto k = node.k;
        const bool is_ml = (k == EK::MapLit && val.rfind("_ml", 0) == 0);
        return (container_tmp(val) || is_ml) &&
               (k == EK::VecLit || k == EK::MapLit || k == EK::StructLit);
    };
    hold(*e.kids[0], a.val);
    std::vector<std::string> lhs_err = drops;
    if (is_lit_ctr(*e.kids[0], a.val)) lhs_err.push_back(a.val);
    if (!a.pre.empty() && may_raise(*e.kids[0])) pre.push_back(err_check_line(lhs_err));
    V b;
    {
        CleanupGuard guard{call_cleanup, call_cleanup.size()};
        for (const auto &d : lhs_err) call_cleanup.push_back(d);
        b = ex(*e.kids[1]);
    }
    for (auto &p : b.pre) pre.push_back(p);
    hold(*e.kids[1], b.val);
    if (!b.pre.empty() && may_raise(*e.kids[1])) {
        std::vector<std::string> rhs_err = drops;
        if (is_lit_ctr(*e.kids[0], a.val)) rhs_err.push_back(a.val);
        if (is_lit_ctr(*e.kids[1], b.val)) rhs_err.push_back(b.val);
        pre.push_back(err_check_line(rhs_err));
    }
    if (op == "+") {
        std::string t = new_tmp("v");
        pre.push_back(cat("Val ", t, " = vadd(", a.val, ", ", b.val, ");"));
        bool has_ctr = false;
        const Expr *nodes[2] = {e.kids[0].get(), e.kids[1].get()};
        const std::string vals[2] = {a.val, b.val};
        for (int i = 0; i < 2; i++) {
            if (is_lit_ctr(*nodes[i], vals[i])) {
                pre.push_back(cat("vrc_dec(", vals[i], ");"));
                has_ctr = true;
            }
        }
        for (auto &d : drops) pre.push_back(cat("vdrop_temp(", d, ");"));
        if (!drops.empty() || has_ctr) pre.push_back(err_check_line());
        return {pre, t};
    }
    // A container built for a binary op is read, not consumed: `v == [1, 2, 3]`
    // left the literal's vector behind on every evaluation.
    std::vector<std::string> cdrops;
    {
        const Expr *nodes[2] = {e.kids[0].get(), e.kids[1].get()};
        const std::string vals[2] = {a.val, b.val};
        for (int i = 0; i < 2; i++) {
            if (is_lit_ctr(*nodes[i], vals[i]))
                cdrops.push_back(cat("vrc_dec(", vals[i], ");"));
        }
    }
    std::string res;
    if (op == "-") res = cat("vsub(", a.val, ", ", b.val, ")");
    else if (op == "*") res = cat("vmul(", a.val, ", ", b.val, ")");
    else if (op == "/") res = cat("vdiv(", a.val, ", ", b.val, ")");
    else if (op == "%") res = cat("vmod(", a.val, ", ", b.val, ")");
    else if (op == "&") res = cat("vband(", a.val, ", ", b.val, ")");
    else if (op == "|") res = cat("vbor(", a.val, ", ", b.val, ")");
    else if (op == "^") res = cat("vbxor(", a.val, ", ", b.val, ")");
    else if (op == "<<") res = cat("vbshl(", a.val, ", ", b.val, ")");
    else if (op == ">>") res = cat("vbshr(", a.val, ", ", b.val, ")");
    else if (op == "==") res = cat("vbool(veq(", a.val, ", ", b.val, "))");
    else if (op == "!=") res = cat("vbool(!veq(", a.val, ", ", b.val, "))");
    else if (op == "<" || op == ">" || op == "<=" || op == ">=")
        res = cat("vbool(vcmpt(", a.val, ", ", b.val, ", \"", op, "\") ", op, " 0)");
    else if (op == "in")
        res = cat("vbool(vcontains(", b.val, ", ", a.val, "))");
    if (!res.empty()) {
        if (drops.empty() && cdrops.empty()) return {pre, res};
        std::string t = new_tmp("v");
        pre.push_back(cat("Val ", t, " = (", res, ");"));
        // A temp the comparison finished with is nobody's on the success path
        // too. Releasing it only when a raise was pending is what leaked one
        // string per `assert("a" + "b" == "ab")`: the error path was covered and
        // the path a correct program takes was not.
        for (auto &d : drops) pre.push_back(cat("vdrop_temp(", d, ");"));
        for (auto &d : cdrops) pre.push_back(d);
        pre.push_back(err_check_line());
        return {pre, t};
    }
    throw EmitError("operator " + op);
}

const std::set<std::string> SIG_TYPES = {"i64", "f32", "f64", "str", "ptr",
                                         "own ptr", "&ptr", "out ptr", "none"};

V Emitter::ex_plugin(const std::string &name, const ExprList &args) {
    if (name == "lib_open" || name == "lib_close") {
        need_arity(args, 1, name);
        const char *fn = (name == "lib_open") ? "vlib_open(" : "vlib_close(";
        return direct_call_result(args, [&](const auto &vals) {
            return cat(fn, vals[0], ")");
        });
    }
    if (name == "lib_sym") {
        need_arity(args, 2, name);
        return direct_call_result(args, [&](const auto &vals) {
            return cat("vlib_sym(", vals[0], ", ", vals[1], ")");
        });
    }
    auto [ret_t, params] = parse_sig(*args[1]);
    if (!SIG_TYPES.count(ret_t) || ret_t == "&ptr" || ret_t == "out ptr")
        throw EmitError("call: unsupported return type `" + ret_t + "`");
    for (const auto &t : params) {
        if (t.rfind("fn(", 0) == 0 && t.find(")->") != std::string::npos) continue;
        if (!SIG_TYPES.count(t) || t == "none")
            throw EmitError("call: unsupported argument type `" + t + "`");
    }
    if (args.size() - 2 != params.size())
        throw EmitError(cat("call: signature wants ", params.size(), " argument(s), got ",
                            args.size() - 2));
    const auto hr_plug = heap_returns();
    const size_t plug_cstart = call_cleanup.size();
    std::vector<std::string> arg_drops;
    V f = ex(*args[0]);
    std::vector<std::string> pre = f.pre;
    std::string fval = f.val;
    if (dec_able(*args[0], hr_plug)) {
        fval = new_tmp("r");
        pre.push_back(cat("Val ", fval, " = (", f.val, ");"));
        call_cleanup.push_back(fval);
        arg_drops.push_back(fval);
    }
    std::string cvals;
    auto saved_out = out_binds;
    out_binds.clear();
    try {
        for (size_t i = 0; i < params.size(); i++) {
            const std::string &t = params[i];
            const Expr &arg = *args[2 + i];
            if (!cvals.empty()) cvals += ", ";
            if (t.rfind("fn(", 0) == 0) {
                check_cb_arg(t, arg, "call", static_cast<int>(i) + 1);
                cvals += cat("(void *)(intptr_t)brat_cb_", arg.a);
                continue;
            }
            if (t == "out ptr" && arg.k == EK::Un && arg.a == "&") {
                in_out_arg = true;
                auto o = out_addr(*arg.kids[0], nullptr);
                in_out_arg = false;
                for (auto &p : o.first) pre.push_back(p);
                cvals += o.second;
                continue;
            }
            V a = ex(arg);
            for (auto &p : a.pre) pre.push_back(p);
            std::string aval = a.val;
            if (dec_able(arg, hr_plug)) {
                aval = new_tmp("r");
                pre.push_back(cat("Val ", aval, " = (", a.val, ");"));
                call_cleanup.push_back(aval);
                arg_drops.push_back(aval);
            }
            if (t == "f64") cvals += cat("(", aval, ").as.fval");
            else if (t == "f32") cvals += cat("(float)vnum(", aval, ")");
            else if (t == "str") cvals += cat("vstr_pin(", aval, ")");
            else if (t == "ptr" || t == "own ptr" || t == "&ptr" || t == "out ptr")
                cvals += cat("(void *)(intptr_t)(", aval, ").as.ival");
            else cvals += cat("(int64_t)(", aval, ").as.ival");
        }
    } catch (...) {
        call_cleanup.resize(plug_cstart);
        out_binds = std::move(saved_out);
        throw;
    }
    call_cleanup.resize(plug_cstart);
    std::vector<std::string> fl = out_binds.empty() ? std::vector<std::string>{} : out_flush();
    out_binds = std::move(saved_out);
    std::string plugin_owner = new_tmp("plugin_owner");
    std::string plugin_fn = new_tmp("plugin_fn");
    auto canon_pslot = [](const std::string &s) -> std::string {
        if (s == "own ptr" || s == "&ptr" || s == "out ptr" || s.rfind("fn(", 0) == 0)
            return "ptr";
        return s;
    };
    std::string canon_sig = canon_pslot(ret_t) + "(";
    for (size_t i = 0; i < params.size(); i++) {
        if (i) canon_sig += ",";
        canon_sig += canon_pslot(params[i]);
    }
    canon_sig += ")";
    pre.push_back(err_check_line(arg_drops));
    pre.push_back(cat("bint ", plugin_owner, " = 0;"));
    pre.push_back(cat("void *", plugin_fn, " = vplugin_acquire_sig(", fval, ", &",
                      plugin_owner, ", \"", canon_sig, "\");"));
    pre.push_back(err_check_line(arg_drops));
    std::vector<std::string> release = fl;
    release.push_back(cat("vplugin_release(", plugin_owner, ");"));
    for (const auto &d : arg_drops) release.push_back(cat("vdrop_temp(", d, ");"));
    release.push_back(err_check_line());
    auto ctype = [](const std::string &t) -> std::string {
        if (t.rfind("fn(", 0) == 0) return "void *";
        if (t == "i64") return "int64_t";
        if (t == "f32") return "float";
        if (t == "f64") return "double";
        if (t == "str") return "const char *";
        if (t == "ptr" || t == "own ptr" || t == "&ptr" || t == "out ptr") return "void *";
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
    if (ret_t == "f32") {
        pre.push_back(cat("float ", t_tmp, " = ", call_expr, ";"));
        for (const auto &line : release) pre.push_back(line);
        return {pre, cat("vfloat((double)", t_tmp, ")")};
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
    if (ret_t == "ptr" || ret_t == "own ptr") {
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
        if (dec_able(*args[0], heap_returns())) {
            std::string tb = new_tmp("sb");
            std::string sr = new_tmp("sl");
            pre.push_back(cat("Val ", tb, " = ", b.val, ";"));
            pre.push_back(cat("Val ", sr, " = vslice_of(", tb, ", ", a.val, ", ", c.val, ");"));
            pre.push_back(cat("vdrop_temp(", tb, ");"));
            return {pre, sr};
        }
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
            vals.push_back(keep(*kid, v.val));
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
        std::vector<std::string> pre = s.pre;
        if (dec_able(*args[0], heap_returns())) {
            std::string ts = new_tmp("ts");
            std::string r = new_tmp("sl");
            pre.push_back(cat("Val ", ts, " = ", s.val, ";"));
            pre.push_back(cat("Val ", r, " = vslice_len(", ts, ");"));
            pre.push_back(cat("vdrop_temp(", ts, ");"));
            return {pre, r};
        }
        return {pre, cat("vslice_len(", s.val, ")")};
    }
    if (name == "slice_get") {
        need_arity(args, 2, name);
        V s = ex(*args[0]);
        V i = ex(*args[1]);
        std::vector<std::string> pre = s.pre;
        for (auto &p : i.pre) pre.push_back(p);
        std::string sval = s.val;
        std::string ts;
        if (dec_able(*args[0], heap_returns())) {
            ts = new_tmp("ts");
            pre.push_back(cat("Val ", ts, " = ", s.val, ";"));
            sval = ts;
        }
        if (std::string se = slice_elem(*args[0]); structs.count(se)) {
            std::string sa = new_tmp("sa");
            std::string r = new_tmp("sr");
            pre.push_back(cat("Val ", sa, " = vslice_at(", sval, ", ", i.val, ");"));
            if (!ts.empty()) pre.push_back(cat("vdrop_temp(", ts, ");"));
            pre.push_back(cat("Val ", r, " = vunwind;"));
            pre.push_back(cat("if (vptr_live(", sa, ")) { ", r, " = ",
                              c_struct_to_val(se, cat("(*(bf_", se, " *)(", sa, ").as.ival)")),
                              "; }"));
            return {pre, r};
        }
        if (!ts.empty()) {
            std::string r = new_tmp("sg");
            pre.push_back(cat("Val ", r, " = vslice_at(", ts, ", ", i.val, ");"));
            pre.push_back(cat("vdrop_temp(", ts, ");"));
            return {pre, r};
        }
        return {pre, cat("vslice_at(", sval, ", ", i.val, ")")};
    }
    if (name == "slice_set") {
        need_arity(args, 3, name);
        V s = ex(*args[0]);
        V i = ex(*args[1]);
        V v = ex(*args[2]);
        std::vector<std::string> pre = s.pre;
        for (auto &p : i.pre) pre.push_back(p);
        for (auto &p : v.pre) pre.push_back(p);
        std::string vval = keep(*args[2], v.val);
        if (dec_able(*args[0], heap_returns())) {
            std::string ts = new_tmp("ts");
            std::string r = new_tmp("ss");
            pre.push_back(cat("Val ", ts, " = ", s.val, ";"));
            pre.push_back(cat("Val ", r, " = vslice_set(", ts, ", ", i.val, ", ", vval, ");"));
            pre.push_back(cat("vdrop_temp(", ts, ");"));
            return {pre, r};
        }
        return {pre, cat("vslice_set(", s.val, ", ", i.val, ", ", vval, ")")};
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
    CleanupGuard guard{call_cleanup, call_cleanup.size()};
    call_cleanup.push_back(cat("vvecv(", t, ")"));
    for (const Expr *x : items) {
        V xv = ex(*x);
        for (auto &p : xv.pre) pre.push_back(p);
        pre.push_back(cat("vec_push(", t, ", ", keep(*x, xv.val), ");"));
        if (may_raise(*x)) pre.push_back(err_check_line());
    }
    return {pre, cat("vvecv(", t, ")")};
}

static bool plain_cstr_key(const Expr &e, std::string &out) {
    if (e.k == EK::RStr) {
        if (e.a.find('\0') != std::string::npos || e.a == "$type") return false;
        out = e.a;
        return true;
    }
    if (e.k == EK::Str) {
        auto parts = split_interp(e.a);
        if (parts.empty()) {
            out.clear();
            return true;
        }
        if (parts.size() == 1 && parts[0].first == "lit" &&
            parts[0].second.find('\0') == std::string::npos && parts[0].second != "$type") {
            out = parts[0].second;
            return true;
        }
    }
    return false;
}

V Emitter::ex_maplit(const ExprList &kids) {
    const size_t npairs = kids.size() / 2;
    if (npairs >= 1 && npairs <= 16) {
        std::vector<std::string> knames;
        knames.reserve(npairs);
        std::set<std::string> uniq;
        bool all_lit = true;
        for (size_t i = 0; i + 1 < kids.size(); i += 2) {
            std::string k;
            if (!plain_cstr_key(*kids[i], k) || !uniq.insert(k).second) {
                all_lit = false;
                break;
            }
            knames.push_back(std::move(k));
        }
        if (all_lit) {
            std::string t = new_tmp("m");
            int sid = shape_id("", knames);
            std::vector<std::string> pre = {cat("Map *", t, " = vmap_shaped(&_shp[", sid,
                                                "], \"\", _sk", sid, ", ",
                                                static_cast<int>(npairs), ");")};
            CleanupGuard guard{call_cleanup, call_cleanup.size()};
            call_cleanup.push_back(cat("vstruct(", t, ")"));
            for (size_t k = 0; k < npairs; k++) {
                const Expr &ve = *kids[k * 2 + 1];
                V vv = ex(ve);
                for (auto &p : vv.pre) pre.push_back(p);
                pre.push_back(cat(t, "->vals[", static_cast<int>(k), "] = ", keep(ve, vv.val), ";"));
                if (may_raise(ve)) pre.push_back(err_check_line());
            }
            return {pre, cat("vstruct(", t, ")")};
        }
    }
    std::string t = new_tmp("m");
    std::vector<std::string> pre = {cat("Map *", t, " = vmap(\"\");")};
    CleanupGuard guard{call_cleanup, call_cleanup.size()};
    call_cleanup.push_back(cat("vstruct(", t, ")"));
    for (size_t i = 0; i + 1 < kids.size(); i += 2) {
        const Expr &ke = *kids[i], &ve = *kids[i + 1];
        V kv = ex(ke);
        for (auto &p : kv.pre) pre.push_back(p);
        std::string kt = new_tmp("mk");
        pre.push_back(cat("Val ", kt, " = ", keep(ke, kv.val), ";"));
        const size_t kbase = call_cleanup.size();
        call_cleanup.push_back(kt);
        if (may_raise(ke)) pre.push_back(err_check_line());
        V vv = ex(ve);
        for (auto &p : vv.pre) pre.push_back(p);
        std::string vt = new_tmp("mv");
        pre.push_back(cat("Val ", vt, " = ", keep(ve, vv.val), ";"));
        call_cleanup.push_back(vt);
        if (may_raise(ve)) pre.push_back(err_check_line());
        call_cleanup.resize(kbase);
        pre.push_back(cat("map_set_v(", t, ", ", kt, ", ", vt, ");"));
    }
    std::string r = new_tmp("ml");
    pre.push_back(cat("Val ", r, " = vmap_has_reserved(", t, ") ? vmapkey_err() :"
                      " (vmap_bad_seen(", t, ") ? vmap_bad_key_err() : vstruct(", t, "));"));
    return {pre, r};
}

V Emitter::ex_struct(const Expr &e) {
    std::string t = new_tmp("m");
    std::set<std::string> uniq(e.names.begin(), e.names.end());
    auto nit = native_structs.find(e.a);
    if (nit != native_structs.end() && !nit->second->empty() &&
        e.names.size() == nit->second->size() && uniq.size() == e.names.size()) {
        std::vector<int> slots(e.names.size(), -1);
        bool all_valid = true;
        for (size_t i = 0; i < e.names.size(); i++) {
            slots[i] = struct_field_slot(e.a, e.names[i]);
            if (slots[i] < 0) {
                all_valid = false;
                break;
            }
        }
        if (all_valid) {
            int sid = canonical_struct_shape(e.a);
            std::vector<std::string> pre = {cat("Map *", t, " = vmap_shaped(&_shp[", sid, "], \"",
                                                clit(e.a), "\", _sk", sid, ", ",
                                                static_cast<int>(nit->second->size()), ");")};
            CleanupGuard guard{call_cleanup, call_cleanup.size()};
            call_cleanup.push_back(cat("vstruct(", t, ")"));
            for (size_t i = 0; i < e.names.size(); i++) {
                const Expr &fv = *e.kids[i];
                V f = ex(fv);
                for (auto &p : f.pre) pre.push_back(p);
                pre.push_back(cat(t, "->vals[", slots[i], "] = ", keep(fv, f.val), ";"));
                if (may_raise(fv)) pre.push_back(err_check_line());
            }
            return {pre, cat("vstruct(", t, ")")};
        }
    }
    if (!e.names.empty() && uniq.size() == e.names.size()) {
        int sid = shape_id(e.a, e.names);
        std::vector<std::string> pre = {cat("Map *", t, " = vmap_shaped(&_shp[", sid, "], \"",
                                            clit(e.a), "\", _sk", sid, ", ",
                                            static_cast<int>(e.names.size()), ");")};
        CleanupGuard guard{call_cleanup, call_cleanup.size()};
        call_cleanup.push_back(cat("vstruct(", t, ")"));
        for (size_t i = 0; i < e.names.size(); i++) {
            const Expr &fv = *e.kids[i];
            V f = ex(fv);
            for (auto &p : f.pre) pre.push_back(p);
            pre.push_back(cat(t, "->vals[", static_cast<int>(i), "] = ", keep(fv, f.val), ";"));
            if (may_raise(fv)) pre.push_back(err_check_line());
        }
        return {pre, cat("vstruct(", t, ")")};
    }
    std::vector<std::string> pre = {cat("Map *", t, " = vmap(\"", clit(e.a), "\");")};
    CleanupGuard guard{call_cleanup, call_cleanup.size()};
    call_cleanup.push_back(cat("vstruct(", t, ")"));
    for (size_t i = 0; i < e.names.size(); i++) {
        const Expr &fv = *e.kids[i];
        V f = ex(fv);
        for (auto &p : f.pre) pre.push_back(p);
        std::string ft = new_tmp("mf");
        pre.push_back(cat("Val ", ft, " = ", keep(fv, f.val), ";"));
        if (may_raise(fv)) pre.push_back(err_check_line({ft}));
        pre.push_back(cat("map_set(", t, ", \"", clit(e.names[i]), "\", ", ft, ");"));
    }
    return {pre, cat("vstruct(", t, ")")};
}

V Emitter::args_vec(const ExprList &args) {
    std::string t = new_tmp("a");
    size_t n = args.size();
    if (n == 0) {
        std::string sv = new_tmp("sv");
        return {{cat("BStackVec ", sv, "; VSTACK_VEC_INIT(", sv, ", NULL, 0); Vec *", t, " = &", sv, ".vec;")}, t};
    }
    const size_t start = call_cleanup.size();
    call_cleanup.push_back("@args:" + t);
    try {
        if (n <= 8) {
            std::string sv = new_tmp("sv");
            std::string ab = new_tmp("ab");
            std::vector<std::string> pre = {cat("Val ", ab, "[", n, "]; BStackVec ", sv, "; VSTACK_VEC_INIT(", sv,
                                                ", ", ab, ", ", n, "); Vec *", t, " = &", sv, ".vec;")};
            for (const Expr *a : args) {
                V v = ex(*a);
                for (auto &p : v.pre) pre.push_back(p);
                pre.push_back(cat(ab, "[", t, "->len++] = ", keep(*a, v.val), ";"));
                pre.push_back(err_check_line());
            }
            call_cleanup.resize(start);
            return {pre, t};
        }
        std::vector<std::string> pre = {cat("Vec *", t, " = vvec();")};
        for (const Expr *a : args) {
            V v = ex(*a);
            for (auto &p : v.pre) pre.push_back(p);
            pre.push_back(cat("vec_push(", t, ", ", keep(*a, v.val), ");"));
            pre.push_back(err_check_line());
        }
        call_cleanup.resize(start);
        return {pre, t};
    } catch (...) {
        call_cleanup.resize(start);
        throw;
    }
}

void Emitter::emit_loop_drops(const StmtList &body, const std::string &ind) {
    std::set<std::string> seen;
    std::function<void(const StmtList &)> walk = [&](const StmtList &stmts) {
        for (const Stmt *st : stmts) {
            if (st->k == SK::Let || st->k == SK::LetTmp) {
                auto name = head_of(st->a);
                if (relet.count(name) && !seen.count(name)) {
                    seen.insert(name);
                    std::string scn = slot_cn(name);
                    ln(ind, cat("vrelease(", scn, ");"));
                    ln(ind, cat(scn, " = vnil();"));
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
            } else if (st->k == SK::Block || st->k == SK::Scope || st->k == SK::Unsafe) {
                walk(view(st->body));
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
static bool body_returns_owned(const StmtList &body, const std::set<std::string> &heap,
                               const std::string *self_a = nullptr,
                               const std::string *self_b = nullptr);
static bool block_returns_owned(const StmtList &body, const std::set<std::string> &heap, bool &saw,
                                bool in_try = false,
                                const std::string *self_a = nullptr,
                                const std::string *self_b = nullptr);
static bool stmt_returns_owned(const Stmt &s, const std::set<std::string> &heap, bool &saw,
                               bool in_try = false,
                               const std::string *self_a = nullptr,
                               const std::string *self_b = nullptr);

static bool expr_raises_err(const Expr *e) {
    if (!e || e->k == EK::Closure) return false;
    if (e->k == EK::Quest) return true;
    if (e->k == EK::Call && e->a == "raise") return true;
    for (const auto &k : e->kids)
        if (expr_raises_err(k.get())) return true;
    return false;
}

// Every `return` this statement contains has to hand back an owned value. `saw`
// records that the function returns at all; a branch or a loop with no `return`
// in it is not a counter-example, which is why the bodies share one `saw`
// instead of each answering for its own shape. An uncaught `raise(...)` or `?`
// also leaves the frame with an owned `Err` tag via `verr_leave()`.
static bool stmt_returns_owned(const Stmt &s, const std::set<std::string> &heap, bool &saw,
                               bool in_try, const std::string *self_a, const std::string *self_b) {
    if (!in_try && (expr_raises_err(s.e1.get()) || expr_raises_err(s.e2.get()) ||
                    expr_raises_err(s.e3.get()))) {
        saw = true;
    }
    switch (s.k) {
    case SK::Return: {
        if (!s.e1) return true;
        if (s.e1->k == EK::Num || s.e1->k == EK::Bool || s.e1->k == EK::None) return true;
        bool is_self = (s.e1->k == EK::Call &&
                        ((self_a && s.e1->a == *self_a) || (self_b && s.e1->a == *self_b)));
        if (!is_self) saw = true;
        if (s.e1->k == EK::Var) return true;
        if (s.e1->k == EK::Move && !s.e1->kids.empty() && s.e1->kids[0]->k == EK::Var) return true;
        return dec_able(*s.e1, heap);
    }
    case SK::Block: return block_returns_owned(view(s.body), heap, saw, in_try, self_a, self_b);
    case SK::If:
        return block_returns_owned(view(s.body), heap, saw, in_try, self_a, self_b) &&
               (!s.els || stmt_returns_owned(*s.els, heap, saw, in_try, self_a, self_b));
    case SK::Spawn: return true;
    case SK::Try:
        return block_returns_owned(view(s.body), heap, saw, true, self_a, self_b) &&
               block_returns_owned(view(s.body2), heap, saw, in_try, self_a, self_b);
    case SK::While: case SK::ForRange: case SK::ForIter:
    case SK::Scope: case SK::Unsafe: case SK::Defer:
        return block_returns_owned(view(s.body), heap, saw, in_try, self_a, self_b);
    default: return true;
    }
}

static bool block_returns_owned(const StmtList &body, const std::set<std::string> &heap, bool &saw,
                                bool in_try, const std::string *self_a, const std::string *self_b) {
    for (const Stmt *s : body)
        if (!stmt_returns_owned(*s, heap, saw, in_try, self_a, self_b)) return false;
    return true;
}

static bool body_returns_owned(const StmtList &body, const std::set<std::string> &heap,
                               const std::string *self_a, const std::string *self_b) {
    bool saw = false;
    return block_returns_owned(body, heap, saw, false, self_a, self_b) && saw;
}

std::set<std::string> Emitter::heap_returns() {
    std::set<std::string> out_;
    auto add_fn_names = [](std::set<std::string> &dst, const std::string &name) {
        dst.insert(name);
        size_t sep = name.find_last_of(":.");
        if (sep != std::string::npos) dst.insert(name.substr(sep + 1));
    };
    for (const auto &[name, ret] : fn_ret) {
        std::string r = ret.value_or("");
        if (r == "vec" || r == "map" || r == "str" || r == "tag" ||
            r.rfind("vec[", 0) == 0 || r.rfind("map[", 0) == 0 ||
            r == "Option" || r.rfind("Option[", 0) == 0 ||
            r == "Result" || r.rfind("Result[", 0) == 0) {
            add_fn_names(out_, name);
        } else if ((ret && (structs.count(r) || native_structs.count(r))) ||
                   (!r.empty() && enums.count(r))) {
            add_fn_names(out_, name);
        }
    }
    // Bodies that prove it, whatever the annotation claims. Two passes reach
    // the usual fixpoint: a helper used by a helper.
    for (;;) {
        std::set<std::string> grown = out_;
        for (const auto &[name, entry] : fns) {
            if (!entry.decl || entry.ctor) continue;
            size_t sep = name.find_last_of(":.");
            std::string short_name = sep == std::string::npos ? name : name.substr(sep + 1);
            // First check non-self base cases (`saw` requires at least one non-self owned return),
            // then verify all returns assuming self is owned by induction on recursion depth.
            std::set<std::string> with_self = grown;
            with_self.insert(short_name);
            with_self.insert(name);
            if (!body_returns_owned(view(entry.decl->body), with_self, &name, &short_name)) continue;
            grown.insert(short_name);
            grown.insert(name);
        }
        if (grown.size() == out_.size()) break;
        out_ = grown;
    }
    // Local closure calls can return any Val. Uppercase bare bindings are
    // encoded as zero-argument calls too; mark them to keep those reads borrowed.
    for (const auto &[name, _] : sym) {
        out_.insert(name);
        out_.insert(LOCAL_BINDING_PREFIX + name);
    }
    if (cap) {
        for (const auto &name : cap->second) {
            out_.insert(name);
            out_.insert(LOCAL_BINDING_PREFIX + name);
        }
    }
    // `Has(Ok(9))` builds a fresh tag in place of the call.
    for (const auto &[name, entry] : fns)
        if (entry.ctor) out_.insert(name);
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
    if (!dec_able(src, heap_returns())) return {a.pre, expr};
    std::optional<std::string> t = owned_temp(src, a.val);
    if (!t) {
        if (src.k == EK::Call || src.k == EK::MCall ||
            src.k == EK::Idx || src.k == EK::Field || src.k == EK::Quest) {
            std::string hold = new_tmp("h");
            std::string r = new_tmp("a");
            std::vector<std::string> pre = a.pre;
            pre.push_back(cat("Val ", hold, " = (", a.val, ");"));
            pre.push_back(cat("Val ", r, " = ", open, hold, close, ";"));
            pre.push_back(cat("vdrop_temp(", hold, ");"));
            return {pre, r};
        }
        return {a.pre, expr};
    }
    std::string r = new_tmp("a");
    std::vector<std::string> pre = a.pre;
    pre.push_back(cat("Val ", r, " = ", expr, ";"));
    pre.push_back(cat("vdrop_temp(", *t, ");"));
    return {pre, r};
}

V Emitter::borrow_builtin(const std::string &c_name, const ExprList &args) {
    std::vector<std::string> pre;
    std::vector<std::string> cargs;
    std::vector<std::string> drops;
    std::vector<std::string> lits;
    std::set<std::string> heap = heap_returns();
    for (const auto &arg : args) {
        V av = ex(*arg);
        for (const auto &p : av.pre) pre.push_back(p);
        if (dec_able(*arg, heap)) {
            if (auto d = owned_temp(*arg, av.val)) {
                cargs.push_back(av.val);
                drops.push_back(*d);
                continue;
            }
        }
        if (arg->k == EK::VecLit) {
            cargs.push_back(av.val);
            if (auto ct = container_tmp(av.val)) lits.push_back(*ct);
        } else if (arg->k == EK::MapLit || arg->k == EK::StructLit) {
            std::string h = new_tmp("h");
            pre.push_back(cat("Val ", h, " = (", av.val, ");"));
            cargs.push_back(h);
            drops.push_back(h);
        } else if ((arg->k == EK::Call || arg->k == EK::MCall ||
                    arg->k == EK::Idx || arg->k == EK::Field || arg->k == EK::Quest) &&
                   dec_able(*arg, heap)) {
            std::string h = new_tmp("h");
            pre.push_back(cat("Val ", h, " = (", av.val, ");"));
            cargs.push_back(h);
            drops.push_back(h);
        } else {
            cargs.push_back(av.val);
        }
    }
    std::string joined;
    for (size_t i = 0; i < cargs.size(); ++i) {
        if (i) joined += ", ";
        joined += cargs[i];
    }
    std::string call = cat(c_name, "(", joined, ")");
    if (!lits.empty()) {
        std::string t = new_tmp("v");
        pre.push_back(cat("Val ", t, " = (", call, ");"));
        for (const auto &l : lits) pre.push_back(cat("vrc_dec(vvecv(", l, "));"));
        call = t;
    }
    if (drops.empty()) return {pre, call};
    std::string r = new_tmp("a");
    pre.push_back(cat("Val ", r, " = ", call, ";"));
    for (const auto &d : drops) pre.push_back(cat("vdrop_temp(", d, ");"));
    return {pre, r};
}

V Emitter::call_result(std::vector<std::string> pre, const std::string &vec, const std::string &call) {
    std::string t = new_tmp("c");
    pre.push_back(cat("Val ", t, " = (", call, ");"));
    pre.push_back(cat("vargs_drop(", vec, ");"));
    return {pre, t};
}

V Emitter::direct_call_result(
    const ExprList &args,
    const std::function<std::string(const std::vector<std::string> &)> &call) {
    std::vector<std::string> pre, vals, held;
    const auto heap = heap_returns();
    std::vector<Names> later_moves(args.size());
    Names future;
    for (size_t i = args.size(); i > 0; i--) {
        const size_t at = i - 1;
        later_moves[at] = future;
        std::function<void(const Expr &)> scan = [&](const Expr &e) {
            if (e.k == EK::Move && !e.kids.empty() && e.kids[0]->k == EK::Var)
                future.insert(e.kids[0]->a);
            for (const auto &kid : e.kids) scan(*kid);
        };
        scan(*args[at]);
    }

    const size_t start = call_cleanup.size();
    try {
        for (size_t i = 0; i < args.size(); i++) {
            const Expr &node = *args[i];
            V value = ex(node);
            for (const auto &line : value.pre) pre.push_back(line);
            bool owned = dec_able(node, heap);
            if (!owned && moves_owned_local(node))
                owned = true;

            bool stable = false;
            if (node.k == EK::Var) {
                auto it = sym.find(node.a);
                const bool scalar = scalar_vars.count(node.a) || ub_ints.count(node.a) || ub_floats.count(node.a);
                stable = scalar || (it != sym.end() && (it->second.own == Own::Owned || it->second.own == Own::Copy) &&
                                    !global_lets.count(node.a) && !(cap && cap->second.count(node.a)));
                stable = stable && !later_moves[i].count(node.a);
            }
            const bool no_raise = node.k == EK::Num || node.k == EK::Bool || node.k == EK::None ||
                                  node.k == EK::Str || node.k == EK::RStr || node.k == EK::Var ||
                                  (node.k == EK::Move && !node.kids.empty() && node.kids[0]->k == EK::Var);
            const bool literal = node.k == EK::Num || node.k == EK::Bool || node.k == EK::None ||
                                 node.k == EK::Str || node.k == EK::RStr;
            if (value.pre.empty() && (literal || (node.k == EK::Var && stable))) {
                vals.push_back(value.val);
                continue;
            }

            std::string val = new_tmp("da");
            pre.push_back(cat("Val ", val, " = (", value.val, ");"));
            if (owned) {
                held.push_back(val);
                call_cleanup.push_back(val);
            } else if (!stable) {
                pre.push_back(cat("vrc_inc(", val, ");"));
                held.push_back(val);
                call_cleanup.push_back(val);
            }
            if (!no_raise) pre.push_back(err_check_line());
            vals.push_back(val);
        }
        std::string result = new_tmp("c");
        pre.push_back(cat("Val ", result, " = (", call(vals), ");"));
        for (const auto &value : held) pre.push_back(cat("vdrop_temp(", value, ");"));
        V out{pre, result};
        call_cleanup.resize(start);
        return out;
    } catch (...) {
        call_cleanup.resize(start);
        throw;
    }
}

bool Emitter::raw_i64_arg(const Expr &e, std::string &out) const {
    if (e.k == EK::Num && !e.is_float) {
        out = clit64(e.i);
        return true;
    }
    if (e.k == EK::Var && ub_ints.count(e.a)) {
        out = cat("ub_", cname(e.a));
        return true;
    }
    return false;
}

bool Emitter::direct_fast_call(const std::string &name, const ExprList &args, V &out) {
    if (args.size() > 4) return false;
    auto kind = fastkinds.find(name);
    auto fn = fastc.find(name);
    if (kind == fastkinds.end() || kind->second != "i64" || fn == fastc.end()) return false;
    const auto &params = fns[name].decl->params;
    if (args.size() != params.size()) return false;
    const std::set<std::string> empty_bs;
    const auto &bs = fastbool.count(name) ? fastbool[name] : empty_bs;

    std::vector<std::string> raw;
    raw.reserve(args.size());
    for (size_t i = 0; i < args.size(); i++) {
        const Expr *arg = args[i];
        if (bs.count(params[i].name)) {
            if (arg->k == EK::Bool) {
                raw.push_back(arg->flag ? "1LL" : "0LL");
            } else {
                return false;
            }
        } else {
            std::string value;
            if (!raw_i64_arg(*arg, value)) return false;
            raw.push_back(std::move(value));
        }
    }
    std::string call = cat(fn->second, "(");
    for (size_t i = 0; i < raw.size(); i++) {
        if (i) call += ", ";
        call += raw[i];
    }
    call += ")";
    bool ret_bool = false;
    auto ret = fn_ret.find(name);
    if (ret != fn_ret.end() && ret->second && *ret->second == "bool") ret_bool = true;
    std::string value = ret_bool ? cat("vbool((", call, ") != 0)") : cat("vint(", call, ")");
    std::string result = new_tmp("c");
    std::vector<std::string> pre = {cat("Val ", result, " = ", value, ";")};
    if (fails.count(name)) pre.push_back(err_check_line());
    out = {std::move(pre), result};
    return true;
}

void Emitter::need_arity(const ExprList &args, size_t n, const std::string &name) {
    if (args.size() != n) throw EmitError(name + " arity");
}

V Emitter::ex_tag(const std::string &tag, const ExprList &args) {
    std::string t = new_tmp("g");
    std::vector<std::string> pre = {cat("Vec *", t, " = vvec();")};
    CleanupGuard guard{call_cleanup, call_cleanup.size()};
    call_cleanup.push_back(cat("vvecv(", t, ")"));
    for (const Expr *a : args) {
        V v = ex(*a);
        for (auto &p : v.pre) pre.push_back(p);
        pre.push_back(cat("vec_push(", t, ", ", keep(*a, v.val), ");"));
        if (may_raise(*a)) pre.push_back(err_check_line());
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
        return call_result(pre, a.val, cat("vcall_method_ic(", o.val, ", \"", clit(meth), "\", ", a.val, ", ", next_ic(), ")"));
    }
    if (upper && parts.size() == 2) {
        auto it = fns.find(head);
        if (it != fns.end() && it->second.ctor && it->second.ctor_arity == 0) {
            const std::string &tag = it->second.ctor_tag;
            std::string base = tag == "none" ? "vnil()" : cat("vtagv(\"", clit(tag), "\", vvec())");
            V a = args_vec(args);
            return call_result(a.pre, a.val,
                               cat("vcall_method_ic(", base, ", \"", clit(meth), "\", ", a.val, ", ", next_ic(), ")"));
        }
        std::string key = head + "." + meth;
        auto kt = fns.find(key);
        if (kt != fns.end() && !kt->second.ctor) {
            const std::vector<Param> &params = kt->second.decl->params;
            if (!params.empty() && params[0].name == "self")
                throw EmitError(key + " needs instance, use value.method()");
            std::vector<const Expr *> padded = pad_defaults(params, args, key);
            if (padded.size() <= 4) {
                V raw;
                if (direct_fast_call(key, padded, raw)) return raw;
                const std::string target = direct_cfn("meth:" + key);
                return direct_call_result(padded, [target](const std::vector<std::string> &vals) {
                    std::string call = cat(target, "(NULL");
                    for (const auto &value : vals) call += cat(", ", value);
                    return call + ")";
                });
            }
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
    return call_result(pre, a.val, cat("vcall_method_ic(", o.val, ", \"", clit(meth), "\", ", a.val, ", ", next_ic(), ")"));
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
    const std::string key = "fn:" + fn_key(name);
    if (args.size() <= 4) {
        V raw;
        if (direct_fast_call(name, args, raw)) return raw;
        const std::string target = direct_cfn(key);
        return direct_call_result(args, [target](const std::vector<std::string> &vals) {
            std::string call = cat(target, "(NULL");
            for (const auto &value : vals) call += cat(", ", value);
            return call + ")";
        });
    }
    V a = args_vec(args);
    return call_result(a.pre, a.val, cat(cfn(key), "(NULL, ", a.val, ")"));
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
    if (meth == "sort_by" && args.size() == 1 && !user_meths.count(meth))
        return ex_hof(*obj, meth, args[0], nullptr);
    if (obj->k == EK::Call && obj->kids.empty() && !obj->a.empty() &&
        obj->a.find('.') == std::string::npos && obj->a[0] >= 'A' && obj->a[0] <= 'Z' &&
        !sym.count(obj->a) && !(cap && cap->second.count(obj->a))) {
        // `Type.make(x)`: an `impl` function without `self`. Only rewrite when
        // the `impl` declares the name without a receiver so unit enum variants
        // like `Red.code()` still construct the variant and dispatch on it.
        auto it = fns.find(obj->a + "." + meth);
        if (it != fns.end() && !it->second.ctor && it->second.decl) {
            // ex_dotted() emits this under the `meth:` key the body is defined
            // with, and answers a method that wants a receiver with `needs
            // instance`. Going through mk_call() instead would ask for
            // `fn:Type.name`, which is forward declared and never defined.
            // `Red.code()` does not come here: the key is `Color.code`, so the
            // receiver is still built and dispatched on its variant.
            return ex_dotted(obj->a + "." + meth, args);
        }
    }
    auto path = name_path(*obj);
    if (path && !sym.count(head_of(*path)) && !(cap && cap->second.count(head_of(*path)))) {
        // Static path: the callee is resolved by name, so no receiver storage is read.
        std::vector<ExprP> call_args;
        for (const Expr *a : args) call_args.push_back(clone(*a));
        owned.push_back(mk_call(*path + "." + meth, std::move(call_args)));
        return ex(*owned.back());
    }
    if (!cap && !user_meths.count(meth) &&
        (((meth == "map" || meth == "filter") && args.size() == 1 && fusible_fnlit(args[0], 1)) ||
         (meth == "fold" && args.size() == 2 && fusible_fnlit(args[1], 2)))) {
        return ex_fused_stream(*obj, meth, args);
    }
    if (meth == "unwrap_or" && args.size() == 1) {
        V a = ex(*obj);
        auto saved_moved = moved;
        V b = ex(*args[0]);
        moved = saved_moved;
        std::string ta = new_tmp("uo");
        std::string tr = new_tmp("ur");
        std::string st = new_tmp("us");
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("Val ", ta, " = (", a.val, ");"));
        const bool drop_a = dec_able(*obj, heap_returns());
        if (!user_meths.count(meth)) {
            pre.push_back(cat("int ", st, " = vunwrap_or_state(", ta, ");"));
            if (drop_a)
                pre.push_back(cat("if (", st, " < 0) { vdrop_temp(", ta, "); ", err_bail(), " }"));
            else
                pre.push_back(cat("if (", st, " < 0) { ", err_bail(), " }"));
            pre.push_back(cat("Val ", tr, " = vnil();"));
            pre.push_back(cat("if (", st, ") { ", tr, " = vtag_payload(", ta, ");",
                              drop_a ? cat(" vdrop_temp(", ta, ");") : "", " } else {"));
            if (drop_a) pre.push_back(cat("    vdrop_temp(", ta, ");"));
            for (auto &p : b.pre) pre.push_back(cat("    ", p));
            pre.push_back(cat("    ", tr, " = (", own_operand(*args[0], b.val), ");"));
            pre.push_back("}");
            return {pre, tr};
        }
        pre.push_back(cat("int ", st, " = vopt_res_state(", ta, ");"));
        pre.push_back(cat("Val ", tr, " = vnil();"));
        pre.push_back(cat("if (", st, " > 0) { ", tr, " = vtag_payload(", ta, ");",
                          drop_a ? cat(" vdrop_temp(", ta, ");") : "", " } else if (", st, " == 0) {"));
        if (drop_a) pre.push_back(cat("    vdrop_temp(", ta, ");"));
        for (auto &p : b.pre) pre.push_back(cat("    ", p));
        pre.push_back(cat("    ", tr, " = (", own_operand(*args[0], b.val), ");"));
        pre.push_back("} else {");
        const size_t cstart = call_cleanup.size();
        if (drop_a) call_cleanup.push_back(ta);
        V av;
        try {
            moved = saved_moved;
            av = args_vec(args);
            call_cleanup.resize(cstart);
        } catch (...) {
            call_cleanup.resize(cstart);
            throw;
        }
        for (auto &p : av.pre) pre.push_back(cat("    ", p));
        pre.push_back(cat("    ", tr, " = vcall_method_ic(", ta, ", \"unwrap_or\", ", av.val, ", ", next_ic(), ");"));
        pre.push_back(cat("    vargs_drop(", av.val, ");"));
        if (drop_a) pre.push_back(cat("    vdrop_recv(", ta, ", ", tr, ");"));
        pre.push_back("}");
        return {pre, tr};
    }
    V o = ex(*obj);
    // `"a,b".split(",")` or `a[0].push(3)` builds or fetches a receiver nothing
    // else names: release it once the call returns unless the method returns the
    // receiver itself (`SELF_RESULT`), which a failed `sort` does not.
    std::string recv;
    if (dec_able(*obj, heap_returns())) {
        recv = new_tmp("r");
        o.pre.push_back("Val " + recv + " = (" + o.val + ");");
        o.val = recv;
    }
    auto done = [&](V res) -> V {
        if (recv.empty()) return res;
        std::string t = new_tmp("c");
        res.pre.push_back("Val " + t + " = (" + res.val + ");");
        if (SELF_RESULT.count(meth))
            res.pre.push_back(cat("if (vhead_of(", t, ") != vhead_of(", recv, ")) vdrop_temp(", recv, ");"));
        else
            res.pre.push_back((PART_RESULT.count(meth) ? "vdrop_recv_part(" : "vdrop_recv(") + recv +
                              ", " + t + ");");
        res.val = t;
        return res;
    };
    const size_t recv_cstart = call_cleanup.size();
    if (!recv.empty()) call_cleanup.push_back(recv);
    auto pop_recv = [&]() { call_cleanup.resize(recv_cstart); };
    try {
        if ((meth == "map" || meth == "filter") && args.size() == 1 && !user_meths.count(meth)) {
            V r = ex_mapfilter(*obj, meth, args[0], &o);
            pop_recv();
            return done(std::move(r));
        }
        if ((meth == "any" || meth == "all" || meth == "find") && args.size() == 1 && !user_meths.count(meth)) {
            V r = ex_hof(*obj, meth, args[0], nullptr, &o);
            pop_recv();
            return done(std::move(r));
        }
        if (meth == "fold" && args.size() == 2 && !user_meths.count(meth)) {
            V r = ex_hof(*obj, meth, args[1], args[0], &o);
            pop_recv();
            return done(std::move(r));
        }
        bool direct = (VBUILTIN_DIRECT.count(meth) || (STRVEC_METH.count(meth) && MAP_METH.count(meth))) &&
                      !user_meths.count(meth);
        if (direct) {
            V a = args_vec(args);
            pop_recv();
            std::vector<std::string> pre = o.pre;
            for (auto &p : a.pre) pre.push_back(p);
            return done(call_result(pre, a.val, cat("vbuiltin(", o.val, ", \"", clit(meth), "\", ", a.val, ")")));
        }
        if (STRVEC_METH.count(meth) && !user_meths.count(meth)) {
            V r = ex_strvec_meth(o.val, o.pre, meth, args);
            pop_recv();
            return done(std::move(r));
        }
        if (MAP_METH.count(meth) && !user_meths.count(meth)) {
            V r = ex_map_meth(o.val, o.pre, meth, args);
            pop_recv();
            return done(std::move(r));
        }
        V a = args_vec(args);
        pop_recv();
        std::vector<std::string> pre = o.pre;
        for (auto &p : a.pre) pre.push_back(p);
        return done(call_result(pre, a.val, cat("vcall_method_ic(", o.val, ", \"", clit(meth), "\", ", a.val, ", ", next_ic(), ")")));
    } catch (...) {
        pop_recv();
        throw;
    }
}

V Emitter::ex_map_meth(const std::string &so, const std::vector<std::string> &po, const std::string &meth,
                       const ExprList &args) {
    auto with_po = [&](V r) -> V {
        std::vector<std::string> pre = po;
        for (auto &p : r.pre) pre.push_back(p);
        return {pre, r.val};
    };
    static const std::map<std::string, const char *> MAP_0 = {
        {"keys", "vmap_keys"}, {"items", "vmap_items"},
        {"vals", "vmap_vals"}, {"copy", "vcopy"},
    };
    if (auto m0 = MAP_0.find(meth); m0 != MAP_0.end()) {
        need_arity(args, 0, meth);
        return {po, cat(m0->second, "(", so, ")")};
    }
    if (meth == "get_or") {
        need_arity(args, 2, meth);
        std::string klit;
        if (plain_cstr_key(*args[0], klit)) {
            std::string fic = next_fic();
            return with_po(direct_call_result({args[1]}, [&](const auto &vals) {
                return cat("vmap_get_or_ic(", so, ", \"", clit(klit), "\", ", vals[0], ", ", fic, ")");
            }));
        }
        return with_po(direct_call_result(args, [&](const auto &vals) {
            return cat("vmap_get_or(", so, ", ", vals[0], ", ", vals[1], ")");
        }));
    }
    if (meth == "get" || meth == "has" || meth == "del") {
        need_arity(args, 1, meth);
        std::string klit;
        if ((meth == "get" || meth == "has") && plain_cstr_key(*args[0], klit)) {
            std::string fn = meth == "get" ? "vmap_get_ic" : "vmap_has_ic";
            return {po, cat(fn, "(", so, ", \"", clit(klit), "\", ", next_fic(), ")")};
        }
        std::string fn = meth == "get" ? "vmap_getv" : meth == "has" ? "vmap_has" : "vmap_del";
        return with_po(direct_call_result(args, [&](const auto &vals) {
            return cat(fn, "(", so, ", ", vals[0], ")");
        }));
    }
    need_arity(args, 2, meth);
    V a = ex(*args[0]);
    std::vector<std::string> pre = po;
    for (auto &p : a.pre) pre.push_back(p);
    std::string kv = new_tmp("mk");
    pre.push_back(cat("Val ", kv, " = (", keep(*args[0], a.val), ");"));
    const size_t cstart = call_cleanup.size();
    call_cleanup.push_back(kv);
    V b;
    try {
        b = ex(*args[1]);
        call_cleanup.resize(cstart);
    } catch (...) {
        call_cleanup.resize(cstart);
        throw;
    }
    for (auto &p : b.pre) pre.push_back(p);
    return {pre, cat("vmap_setv(", so, ", ", kv, ", ", keep(*args[1], b.val), ")")};
}

V Emitter::ex_strvec_meth(const std::string &so, const std::vector<std::string> &po, const std::string &meth,
                          const ExprList &args) {
    auto with_po = [&](V r) -> V {
        std::vector<std::string> pre = po;
        for (auto &p : r.pre) pre.push_back(p);
        return {pre, r.val};
    };
    auto simple = SV_SIMPLE.find(meth);
    if (simple != SV_SIMPLE.end()) {
        const std::string &fn = simple->second.first;
        need_arity(args, static_cast<size_t>(simple->second.second), meth);
        if (args.empty()) return {po, cat(fn, "(", so, ")")};
        if (meth == "ins") {
            V a0 = ex(*args[0]);
            std::vector<std::string> pre = po;
            for (auto &p : a0.pre) pre.push_back(p);
            const bool drop0 = dec_able(*args[0], heap_returns());
            std::string idxv = a0.val;
            const size_t cstart = call_cleanup.size();
            if (drop0) {
                idxv = new_tmp("mi");
                pre.push_back(cat("Val ", idxv, " = (", a0.val, ");"));
                call_cleanup.push_back(idxv);
            }
            V a1;
            try {
                a1 = ex(*args[1]);
                call_cleanup.resize(cstart);
            } catch (...) {
                call_cleanup.resize(cstart);
                throw;
            }
            for (auto &p : a1.pre) pre.push_back(p);
            std::string call = cat(fn, "(", so, ", ", idxv, ", ", keep(*args[1], a1.val), ")");
            if (!drop0) return {pre, call};
            std::string rv = new_tmp("c");
            pre.push_back(cat("Val ", rv, " = (", call, ");"));
            pre.push_back(cat("vdrop_temp(", idxv, ");"));
            return {pre, rv};
        }
        return with_po(direct_call_result(args, [&](const auto &vals) {
            std::string joined;
            for (const auto &c : vals) joined += ", " + c;
            return cat(fn, "(", so, joined, ")");
        }));
    }
    if (meth == "split") {
        if (args.size() > 1) throw EmitError("split arity");
        if (args.empty()) return {po, cat("vsplit_ws(", so, ")")};
        return with_po(direct_call_result(args, [&](const auto &vals) {
            return cat("vsplit(", so, ", ", vals[0], ")");
        }));
    }
    if (meth == "lower" || meth == "upper") {
        need_arity(args, 0, meth);
        return {po, cat("vcase(", so, ", ", meth == "upper" ? 1 : 0, ")")};
    }
    if (meth == "starts_with" || meth == "ends_with" || meth == "contains") {
        need_arity(args, 1, meth);
        const std::string flags = meth == "starts_with" ? "1, 0" : meth == "ends_with" ? "0, 1" : "0, 0";
        return with_po(direct_call_result(args, [&](const auto &vals) {
            return cat("vbool(vaffix(", so, ", ", vals[0], ", ", flags, "))");
        }));
    }
    if (meth == "push") {
        need_arity(args, 1, meth);
        V a = ex(*args[0]);
        std::vector<std::string> pre = po;
        for (auto &p : a.pre) pre.push_back(p);
        return {pre, cat("vpush(", so, ", ", keep(*args[0], a.val), ")")};
    }
    throw EmitError("method " + meth);
}

V Emitter::ex_hof(const Expr &obj, const std::string &meth, const Expr *arg, const Expr *first,
                  const V *recv) {
    V o = recv ? *recv : ex(obj);
    const FnDecl *entry = nullptr;
    if (arg->k == EK::Var) {
        auto it = fns.find(arg->a);
        if (it != fns.end() && !it->second.ctor) entry = it->second.decl;
    }
    std::vector<std::string> pre = o.pre;
    const size_t hof_cstart = call_cleanup.size();
    if (!recv && dec_able(obj, heap_returns())) {
        std::string rtmp = new_tmp("r");
        pre.push_back(cat("Val ", rtmp, " = (", o.val, ");"));
        o.val = rtmp;
        call_cleanup.push_back(rtmp);
    }
    std::string init_v;
    try {
        if (first) {
            V f = ex(*first);
            for (auto &p : f.pre) pre.push_back(p);
            init_v = new_tmp("c");
            pre.push_back(cat("Val ", init_v, " = (", keep(*first, f.val), ");"));
            call_cleanup.push_back(init_v);
        }
        std::string fnv;
        if (entry) {
            int code = fn_code("fn:" + arg->a);
            std::string t = new_tmp("c");
            pre.push_back(cat("Val ", t, " = vclosure0(", code, ");"));
            fnv = t;
        } else {
            V a = ex(*arg);
            for (auto &p : a.pre) pre.push_back(p);
            fnv = keep(*arg, a.val);
        }
        call_cleanup.resize(hof_cstart);
        if (first) return {pre, cat("vfold(", o.val, ", ", init_v, ", ", fnv, ")")};
        if (meth == "any") return {pre, cat("vpred(", o.val, ", ", fnv, ", \"any\")")};
        if (meth == "all") return {pre, cat("vpred(", o.val, ", ", fnv, ", \"all\")")};
        if (meth == "find") return {pre, cat("vpred(", o.val, ", ", fnv, ", \"find\")")};
        return {pre, cat("vsort_by(", keep(obj, o.val), ", ", fnv, ")")};
    } catch (...) {
        call_cleanup.resize(hof_cstart);
        throw;
    }
}

V Emitter::ex_mapfilter(const Expr &obj, const std::string &meth, const Expr *arg,
                        const V *recv) {
    V o = recv ? *recv : ex(obj);
    const FnDecl *entry = nullptr;
    if (arg->k == EK::Var) {
        auto it = fns.find(arg->a);
        if (it != fns.end() && !it->second.ctor) entry = it->second.decl;
    }
    if (entry) {
        int code = fn_code("fn:" + arg->a);
        std::string t = new_tmp("c");
        std::vector<std::string> pre = o.pre;
        pre.push_back(cat("Val ", t, " = vclosure0(", code, ");"));
        return {pre, cat("vmap_with(", o.val, ", ", t, ", \"", clit(meth), "\")")};
    }
    V a = ex(*arg);
    std::vector<std::string> pre = o.pre;
    for (auto &p : a.pre) pre.push_back(p);
    // vmap_with releases the closure it is given, so a binding the caller still
    // owns gets one more reference and a temporary is handed over as it is.
    return {pre, cat("vmap_with(", o.val, ", ", keep(*arg, a.val), ", \"", clit(meth), "\")")};
}

bool Emitter::fusible_expr(const Expr &e, const std::set<std::string> &pset) const {
    switch (e.k) {
    case EK::Num:
    case EK::Bool:
    case EK::RStr:
    case EK::None:
        return true;
    case EK::Str:
        for (const auto &[ck, cv] : split_interp(e.a)) {
            if (ck == "var" && !pset.count(cv) && (!sym.count(cv) || global_lets.count(cv)))
                return false;
        }
        return true;
    case EK::Var:
        return pset.count(e.a) > 0 || (sym.count(e.a) > 0 && !global_lets.count(e.a));
    case EK::Un:
        return (e.a == "-" || e.a == "!") && !e.kids.empty() && fusible_expr(*e.kids[0], pset);
    case EK::Bin:
        return (e.a == "+" || e.a == "-" || e.a == "*" || e.a == "/" || e.a == "%" ||
                e.a == "==" || e.a == "!=" || e.a == "<" || e.a == ">" ||
                e.a == "<=" || e.a == ">=" || e.a == "&&" || e.a == "||") &&
               e.kids.size() == 2 && fusible_expr(*e.kids[0], pset) && fusible_expr(*e.kids[1], pset);
    case EK::Idx:
        return e.kids.size() == 2 && fusible_expr(*e.kids[0], pset) && fusible_expr(*e.kids[1], pset);
    case EK::Field:
        return !e.kids.empty() && fusible_expr(*e.kids[0], pset);
    case EK::Call: {
        const std::string &cn = e.a;
        auto it = fns.find(cn);
        bool ok_fn = (cn == "str" || cn == "len" || cn == "int" || cn == "float" || cn == "abs") ||
                     (it != fns.end() && !it->second.ctor && it->second.decl != nullptr &&
                      !pset.count(cn) && !sym.count(cn));
        if (!ok_fn) return false;
        for (const auto &a : e.kids)
            if (!fusible_expr(*a, pset)) return false;
        return true;
    }
    default:
        return false;
    }
}

bool Emitter::fusible_fnlit(const Expr *fn_node, size_t arity) const {
    if (!fn_node || fn_node->k != EK::Closure || !fn_node->fn || fn_node->fn->name.has_value())
        return false;
    const auto &params = fn_node->fn->params;
    const auto &body = fn_node->fn->body;
    if (params.size() != arity || body.size() != 1 || !body[0]) return false;
    std::set<std::string> pset;
    for (const auto &p : params) {
        if (p.mode != Mode::Ref || p.def) return false;
        pset.insert(p.name);
    }
    if (body[0]->k != SK::Return || !body[0]->e1) return false;
    return fusible_expr(*body[0]->e1, pset);
}

V Emitter::ex_fused_stream(const Expr &obj, const std::string &meth, const ExprList &args) {
    const Expr *fold_init = nullptr;
    const Expr *fold_fn = nullptr;
    const Expr *cur_obj = &obj;
    std::vector<std::pair<std::string, const Expr *>> stages;
    if (meth == "fold") {
        fold_init = args[0];
        fold_fn = args[1];
    } else {
        stages.push_back({meth, args[0]});
    }
    while (cur_obj->k == EK::MCall && !cur_obj->kids.empty()) {
        const Expr *sub_obj = cur_obj->kids[0].get();
        const std::string &sub_meth = cur_obj->b;
        auto spath = name_path(*sub_obj);
        if (spath && !sym.count(head_of(*spath))) break;
        if ((sub_meth == "map" || sub_meth == "filter") && !user_meths.count(sub_meth) &&
            cur_obj->kids.size() == 2 && fusible_fnlit(cur_obj->kids[1].get(), 1)) {
            stages.push_back({sub_meth, cur_obj->kids[1].get()});
            cur_obj = sub_obj;
        } else {
            break;
        }
    }
    std::reverse(stages.begin(), stages.end());
    const std::string first_meth = stages.empty() ? "fold" : stages[0].first;
    V o = ex(*cur_obj);
    std::vector<std::string> po = o.pre;
    const auto hr = heap_returns();
    const bool own_base = dec_able(*cur_obj, hr);
    std::string base_recv = new_tmp("r");
    po.push_back(cat("Val ", base_recv, " = ",
                     own_base ? cat("(", o.val, ")") : cat("vrc_inc(", o.val, ")"), ";"));
    std::string so = base_recv;
    std::string vec_var, out_var;
    if (fold_fn) {
        const size_t fs_cstart = call_cleanup.size();
        call_cleanup.push_back(base_recv);
        V iv;
        try {
            iv = ex(*fold_init);
        } catch (...) {
            call_cleanup.resize(fs_cstart);
            throw;
        }
        call_cleanup.resize(fs_cstart);
        for (const auto &l : iv.pre) po.push_back(l);
        out_var = new_tmp("fa");
        po.push_back(cat("Val ", out_var, " = (", keep(*fold_init, iv.val), ");"));
    }
    std::string w_var = new_tmp("fw");
    po.push_back(cat("Vec *", w_var, " = vvec_of_m(", so, ", \"", clit(first_meth), "\");"));
    std::vector<std::string> err_drops{base_recv};
    if (fold_fn) err_drops.push_back(out_var);
    po.push_back(err_check_line(err_drops));
    if (!fold_fn) {
        vec_var = new_tmp("fo");
        out_var = new_tmp("fv");
        po.push_back(cat("Vec *", vec_var, " = vvec();"));
        po.push_back(cat("Val ", out_var, " = vvecv(", vec_var, ");"));
    }
    std::string idx_var = new_tmp("fi");
    std::string cur_var = new_tmp("fx");
    std::string err_lbl = "__ferr" + cur_var;
    po.push_back(cat("Val ", cur_var, " = vnil();"));

    std::vector<std::string> loop_lines;
    loop_lines.push_back(cat("for (int64_t ", idx_var, " = 0; ", idx_var, " < ", w_var, "->len; ", idx_var, "++) {"));
    loop_lines.push_back(cat("  ", cur_var, " = vrc_inc(", w_var, "->items[", idx_var, "]);"));

    auto saved_goto = err_goto;
    size_t saved_scope_depth = err_scope_depth;
    size_t saved_iter_depth = err_iter_depth;
    err_goto = err_lbl;
    err_scope_depth = scopes.size();
    err_iter_depth = iter_cleanups.size();

    for (const auto &[smeth, sfn] : stages) {
        const std::string &pname = sfn->fn->params[0].name;
        const Expr &ret_e = *sfn->fn->body[0]->e1;
        bool had_sym = sym.count(pname) > 0;
        Sym old_sym = had_sym ? sym[pname] : Sym{};
        bool had_ub = ub_ints.erase(pname) > 0;
        bool had_ubf = ub_floats.erase(pname) > 0;
        bool had_vs = var_structs.count(pname) > 0;
        std::string old_vs = had_vs ? var_structs[pname] : std::string{};
        var_structs.erase(pname);
        sym[pname] = Sym{cur_var, Own::Copy, "", ""};
        V sv = ex(ret_e);
        if (had_sym) sym[pname] = old_sym;
        else sym.erase(pname);
        if (had_ub) ub_ints.insert(pname);
        if (had_ubf) ub_floats.insert(pname);
        if (had_vs) var_structs[pname] = old_vs;
        else var_structs.erase(pname);
        for (const auto &l : sv.pre) loop_lines.push_back("  " + l);
        if (smeth == "filter") {
            std::string cv = new_tmp("fc");
            std::string okv = new_tmp("fk");
            loop_lines.push_back(cat("  Val ", cv, " = (", sv.val, ");"));
            loop_lines.push_back(cat("  int ", okv, " = vis_true(", cv, ");"));
            if (dec_able(ret_e, hr)) loop_lines.push_back(cat("  vdrop_temp(", cv, ");"));
            loop_lines.push_back(cat("  if (!", okv, ") { vrc_dec(", cur_var, "); ", cur_var, " = vnil(); continue; }"));
        } else {
            std::string nv = new_tmp("fm");
            loop_lines.push_back(cat("  Val ", nv, " = (", keep(ret_e, sv.val), ");"));
            loop_lines.push_back(cat("  vrc_dec(", cur_var, ");"));
            loop_lines.push_back(cat("  ", cur_var, " = ", nv, ";"));
        }
    }
    if (fold_fn) {
        const std::string &acc_p = fold_fn->fn->params[0].name;
        const std::string &x_p = fold_fn->fn->params[1].name;
        const Expr &fold_ret = *fold_fn->fn->body[0]->e1;
        struct Saved { bool hs; Sym osym; bool hub; bool hubf; bool hvs; std::string ovs; };
        std::map<std::string, Saved> saved_syms;
        for (const auto &p : {acc_p, x_p}) {
            if (!saved_syms.count(p))
                saved_syms[p] = Saved{sym.count(p) > 0, sym.count(p) ? sym[p] : Sym{},
                                      ub_ints.count(p) > 0, ub_floats.count(p) > 0,
                                      var_structs.count(p) > 0,
                                      var_structs.count(p) ? var_structs[p] : std::string{}};
        }
        ub_ints.erase(acc_p);
        ub_ints.erase(x_p);
        ub_floats.erase(acc_p);
        ub_floats.erase(x_p);
        var_structs.erase(acc_p);
        var_structs.erase(x_p);
        sym[acc_p] = Sym{out_var, Own::Copy, "", ""};
        sym[x_p] = Sym{cur_var, Own::Copy, "", ""};
        V fv = ex(fold_ret);
        for (const auto &[p, st] : saved_syms) {
            if (st.hs) sym[p] = st.osym;
            else sym.erase(p);
            if (st.hub) ub_ints.insert(p);
            if (st.hubf) ub_floats.insert(p);
            if (st.hvs) var_structs[p] = st.ovs;
            else var_structs.erase(p);
        }
        for (const auto &l : fv.pre) loop_lines.push_back("  " + l);
        std::string nfv = new_tmp("fn");
        loop_lines.push_back(cat("  Val ", nfv, " = (", keep(fold_ret, fv.val), ");"));
        loop_lines.push_back(cat("  vrc_dec(", cur_var, "); ", cur_var, " = vnil();"));
        loop_lines.push_back(cat("  vrc_dec(", out_var, ");"));
        loop_lines.push_back(cat("  ", out_var, " = ", nfv, ";"));
    } else {
        loop_lines.push_back(cat("  vec_push(", vec_var, ", ", cur_var, "); ", cur_var, " = vnil();"));
    }
    loop_lines.push_back("}");
    err_goto = saved_goto;
    err_scope_depth = saved_scope_depth;
    err_iter_depth = saved_iter_depth;

    bool used_err = false;
    for (const auto &l : loop_lines) {
        if (l.find(err_lbl) != std::string::npos) used_err = true;
        po.push_back(l);
    }
    if (used_err) {
        po.push_back(cat("if (0) { ", err_lbl, ": vdrop_temp(", cur_var, "); vrc_dec(", out_var,
                         "); vdrop_temp(", base_recv, "); ", err_bail(), " }"));
    }
    po.push_back(own_base ? cat("vdrop_recv(", base_recv, ", ", out_var, ");")
                          : cat("vdrop_temp(", base_recv, ");"));
    return {po, out_var};
}

std::set<std::string> Emitter::closure_free_vars(const FnDecl &decl) const {
    std::set<std::string> free;
    std::set<std::string> bound0;
    for (const auto &p : decl.params) {
        if (p.def) closure_walk_expr(p.def.get(), bound0, free);
        bound0.insert(p.name);
    }
    closure_walk_stmts(view(decl.body), bound0, free);
    return free;
}

void Emitter::closure_walk_expr(const Expr *e, const std::set<std::string> &bound,
                                std::set<std::string> &free) const {
    if (!e) return;
    switch (e->k) {
    case EK::Var: {
        std::string root = e->a.substr(0, e->a.find('.'));
        if (!bound.count(root)) free.insert(root);
        return;
    }
    case EK::Str:
        for (const auto &[ck, cv] : split_interp(e->a)) {
            if (ck == "var") {
                std::string root = cv.substr(0, cv.find('.'));
                if (!bound.count(root)) free.insert(root);
            }
        }
        return;
    case EK::Call: {
        std::string root = e->a.substr(0, e->a.find('.'));
        if (!bound.count(root)) free.insert(root);
        for (const auto &a : e->kids) closure_walk_expr(a.get(), bound, free);
        return;
    }
    case EK::Closure: {
        auto sub = closure_free_vars(*e->fn);
        if (e->fn->name) sub.erase(*e->fn->name);
        for (const auto &r : sub)
            if (!bound.count(r)) free.insert(r);
        return;
    }
    case EK::ParFor: {
        closure_walk_expr(e->kids[0].get(), bound, free);
        closure_walk_expr(e->kids[1].get(), bound, free);
        if (!e->b.empty() && !bound.count(e->b)) free.insert(e->b);
        auto sub = closure_free_vars(*e->fn);
        for (const auto &r : sub)
            if (!bound.count(r)) free.insert(r);
        return;
    }
    default:
        for (const auto &sub : e->kids) closure_walk_expr(sub.get(), bound, free);
        return;
    }
}

void Emitter::closure_walk_stmts(const StmtList &stmts, const std::set<std::string> &bound,
                                 std::set<std::string> &free) const {
    std::set<std::string> cur = bound;
    for (const Stmt *sp : stmts) {
        if (!sp) continue;
        const Stmt &s = *sp;
        switch (s.k) {
        case SK::Let:
        case SK::LetTmp:
            closure_walk_expr(s.e1.get(), cur, free);
            if (s.a.find('.') != std::string::npos) {
                std::string root = s.a.substr(0, s.a.find('.'));
                if (!cur.count(root)) free.insert(root);
            } else {
                cur.insert(s.a);
            }
            break;
        case SK::Set: {
            closure_walk_expr(s.e1.get(), cur, free);
            std::string root = s.a.substr(0, s.a.find('.'));
            if (!cur.count(root)) free.insert(root);
            break;
        }
        case SK::SetIdx:
            closure_walk_expr(s.e1.get(), cur, free);
            closure_walk_expr(s.e2.get(), cur, free);
            closure_walk_expr(s.e3.get(), cur, free);
            break;
        case SK::SetField:
            closure_walk_expr(s.e1.get(), cur, free);
            closure_walk_expr(s.e2.get(), cur, free);
            break;
        case SK::Return:
        case SK::Expr:
        case SK::Defer:
            closure_walk_expr(s.e1.get(), cur, free);
            break;
        case SK::If:
            closure_walk_expr(s.e1.get(), cur, free);
            closure_walk_stmts(view(s.body), cur, free);
            if (const Stmt *els = real_else(s)) {
                StmtList eb = els->k == SK::Block ? view(els->body) : StmtList{els};
                closure_walk_stmts(eb, cur, free);
            }
            break;
        case SK::While:
            closure_walk_expr(s.e1.get(), cur, free);
            closure_walk_stmts(view(s.body), cur, free);
            break;
        case SK::ForRange: {
            closure_walk_expr(s.e1.get(), cur, free);
            closure_walk_expr(s.e2.get(), cur, free);
            auto sub = cur;
            sub.insert(s.a);
            closure_walk_stmts(view(s.body), sub, free);
            break;
        }
        case SK::ForIter: {
            closure_walk_expr(s.e1.get(), cur, free);
            auto sub = cur;
            sub.insert(s.a);
            closure_walk_stmts(view(s.body), sub, free);
            break;
        }
        case SK::Try: {
            closure_walk_stmts(view(s.body), cur, free);
            auto cbound = cur;
            if (s.opt1) cbound.insert(*s.opt1);
            closure_walk_stmts(view(s.body2), cbound, free);
            break;
        }
        case SK::Unsafe:
        case SK::Block:
        case SK::Scope:
        case SK::Spawn:
        case SK::Pub:
            closure_walk_stmts(view(s.body), cur, free);
            break;
        case SK::Fn:
            if (!s.fns.empty()) {
                const FnDecl &f = s.fns[0];
                if (f.name) cur.insert(*f.name);
                auto fbound = cur;
                for (const auto &p : f.params) {
                    if (p.def) closure_walk_expr(p.def.get(), fbound, free);
                    fbound.insert(p.name);
                }
                closure_walk_stmts(view(f.body), fbound, free);
            }
            break;
        default:
            break;
        }
    }
}

V Emitter::ex_parfor(const Expr &e) {
    V it = ex(*e.kids[0]);
    V unit = ex(*e.kids[1]);
    const FnDecl &decl = *e.fn;
    const int code = fn_code(cat("clo:", clos.size()));
    auto free = closure_free_vars(decl);
    std::set<std::string> avail;
    for (const auto &[n, s] : sym)
        if (assigned.count(n)) avail.insert(n);
    if (cap)
        for (const auto &n : cap->second) avail.insert(n);
    avail.erase(e.a);
    std::set<std::string> capnames;
    for (const auto &n : avail)
        if (free.count(n)) capnames.insert(n);
    clos.push_back({&decl.params, view(decl.body), capnames, is_result_ret_ann(decl.ret),
                    lookup_fn_file("", &decl)});
    std::string m = new_tmp("m");
    const int body_cb = tmp;
    std::string opc = new_tmp("c");
    std::vector<std::string> pre = it.pre;
    pre.insert(pre.end(), unit.pre.begin(), unit.pre.end());
    if (capnames.empty()) {
        pre.push_back(cat("Val ", m, " = vclosure0(", code, ");"));
    } else {
        pre.push_back(cat("Map *_cm", body_cb, " = vclosure_env_new(", capnames.size(), ");"));
        int64_t capture_slot = 0;
        for (const auto &var : capnames) {
            for (auto &l : capture(cat("_cm", body_cb), var, capture_slot)) pre.push_back(l);
            capture_slot++;
        }
        pre.push_back(cat("Vec *_cb", body_cb, " = vvec();"));
        pre.push_back(cat("vec_push(_cb", body_cb, ", vint(", code, "));"));
        pre.push_back(cat("{ Val _cv = vstruct(_cm", body_cb, "); vec_push(_cb", body_cb,
                          ", _cv); }"));
        pre.push_back(cat("Val ", m, " = vtagv(\"closure\", _cb", body_cb, ");"));
    }
    const int opcode = fn_code("fn:" + e.b);
    pre.push_back(cat("Val ", opc, " = vclosure0(", opcode, ");"));
    std::string t = new_tmp("pf");
    pre.push_back(cat("Val ", t, " = vparfor(", it.val, ", ", m, ", ", unit.val,
                      ", ", opc, ");"));
    return {pre, t};
}

V Emitter::ex_closure(const Expr &e) {
    const FnDecl &decl = *e.fn;
    int code = fn_code(cat("clo:", clos.size()));
    auto free = closure_free_vars(decl);
    std::set<std::string> avail;
    for (const auto &[n, s] : sym)
        if (assigned.count(n)) avail.insert(n);
    if (cap)
        for (const auto &n : cap->second) avail.insert(n);
    bool named = decl.name.has_value();
    bool self_used = named && free.count(*decl.name) > 0;
    std::set<std::string> others;
    for (const auto &n : avail)
        if (free.count(n) && (!named || n != *decl.name)) others.insert(n);
    std::set<std::string> capnames = others;
    if (self_used) capnames.insert(*decl.name);
    clos.push_back({&decl.params, view(decl.body), capnames, is_result_ret_ann(decl.ret),
                    lookup_fn_file("", &decl)});
    std::string t = new_tmp("m");
    if (capnames.empty()) {
        return {{cat("Val ", t, " = vclosure0(", code, ");")}, t};
    }
    int n = tmp;
    std::vector<std::string> pre = {cat("Map *_cm", n, " = vclosure_env_new(", capnames.size(), ");")};
    int64_t capture_slot = 0, self_slot = -1;
    for (const auto &var : capnames) {
        if (others.count(var))
            for (auto &l : capture(cat("_cm", n), var, capture_slot)) pre.push_back(l);
        if (self_used && var == *decl.name) self_slot = capture_slot;
        capture_slot++;
    }
    pre.push_back(cat("Vec *_cb", n, " = vvec();"));
    pre.push_back(cat("vec_push(_cb", n, ", vint(", code, "));"));
    pre.push_back(cat("{ Val _cv = vstruct(_cm", n, "); vec_push(_cb", n, ", _cv); }"));
    pre.push_back(cat("Val ", t, " = vtagv(\"closure\", _cb", n, ");"));
    if (self_used) pre.push_back(cat("vclosure_env_set_weak_capture(", t, ", ", self_slot, ");"));
    return {pre, t};
}


std::string Emitter::var_ref(const std::string &name) {
    if (ub_ints.count(name)) return cat("vint(ub_", cname(name), ")");
    if (ub_floats.count(name)) return cat("vfloat(ubf_", cname(name), ")");
    auto it = sym.find(name);
    if (it != sym.end()) return it->second.cn;
    if (cap && cap->second.count(name)) return cat("(*vclosure_env_at(", cap->first, ", ", cap_slots[name], "))");
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
    std::string cn = slot_cn(name);
    auto it = sym.find(name);
    if (it != sym.end() && it->second.cn == cn && it->second.own == Own::Owned)
        own = Own::Owned;
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

bool Emitter::moves_owned_local(const Expr &e) const {
    if (e.k != EK::Move || e.kids.empty() || e.kids[0]->k != EK::Var) return false;
    auto it = sym.find(e.kids[0]->a);
    return it != sym.end() && it->second.own == Own::Owned;
}

std::string Emitter::keep(const Expr &e, const std::string &val) {
    if (dec_able(e, heap_returns())) return val;
    if (moves_owned_local(e)) return val;
    return cat("vrc_inc(", val, ")");
}

std::string Emitter::own_operand(const Expr &e, const std::string &val) {
    if (is_imm_expr(e)) return val;
    if (e.k == EK::Call && dec_able(e, heap_returns())) return val;
    return keep(e, val);
}

// A condition only asks whether a value is true. Drop any temporary heap value
// built to test the condition (`&&`/`||`/`or`, `xs[0]`, `p.s`, `f()`).
std::string Emitter::truth(const Expr &e, const std::string &val, std::vector<std::string> &lines) {
    if (is_imm_expr(e) || (!yields_operand(e) && !dec_able(e, heap_returns())))
        return cat("vis_true(", val, ")");
    const std::string tv = new_tmp("tv");
    const std::string ok = new_tmp("ok");
    lines.push_back(cat("Val ", tv, " = (", val, ");"));
    lines.push_back(cat("int ", ok, " = vis_true(", tv, ");"));
    lines.push_back(cat("vdrop_temp(", tv, ");"));
    return ok;
}

std::vector<std::string> Emitter::capture(const std::string &cm, const std::string &var, int64_t slot) {
    std::string ref = var_ref(var);
    // The frame and closure hold independent references to captured values.
    captured.insert(var);
    return {cat("vrc_inc(", ref, ");"),
            cat("*vclosure_env_at(", cm, ", ", slot, ") = ", ref, ";")};
}

Own Emitter::own_of(const Expr &e) {
    switch (e.k) {
    case EK::Num:
    case EK::Bool:
    case EK::None:
    case EK::Str:
    case EK::RStr:
        return Own::Copy;   // scalars and string literals are immortal
    case EK::Var:
        // The binding/set path increments aliases before storing them. That
        // increment is owned by the destination even when the source is a
        // parameter, capture, or another local; treating it as a borrow leaked
        // one reference at every scope exit.
        return Own::Owned;
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
        return Own::Owned;    // `s.f` hands back its own reference
    case EK::Idx:
        return Own::Owned;    // `a[i]` hands back its own reference (vat_owned)
    case EK::Call:
    case EK::MCall:
        if (e.k == EK::Call && (e.a == "Ok" || e.a == "Err" || e.a == "Some")) return Own::Owned;
        if (e.k == EK::MCall && SELF_RESULT.count(e.b)) return Own::Owned;
        return dec_able(e, heap_returns()) ? Own::Owned : Own::Copy;
    }
    return Own::Owned;
}

// True when `e` reads a part (element, field) of the binding `root` rather than
// the binding itself.
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

// Release owned locals at function exit. `at_return` marks an explicit `return`
// path and `skip` names the binding transferred to the caller on that path.
void Emitter::emit_fn_drops(const std::string &ind, const std::string &skip,
                            bool at_return, const Names &keep) {
    if (!at_return && skip.empty() && keep.empty() && frame_arr_n > 0) {
        std::vector<int> fidxs;
        std::vector<std::string> extras;
        for (const auto &name : sorted_sym_names()) {
            const Sym &sy = sym[name];
            if (!move_rt || sy.own != Own::Owned) continue;
            if (shared.count(name)) continue;
            auto fit = frame_idx.find(name);
            if (fit != frame_idx.end()) fidxs.push_back(fit->second);
            else extras.push_back(sy.cn);
        }
        if (fidxs.size() == frame_arr_n) {
            ln(ind, cat("for (size_t __fi = 0; __fi < ", frame_arr_n,
                        "; ++__fi) { vrelease(__fv[__fi]); __fv[__fi] = vnil(); }"));
        } else if (!fidxs.empty()) {
            std::string list;
            for (size_t i = 0; i < fidxs.size(); i++) {
                if (i) list += ", ";
                list += std::to_string(fidxs[i]);
            }
            std::string arr = new_tmp("fd");
            ln(ind, cat("{ static const int ", arr, "[] = {", list, "}; for (size_t __fi = 0; __fi < ",
                        fidxs.size(), "; ++__fi) { int __fk = ", arr,
                        "[__fi]; vrelease(__fv[__fk]); __fv[__fk] = vnil(); } }"));
        }
        for (const auto &cn : extras) {
            ln(ind, cat("vrelease(", cn, ");"));
            ln(ind, cat(cn, " = vnil();"));
        }
        return;
    }
    for (const auto &name : sorted_sym_names()) {
        const Sym &sy = sym[name];
        if (!move_rt || sy.own != Own::Owned) continue;
        if ((at_return && moved.count(name)) || name == skip || keep.count(name)) continue;
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
    for (const auto &ccn : catch_cns) {
        ln(ind, cat("vrelease(", ccn, ");"));
        ln(ind, cat(ccn, " = vnil();"));
    }
    catch_cns.clear();
}

std::vector<std::string> Emitter::sorted_sym_names() const {
    std::vector<std::string> out;
    for (const auto &[name, _s] : sym) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

void Emitter::ub_assign(const std::string &name, const Expr &rhs, const std::string &ind) {
    std::string ucn = cat("ub_", cname(name));
    if (rhs.k == EK::Call && rhs.a == "len" && rhs.kids.size() == 1) {
        V lv = read_only(*rhs.kids[0], "vlen");
        for (auto &p : lv.pre) ln(ind, p);
        std::string t = new_tmp("ubl");
        ln(ind, cat("Val ", t, " = (", lv.val, ");"));
        emit_err_check(ind);
        ln(ind, cat(ucn, " = (", t, ").as.ival;"));
    } else if (rhs.k == EK::Call && (rhs.a == "time_ms" || rhs.a == "mono_ns") && rhs.kids.empty()) {
        ln(ind, cat(ucn, " = (v", rhs.a, "()).as.ival;"));
    } else if (ub_may_raise(rhs)) {
        std::string t = new_tmp("ub");
        ln(ind, cat("int64_t ", t, " = ", ub_ex(rhs), ";"));
        emit_err_check(ind);
        ln(ind, cat(ucn, " = ", t, ";"));
    } else {
        std::string ex = ub_ex(rhs);
        if (ex != ucn) ln(ind, cat(ucn, " = ", ex, ";"));
    }
    assigned.insert(name);
}

void Emitter::ubf_assign(const std::string &name, const Expr &rhs, const std::string &ind) {
    std::string ucn = cat("ubf_", cname(name));
    if (ubf_may_raise(rhs)) {
        std::string t = new_tmp("ubf");
        ln(ind, cat("double ", t, " = ", ubf_ex(rhs), ";"));
        emit_err_check(ind);
        ln(ind, cat(ucn, " = ", t, ";"));
    } else {
        std::string ex = ubf_ex(rhs);
        if (ex != ucn) ln(ind, cat(ucn, " = ", ex, ";"));
    }
    assigned.insert(name);
}

std::set<std::string> try_let_names(const StmtList &body);

std::set<std::string> Emitter::compute_ub_vars(const StmtList &body, const std::set<std::string> &vars_,
                                               const std::set<std::string> &param_names, bool is_float) {
    std::set<std::string> cand;
    for (const auto &v : vars_) {
        if (!param_names.count(v) && !global_lets.count(v) && !v.empty() && v[0] != '@')
            cand.insert(v);
    }
    if (cand.empty()) return {};

    auto expr_refs = [&](const Expr *e, Names &r) {
        refs_in_expr(e, r);
        if (!e) return;
        std::vector<const Expr *> st{e};
        while (!st.empty()) {
            const Expr *cur = st.back();
            st.pop_back();
            if (!cur) continue;
            if (cur->k == EK::Str) {
                for (const auto &[ck, cv] : split_interp(cur->a))
                    if (ck == "var") r.insert(cv.substr(0, cv.find('.')));
                continue;
            }
            if (cur->k == EK::Call && !cur->a.empty())
                r.insert(cur->a.substr(0, cur->a.find('.')));
            for (const auto &k : cur->kids)
                if (k) st.push_back(k.get());
        }
    };

    auto drop_expr_refs = [&](const Expr *e) {
        Names r;
        expr_refs(e, r);
        if (e && e->fn) bound_in(view(e->fn->body), r);
        for (const auto &x : r) cand.erase(x);
    };

    std::function<void(const Expr *)> ban_expr = [&](const Expr *e) {
        if (!e) return;
        if (e->k == EK::Closure) {
            auto sub = closure_free_vars(*e->fn);
            if (e->fn->name) sub.erase(*e->fn->name);
            for (const auto &x : sub) cand.erase(x);
            return;
        }
        if (e->k == EK::ParFor) {
            ban_expr(e->kids[0].get());
            ban_expr(e->kids[1].get());
            if (!e->b.empty()) cand.erase(e->b);
            auto sub = closure_free_vars(*e->fn);
            for (const auto &x : sub) cand.erase(x);
            return;
        }
        if (e->k == EK::Move || (e->k == EK::Un && e->a == "&")) {
            drop_expr_refs(e);
            return;
        }
        for (const auto &sub : e->kids) ban_expr(sub.get());
    };

    std::function<void(const StmtList &)> ban_stmts = [&](const StmtList &stmts) {
        for (const Stmt *sp : stmts) {
            if (!sp) continue;
            const Stmt &s = *sp;
            if (s.k == SK::Spawn || s.k == SK::Scope || s.k == SK::Defer || s.k == SK::Fn ||
                s.k == SK::Impl || s.k == SK::Trait || s.k == SK::Enum || s.k == SK::Struct ||
                s.k == SK::Pub) {
                cand.clear();
                return;
            }
            if (s.k == SK::Let || s.k == SK::LetTmp) {
                if (s.a.find('.') != std::string::npos ||
                    (s.ann && (s.ann->rfind("ptr[", 0) == 0 || s.ann->rfind("slice[", 0) == 0)))
                    cand.erase(s.a.substr(0, s.a.find('.')));
                ban_expr(s.e1.get());
            } else if (s.k == SK::Set) {
                if (s.a.find('.') != std::string::npos)
                    cand.erase(s.a.substr(0, s.a.find('.')));
                ban_expr(s.e1.get());
            } else if (s.k == SK::ForIter) {
                cand.erase(s.a);
                ban_expr(s.e1.get());
                ban_stmts(view(s.body));
            } else if (s.k == SK::ForRange) {
                if (is_float) cand.erase(s.a);
                ban_expr(s.e1.get());
                ban_expr(s.e2.get());
                ban_stmts(view(s.body));
            } else if (s.k == SK::If) {
                ban_expr(s.e1.get());
                ban_stmts(view(s.body));
                if (const Stmt *els = real_else(s)) {
                    StmtList eb = els->k == SK::Block ? view(els->body) : StmtList{els};
                    ban_stmts(eb);
                }
            } else if (s.k == SK::While) {
                ban_expr(s.e1.get());
                ban_stmts(view(s.body));
            } else if (s.k == SK::Try) {
                for (const auto &tn : try_let_names(view(s.body))) cand.erase(tn);
                if (s.opt1) cand.erase(*s.opt1);
                ban_stmts(view(s.body));
                ban_stmts(view(s.body2));
            } else if (s.k == SK::Unsafe || s.k == SK::Block) {
                ban_stmts(view(s.body));
            } else if (s.k == SK::Return || s.k == SK::Expr) {
                ban_expr(s.e1.get());
            } else if (s.k == SK::SetIdx) {
                ban_expr(s.e1.get());
                ban_expr(s.e2.get());
                ban_expr(s.e3.get());
            } else if (s.k == SK::SetField) {
                ban_expr(s.e1.get());
                ban_expr(s.e2.get());
            }
        }
    };

    ban_stmts(body);
    if (cand.empty()) return {};

    auto check_expr_init = [&](const Expr *e, const std::set<std::string> &init) {
        Names r;
        expr_refs(e, r);
        for (const auto &x : r)
            if (!init.count(x)) cand.erase(x);
    };

    std::function<void(const StmtList &, std::set<std::string> &)> check_init =
        [&](const StmtList &stmts, std::set<std::string> &init) {
        for (const Stmt *sp : stmts) {
            if (!sp) continue;
            const Stmt &s = *sp;
            if (s.k == SK::Let || s.k == SK::LetTmp) {
                check_expr_init(s.e1.get(), init);
                if (s.a.find('.') == std::string::npos) init.insert(s.a);
            } else if (s.k == SK::Set) {
                check_expr_init(s.e1.get(), init);
                std::string nm = s.a.substr(0, s.a.find('.'));
                if (!init.count(nm)) cand.erase(nm);
            } else if (s.k == SK::ForRange) {
                check_expr_init(s.e1.get(), init);
                check_expr_init(s.e2.get(), init);
                std::set<std::string> sub = init;
                sub.insert(s.a);
                check_init(view(s.body), sub);
            } else if (s.k == SK::ForIter) {
                check_expr_init(s.e1.get(), init);
                std::set<std::string> sub = init;
                check_init(view(s.body), sub);
            } else if (s.k == SK::If) {
                check_expr_init(s.e1.get(), init);
                std::set<std::string> sub1 = init;
                check_init(view(s.body), sub1);
                if (const Stmt *els = real_else(s)) {
                    std::set<std::string> sub2 = init;
                    StmtList eb = els->k == SK::Block ? view(els->body) : StmtList{els};
                    check_init(eb, sub2);
                }
            } else if (s.k == SK::While) {
                check_expr_init(s.e1.get(), init);
                std::set<std::string> sub = init;
                check_init(view(s.body), sub);
            } else if (s.k == SK::Try) {
                std::set<std::string> sub1 = init, sub2 = init;
                check_init(view(s.body), sub1);
                check_init(view(s.body2), sub2);
            } else if (s.k == SK::Unsafe || s.k == SK::Block) {
                check_init(view(s.body), init);
            } else if (s.k == SK::Return || s.k == SK::Expr) {
                check_expr_init(s.e1.get(), init);
            } else if (s.k == SK::SetIdx) {
                check_expr_init(s.e1.get(), init);
                check_expr_init(s.e2.get(), init);
                check_expr_init(s.e3.get(), init);
            } else if (s.k == SK::SetField) {
                check_expr_init(s.e1.get(), init);
                check_expr_init(s.e2.get(), init);
            }
        }
    };

    {
        std::set<std::string> init = param_names;
        check_init(body, init);
    }
    if (cand.empty()) return {};

    auto rhs_ok = [&](const Expr &e) -> bool {
        if (is_float) return ubf_expr_ok(e, cand);
        if (ub_expr_ok(e, cand)) return true;
        if (e.k == EK::Call) {
            if (e.a == "len" && e.kids.size() == 1) return true;
            if ((e.a == "time_ms" || e.a == "mono_ns") && e.kids.empty()) return true;
        }
        return false;
    };

    std::set<std::string> writes;
    std::function<bool(const StmtList &)> step = [&](const StmtList &stmts) -> bool {
        bool dropped = false;
        for (const Stmt *sp : stmts) {
            if (!sp) continue;
            const Stmt &s = *sp;
            if (s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::Set) {
                std::string nm = s.a.substr(0, s.a.find('.'));
                if (s.a.find('.') == std::string::npos) writes.insert(nm);
                if (cand.count(nm) && (s.a.find('.') != std::string::npos || !rhs_ok(*s.e1))) {
                    cand.erase(nm);
                    dropped = true;
                }
            } else if (s.k == SK::ForRange) {
                if (!is_float) writes.insert(s.a);
                if (step(view(s.body))) dropped = true;
            } else if (s.k == SK::ForIter || s.k == SK::While || s.k == SK::Unsafe || s.k == SK::Block) {
                if (step(view(s.body))) dropped = true;
            } else if (s.k == SK::If) {
                if (step(view(s.body))) dropped = true;
                if (const Stmt *els = real_else(s)) {
                    StmtList eb = els->k == SK::Block ? view(els->body) : StmtList{els};
                    if (step(eb)) dropped = true;
                }
            } else if (s.k == SK::Try) {
                if (step(view(s.body))) dropped = true;
                if (step(view(s.body2))) dropped = true;
            }
        }
        return dropped;
    };

    while (!cand.empty() && step(body)) {}
    std::set<std::string> out_;
    for (const auto &v : cand)
        if (writes.count(v)) out_.insert(v);
    return out_;
}

std::set<std::string> Emitter::compute_ub_ints(const StmtList &body, const std::set<std::string> &vars_,
                                               const std::set<std::string> &param_names) {
    return compute_ub_vars(body, vars_, param_names, false);
}

std::set<std::string> Emitter::compute_ub_floats(const StmtList &body, const std::set<std::string> &vars_,
                                                 const std::set<std::string> &param_names) {
    return compute_ub_vars(body, vars_, param_names, true);
}
void collect_nested_lets(const StmtList &body, std::vector<std::string> &out, bool include_for_vars) {
    for (const Stmt *st : body) {
        if (!st) continue;
        switch (st->k) {
        case SK::Let:
        case SK::LetTmp:
            out.push_back(st->a.substr(0, st->a.find('.')));
            break;
        case SK::If:
            collect_nested_lets(view(st->body), out, include_for_vars);
            if (st->els) {
                if (st->els->k == SK::Block) collect_nested_lets(view(st->els->body), out, include_for_vars);
                else collect_nested_lets({st->els.get()}, out, include_for_vars);
            } else {
                collect_nested_lets(view(st->body2), out, include_for_vars);
            }
            break;
        case SK::While:
        case SK::Block:
        case SK::Scope:
        case SK::Unsafe:
            collect_nested_lets(view(st->body), out, include_for_vars);
            break;
        case SK::ForIter:
        case SK::ForRange:
            if (include_for_vars) out.push_back(st->a);
            collect_nested_lets(view(st->body), out, include_for_vars);
            break;
        case SK::Try:
            collect_nested_lets(view(st->body), out, include_for_vars);
            collect_nested_lets(view(st->body2), out, include_for_vars);
            break;
        default:
            break;
        }
    }
}

std::set<std::string> try_let_names(const StmtList &body) {
    std::vector<std::string> vec;
    collect_nested_lets(body, vec, true);
    return std::set<std::string>(vec.begin(), vec.end());
}

std::vector<std::string> nested_owned_let_names(const StmtList &body) {
    std::vector<std::string> vec, dedup;
    collect_nested_lets(body, vec, false);
    std::set<std::string> seen;
    for (const auto &nm : vec)
        if (seen.insert(nm).second) dedup.push_back(nm);
    return dedup;
}

void Emitter::drop_alias_temps(const StmtList &body, const std::string &ind) {
    for (const auto &name : block_let_names(body)) {
        if (alias_temps.count(name)) {
            std::string scn = slot_cn(name);
            ln(ind, cat("vrc_dec(", scn, ");"));
            ln(ind, cat(scn, " = vnil();"));
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
    auto bnames = block_let_names(body);
    dfb_vars.push_back(nested_owned_let_names(body));
    for (const Stmt *t : body) st(*t, ind);
    dfb_vars.pop_back();
    dfb.pop_back();
    if (move_rt)
        for (const auto &name : bnames) emit_drop(name, ind);
    drop_alias_temps(body, ind);
    ln(ind, cat("vdfreset(", b, ");"));
}

void Emitter::emit_loop_exit(const std::string &k, const std::string &ind) {
    // Every lexical block between the loop boundary and this exit is left by
    // break/continue. Release block-local owned slots before draining defers
    // (such as $arena_leave) so recycled arena chunks never leave stale BHead
    // pointers in loop-local slots, then join any scope it leaves.
    const size_t scope_depth = loop_exit_depths.empty()
        ? scopes.size() : loop_exit_depths.back().first;
    const size_t defer_depth = loop_exit_depths.empty()
        ? (dfb.empty() ? 0 : dfb.size() - 1) : loop_exit_depths.back().second;
    for (size_t i = dfb.size(); i > defer_depth; --i) {
        if (move_rt && i - 1 < dfb_vars.size()) {
            for (const auto &vname : dfb_vars[i - 1]) {
                auto sit = sym.find(vname);
                if (sit != sym.end() && (sit->second.own == Own::Owned || alias_temps.count(vname)) &&
                    !moved.count(vname) && !shared.count(vname)) {
                    ln(ind, cat("vrelease(", sit->second.cn, ");"));
                    ln(ind, cat(sit->second.cn, " = vnil();"));
                }
            }
        }
        ln(ind, cat("vdfreset(", dfb[i - 1], ");"));
    }
    for (size_t i = scopes.size(); i > scope_depth; --i)
        ln(ind, cat("vscope_end(vint(", scopes[i - 1], "));"));
    ln(ind, k + ";");
}

void Emitter::emit_defer(const Expr &e, const std::string &ind, int line) {
    if (fin == "__fin_top" && dfb.empty()) return;
    int code = fn_code(cat("clo:", clos.size()));
    std::set<std::string> outer;
    if (cap) outer = cap->second;
    std::set<std::string> reads;
    expr_vars(e, reads);
    std::set<std::string> visible;
    for (const auto &[n, s] : sym) visible.insert(n);
    for (const auto &n : dfb_locals) visible.insert(n);
    for (const auto &n : outer) visible.insert(n);
    std::set<std::string> capnames;
    for (const auto &n : reads)
        if (visible.count(n)) capnames.insert(n);
    for (const auto &n : outer) capnames.insert(n);
    const std::string sfile = !cur_src_file.empty() ? cur_src_file : root_src_file;
    Stmt &syn = synth.emplace_back();
    syn.k = SK::Expr;
    syn.line = line > 0 ? line : 1;
    syn.e1 = clone(e);
    if (!sfile.empty()) local_stmt_files[&syn] = sfile;
    clos.push_back({&no_params(), StmtList{&syn}, capnames, false, sfile});
    std::string cm = new_tmp("m");
    std::string cb = new_tmp("cb");
    std::vector<std::string> pre = {cat("Map *", cm, " = vclosure_env_new(", capnames.size(), ");")};
    int64_t capture_slot = 0;
    for (const auto &var : capnames) {
        for (auto &l : capture(cm, var, capture_slot)) pre.push_back(l);
        capture_slot++;
    }
    pre.push_back(cat("Vec *", cb, " = vvec();"));
    pre.push_back(cat("vec_push(", cb, ", vint(", code, "));"));
    pre.push_back(cat("{ Val _cv = vstruct(", cm, "); vec_push(", cb, ", _cv); }"));
    pre.push_back(cat("vdfadd(vtagv(\"closure\", ", cb, "));"));
    for (auto &p : pre) ln(ind, p);
}

static std::set<std::string> intersect_names(const std::set<std::string> &a,
                                             const std::set<std::string> &b) {
    std::set<std::string> out;
    for (const auto &x : a)
        if (b.count(x)) out.insert(x);
    return out;
}

static void assigned_vars_in(const StmtList &stmts, std::set<std::string> &out) {
    for (const Stmt *sp : stmts) {
        if (!sp) continue;
        const Stmt &s = *sp;
        switch (s.k) {
        case SK::Let:
        case SK::LetTmp:
        case SK::Set:
            out.insert(head_of(s.a));
            break;
        case SK::ForRange:
        case SK::ForIter:
            out.insert(s.a);
            assigned_vars_in(view(s.body), out);
            break;
        case SK::While:
        case SK::Block:
        case SK::Unsafe:
        case SK::Scope:
        case SK::Pub:
            assigned_vars_in(view(s.body), out);
            break;
        case SK::If:
            assigned_vars_in(view(s.body), out);
            if (const Stmt *els = real_else(s)) {
                if (els->k == SK::Block) assigned_vars_in(view(els->body), out);
                else assigned_vars_in({els}, out);
            }
            break;
        case SK::Try:
            assigned_vars_in(view(s.body), out);
            assigned_vars_in(view(s.body2), out);
            break;
        default:
            break;
        }
    }
}

static bool block_returns(const StmtList &stmts);

static bool stmt_returns(const Stmt &s) {
    switch (s.k) {
    case SK::Return:
        return true;
    case SK::Block:
    case SK::Unsafe:
    case SK::Scope:
        return block_returns(view(s.body));
    case SK::If: {
        const Stmt *els = real_else(s);
        if (!els) return false;
        return block_returns(view(s.body)) &&
               (els->k == SK::Block ? block_returns(view(els->body)) : stmt_returns(*els));
    }
    case SK::Try:
        return block_returns(view(s.body)) && block_returns(view(s.body2));
    default:
        return false;
    }
}

static bool block_returns(const StmtList &stmts) {
    for (const Stmt *sp : stmts)
        if (sp && stmt_returns(*sp)) return true;
    return false;
}

void Emitter::st(const Stmt &s, const std::string &ind) {
    switch (s.k) {
    case SK::Block:
    case SK::Unsafe:
        emit_block(view(s.body), ind);
        return;
    case SK::Pub:
        st(*s.body[0], ind);
        return;
    case SK::Trait:
    case SK::Enum:
    case SK::Struct:
    case SK::Impl:
    case SK::Import:
    case SK::PubUse:
        return;
    default:
        break;
    }
    if ((debug_lines || trace_lines) && s.line > 0) {
        switch (s.k) {
        case SK::Let: case SK::LetTmp: case SK::Set: case SK::SetIdx: case SK::SetField:
        case SK::Expr: case SK::If: case SK::While: case SK::ForRange: case SK::ForIter:
        case SK::Return: case SK::Break: case SK::Continue: case SK::Spawn: case SK::Scope:
        case SK::Defer: case SK::Try:
            emit_dbg_stmt(s);
            if (trace_lines) ln(ind, cat("btrace_line(", s.line, ");"));
            break;
        default:
            break;
        }
    }
    switch (s.k) {
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
        if (name.find('.') == std::string::npos && ub_ints.count(name)) {
            ub_assign(name, e, ind);
            return;
        }
        if (name.find('.') == std::string::npos && ub_floats.count(name)) {
            ubf_assign(name, e, ind);
            return;
        }
        V v = ex(e);
        for (auto &p : v.pre) ln(ind, p);
        if (name.find('.') != std::string::npos) {
            auto dot = name.find('.');
            std::string head = name.substr(0, dot), rest = name.substr(dot + 1);
            if (sym.count(head)) {
                ln(ind, cat("vfieldset_ic(", sym[head].cn, ", \"", rest, "\", ", keep(e, v.val), ", ", next_fic(), ");"));
                emit_err_check(ind);
                return;
            }
        }
        Own own = own_of(e);
        bool fresh = dec_able(e, heap_returns());
        if (!fresh && e.k != EK::Call && !moves_owned_local(e)) {
            v.val = cat("vrc_inc(", v.val, ")");
            own = Own::Owned;
        }
        bool lit = v.val.rfind("vlit(", 0) == 0;
        if (reclaim.count(name) && lit) {
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
                        !dropped.count(name);
        if (relet.count(name) && own == Own::Copy) own = Own::Owned;
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
            ln(ind, cat("vrelease(", d->second.cn, ");"));
            ln(ind, cat(cn, " = ", tmp, ";"));
        } else {
            ln(ind, cat(cn, " = (", v.val, ");"));
        }
        emit_err_check(ind);
        moved.erase(name);
        dropped.erase(name);
        if (!name.empty() && name[0] == '@' && !fresh) alias_temps.insert(name);
        return;
    }
    case SK::Set: {
        const std::string &tgt = s.a;
        const Expr &e = *s.e1;
        const bool global_binding = global_lets.count(tgt) && !dfb_locals.count(tgt) &&
                                    !ub_ints.count(tgt) && !ub_floats.count(tgt) &&
                                    !cur_params.count(tgt) && !(cap && cap->second.count(tgt));
        if (tgt.find('.') == std::string::npos && ub_ints.count(tgt)) {
            ub_assign(tgt, e, ind);
            return;
        }
        if (tgt.find('.') == std::string::npos && ub_floats.count(tgt)) {
            ubf_assign(tgt, e, ind);
            return;
        }
        if (e.k == EK::Var && e.a == tgt) {
            assigned.insert(tgt);
            return;
        }
        auto d = sym.find(tgt);
        if (reclaim.count(tgt) && d != sym.end() && !moved.count(tgt) && !dropped.count(tgt) &&
            !global_binding && !cap && err_scope_depth == 0 && e.k == EK::Bin && e.a == "+") {
            std::vector<const Expr *> concat_rhs;
            const Expr *cur = &e;
            while (cur->k == EK::Bin && cur->a == "+" && cur->kids.size() == 2) {
                concat_rhs.push_back(cur->kids[1].get());
                cur = cur->kids[0].get();
            }
            if (cur->k == EK::Var && cur->a == tgt && concat_rhs.size() > 1) {
                std::reverse(concat_rhs.begin(), concat_rhs.end());
                std::function<bool(const Expr *)> mentions_tgt = [&](const Expr *x) -> bool {
                    if (!x) return false;
                    if (x->k == EK::Var && x->a == tgt) return true;
                    if (x->k == EK::Str) {
                        for (const auto &[ck, cv] : split_interp(x->a))
                            if (ck == "var" && (cv == tgt || cv.rfind(tgt + ".", 0) == 0))
                                return true;
                    }
                    if (x->fn) return true;
                    for (const auto &k : x->kids)
                        if (mentions_tgt(k.get())) return true;
                    return false;
                };
                bool tail_clean = true;
                for (size_t ci = 1; ci < concat_rhs.size(); ++ci) {
                    if (mentions_tgt(concat_rhs[ci])) {
                        tail_clean = false;
                        break;
                    }
                }
                if (tail_clean) {
                    for (const Expr *re : concat_rhs) {
                        auto lhs = std::make_unique<Expr>();
                        lhs->k = EK::Var;
                        lhs->a = tgt;
                        auto bin = std::make_unique<Expr>();
                        bin->k = EK::Bin;
                        bin->a = "+";
                        bin->kids.push_back(std::move(lhs));
                        bin->kids.push_back(clone(*re));
                        Stmt step;
                        step.k = SK::Set;
                        step.line = s.line;
                        step.a = tgt;
                        step.e1 = std::move(bin);
                        st(step, ind);
                    }
                    return;
                }
            }
        }
        bool self_concat = reclaim.count(tgt) && d != sym.end() &&
                           !moved.count(tgt) && !dropped.count(tgt) &&
                           e.k == EK::Bin && e.a == "+" &&
                           e.kids[0]->k == EK::Var && e.kids[0]->a == tgt;
        Own own;
        V v;
        std::vector<std::string> drop;
        if (self_concat) {
            v = ex(*e.kids[1]);
            for (auto &p : v.pre) ln(ind, p);
            const bool rhs_owned = dec_able(*e.kids[1], heap_returns());
            if (rhs_owned) {
                if (auto d = owned_temp(*e.kids[1], v.val)) drop.push_back(*d);
            }
            if (drop.empty() &&
                (e.kids[1]->k == EK::Call || e.kids[1]->k == EK::MCall ||
                 e.kids[1]->k == EK::Idx || e.kids[1]->k == EK::Field ||
                 e.kids[1]->k == EK::Quest) &&
                rhs_owned) {
                std::string h = new_tmp("h");
                ln(ind, cat("Val ", h, " = (", v.val, ");"));
                v.val = h;
                drop.push_back(h);
            }
            // `s = s + f()`: the append must not read the value a failed `f`
            // left behind; the error goes to the enclosing try.
            if (!v.pre.empty() && may_raise(*e.kids[1])) emit_err_check(ind);
            own = global_binding ? Own::Copy : Own::Owned;
            std::string cn_tmp = var_def(tgt, own);
            v.val = cat("vadd_self(", cn_tmp, ", ", v.val, ")");
        } else {
            v = ex(e);
            for (auto &p : v.pre) ln(ind, p);
            if (dec_able(e, heap_returns())) {
                if (auto d = owned_temp(e, v.val)) drop.push_back(*d);
            }
            // `n = n + f()`: see the append case above.
            if (!v.pre.empty() && may_raise(e)) emit_err_check(ind);
            own = own_of(e);
            if ((tgt.empty() || tgt[0] != '@') && e.k == EK::Field) own = Own::Owned;
            if (reclaim.count(tgt) && v.val.rfind("vlit(", 0) == 0) {
                v.val = "vstr(" + v.val.substr(5);
                own = Own::Owned;
            }
        }
        if (reclaim.count(tgt) && v.val.rfind("vlit(", 0) == 0 && !self_concat) {
            v.val = "vstr(" + v.val.substr(5);
            own = Own::Owned;
        }
        bool release = (reclaim.count(tgt) || global_binding) && d != sym.end() &&
                       !moved.count(tgt) && !dropped.count(tgt);
        if (!dec_able(e, heap_returns()) && e.k != EK::Call && !moves_owned_local(e)) {
            v.val = cat("vrc_inc(", v.val, ")");
            own = Own::Owned;
        }
        if (reclaim.count(tgt) && !global_binding && own == Own::Copy) own = Own::Owned;
        if (global_binding) own = Own::Copy;
        std::string cn = var_def(tgt, own);
        assigned.insert(tgt);
        // Do not overwrite the target binding if evaluating the RHS raised an error.
        if (self_concat || release || may_raise(e)) {
            std::string tmp = new_tmp("v");
            ln(ind, cat("Val ", tmp, " = (", v.val, ");"));
            emit_err_check(ind, drop);
            // After the RHS, so a hot store loop pays one dec per store. Never
            // for an append: `vadd_self` hands back the receiver itself.
            if (release && !self_concat) ln(ind, cat("vrelease(", d->second.cn, ");"));
            ln(ind, cat(cn, " = ", tmp, ";"));
            // `vadd_self` copies the right side in and keeps its own reference,
            // so the temp this statement built dies here, not only on error.
            if (self_concat)
                for (auto &t : drop) ln(ind, cat("vdrop_temp(", t, ");"));
        } else {
            ln(ind, cat(cn, " = (", v.val, ");"));
            emit_err_check(ind);
        }
        moved.erase(tgt);
        dropped.erase(tgt);
        return;
    }
    case SK::SetIdx: {
        if (std::string pe = ptr_elem(*s.e1); !pe.empty()) {
            st_ptr_setidx(pe, s, ind);
            return;
        }
        V b = ex(*s.e1);
        for (auto &p : b.pre) ln(ind, p);
        const bool b_owned = dec_able(*s.e1, heap_returns());
        std::string r = b.val;
        if (b_owned || may_raise(*s.e1)) {
            r = new_tmp("r");
            ln(ind, cat("Val ", r, " = (", b.val, ");"));
            if (may_raise(*s.e1)) {
                emit_err_check(ind, b_owned ? std::vector<std::string>{r} : std::vector<std::string>{});
            }
        }
        const size_t cstart = call_cleanup.size();
        if (b_owned) call_cleanup.push_back(r);
        V i, v;
        std::string ik;
        try {
            i = ex(*s.e2);
            for (auto &p : i.pre) ln(ind, p);
            ik = new_tmp("ik");
            ln(ind, cat("Val ", ik, " = (", keep(*s.e2, i.val), ");"));
            if (may_raise(*s.e2)) emit_err_check(ind, {ik});
            call_cleanup.push_back(ik);
            v = ex(*s.e3);
            call_cleanup.resize(cstart);
        } catch (...) {
            call_cleanup.resize(cstart);
            throw;
        }
        for (auto &p : v.pre) ln(ind, p);
        std::string vv = new_tmp("vv");
        ln(ind, cat("Val ", vv, " = (", keep(*s.e3, v.val), ");"));
        if (may_raise(*s.e3)) {
            std::vector<std::string> d3 = {vv, ik};
            if (b_owned) d3.push_back(r);
            emit_err_check(ind, d3);
        }
        ln(ind, cat("vset_at(", r, ", ", ik, ", ", vv, ");"));
        if (b_owned) ln(ind, cat("vdrop_temp(", r, ");"));
        emit_err_check(ind);
        if (s.e1->k == EK::Var) assigned.insert(s.e1->a);
        return;
    }
    case SK::SetField: {
        V b = ex(*s.e1);
        for (auto &p : b.pre) ln(ind, p);
        const bool b_owned = dec_able(*s.e1, heap_returns());
        std::string r = b.val;
        if (b_owned || may_raise(*s.e1)) {
            r = new_tmp("r");
            ln(ind, "Val " + r + " = (" + b.val + ");");
            if (may_raise(*s.e1)) {
                emit_err_check(ind, b_owned ? std::vector<std::string>{r} : std::vector<std::string>{});
            }
        }
        const size_t cstart = call_cleanup.size();
        if (b_owned) call_cleanup.push_back(r);
        V v;
        try {
            v = ex(*s.e2);
            call_cleanup.resize(cstart);
        } catch (...) {
            call_cleanup.resize(cstart);
            throw;
        }
        for (auto &p : v.pre) ln(ind, p);
        std::string vv = new_tmp("vv");
        ln(ind, cat("Val ", vv, " = (", keep(*s.e2, v.val), ");"));
        if (may_raise(*s.e2)) {
            std::vector<std::string> d2 = {vv};
            if (b_owned) d2.push_back(r);
            emit_err_check(ind, d2);
        }
        std::string st = infer_expr_struct(s.e1.get(), var_structs);
        int slot = !st.empty() ? struct_field_slot(st, s.a) : -1;
        if (slot >= 0) {
            int sid = canonical_struct_shape(st);
            ln(ind, cat("vstruct_slot_set(", r, ", &_shp[", sid, "], ", slot, ", \"",
                        clit(s.a), "\", ", vv, ");"));
        } else {
            ln(ind, cat("vfieldset_ic(", r, ", \"", s.a, "\", ", vv, ", ",
                        next_fic(), ");"));
        }
        if (b_owned) ln(ind, "vdrop_temp(" + r + ");");
        emit_err_check(ind);
        return;
    }
    case SK::Return: {
        V v = ex(*s.e1);
        for (auto &p : v.pre) ln(ind, p);
        emit_err_check(ind);
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            ln(ind, cat("vscope_end(vint(", *it, "));"));
        if (!scopes.empty()) {
            size_t saved_scope_depth = err_scope_depth;
            err_scope_depth = scopes.size();
            emit_err_check(ind);
            err_scope_depth = saved_scope_depth;
        }
        // The value leaves with the caller; the exit drops must not free it.
        // `a[i]` already hands back a reference the holder owns (vat_owned), so
        // the container can just be dropped at the exit like any other local;
        // lifting the part out and stealing it was the pre-vat_owned shape.
        const bool owned_part = dec_able(*s.e1, heap_returns());
        // `r.sort()` is `r` itself and `r.first()` is a part of it, so the value belongs to the
        // root `r` belongs to.
        const Expr *alias = s.e1.get();
        bool part_call = false;
        while (alias->k == EK::MCall && !alias->kids.empty() &&
               (SELF_RESULT.count(alias->b) || PART_RESULT.count(alias->b))) {
            part_call = part_call || PART_RESULT.count(alias->b) > 0;
            alias = alias->kids[0].get();
        }
        auto root = place_root(*alias);
        const bool is_part = root && sym.count(*root) &&
                             (part_of_local(*alias, *root) ||
                              (part_call && sym[*root].own == Own::Owned));
        std::string steal_cn;
        if (root && !owned_part && sym.count(*root)) {
            if (is_part) {
                // `return v[0]` hands out a part of a local container: the part is
                // lifted into a temp, then the container goes.
                std::string t = new_tmp("c");
                ln(ind, "Val " + t + " = (" + v.val + ");");
                ln(ind, "vdrop_recv_part(" + sym[*root].cn + ", " + t + ");");
                // The container reference was consumed above. Clear its slot so
                // shared exit cleanup cannot release the stale pointer again.
                ln(ind, sym[*root].cn + " = vnil();");
                v.val = t;
            } else if (global_lets.count(*root) || fin == "__fin_top") {
                // A script-level binding outlives the frame that returns it, so
                // the caller gets a reference of its own; without the inc nobody
                // may release the result.
                if (global_lets.count(*root)) v.val = cat("vrc_inc(", v.val, ")");
                shared.insert(*root);
            } else if (sym[*root].own == Own::Owned) {
                steal_cn = sym[*root].cn;
            } else {
                v.val = cat("vrc_inc(", v.val, ")");
                shared.insert(*root);
            }
        } else if (root && !owned_part && cap && cap->second.count(*root)) {
            v.val = cat("vrc_inc(", v.val, ")");
        }
        std::string handed = (root && !owned_part && !is_part) ? *root : "";
        if (fin == "__fin_top") {
            Names keep;
            refs_in_expr(s.e1.get(), keep);
            for (size_t i = iter_cleanups.size(); i > 0; --i) ln(ind, iter_cleanups[i - 1]);
            emit_fn_drops(ind, handed, true, keep);
            ln(ind, cat("{ fprintf(stderr, \"unhandled: \"); vprint_err(", v.val,
                        "); fprintf(stderr, \"\\n\"); return 1; }"));
        } else {
            if ((!shared.empty() && move_rt) || !iter_cleanups.empty()) {
                std::string rv = new_tmp("rv");
                ln(ind, cat("Val ", rv, " = (", v.val, ");"));
                v.val = rv;
                for (size_t i = iter_cleanups.size(); i > 0; --i) ln(ind, iter_cleanups[i - 1]);
                if (!shared.empty() && move_rt) {
                    for (const auto &sname : shared) {
                        auto sit = sym.find(sname);
                        if (sit == sym.end() || sit->second.own != Own::Owned) continue;
                        if (moved.count(sname) || sname == handed) continue;
                        ln(ind, cat("vrelease(", sit->second.cn, ");"));
                        ln(ind, cat(sit->second.cn, " = vnil();"));
                    }
                }
            }
            if (!steal_cn.empty()) {
                ln(ind, cat("{ __ret = (", v.val, "); ", steal_cn, " = vnil(); goto ", fin, "; }"));
            } else {
                ln(ind, cat("{ __ret = (", v.val, "); goto ", fin, "; }"));
            }
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
        std::string cond_c;
        if (!ub_ints.empty() && s.e1->k == EK::Bin &&
            (s.e1->a == "==" || s.e1->a == "!=" || s.e1->a == "<" || s.e1->a == ">" ||
             s.e1->a == "<=" || s.e1->a == ">=") &&
            ub_expr_ok(*s.e1->kids[0]) && ub_expr_ok(*s.e1->kids[1]) &&
            !ub_may_raise(*s.e1->kids[0]) && !ub_may_raise(*s.e1->kids[1])) {
            cond_c = cat("(", ub_ex(*s.e1->kids[0]), ") ", s.e1->a, " (", ub_ex(*s.e1->kids[1]), ")");
        } else if (!ub_floats.empty() && s.e1->k == EK::Bin &&
            (s.e1->a == "==" || s.e1->a == "!=" || s.e1->a == "<" || s.e1->a == ">" ||
             s.e1->a == "<=" || s.e1->a == ">=") &&
            ubf_expr_ok(*s.e1->kids[0]) && ubf_expr_ok(*s.e1->kids[1]) &&
            !ubf_may_raise(*s.e1->kids[0]) && !ubf_may_raise(*s.e1->kids[1])) {
            cond_c = ubf_cmp_c(s.e1->a, ubf_ex(*s.e1->kids[0]), ubf_ex(*s.e1->kids[1]));
        } else {
            V c = ex(*s.e1);
            for (auto &p : c.pre) ln(ind, p);
            emit_err_check(ind);
            std::vector<std::string> test;
            cond_c = truth(*s.e1, c.val, test);
            for (auto &p : test) ln(ind, p);
        }
        auto pre_moved = moved;
        auto pre_dropped = dropped;
        ln(ind, cat("if (", cond_c, ") {"));
        emit_block(view(s.body), ind + "  ");
        auto then_moved = moved;
        auto then_dropped = dropped;
        const bool then_ret = block_returns(view(s.body));
        moved = pre_moved;
        dropped = pre_dropped;
        const Stmt *els = real_else(s);
        if (!els) {
            ln(ind, "}");
            if (!then_ret) {
                moved = intersect_names(then_moved, pre_moved);
                dropped = intersect_names(then_dropped, pre_dropped);
            }
            return;
        }
        if (els->k != SK::Block) {
            // Braced even for a single statement: the statement may have pre-lines
            // of its own, and a declaration cannot follow a bare `else`
            // ("expected expression before `volatile`").
            ln(ind, "} else {");
            st(*els, ind + "  ");
            ln(ind, "}");
        } else {
            ln(ind, "} else {");
            emit_block(view(els->body), ind + "  ");
            ln(ind, "}");
        }
        const bool else_ret = (els->k == SK::Block) ? block_returns(view(els->body))
                                                     : stmt_returns(*els);
        if (then_ret && !else_ret) {
            // Keep `moved` / `dropped` from `else`.
        } else if (!then_ret && else_ret) {
            moved = std::move(then_moved);
            dropped = std::move(then_dropped);
        } else {
            moved = intersect_names(then_moved, moved);
            dropped = intersect_names(then_dropped, dropped);
        }
        return;
    }
    case SK::While: {
        std::set<std::string> loop_defs;
        assigned_vars_in(view(s.body), loop_defs);
        for (const auto &nm : loop_defs) {
            moved.erase(nm);
            dropped.erase(nm);
        }
        auto pre_moved = moved;
        auto pre_dropped = dropped;
        auto finish_loop = [&]() {
            moved = intersect_names(pre_moved, moved);
            dropped = intersect_names(pre_dropped, dropped);
        };
        if (!ub_ints.empty() && s.e1->k == EK::Bin &&
            (s.e1->a == "==" || s.e1->a == "!=" || s.e1->a == "<" || s.e1->a == ">" ||
             s.e1->a == "<=" || s.e1->a == ">=") &&
            ub_expr_ok(*s.e1->kids[0]) && ub_expr_ok(*s.e1->kids[1]) &&
            !ub_may_raise(*s.e1->kids[0]) && !ub_may_raise(*s.e1->kids[1])) {
            ln(ind, cat("while ((", ub_ex(*s.e1->kids[0]), ") ", s.e1->a, " (", ub_ex(*s.e1->kids[1]), ")) {"));
            loop_exit_depths.emplace_back(scopes.size(), dfb.size());
            emit_block(view(s.body), ind + "  ");
            loop_exit_depths.pop_back();
            ln(ind, "}");
            emit_loop_drops(view(s.body), ind);
            finish_loop();
            return;
        }
        if (!ub_floats.empty() && s.e1->k == EK::Bin &&
            (s.e1->a == "==" || s.e1->a == "!=" || s.e1->a == "<" || s.e1->a == ">" ||
             s.e1->a == "<=" || s.e1->a == ">=") &&
            ubf_expr_ok(*s.e1->kids[0]) && ubf_expr_ok(*s.e1->kids[1]) &&
            !ubf_may_raise(*s.e1->kids[0]) && !ubf_may_raise(*s.e1->kids[1])) {
            ln(ind, cat("while (", ubf_cmp_c(s.e1->a, ubf_ex(*s.e1->kids[0]), ubf_ex(*s.e1->kids[1])), ") {"));
            loop_exit_depths.emplace_back(scopes.size(), dfb.size());
            emit_block(view(s.body), ind + "  ");
            loop_exit_depths.pop_back();
            ln(ind, "}");
            emit_loop_drops(view(s.body), ind);
            finish_loop();
            return;
        }
        V c = ex(*s.e1);
        ln(ind, "while (1) {");
        for (auto &p : c.pre) ln(ind + "  ", p);
        emit_err_check(ind + "  ");
        std::vector<std::string> test;
        const std::string ok = truth(*s.e1, c.val, test);
        for (auto &p : test) ln(ind + "  ", p);
        ln(ind + "  ", cat("if (!", ok, ") break;"));
        loop_exit_depths.emplace_back(scopes.size(), dfb.size());
        emit_block(view(s.body), ind + "  ");
        loop_exit_depths.pop_back();
        ln(ind, "}");
        emit_loop_drops(view(s.body), ind);
        finish_loop();
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
        std::set<std::string> loop_defs{s.a};
        assigned_vars_in(view(s.body), loop_defs);
        for (const auto &nm : loop_defs) {
            moved.erase(nm);
            dropped.erase(nm);
        }
        auto pre_moved = moved;
        auto pre_dropped = dropped;
        ln(ind, cat("for (int64_t ", kk, " = ", lo, "; ", kk, " < ", hi, "; ++", kk, ") {"));
        if (ub_ints.count(s.a)) {
            ln(ind + "  ", cat("ub_", cname(s.a), " = ", kk, ";"));
        } else if (reclaim.count(s.a) || relet.count(s.a)) {
            std::string cn = var_def(s.a, Own::Owned);
            ln(ind + "  ", cat("vrelease(", cn, ");"));
            ln(ind + "  ", cat(cn, " = vint(", kk, ");"));
        } else {
            std::string cn = var_def(s.a, Own::Copy);
            ln(ind + "  ", cat(cn, " = vint(", kk, ");"));
        }
        assigned.insert(s.a);
        loop_exit_depths.emplace_back(scopes.size(), dfb.size());
        emit_block(view(s.body), ind + "  ");
        loop_exit_depths.pop_back();
        ln(ind, "}");
        emit_loop_drops(view(s.body), ind);
        moved = intersect_names(pre_moved, moved);
        dropped = intersect_names(pre_dropped, dropped);
        return;
    }
    case SK::ForIter: {
        V v = ex(*s.e1);
        for (auto &p : v.pre) ln(ind, p);
        emit_err_check(ind);
        std::string vv = new_tmp("v"), kk = new_tmp("k"), src = new_tmp("v");
        const bool drop_src = dec_able(*s.e1, heap_returns());
        ln(ind, cat("Val ", src, " = ", drop_src ? cat("(", v.val, ")") : cat("vrc_inc(", v.val, ")"), ";"));
        ln(ind, cat("Vec *", vv, " = viter_vec(", src, ");"));
        emit_err_check(ind, std::vector<std::string>{src});
        std::string iclean = cat("viter_done(", src, ", ", vv, "); vdrop_temp(", src, "); ");
        std::set<std::string> loop_defs{s.a};
        assigned_vars_in(view(s.body), loop_defs);
        for (const auto &nm : loop_defs) {
            moved.erase(nm);
            dropped.erase(nm);
        }
        auto pre_moved = moved;
        auto pre_dropped = dropped;
        ln(ind, cat("for (int64_t ", kk, " = 0; ", kk, " < ", vv, "->len; ++", kk, ") {"));
        if (reclaim.count(s.a) || relet.count(s.a)) {
            std::string cn = var_def(s.a, Own::Owned);
            ln(ind + "  ", cat("vrelease(", cn, ");"));
            ln(ind + "  ", cat(cn, " = vrc_inc(", vv, "->items[", kk, "]);"));
        } else {
            std::string cn = var_def(s.a, Own::Copy);
            ln(ind + "  ", cat(cn, " = ", vv, "->items[", kk, "];"));
        }
        assigned.insert(s.a);
        loop_exit_depths.emplace_back(scopes.size(), dfb.size());
        iter_cleanups.push_back(iclean);
        emit_block(view(s.body), ind + "  ");
        iter_cleanups.pop_back();
        loop_exit_depths.pop_back();
        ln(ind, "}");
        ln(ind, cat("viter_done(", src, ", ", vv, ");"));
        ln(ind, cat("vdrop_temp(", src, ");"));
        emit_loop_drops(view(s.body), ind);
        moved = intersect_names(pre_moved, moved);
        dropped = intersect_names(pre_dropped, dropped);
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
        size_t saved_scope_depth = err_scope_depth;
        size_t saved_iter_depth = err_iter_depth;
        auto pre_moved = moved;
        auto pre_dropped = dropped;
        std::string b = new_tmp("dfb");
        ln(ind, "{");
        ln(ind + "  ", cat("int64_t ", b, " = vdfsp;"));
        err_goto = catch_lbl;
        err_scope_depth = scopes.size();
        err_iter_depth = iter_cleanups.size();
        emit_block(view(s.body), ind + "  ");
        auto try_moved = moved;
        auto try_dropped = dropped;
        const bool try_ret = block_returns(view(s.body));
        err_goto = saved_goto;
        err_scope_depth = saved_scope_depth;
        err_iter_depth = saved_iter_depth;
        ln(ind + "  ", cat("goto ", after_lbl, ";"));
        ln(ind, cat(catch_lbl, ":;"));
        std::set<std::string> try_defs;
        assigned_vars_in(view(s.body), try_defs);
        moved = pre_moved;
        dropped = pre_dropped;
        for (const auto &nm : try_defs) {
            moved.erase(nm);
            dropped.erase(nm);
        }
        for (const auto &nm : nested_owned_let_names(view(s.body))) {
            if (ub_ints.count(nm) || ub_floats.count(nm)) continue;
            auto sit = sym.find(nm);
            if (sit == sym.end()) continue;
            if ((sit->second.own != Own::Owned && !alias_temps.count(nm)) || shared.count(nm))
                continue;
            std::string scn = sit->second.cn;
            ln(ind + "  ", cat("vrelease(", scn, ");"));
            ln(ind + "  ", cat(scn, " = vnil();"));
        }
        ln(ind + "  ", cat("vdfreset(", b, ");"));
        bool had_prev = false;
        Sym prev_sym;
        bool had_prev_assigned = false;
        bool had_prev_moved = false;
        bool had_prev_dropped = false;
        bool had_prev_ub = false;
        bool had_prev_ubf = false;
        bool had_prev_vs = false;
        std::string prev_vs;
        std::string cn;
        if (s.opt1) {
            auto pit = sym.find(*s.opt1);
            if (pit != sym.end()) { had_prev = true; prev_sym = pit->second; }
            had_prev_assigned = assigned.count(*s.opt1) > 0;
            had_prev_moved = moved.count(*s.opt1) > 0;
            had_prev_dropped = dropped.count(*s.opt1) > 0;
            had_prev_ub = ub_ints.erase(*s.opt1) > 0;
            had_prev_ubf = ub_floats.erase(*s.opt1) > 0;
            auto vsit = var_structs.find(*s.opt1);
            if (vsit != var_structs.end()) {
                had_prev_vs = true;
                prev_vs = vsit->second;
                var_structs.erase(vsit);
            }
            moved.erase(*s.opt1);
            dropped.erase(*s.opt1);
            cn = slot_cn("@catch_" + *s.opt1);
            sym[*s.opt1] = Sym{cn, Own::Owned, "", ""};
            catch_cns.insert(cn);
            ln(ind + "  ", cat("vrelease(", cn, ");"));
            ln(ind + "  ", cat(cn, " = verr_catch_part();"));
            assigned.insert(*s.opt1);
        } else {
            ln(ind + "  ", "verr_catch_discard();");
        }
        emit_block(view(s.body2), ind + "  ");
        const bool catch_ret = block_returns(view(s.body2));
        if (s.opt1) {
            ln(ind + "  ", cat("vrelease(", cn, ");"));
            ln(ind + "  ", cat(cn, " = vnil();"));
            if (had_prev) sym[*s.opt1] = prev_sym;
            else sym.erase(*s.opt1);
            if (!had_prev_assigned) assigned.erase(*s.opt1);
            if (had_prev_moved) moved.insert(*s.opt1);
            else moved.erase(*s.opt1);
            if (had_prev_dropped) dropped.insert(*s.opt1);
            else dropped.erase(*s.opt1);
            if (had_prev_ub) ub_ints.insert(*s.opt1);
            else ub_ints.erase(*s.opt1);
            if (had_prev_ubf) ub_floats.insert(*s.opt1);
            else ub_floats.erase(*s.opt1);
            if (had_prev_vs) var_structs[*s.opt1] = prev_vs;
            else var_structs.erase(*s.opt1);
        }
        if (try_ret && !catch_ret) {
            // Keep `moved` / `dropped` from `catch`.
        } else if (!try_ret && catch_ret) {
            moved = std::move(try_moved);
            dropped = std::move(try_dropped);
        } else {
            moved = intersect_names(try_moved, moved);
            dropped = intersect_names(try_dropped, dropped);
        }
        ln(ind, cat(after_lbl, ":;"));
        ln(ind, "}");
        return;
    }
    case SK::Scope: {
        std::string sc = new_tmp("sc");
        ln(ind, cat("int64_t ", sc, " = vscope_begin().as.ival;"));
        scopes.push_back(sc);
        emit_err_check(ind);
        emit_block(view(s.body), ind);
        scopes.pop_back();
        ln(ind, cat("vscope_end(vint(", sc, "));"));
        emit_err_check(ind);
        return;
    }
    case SK::Spawn: {
        int code = fn_code(cat("clo:", clos.size()));
        // `spawn` snapshots the enclosing frame: capture every local or closure
        // binding referenced in the spawned block, even if assigned later in
        // the enclosing scope.
        std::set<std::string> capnames;
        Names mentioned;
        refs_in(view(s.body), mentioned);
        for (const auto &[nm, sy] : sym)
            if (mentioned.count(nm)) capnames.insert(nm);
        if (cap)
            for (const auto &nm : cap->second) capnames.insert(nm);
        clos.push_back({&no_params(), view(s.body), capnames, false, lookup_stmt_file(&s)});
        new_tmp("m");
        int n = tmp;
        ln(ind, cat("Map *_cm", n, " = vclosure_env_new(", capnames.size(), ");"));
        int64_t capture_slot = 0;
        for (const auto &var : capnames) {
            for (auto &l : capture(cat("_cm", n), var, capture_slot)) ln(ind, l);
            capture_slot++;
        }
        ln(ind, cat("Vec *_cb", n, " = vvec();"));
        ln(ind, cat("vec_push(_cb", n, ", vint(", code, "));"));
        ln(ind, cat("{ Val _cv = vstruct(_cm", n, "); vec_push(_cb", n, ", _cv); }"));
        ln(ind, cat("vspawn(vtagv(\"closure\", _cb", n, "));"));
        emit_err_check(ind);
        return;
    }
    case SK::Defer:
        emit_defer(*s.e1, ind, s.line);
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
            late.push_back({name, &f, lookup_fn_file(name, &f)});
        } else {
            std::string mangled;
            auto it = fn_alias.find(name);
            if (it == fn_alias.end()) {
                nested_seq++;
                mangled = cat(name, "@", nested_seq);
                fns[mangled].decl = &f;
                fn_alias[name] = mangled;
                late.push_back({mangled, &f, lookup_fn_file(name, &f)});
            } else {
                mangled = it->second;
            }
            bool have = false;
            for (const auto &l : late)
                if (l.name == mangled) have = true;
            if (!have) late.push_back({mangled, &f, lookup_fn_file(name, &f)});
        }
        return;
    }
    case SK::Extern:
    case SK::ExternStruct:
    default:
        return;
    }
}


std::pair<std::set<std::string>, std::set<std::string>> reclaim_names(
    const StmtList &body, const std::set<std::string> &params,
    const std::set<std::string> &heap) {
    (void)heap;
    std::map<std::string, int> let_cnt, for_iter_cnt, for_range_cnt;
    std::set<std::string> has_set, loop_let;
    std::function<void(const StmtList &, bool)> walk = [&](const StmtList &stmts, bool loop) {
        for (const Stmt *st : stmts) {
            switch (st->k) {
            case SK::Let:
            case SK::LetTmp: {
                if (st->a.find('.') == std::string::npos) {
                    let_cnt[st->a]++;
                    if (loop) loop_let.insert(st->a);
                }
                break;
            }
            case SK::Set: {
                if (st->a.find('.') == std::string::npos) {
                    has_set.insert(st->a);
                }
                break;
            }
            case SK::ForIter:
                for_iter_cnt[st->a]++;
                walk(view(st->body), true);
                break;
            case SK::ForRange:
                for_range_cnt[st->a]++;
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
            case SK::Block:
            case SK::Scope:
            case SK::Unsafe:
                walk(view(st->body), loop);
                break;
            case SK::Try:
                walk(view(st->body), loop);
                walk(view(st->body2), loop);
                break;
            default:
                break;   // a spawn body is its own frame, like a closure
            }
        }
    };
    walk(body, false);
    std::set<std::string> reclaim = has_set;
    std::set<std::string> relet;
    for (const auto &[n, cnt] : for_iter_cnt) {
        if (cnt > 0 && (params.count(n) || let_cnt[n] > 0 || has_set.count(n)))
            reclaim.insert(n);
    }
    for (const auto &[n, cnt] : for_range_cnt) {
        if (cnt > 0 && (params.count(n) || let_cnt[n] > 0 || has_set.count(n)))
            reclaim.insert(n);
    }
    for (const auto &[n, cnt] : let_cnt) {
        if (cnt <= 0) continue;
        if (cnt > 1 || loop_let.count(n) || params.count(n) || has_set.count(n) ||
            for_iter_cnt[n] > 0 || for_range_cnt[n] > 0) {
            relet.insert(n);
        }
    }
    return {reclaim, relet};
}


void Emitter::define_fn(const std::string &cn, const std::vector<Param> &params, const StmtList &body,
                        const std::set<std::string> *caps, const std::string *fastraw,
                        const std::string *fastkind, const std::string *fastname, bool positional,
                        bool ret_result, const std::string *self_struct) {
    std::vector<std::string> names;
    for (const auto &p : params) names.push_back(p.name);
    const std::string impl = positional ? cn + "_p" : cn;
    std::vector<std::string> arg_exprs;
    for (size_t i = 0; i < names.size(); i++)
        arg_exprs.push_back(positional ? cat("bf_arg_", i) : cat("args->items[", i, "]"));
    auto saved_params = cur_params;
    cur_params = std::set<std::string>(names.begin(), names.end());
    bool saved_ret_result = cur_ret_result;
    cur_ret_result = ret_result;
    auto saved_var_structs = var_structs;
    var_structs = compute_var_structs(params, body, self_struct);
    auto saved_scalar_vars = scalar_vars;
    scalar_vars.clear();
    static const std::set<std::string> scalar_types = {
        "bool", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "f32", "f64", "ptr"};
    for (const auto &p : params)
        if (p.ann && (scalar_types.count(*p.ann) || p.ann->rfind("ptr[", 0) == 0))
            scalar_vars.insert(p.name);
    auto saved_sym = sym;
    auto saved_cap = cap;
    auto saved_cap_slots = cap_slots;
    auto saved_dfb = dfb;
    auto saved_dfb_vars = dfb_vars;
    auto saved_fin = fin;
    auto saved_scopes = scopes;
    auto saved_iter_cleanups = iter_cleanups;
    size_t saved_err_scope_depth = err_scope_depth;
    size_t saved_err_iter_depth = err_iter_depth;
    // A top-level `let` is a file-scope Val; a function that reads it resolves
    // the name through the same table as its own locals. A name this closure
    // captured is the exception: it resolves through the capture map, because
    // the child holds the value the parent had at the launch, not a window onto
    // the global the parent may be writing.
    sym.clear();
    for (const auto &n : global_lets)
        if (!caps || !caps->count(n)) sym[n] = Sym{cname(n), Own::Copy, "", ""};
    auto saved_assigned = assigned;
    assigned.clear();
    cap.reset();
    cap_slots.clear();
    if (caps) {
        cap = std::make_pair(std::string("cap"), *caps);
        int64_t slot = 0;
        for (const auto &name : *caps) cap_slots[name] = slot++;
    }
    std::string dfbname = cat("__vdf_", cn);
    dfb.clear();
    dfb_vars.clear();
    scopes.clear();
    iter_cleanups.clear();
    err_scope_depth = 0;
    err_iter_depth = 0;
    fin = cat("__fin_", cn);
    const int fn_line = (!body.empty() && body[0]->line > 0) ? body[0]->line : 1;
    if (positional) {
        emit_dbg_loc(fn_line, cur_src_file);
        ln("", cat("static Val ", cn, "(Map *cap, Vec *args) {"));
        ln("  ", "(void)cap;");
        ln("  ", cat("if (!args || args->len != ", names.size(), ") { vraise(\"arity\"); return vunwind; }"));
        std::string call = cat(impl, "(cap");
        for (size_t i = 0; i < names.size(); i++) call += cat(", args->items[", i, "]");
        ln("  ", cat("return ", call, ");"));
        ln("", "}");
        std::string sig = cat("static Val ", impl, "(Map *cap");
        for (size_t i = 0; i < names.size(); i++) sig += cat(", Val bf_arg_", i);
        emit_dbg_loc(fn_line, cur_src_file);
        ln("", cat(sig, ") {"));
    } else {
        emit_dbg_loc(fn_line, cur_src_file);
        ln("", cat("static Val ", cn, "(Map *cap, Vec *args) {"));
        ln("  ", "(void)cap;");
        ln("  ", cat("if (!args || args->len != ", names.size(), ") { vraise(\"arity\"); return vunwind; }"));
    }
    ln("  ", "(void)cap;");
    if (fastraw) {
        const std::string &kind = *fastkind;
        // Raw-C fast functions return an integer C expression; inspect `fn_ret`
        // so `-> bool` wraps the result with `vbool(...)` rather than `vint(...)`.
        bool ret_bool = false;
        if (fastname) {
            auto ri = fn_ret.find(*fastname);
            if (ri != fn_ret.end() && ri->second && *ri->second == "bool") ret_bool = true;
        }
        const std::string vret =
            ret_bool ? "vbool((__r) != 0)"
                     : ((kind == "f64" || kind == "f64_dyn" || kind == "f64v") ? "vfloat(__r)" : "vint(__r)");
        const std::set<std::string> empty_bs;
        const std::set<std::string> &bv = (fastname && fastbool.count(*fastname)) ? fastbool[*fastname] : empty_bs;
        if (kind == "f64") {
            std::string checks, rawargs;
            for (size_t i = 0; i < names.size(); i++) {
                if (i) {
                    checks += " && ";
                    rawargs += ", ";
                }
                checks += cat("(", arg_exprs[i], ".tag == V_INT || ", arg_exprs[i], ".tag == V_FLOAT)");
                rawargs += cat("(", arg_exprs[i], ".tag == V_INT ? (double)", arg_exprs[i],
                               ".as.ival : ", arg_exprs[i], ".as.fval)");
            }
            ln("  ", cat("if (", checks.empty() ? "1" : checks, ") { double __r = ", *fastraw, "(", rawargs,
                         "); if (verr_pending()) return verr_leave(); return ", vret, "; }"));
        } else if (kind == "f64_dyn") {
            std::string checks, rawargs;
            for (size_t i = 0; i < names.size(); i++) {
                if (i) {
                    checks += " && ";
                    rawargs += ", ";
                }
                checks += cat(arg_exprs[i], ".tag == V_FLOAT");
                rawargs += cat(arg_exprs[i], ".as.fval");
            }
            ln("  ", cat("if (", checks.empty() ? "1" : checks, ") { double __r = ", *fastraw, "(", rawargs,
                         "); if (verr_pending()) return verr_leave(); return ", vret, "; }"));
        } else if (kind == "f64v") {
            const std::set<std::string> &vv = fastvec[*fastname];
            std::string checks, rawargs;
            for (size_t i = 0; i < names.size(); i++) {
                if (i) {
                    checks += " && ";
                    rawargs += ", ";
                }
                if (vv.count(names[i])) {
                    checks += cat(arg_exprs[i], ".tag == V_VEC && vvec_all_float(", arg_exprs[i], ".as.vec)");
                    rawargs += cat(arg_exprs[i], ".as.vec");
                } else {
                    checks += cat("(", arg_exprs[i], ".tag == V_INT || ", arg_exprs[i], ".tag == V_FLOAT)");
                    rawargs += cat("(", arg_exprs[i], ".tag == V_INT ? (double)", arg_exprs[i],
                                   ".as.ival : ", arg_exprs[i], ".as.fval)");
                }
            }
            ln("  ", cat("if (", checks.empty() ? "1" : checks, ") { double __r = ", *fastraw, "(", rawargs,
                         "); if (verr_pending()) return verr_leave(); return ", vret, "; }"));
        } else if (kind == "i64v" || kind == "i64v_dyn") {
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
                    checks += cat(arg_exprs[i], ".tag == V_VEC && vvec_all_int(", arg_exprs[i], ".as.vec)");
                    rawargs += cat(arg_exprs[i], ".as.vec");
                } else if (bv.count(names[i])) {
                    checks += cat(arg_exprs[i], ".tag == V_BOOL");
                    rawargs += cat("(int64_t)", arg_exprs[i], ".as.b");
                } else {
                    checks += cat(arg_exprs[i], ".tag == V_INT");
                    rawargs += cat(arg_exprs[i], ".as.ival");
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
                if (bv.count(names[i])) {
                    checks += cat(arg_exprs[i], ".tag == V_BOOL");
                    rawargs += cat("(int64_t)", arg_exprs[i], ".as.b");
                } else {
                    checks += cat(arg_exprs[i], ".tag == V_INT");
                    rawargs += cat(arg_exprs[i], ".as.ival");
                }
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
    const std::set<std::string> param_names(names.begin(), names.end());
    auto rr = reclaim_names(body, param_names, heap_returns());
    reclaim = rr.first;
    relet = rr.second;
    Names local_bindings;
    bound_in(body, local_bindings);
    for (const auto &g : global_lets) {
        if (caps_set.count(g) || param_names.count(g) || local_bindings.count(g)) continue;
        reclaim.erase(g);
        relet.erase(g);
    }
    for (const auto &c : caps_set) {
        reclaim.erase(c);
        relet.erase(c);
    }
    auto saved_moved = moved;
    auto saved_alias_temps = alias_temps;
    auto saved_shared = shared;
    auto saved_captured = captured;
    auto saved_dropped = dropped;
    auto saved_catch_cns = catch_cns;
    moved.clear();
    alias_temps.clear();
    shared.clear();
    // Per function: a name dropped in one function must still be released in
    // the next one that happens to use the same identifier.
    dropped.clear();
    captured.clear();
    catch_cns.clear();
    auto saved_locals = dfb_locals;
    auto saved_ub = ub_ints;
    auto saved_ubf = ub_floats;
    ub_ints = (caps == nullptr) ? compute_ub_ints(body, vars, param_names) : std::set<std::string>();
    std::set<std::string> rem_vars;
    for (const auto &v : vars)
        if (!ub_ints.count(v)) rem_vars.insert(v);
    ub_floats = (caps == nullptr) ? compute_ub_floats(body, rem_vars, param_names) : std::set<std::string>();
    for (const auto &u : ub_ints) {
        reclaim.erase(u);
        relet.erase(u);
    }
    for (const auto &u : ub_floats) {
        reclaim.erase(u);
        relet.erase(u);
    }
    auto saved_frame_arr_n = frame_arr_n;
    auto saved_frame_idx = frame_idx;
    frame_arr_n = 0;
    frame_idx.clear();
    dfb_locals.clear();
    std::vector<std::string> val_vars;
    for (const auto &n : vars) {
        if (!ub_ints.count(n) && !ub_floats.count(n)) {
            dfb_locals.insert(n);
            val_vars.push_back(n);
        }
    }
    for (const auto &n : vars) {
        if (ub_ints.count(n)) {
            ln("  ", cat("int64_t BRAT_UNUSED ub_", cname(n), " = 0;"));
        } else if (ub_floats.count(n)) {
            ln("  ", cat("double BRAT_UNUSED ubf_", cname(n), " = 0.0;"));
        }
    }
    if (val_vars.size() >= 24) {
        frame_arr_n = val_vars.size();
        ln("  ", cat("Val __fv[", frame_arr_n, "];"));
        ln("  ", cat("for (size_t __fi = 0; __fi < ", frame_arr_n, "; ++__fi) __fv[__fi] = vnil();"));
        for (size_t idx = 0; idx < val_vars.size(); idx++) {
            const std::string &n = val_vars[idx];
            Own own = (reclaim.count(n) || relet.count(n))
                          ? Own::Owned
                          : (param_names.count(n) ? Own::Borrow : Own::Copy);
            std::string fcn = cat("__fv[", idx, "]");
            sym[n] = Sym{fcn, own, "", ""};
            frame_idx[n] = static_cast<int>(idx);
        }
    } else {
        for (const auto &n : val_vars) {
            Own own = (reclaim.count(n) || relet.count(n))
                          ? Own::Owned
                          : (param_names.count(n) ? Own::Borrow : Own::Copy);
            sym[n] = Sym{cname(n), own, "", ""};
            ln("  ", cat("Val BRAT_UNUSED ", cname(n), " = vnil();"));
        }
        for (const auto &n : relet)
            if (vars.count(n) && !ub_ints.count(n) && !ub_floats.count(n))
                ln("  ", cat(slot_cn(n), " = vnil();"));
    }
    for (const auto &n : vars)
        if (caps_set.count(n) && !param_names.count(n))
            ln("  ", cat(slot_cn(n), " = (*vclosure_env_at(cap, ", cap_slots[n], "));"));
    for (size_t i = 0; i < names.size(); i++) {
        std::string init_expr = (reclaim.count(names[i]) || relet.count(names[i]))
            ? cat("vrc_inc(", arg_exprs[i], ")")
            : arg_exprs[i];
        ln("  ", cat(slot_cn(names[i]), " = ", init_expr, ";"));
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
            if (it == sym.end()) continue;
            ln("  ", cat("vrc_dec(", it->second.cn, ");"));
            ln("  ", cat(it->second.cn, " = vnil();"));
            alias_temps.erase(name);
        }
        for (const auto &ccn : catch_cns) {
            ln("  ", cat("vrelease(", ccn, ");"));
            ln("  ", cat(ccn, " = vnil();"));
        }
    }
    ln("  ", "bdepth_pop();");
    ln("  ", cat("vdfreset(", dfbname, ");"));
    ln("  ", "return __ret;");
    ln("", "}");
    ln("", "");
    cur_params = saved_params;
    scalar_vars = saved_scalar_vars;
    sym = saved_sym;
    cap = saved_cap;
    cap_slots = saved_cap_slots;
    dfb = saved_dfb;
    dfb_vars = std::move(saved_dfb_vars);
    fin = saved_fin;
    scopes = saved_scopes;
    iter_cleanups = std::move(saved_iter_cleanups);
    err_scope_depth = saved_err_scope_depth;
    err_iter_depth = saved_err_iter_depth;
    dfb_locals = saved_locals;
    ub_ints = saved_ub;
    ub_floats = saved_ubf;
    assigned = saved_assigned;
    reclaim = saved_reclaim;
    relet = saved_relet;
    moved = std::move(saved_moved);
    alias_temps = std::move(saved_alias_temps);
    shared = saved_shared;
    captured = saved_captured;
    dropped = saved_dropped;
    catch_cns = saved_catch_cns;
    frame_arr_n = saved_frame_arr_n;
    frame_idx = saved_frame_idx;
    cur_ret_result = saved_ret_result;
    var_structs = std::move(saved_var_structs);
}

int Emitter::struct_field_slot(const std::string &sname, const std::string &fname) const {
    auto it = native_structs.find(sname);
    if (it == native_structs.end() || !it->second) return -1;
    const auto &flds = *it->second;
    for (size_t i = 0; i < flds.size(); i++) {
        if (flds[i].name == fname) return static_cast<int>(i);
    }
    return -1;
}

std::optional<std::string> Emitter::struct_field_ann(const std::string &sname,
                                                     const std::string &fname) const {
    auto it = native_structs.find(sname);
    if (it == native_structs.end() || !it->second) return std::nullopt;
    for (const auto &f : *it->second) {
        if (f.name == fname) return f.ann;
    }
    return std::nullopt;
}

int Emitter::canonical_struct_shape(const std::string &sname) {
    auto it = native_structs.find(sname);
    std::vector<std::string> fnames;
    if (it != native_structs.end() && it->second) {
        fnames.reserve(it->second->size());
        for (const auto &f : *it->second) fnames.push_back(f.name);
    }
    return shape_id(sname, fnames);
}

std::string Emitter::infer_expr_struct(const Expr *e,
                                       const std::map<std::string, std::string> &known) const {
    if (!e) return "";
    switch (e->k) {
    case EK::StructLit:
        return native_structs.count(e->a) ? e->a : "";
    case EK::Var: {
        auto it = known.find(e->a);
        return it != known.end() ? it->second : "";
    }
    case EK::Move:
        return e->kids.empty() ? "" : infer_expr_struct(e->kids[0].get(), known);
    case EK::Call: {
        auto rit = fn_ret.find(fn_key(e->a));
        if (rit != fn_ret.end() && rit->second && native_structs.count(*rit->second))
            return *rit->second;
        return "";
    }
    case EK::MCall: {
        if (e->kids.empty()) return "";
        std::string ot = infer_expr_struct(e->kids[0].get(), known);
        if (!ot.empty()) {
            auto rit = fn_ret.find(ot + "." + e->b);
            if (rit != fn_ret.end() && rit->second && native_structs.count(*rit->second))
                return *rit->second;
        }
        return "";
    }
    case EK::Field: {
        if (e->kids.empty()) return "";
        std::string ot = infer_expr_struct(e->kids[0].get(), known);
        if (!ot.empty()) {
            auto fann = struct_field_ann(ot, e->b);
            if (fann && native_structs.count(*fann)) return *fann;
        }
        return "";
    }
    default:
        return "";
    }
}

std::map<std::string, std::string> Emitter::compute_var_structs(const std::vector<Param> &params,
                                                                const StmtList &body,
                                                                const std::string *self_struct) const {
    if (native_structs.empty()) return {};
    std::map<std::string, std::string> known;
    std::set<std::string> poisoned = global_lets;
    for (const auto &p : params) {
        if (poisoned.count(p.name)) continue;
        if (p.name == "self" && self_struct && native_structs.count(*self_struct)) {
            known[p.name] = *self_struct;
        } else if (p.ann && native_structs.count(*p.ann)) {
            known[p.name] = *p.ann;
        }
    }
    std::function<void(const StmtList &)> scan = [&](const StmtList &stmts) {
        for (const Stmt *sp : stmts) {
            if (!sp) continue;
            const Stmt &s = *sp;
            switch (s.k) {
            case SK::Fn: case SK::ExportFn: case SK::Impl: case SK::Trait:
            case SK::Enum: case SK::Struct:
                break;
            case SK::Pub:
                if (!s.body.empty()) scan(view(s.body));
                break;
            case SK::Let:
            case SK::LetTmp: {
                if (s.a.find('.') != std::string::npos) {
                    std::string root = s.a.substr(0, s.a.find('.'));
                    poisoned.insert(root);
                    known.erase(root);
                    break;
                }
                std::string st = (s.ann && native_structs.count(*s.ann))
                                     ? *s.ann
                                     : infer_expr_struct(s.e1.get(), known);
                if (st.empty() || poisoned.count(s.a) ||
                    (known.count(s.a) && known[s.a] != st)) {
                    poisoned.insert(s.a);
                    known.erase(s.a);
                } else {
                    known[s.a] = st;
                }
                break;
            }
            case SK::Set: {
                if (s.a.find('.') != std::string::npos) break;
                std::string st = infer_expr_struct(s.e1.get(), known);
                if (st.empty() || poisoned.count(s.a) ||
                    (known.count(s.a) && known[s.a] != st)) {
                    poisoned.insert(s.a);
                    known.erase(s.a);
                } else {
                    known[s.a] = st;
                }
                break;
            }
            case SK::ForRange:
            case SK::ForIter:
                if (!s.a.empty()) {
                    poisoned.insert(s.a);
                    known.erase(s.a);
                }
                scan(view(s.body));
                break;
            case SK::Try:
                if (s.opt1 && !s.opt1->empty()) {
                    poisoned.insert(*s.opt1);
                    known.erase(*s.opt1);
                }
                scan(view(s.body));
                scan(view(s.body2));
                break;
            default:
                if (!s.body.empty()) scan(view(s.body));
                if (!s.body2.empty()) scan(view(s.body2));
                if (s.els) {
                    StmtList el{s.els.get()};
                    scan(el);
                }
                break;
            }
        }
    };
    scan(body);
    scan(body);
    for (const auto &p : poisoned) known.erase(p);
    return known;
}

bool Emitter::sra_round(const std::string &fn_name, FnDecl &decl) {
    if (native_structs.empty()) return false;
    std::set<std::string> banned = global_lets;
    for (const auto &p : decl.params) banned.insert(p.name);
    std::map<std::string, int> let_count;
    std::map<std::string, std::string> cand_type;
    std::map<std::string, int> cand_line;
    std::map<std::string, std::set<std::string>> used_fields;

    auto is_full_struct_lit = [&](const Expr *e) -> std::string {
        if (!e || e->k != EK::StructLit) return "";
        auto it = native_structs.find(e->a);
        if (it == native_structs.end() || !it->second || it->second->empty()) return "";
        if (e->names.size() != it->second->size() || e->kids.size() != e->names.size()) return "";
        std::set<std::string> seen;
        for (const auto &nm : e->names) {
            if (!seen.insert(nm).second || struct_field_slot(e->a, nm) < 0) return "";
        }
        return e->a;
    };

    std::function<void(const Expr *, std::set<std::string> &)> all_mentions_expr;
    std::function<void(const std::vector<StmtP> &, std::set<std::string> &)> all_mentions_stmts;
    all_mentions_expr = [&](const Expr *e, std::set<std::string> &acc) {
        if (!e) return;
        if (e->k == EK::Var) acc.insert(e->a);
        else if (e->k == EK::Call && !e->a.empty())
            acc.insert(e->a.substr(0, e->a.find('.')));
        else if (e->k == EK::Str) {
            for (const auto &[ck, cv] : split_interp(e->a))
                if (ck == "var") acc.insert(cv.substr(0, cv.find('.')));
        }
        for (const auto &k : e->kids) all_mentions_expr(k.get(), acc);
        if (e->fn) {
            for (const auto &p : e->fn->params) all_mentions_expr(p.def.get(), acc);
            all_mentions_stmts(e->fn->body, acc);
        }
    };
    std::function<void(const Stmt *, std::set<std::string> &)> all_mentions_one =
        [&](const Stmt *sp, std::set<std::string> &acc) {
        if (!sp) return;
        if (sp->k == SK::Let || sp->k == SK::LetTmp || sp->k == SK::Set)
            acc.insert(sp->a.substr(0, sp->a.find('.')));
        all_mentions_expr(sp->e1.get(), acc);
        all_mentions_expr(sp->e2.get(), acc);
        all_mentions_expr(sp->e3.get(), acc);
        all_mentions_stmts(sp->body, acc);
        all_mentions_stmts(sp->body2, acc);
        if (sp->els) all_mentions_one(sp->els.get(), acc);
        for (const auto &f : sp->fns) {
            for (const auto &p : f.params) all_mentions_expr(p.def.get(), acc);
            all_mentions_stmts(f.body, acc);
        }
    };
    all_mentions_stmts = [&](const std::vector<StmtP> &stmts, std::set<std::string> &acc) {
        for (const auto &sp : stmts) all_mentions_one(sp.get(), acc);
    };

    auto plain_lit_str = [](const Expr *e, std::string &out) -> bool {
        if (!e || e->k != EK::Str) return false;
        auto parts = split_interp(e->a);
        if (parts.empty()) {
            out.clear();
            return true;
        }
        if (parts.size() == 1 && parts[0].first == "lit") {
            out = parts[0].second;
            return true;
        }
        return false;
    };

    std::set<std::string> copy_called;
    std::map<std::string, std::set<std::string>> copy_deps;
    std::map<std::string, std::string> copy_ann;
    auto is_var_copy = [](const Expr *e, std::string &src_out) -> bool {
        if (e && e->k == EK::MCall && e->b == "copy" && e->kids.size() == 1 &&
            e->kids[0] && e->kids[0]->k == EK::Var) {
            src_out = e->kids[0]->a;
            return true;
        }
        return false;
    };
    std::function<void(const Expr *)> scan_expr = [&](const Expr *e) {
        if (!e) return;
        switch (e->k) {
        case EK::Var:
            banned.insert(e->a);
            return;
        case EK::Field:
            if (!e->kids.empty() && e->kids[0] && e->kids[0]->k == EK::Var) {
                used_fields[e->kids[0]->a].insert(e->b);
                return;
            }
            for (const auto &k : e->kids) scan_expr(k.get());
            return;
        case EK::MCall:
            if (e->b == "copy" && e->kids.size() == 1 && e->kids[0] &&
                e->kids[0]->k == EK::Var) {
                copy_called.insert(e->kids[0]->a);
                return;
            }
            for (const auto &k : e->kids) scan_expr(k.get());
            return;
        case EK::Move:
            if (!e->kids.empty() && e->kids[0] && e->kids[0]->k == EK::Field &&
                !e->kids[0]->kids.empty() && e->kids[0]->kids[0] &&
                e->kids[0]->kids[0]->k == EK::Var) {
                used_fields[e->kids[0]->kids[0]->a].insert(e->kids[0]->b);
                return;
            }
            for (const auto &k : e->kids) scan_expr(k.get());
            return;
        case EK::Call: {
            std::string lit;
            if ((e->a == "__stag_is" || e->a == "__shas") && e->kids.size() == 2 &&
                e->kids[0] && e->kids[0]->k == EK::Var && plain_lit_str(e->kids[1].get(), lit)) {
                return;
            }
            if (!e->a.empty()) banned.insert(e->a.substr(0, e->a.find('.')));
            for (const auto &k : e->kids) scan_expr(k.get());
            return;
        }
        case EK::Str:
            for (const auto &[ck, cv] : split_interp(e->a)) {
                if (ck != "var") continue;
                size_t dot = cv.find('.');
                if (dot == std::string::npos) {
                    banned.insert(cv);
                } else {
                    std::string root = cv.substr(0, dot);
                    std::string rest = cv.substr(dot + 1);
                    std::string fld = rest.substr(0, rest.find('.'));
                    used_fields[root].insert(fld);
                }
            }
            return;
        case EK::Closure:
            if (e->fn) {
                auto sub = closure_free_vars(*e->fn);
                if (e->fn->name) sub.erase(*e->fn->name);
                for (const auto &x : sub) banned.insert(x);
            }
            return;
        case EK::ParFor:
            if (e->kids.size() >= 2) {
                scan_expr(e->kids[0].get());
                scan_expr(e->kids[1].get());
            }
            if (!e->b.empty()) banned.insert(e->b);
            if (e->fn) {
                auto sub = closure_free_vars(*e->fn);
                for (const auto &x : sub) banned.insert(x);
            }
            return;
        default:
            for (const auto &k : e->kids) scan_expr(k.get());
            return;
        }
    };

    std::function<void(const Stmt &)> scan_stmt;
    auto scan_stmts = [&](const std::vector<StmtP> &stmts) {
        for (const auto &sp : stmts)
            if (sp) scan_stmt(*sp);
    };
    scan_stmt = [&](const Stmt &s) {
        switch (s.k) {
        case SK::Fn:
        case SK::ExportFn:
        case SK::Impl: {
            std::set<std::string> m;
            for (const auto &f : s.fns) {
                for (const auto &p : f.params) all_mentions_expr(p.def.get(), m);
                all_mentions_stmts(f.body, m);
            }
            for (const auto &x : m) banned.insert(x);
            return;
        }
        case SK::Spawn:
        case SK::Defer: {
            std::set<std::string> m;
            all_mentions_expr(s.e1.get(), m);
            all_mentions_stmts(s.body, m);
            for (const auto &x : m) banned.insert(x);
            return;
        }
        case SK::Let:
        case SK::LetTmp: {
            if (s.a.find('.') != std::string::npos) {
                banned.insert(s.a.substr(0, s.a.find('.')));
                scan_expr(s.e1.get());
                return;
            }
            let_count[s.a]++;
            if (let_count[s.a] > 1) banned.insert(s.a);
            std::string st = is_full_struct_lit(s.e1.get());
            std::string csrc;
            if (!st.empty() && (!s.ann || *s.ann == st)) {
                if (cand_type.count(s.a) && cand_type[s.a] != st) banned.insert(s.a);
                else {
                    cand_type[s.a] = st;
                    if (!cand_line.count(s.a)) cand_line[s.a] = static_cast<int>(s.line);
                }
            } else if (is_var_copy(s.e1.get(), csrc) && csrc != s.a) {
                copy_called.insert(csrc);
                copy_deps[s.a].insert(csrc);
                if (s.ann) copy_ann[s.a] = *s.ann;
                if (!cand_line.count(s.a)) cand_line[s.a] = static_cast<int>(s.line);
            } else {
                banned.insert(s.a);
            }
            std::set<std::string> self_m;
            all_mentions_expr(s.e1.get(), self_m);
            if (self_m.count(s.a)) banned.insert(s.a);
            if (s.e1 && s.e1->k == EK::StructLit && !st.empty()) {
                for (const auto &k : s.e1->kids) scan_expr(k.get());
            } else {
                scan_expr(s.e1.get());
            }
            return;
        }
        case SK::Set: {
            if (s.a.find('.') != std::string::npos) {
                banned.insert(s.a.substr(0, s.a.find('.')));
                scan_expr(s.e1.get());
                return;
            }
            std::string st = is_full_struct_lit(s.e1.get());
            std::string csrc;
            if (!st.empty()) {
                if (cand_type.count(s.a) && cand_type[s.a] != st) banned.insert(s.a);
                else cand_type[s.a] = st;
                for (const auto &k : s.e1->kids) scan_expr(k.get());
            } else if (is_var_copy(s.e1.get(), csrc)) {
                copy_called.insert(csrc);
                copy_deps[s.a].insert(csrc);
                scan_expr(s.e1.get());
            } else {
                banned.insert(s.a);
                scan_expr(s.e1.get());
            }
            return;
        }
        case SK::SetField:
            if (s.e1 && s.e1->k == EK::Var) {
                used_fields[s.e1->a].insert(s.a);
            } else {
                scan_expr(s.e1.get());
            }
            scan_expr(s.e2.get());
            return;
        case SK::ForRange:
        case SK::ForIter:
            if (!s.a.empty()) banned.insert(s.a);
            scan_expr(s.e1.get());
            scan_expr(s.e2.get());
            scan_stmts(s.body);
            return;
        case SK::Try:
            if (s.opt1 && !s.opt1->empty()) banned.insert(*s.opt1);
            scan_stmts(s.body);
            scan_stmts(s.body2);
            return;
        default:
            scan_expr(s.e1.get());
            scan_expr(s.e2.get());
            scan_expr(s.e3.get());
            scan_stmts(s.body);
            scan_stmts(s.body2);
            if (s.els) scan_stmt(*s.els);
            return;
        }
    };
    scan_stmts(decl.body);

    for (bool prog = true; prog;) {
        prog = false;
        for (const auto &[dst, srcs] : copy_deps) {
            for (const auto &src : srcs) {
                auto sit = cand_type.find(src);
                if (sit == cand_type.end()) continue;
                const std::string &st = sit->second;
                if (copy_ann.count(dst) && copy_ann[dst] != st) {
                    banned.insert(dst);
                    continue;
                }
                auto dit = cand_type.find(dst);
                if (dit == cand_type.end()) {
                    cand_type[dst] = st;
                    prog = true;
                } else if (dit->second != st) {
                    banned.insert(dst);
                }
            }
        }
    }

    std::set<std::string> approved;
    for (const auto &[v, st] : cand_type) {
        if (banned.count(v) || let_count[v] != 1) continue;
        if (copy_called.count(v) && mtab.count(std::make_pair(st, std::string("copy")))) continue;
        bool fields_ok = true;
        auto uf = used_fields.find(v);
        if (uf != used_fields.end()) {
            for (const auto &fnm : uf->second) {
                if (struct_field_slot(st, fnm) < 0) {
                    fields_ok = false;
                    break;
                }
            }
        }
        if (fields_ok) approved.insert(v);
    }
    for (bool dep_changed = true; dep_changed;) {
        dep_changed = false;
        for (const auto &[dst, srcs] : copy_deps) {
            if (!approved.count(dst)) continue;
            for (const auto &src : srcs) {
                if (!approved.count(src)) {
                    approved.erase(dst);
                    dep_changed = true;
                    break;
                }
            }
        }
    }
    if (approved.empty()) return false;

    auto clean_ident = [](const std::string &s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') r.push_back(c);
            else r.push_back('_');
        }
        return r;
    };

    std::map<std::string, std::map<std::string, std::string>> sra_names;
    for (const auto &v : approved) {
        const std::string &st = cand_type[v];
        int id = sra_seq++;
        for (const auto &fld : *native_structs.at(st)) {
            sra_names[v][fld.name] = cat("_sra_", id, "_", clean_ident(v), "_", fld.name);
        }
        if (opt >= 2 && proofs && v.rfind("_sra_", 0) != 0) {
            proofs->push_back(ir::Proof{
                fn_name, cand_line[v],
                cat("sra: scalarized non-escaping struct `", st, "` local `", v, "` (",
                    static_cast<int>(native_structs.at(st)->size()), " fields)")});
        }
    }

    std::function<ExprP(ExprP)> rw_expr = [&](ExprP e) -> ExprP {
        if (!e) return nullptr;
        if (e->k == EK::Field && !e->kids.empty() && e->kids[0] && e->kids[0]->k == EK::Var &&
            approved.count(e->kids[0]->a)) {
            return mk_var(sra_names[e->kids[0]->a][e->b]);
        }
        if (e->k == EK::MCall && e->b == "copy" && e->kids.size() == 1 && e->kids[0] &&
            e->kids[0]->k == EK::Var && approved.count(e->kids[0]->a)) {
            const std::string &v = e->kids[0]->a;
            const std::string &st = cand_type[v];
            auto lit = std::make_unique<Expr>();
            lit->k = EK::StructLit;
            lit->a = st;
            for (const auto &fld : *native_structs.at(st)) {
                lit->names.push_back(fld.name);
                lit->kids.push_back(mk_var(sra_names[v][fld.name]));
            }
            return lit;
        }
        if (e->k == EK::Move && !e->kids.empty() && e->kids[0] && e->kids[0]->k == EK::Field &&
            !e->kids[0]->kids.empty() && e->kids[0]->kids[0] &&
            e->kids[0]->kids[0]->k == EK::Var && approved.count(e->kids[0]->kids[0]->a)) {
            return mk_move(mk_var(sra_names[e->kids[0]->kids[0]->a][e->kids[0]->b]));
        }
        if (e->k == EK::Call && (e->a == "__stag_is" || e->a == "__shas") &&
            e->kids.size() == 2 && e->kids[0] && e->kids[0]->k == EK::Var &&
            approved.count(e->kids[0]->a)) {
            std::string lit;
            if (plain_lit_str(e->kids[1].get(), lit)) {
                const std::string &st = cand_type[e->kids[0]->a];
                bool ans = (e->a == "__stag_is") ? (st == lit) : (struct_field_slot(st, lit) >= 0);
                return mk_bool(ans);
            }
        }
        if (e->k == EK::Str) {
            auto parts = split_interp(e->a);
            bool changed = false;
            std::string rebuilt;
            for (const auto &[ck, cv] : parts) {
                if (ck == "lit") {
                    rebuilt += cv;
                } else {
                    size_t dot = cv.find('.');
                    if (dot != std::string::npos) {
                        std::string root = cv.substr(0, dot);
                        if (approved.count(root)) {
                            std::string rest = cv.substr(dot + 1);
                            size_t d2 = rest.find('.');
                            std::string fld = rest.substr(0, d2);
                            std::string tail = d2 == std::string::npos ? "" : rest.substr(d2);
                            rebuilt += "{" + sra_names[root][fld] + tail + "}";
                            changed = true;
                            continue;
                        }
                    }
                    rebuilt += "{" + cv + "}";
                }
            }
            if (changed) e->a = std::move(rebuilt);
            return e;
        }
        if (e->k == EK::Closure || e->k == EK::ParFor) {
            for (auto &k : e->kids) k = rw_expr(std::move(k));
            return e;
        }
        for (auto &k : e->kids) k = rw_expr(std::move(k));
        return e;
    };

    std::function<void(std::vector<StmtP> &)> rw_stmts;
    std::function<StmtP(StmtP)> rw_single = [&](StmtP s) -> StmtP {
        if (!s) return nullptr;
        if (s->k == SK::Block || s->k == SK::Unsafe) {
            rw_stmts(s->body);
            return s;
        }
        if (s->k == SK::If) {
            s->e1 = rw_expr(std::move(s->e1));
            rw_stmts(s->body);
            if (s->els) s->els = rw_single(std::move(s->els));
            return s;
        }
        std::vector<StmtP> one;
        one.push_back(std::move(s));
        rw_stmts(one);
        if (one.size() == 1) return std::move(one[0]);
        return mk_block(std::move(one));
    };

    rw_stmts = [&](std::vector<StmtP> &stmts) {
        std::vector<StmtP> out_s;
        out_s.reserve(stmts.size());
        for (auto &sp : stmts) {
            if (!sp) continue;
            if (sp->k == SK::Fn || sp->k == SK::ExportFn || sp->k == SK::Impl ||
                sp->k == SK::Spawn || sp->k == SK::Defer) {
                out_s.push_back(std::move(sp));
                continue;
            }
            if ((sp->k == SK::Let || sp->k == SK::LetTmp) && approved.count(sp->a)) {
                const std::string &st = cand_type[sp->a];
                if (sp->e1 && sp->e1->k == EK::MCall) sp->e1 = rw_expr(std::move(sp->e1));
                Expr *lit = sp->e1.get();
                for (size_t k = 0; k < lit->names.size(); k++) {
                    const std::string &fnm = lit->names[k];
                    ExprP init = rw_expr(std::move(lit->kids[k]));
                    auto fann = struct_field_ann(st, fnm);
                    StmtP ls = mk_let(sra_names[sp->a][fnm], std::move(init), true, false, fann);
                    ls->line = sp->line;
                    out_s.push_back(std::move(ls));
                }
                continue;
            }
            if (sp->k == SK::Set && approved.count(sp->a)) {
                const std::string &st = cand_type[sp->a];
                if (sp->e1 && sp->e1->k == EK::MCall) sp->e1 = rw_expr(std::move(sp->e1));
                Expr *lit = sp->e1.get();
                const size_t klen = lit->names.size();
                if (klen == 1) {
                    const std::string &fnm = lit->names[0];
                    auto ns = std::make_unique<Stmt>();
                    ns->k = SK::Set;
                    ns->line = sp->line;
                    ns->a = sra_names[sp->a][fnm];
                    ns->e1 = rw_expr(std::move(lit->kids[0]));
                    out_s.push_back(std::move(ns));
                } else {
                    std::vector<std::string> tnames(klen);
                    for (size_t k = 0; k < klen; k++) {
                        const std::string &fnm = lit->names[k];
                        tnames[k] = cat("_srat_", sra_seq++, "_", fnm);
                        ExprP rhs = rw_expr(std::move(lit->kids[k]));
                        StmtP lt = mk_let(tnames[k], std::move(rhs), false, false,
                                          struct_field_ann(st, fnm));
                        lt->line = sp->line;
                        out_s.push_back(std::move(lt));
                    }
                    for (size_t k = 0; k < klen; k++) {
                        const std::string &fnm = lit->names[k];
                        auto ns = std::make_unique<Stmt>();
                        ns->k = SK::Set;
                        ns->line = sp->line;
                        ns->a = sra_names[sp->a][fnm];
                        ns->e1 = mk_var(tnames[k]);
                        out_s.push_back(std::move(ns));
                    }
                }
                continue;
            }
            if (sp->k == SK::SetField && sp->e1 && sp->e1->k == EK::Var &&
                approved.count(sp->e1->a)) {
                auto ns = std::make_unique<Stmt>();
                ns->k = SK::Set;
                ns->line = sp->line;
                ns->a = sra_names[sp->e1->a][sp->a];
                ns->e1 = rw_expr(std::move(sp->e2));
                out_s.push_back(std::move(ns));
                continue;
            }
            sp->e1 = rw_expr(std::move(sp->e1));
            sp->e2 = rw_expr(std::move(sp->e2));
            sp->e3 = rw_expr(std::move(sp->e3));
            rw_stmts(sp->body);
            rw_stmts(sp->body2);
            if (sp->els) sp->els = rw_single(std::move(sp->els));
            out_s.push_back(std::move(sp));
        }
        stmts = std::move(out_s);
    };
    rw_stmts(decl.body);
    return true;
}

bool Emitter::sra_rewrite_decl(const std::string &fn_name, FnDecl &decl) {
    bool any = false;
    for (int round = 0; round < 4; round++) {
        if (!sra_round(fn_name, decl)) break;
        any = true;
    }
    std::function<void(std::vector<StmtP> &)> walk_clos_stmts;
    std::function<void(Expr *)> walk_clos_expr = [&](Expr *e) {
        if (!e) return;
        for (auto &k : e->kids) walk_clos_expr(k.get());
        if (e->k == EK::Closure && e->fn) {
            if (sra_rewrite_decl(fn_name, *e->fn)) any = true;
        }
    };
    walk_clos_stmts = [&](std::vector<StmtP> &stmts) {
        for (auto &sp : stmts) {
            if (!sp) continue;
            if (sp->k == SK::Fn || sp->k == SK::ExportFn || sp->k == SK::Impl) continue;
            walk_clos_expr(sp->e1.get());
            walk_clos_expr(sp->e2.get());
            walk_clos_expr(sp->e3.get());
            walk_clos_stmts(sp->body);
            walk_clos_stmts(sp->body2);
            if (sp->els) {
                Stmt *cur = sp->els.get();
                while (cur) {
                    walk_clos_expr(cur->e1.get());
                    walk_clos_expr(cur->e2.get());
                    walk_clos_expr(cur->e3.get());
                    walk_clos_stmts(cur->body);
                    walk_clos_stmts(cur->body2);
                    cur = cur->els.get();
                }
            }
        }
    };
    walk_clos_stmts(decl.body);
    return any;
}

static void map_cloned_nested_expr(const Expr *oe, const Expr *ne,
                                   std::map<const FnDecl *, const FnDecl *> &rep);

static void map_cloned_nested_stmts(const std::vector<StmtP> &ob,
                                    const std::vector<StmtP> &nb,
                                    std::map<const FnDecl *, const FnDecl *> &rep) {
    for (size_t i = 0; i < ob.size() && i < nb.size(); i++) {
        if (!ob[i] || !nb[i]) continue;
        const Stmt &os = *ob[i];
        const Stmt &ns = *nb[i];
        map_cloned_nested_expr(os.e1.get(), ns.e1.get(), rep);
        map_cloned_nested_expr(os.e2.get(), ns.e2.get(), rep);
        map_cloned_nested_expr(os.e3.get(), ns.e3.get(), rep);
        map_cloned_nested_stmts(os.body, ns.body, rep);
        map_cloned_nested_stmts(os.body2, ns.body2, rep);
        if (os.els && ns.els) {
            std::vector<StmtP> tmp_o, tmp_n;
            for (const Stmt *co = os.els.get(), *cn = ns.els.get(); co && cn;
                 co = co->els.get(), cn = cn->els.get()) {
                map_cloned_nested_expr(co->e1.get(), cn->e1.get(), rep);
                map_cloned_nested_expr(co->e2.get(), cn->e2.get(), rep);
                map_cloned_nested_expr(co->e3.get(), cn->e3.get(), rep);
                map_cloned_nested_stmts(co->body, cn->body, rep);
                map_cloned_nested_stmts(co->body2, cn->body2, rep);
            }
        }
        for (size_t j = 0; j < os.fns.size() && j < ns.fns.size(); j++) {
            rep[&os.fns[j]] = &ns.fns[j];
            map_cloned_nested_stmts(os.fns[j].body, ns.fns[j].body, rep);
        }
    }
}

static void map_cloned_nested_expr(const Expr *oe, const Expr *ne,
                                   std::map<const FnDecl *, const FnDecl *> &rep) {
    if (!oe || !ne) return;
    for (size_t i = 0; i < oe->kids.size() && i < ne->kids.size(); i++)
        map_cloned_nested_expr(oe->kids[i].get(), ne->kids[i].get(), rep);
    if (oe->fn && ne->fn)
        map_cloned_nested_stmts(oe->fn->body, ne->fn->body, rep);
}

namespace {

struct ShadowRenamer {
    int seq = 0;
    bool changed = false;

    void walk_expr(Expr *e, const std::map<std::string, std::string> &renames,
                   std::set<std::string> &seen) {
        if (!e) return;
        if (e->k == EK::Var || e->k == EK::Call) {
            auto it = renames.find(e->a);
            if (it != renames.end()) {
                e->a = it->second;
                changed = true;
            }
        }
        for (auto &k : e->kids) walk_expr(k.get(), renames, seen);
        if (e->fn) {
            auto sub_ren = renames;
            auto sub_seen = seen;
            for (auto &p : e->fn->params) {
                if (p.def) walk_expr(p.def.get(), renames, seen);
                sub_ren.erase(p.name);
                if (!p.name.empty() && p.name != "_") sub_seen.insert(p.name);
            }
            walk_block(e->fn->body, sub_ren, sub_seen, false);
        }
    }

    void walk_stmt(Stmt &s, std::map<std::string, std::string> &renames,
                   std::map<std::string, std::string> &block_map,
                   std::set<std::string> &seen, bool nested) {
        switch (s.k) {
        case SK::Let:
        case SK::LetTmp: {
            walk_expr(s.e1.get(), renames, seen);
            if (!s.a.empty() && s.a != "_") {
                if (nested) {
                    auto bit = block_map.find(s.a);
                    if (bit != block_map.end()) {
                        renames[s.a] = bit->second;
                        if (s.a != bit->second) {
                            s.a = bit->second;
                            changed = true;
                        }
                    } else if (seen.count(s.a)) {
                        std::string fresh = s.a + "$sh" + std::to_string(++seq);
                        block_map[s.a] = fresh;
                        renames[s.a] = fresh;
                        seen.insert(fresh);
                        s.a = fresh;
                        changed = true;
                    } else {
                        seen.insert(s.a);
                        block_map[s.a] = s.a;
                        renames.erase(s.a);
                    }
                } else {
                    seen.insert(s.a);
                }
            }
            break;
        }
        case SK::Set: {
            walk_expr(s.e1.get(), renames, seen);
            auto dot = s.a.find('.');
            std::string head = dot == std::string::npos ? s.a : s.a.substr(0, dot);
            auto it = renames.find(head);
            if (it != renames.end()) {
                s.a = it->second + (dot == std::string::npos ? "" : s.a.substr(dot));
                changed = true;
            }
            break;
        }
        case SK::SetIdx:
            walk_expr(s.e1.get(), renames, seen);
            walk_expr(s.e2.get(), renames, seen);
            walk_expr(s.e3.get(), renames, seen);
            break;
        case SK::SetField:
            walk_expr(s.e1.get(), renames, seen);
            walk_expr(s.e2.get(), renames, seen);
            break;
        case SK::Expr:
        case SK::Return:
        case SK::Defer:
            walk_expr(s.e1.get(), renames, seen);
            break;
        case SK::Block:
        case SK::Scope:
        case SK::Unsafe:
        case SK::Spawn: {
            auto sub_ren = renames;
            walk_block(s.body, sub_ren, seen, true);
            break;
        }
        case SK::If: {
            walk_expr(s.e1.get(), renames, seen);
            {
                auto sub_ren = renames;
                walk_block(s.body, sub_ren, seen, true);
            }
            if (s.els) {
                if (s.els->k == SK::Block) {
                    auto sub_ren = renames;
                    walk_block(s.els->body, sub_ren, seen, true);
                } else {
                    auto sub_ren = renames;
                    std::map<std::string, std::string> sub_bmap;
                    walk_stmt(*s.els, sub_ren, sub_bmap, seen, true);
                }
            }
            break;
        }
        case SK::While: {
            walk_expr(s.e1.get(), renames, seen);
            auto sub_ren = renames;
            walk_block(s.body, sub_ren, seen, true);
            break;
        }
        case SK::ForRange:
        case SK::ForIter: {
            walk_expr(s.e1.get(), renames, seen);
            walk_expr(s.e2.get(), renames, seen);
            auto sub_ren = renames;
            if (!s.a.empty() && s.a != "_") {
                if (seen.count(s.a)) {
                    std::string fresh = s.a + "$sh" + std::to_string(++seq);
                    sub_ren[s.a] = fresh;
                    seen.insert(fresh);
                    s.a = fresh;
                    changed = true;
                } else {
                    seen.insert(s.a);
                    sub_ren.erase(s.a);
                }
            }
            walk_block(s.body, sub_ren, seen, true);
            break;
        }
        case SK::Try: {
            {
                auto sub_ren = renames;
                walk_block(s.body, sub_ren, seen, true);
            }
            {
                auto catch_ren = renames;
                if (s.opt1 && !s.opt1->empty() && *s.opt1 != "_") {
                    if (seen.count(*s.opt1)) {
                        std::string fresh = *s.opt1 + "$sh" + std::to_string(++seq);
                        catch_ren[*s.opt1] = fresh;
                        seen.insert(fresh);
                        *s.opt1 = fresh;
                        changed = true;
                    } else {
                        seen.insert(*s.opt1);
                        catch_ren.erase(*s.opt1);
                    }
                }
                walk_block(s.body2, catch_ren, seen, true);
            }
            if (s.els) {
                auto sub_ren = renames;
                if (s.els->k == SK::Block) walk_block(s.els->body, sub_ren, seen, true);
                else {
                    std::map<std::string, std::string> sub_bmap;
                    walk_stmt(*s.els, sub_ren, sub_bmap, seen, true);
                }
            }
            break;
        }
        case SK::Fn:
        case SK::ExportFn:
            for (auto &nf : s.fns) {
                auto fn_ren = renames;
                auto fn_seen = seen;
                for (auto &p : nf.params) {
                    if (p.def) walk_expr(p.def.get(), renames, seen);
                    fn_ren.erase(p.name);
                    if (!p.name.empty() && p.name != "_") fn_seen.insert(p.name);
                }
                walk_block(nf.body, fn_ren, fn_seen, false);
            }
            break;
        case SK::Pub:
            if (!s.body.empty() && s.body[0]) walk_stmt(*s.body[0], renames, block_map, seen, nested);
            break;
        default:
            break;
        }
    }

    void walk_block(std::vector<StmtP> &body, std::map<std::string, std::string> &renames,
                    std::set<std::string> &seen, bool nested) {
        std::map<std::string, std::string> block_map;
        for (auto &sp : body) {
            if (sp) walk_stmt(*sp, renames, block_map, seen, nested);
        }
    }
};

}  // namespace

void Emitter::run_shadow_disambig() {
    std::map<const FnDecl *, const FnDecl *> replaced;
    for (auto &[name, entry] : fns) {
        if (entry.ctor || !entry.decl) continue;
        FnDecl copy = clone_fn(*entry.decl);
        ShadowRenamer sr;
        std::map<std::string, std::string> renames;
        std::set<std::string> seen = global_lets;
        for (auto &p : copy.params) {
            if (p.def) sr.walk_expr(p.def.get(), renames, seen);
            if (!p.name.empty() && p.name != "_") seen.insert(p.name);
        }
        sr.walk_block(copy.body, renames, seen, false);
        if (sr.changed) {
            const FnDecl *old_ptr = entry.decl;
            map_cloned_nested_stmts(old_ptr->body, copy.body, replaced);
            sra_fns.push_back(std::move(copy));
            entry.decl = &sra_fns.back();
            replaced[old_ptr] = entry.decl;
        }
    }
    for (auto &l : late) {
        auto it = replaced.find(l.decl);
        if (it != replaced.end()) l.decl = it->second;
    }
    if (!top_exe.empty()) {
        std::vector<StmtP> cloned_top;
        std::vector<std::string> old_files;
        cloned_top.reserve(top_exe.size());
        old_files.reserve(top_exe.size());
        for (const Stmt *s : top_exe) {
            cloned_top.push_back(clone_stmt(*s));
            old_files.push_back(lookup_stmt_file(s));
        }
        ShadowRenamer sr;
        std::map<std::string, std::string> renames;
        std::set<std::string> seen = global_lets;
        sr.walk_block(cloned_top, renames, seen, false);
        if (sr.changed) {
            top_exe.clear();
            for (size_t i = 0; i < cloned_top.size(); ++i) {
                shadow_top_stmts.push_back(std::move(cloned_top[i]));
                const Stmt *np = shadow_top_stmts.back().get();
                if (!old_files[i].empty()) local_stmt_files[np] = old_files[i];
                top_exe.push_back(np);
            }
        }
    }
}

void Emitter::run_sra() {
    if (native_structs.empty()) return;
    std::map<const FnDecl *, const FnDecl *> replaced;
    for (auto &[name, entry] : fns) {
        if (entry.ctor || !entry.decl) continue;
        FnDecl copy = clone_fn(*entry.decl);
        if (sra_rewrite_decl(name, copy)) {
            const FnDecl *old_ptr = entry.decl;
            map_cloned_nested_stmts(old_ptr->body, copy.body, replaced);
            sra_fns.push_back(std::move(copy));
            entry.decl = &sra_fns.back();
            replaced[old_ptr] = entry.decl;
        }
    }
    for (auto &l : late) {
        auto it = replaced.find(l.decl);
        if (it != replaced.end()) l.decl = it->second;
    }
}

void Emitter::emit_fn_by_key(const std::string &key) {
    auto safe_comment = [](const std::string &s) {
        std::string r;
        r.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/') {
                r += "* /";
                i++;
            } else {
                r.push_back(s[i]);
            }
        }
        return r;
    };
    std::string saved_src_file = cur_src_file;
    if (key.rfind("fn:", 0) == 0) {
        std::string name = key.substr(3);
        const FnDecl &f = *fns[name].decl;
        cur_src_file = lookup_fn_file(name, &f);
        const std::string *raw = nullptr, *kind = nullptr, *fname = nullptr;
        if (fastc.count(name)) {
            raw = &fastc[name];
            kind = &fastkinds[name];
            fname = &name;
        }
        ln("", cat("/* fn ", safe_comment(name), " */"));
        define_fn(cfn(key), f.params, view(f.body), nullptr, raw, kind, fname, true,
                  is_result_ret_ann(f.ret));
    } else if (key.rfind("meth:", 0) == 0) {
        std::string rest = key.substr(5);
        auto dot = rest.rfind('.');
        std::string tname = rest.substr(0, dot), mname = rest.substr(dot + 1);
        const FnDecl &f = *fns[tname + "." + mname].decl;
        cur_src_file = lookup_fn_file(tname + "." + mname, &f);
        const std::string *self_st = native_structs.count(tname) ? &tname : nullptr;
        ln("", cat("/* meth ", safe_comment(tname + "." + mname), " */"));
        define_fn(cfn(key), f.params, view(f.body), nullptr, nullptr, nullptr, nullptr, true,
                  is_result_ret_ann(f.ret), self_st);
    } else {
        throw EmitError("bad fn key " + key);
    }
    cur_src_file = saved_src_file;
}

void Emitter::emit_prelude_tables() {
    ln("", "static const struct { const char *type, *meth; int code; BMethFn fn; } MTAB[] = {");
    std::vector<std::pair<std::pair<std::string, std::string>, int>> mt(mtab.begin(), mtab.end());
    std::sort(mt.begin(), mt.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    for (const auto &[k, code] : mt)
        ln("  ", cat("{\"", clit(k.first), "\", \"", clit(k.second), "\", ", code, ", ",
                     cfn("meth:" + k.first + "." + k.second), "},"));
    ln("  ", "{0, 0, 0, 0}");
    ln("", "};");
    ln("", "static const struct { const char *tag, *type, *disp; } VTAB[] = {");
    for (const auto &[tag, tname] : vtab) {
        auto dit = vdisp.find(tag);
        std::string dn = dit != vdisp.end() ? dit->second : tag;
        ln("  ", cat("{\"", clit(tag), "\", \"", clit(tname), "\", \"", clit(dn), "\"},"));
    }
    ln("  ", "{0, 0, 0}");
    ln("", "};");
    ln("", "static const char *_brat_vtag_disp(const char *t) {");
    ln("  ", "int64_t i;");
    ln("  ", "for (i = 0; VTAB[i].tag; i++) if (!strcmp(VTAB[i].tag, t)) return VTAB[i].disp;");
    ln("  ", "return NULL;");
    ln("", "}");
    ln("", "static Val vcall_any(Val fn, Vec *a);");
    ln("", "static Val vcall_any(Val fn, Vec *a) {");
    ln("  ", "(void)a;");
    ln("  ", "if (fn.tag != V_TAG || strcmp(fn.as.tv->tag, \"closure\") || fn.as.tv->len != 2) { "
            "vraise(\"not callable\"); return vunwind; }");
    ln("  ", "int64_t code = vlong(fn.as.tv->items[0]);");
    ln("  ", "Val cp = fn.as.tv->items[1];");
    ln("  ", "Map *cap = cp.tag == V_STRUCT ? cp.as.map : NULL;");
    ln("  ", "(void)cap;");
    ln("  ", "switch (code) {");
    std::vector<std::pair<std::string, int>> cd(codes.begin(), codes.end());
    std::sort(cd.begin(), cd.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    for (const auto &[key, code] : cd) ln("    ", cat("case ", code, ": return ", cfn(key), "(cap, a);"));
    ln("  ", "}");
    ln("  ", "vraise(\"bad code\"); return vunwind;");
    ln("", "}");
    ln("", "static Val __attribute__((unused)) vcall_method_ic(Val obj, const char *meth, Vec *a, BMethIC *ic) {");
    ln("  ", "(void)a;");
    ln("  ", "const char *rkey = NULL;");
    ln("  ", "if (obj.tag == V_STRUCT) rkey = obj.as.map->type;");
    ln("  ", "else if (obj.tag == V_TAG) rkey = obj.as.tv->tag;");
    ln("  ", "else {");
    if (user_meths.count("map") || user_meths.count("filter")) {
        ln("    ", "if (obj.tag == V_VEC && a->len == 1 && (!strcmp(meth, \"map\") || !strcmp(meth, \"filter\")))");
        ln("      ", "return vmap_with(obj, vrc_inc(VARG(a, 0)), meth);");
    }
    ln("    ", "return vbuiltin(obj, meth, a);");
    ln("  ", "}");
    ln("  ", "BMethFn fn = NULL;");
    ln("  ", "int _mhit = 0;");
    ln("  ", "if (ic) {");
    ln("    ", "for (int64_t _w = 0; _w < 4; _w++) {");
    ln("      ", "if (!ic->hits[_w]) break;");
    ln("      ", "if (ic->tags[_w] == obj.tag && (ic->tns[_w] == rkey || !strcmp(ic->tns[_w], rkey))) {");
    ln("        ", "fn = ic->fns[_w]; _mhit = 1; break;");
    ln("      ", "}");
    ln("    ", "}");
    ln("  ", "}");
    ln("  ", "if (!_mhit) {");
    ln("    ", "const char *tn = NULL;");
    ln("    ", "const char *ck = NULL;");
    ln("    ", "int64_t i;");
    ln("    ", "if (obj.tag == V_STRUCT) { tn = rkey; ck = *rkey ? rkey : \"\"; }");
    ln("    ", "else { for (i = 0; VTAB[i].tag; i++) if (!strcmp(VTAB[i].tag, rkey)) { tn = VTAB[i].type; ck = VTAB[i].tag; break; } }");
    ln("    ", "if (tn) for (i = 0; MTAB[i].type; i++) if (!strcmp(MTAB[i].type, tn) && !strcmp(MTAB[i].meth, meth)) {");
    ln("      ", "fn = MTAB[i].fn;");
    ln("      ", "if (obj.tag == V_STRUCT) ck = MTAB[i].type;");
    ln("      ", "break;");
    ln("    ", "}");
    ln("    ", "if (ic && ck) { unsigned _iw = (unsigned)(ic->next & 3u); ic->tns[_iw] = ck; ic->fns[_iw] = fn; ic->tags[_iw] = obj.tag; ic->hits[_iw] = 1; ic->next = (unsigned char)((_iw + 1u) & 3u); }");
    ln("  ", "}");
    ln("  ", "if (fn) {");
    ln("    ", "if (a->len <= 8) {");
    ln("      ", "Val _mb[9]; BStackVec _msv; VSTACK_VEC_INIT(_msv, _mb, 9);");
    ln("      ", "_mb[0] = obj;");
    ln("      ", "for (int64_t j = 0; j < a->len; j++) _mb[j + 1] = a->items[j];");
    ln("      ", "_msv.vec.len = a->len + 1;");
    ln("      ", "return fn(NULL, &_msv.vec);");
    ln("    ", "}");
    ln("    ", "Vec *b = vvec(); vec_push(b, obj);");
    ln("    ", "for (int64_t j = 0; j < a->len; j++) vec_push(b, a->items[j]);");
    if (move_rt)
        ln("    ", "Val r = fn(NULL, b); b->len = 0; vrc_dec(vvecv(b)); return r;");
    else
        ln("    ", "return fn(NULL, b);");
    ln("  ", "}");
    ln("  ", "return vbuiltin(obj, meth, a);");
    ln("", "}");
    ln("", "static Val __attribute__((unused)) vcall_method(Val obj, const char *meth, Vec *a) {");
    ln("  ", "return vcall_method_ic(obj, meth, a, NULL);");
    ln("", "}");
    ln("", "static Val __attribute__((unused)) vmap_with(Val obj, Val fn, "
           "const char *meth) {");
    ln("  ", "Vec *w = vvec_of_m(obj, meth);");
    ln("  ", "if (verr_pending()) { vrc_dec(fn); return vunwind; }");
    ln("  ", "vrc_inc(obj);");
    ln("  ", "Vec *o = meth[0] == 'm' ? vvec_cap(w->len) : vvec();");
    ln("  ", "Val _ob[1]; BStackVec _osv; VSTACK_VEC_INIT(_osv, _ob, 1);");
    ln("  ", "_osv.vec.len = 1;");
    ln("  ", "for (int64_t i = 0; i < w->len; i++) {");
    ln("    ", "Val _cur = vrc_inc(w->items[i]);");
    ln("    ", "_ob[0] = _cur;");
    ln("    ", "Val r = vcall_any(fn, &_osv.vec);");
    ln("    ", "if (verr_pending()) {");
    ln("      ", "vrc_dec(_cur); vrc_dec(fn); vrc_dec(vvecv(o)); vrc_dec(obj);");
    ln("      ", "return r;");
    ln("    ", "}");
    ln("    ", "if (meth[0] == 'm') vec_push(o, r);");
    ln("    ", "else { if (vis_true(r)) vec_push(o, vrc_inc(_cur)); vrc_dec(r); }");
    ln("    ", "vrc_dec(_cur);");
    ln("  ", "}");
    ln("  ", "vrc_dec(fn);");
    ln("  ", "vrc_dec(obj);");
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
    if (!shadows_done) {
        shadows_done = true;
        run_shadow_disambig();
    }
    if (opt >= 1 && !sra_done) {
        sra_done = true;
        run_sra();
    }
    compute_fast();
    if (opt >= 1) run_opt();
    const std::string opt_unused = "BRAT_UNUSED ";
    if (need_idiv) {
        ln("", cat("static ", opt_unused, "int64_t bf_idiv(int64_t a, int64_t b) {"));
        ln("  ", "if (!b) { vraise(\"div by zero\"); return 0; }");
        ln("  ", "if (a == INT64_MIN && b == -1) { return INT64_MIN; }");
        ln("  ", "int64_t q = a / b;");
        ln("  ", "if ((a % b) && ((a < 0) != (b < 0))) q--;");
        ln("  ", "return q;");
        ln("", "}");
    }
    if (need_imod) {
        ln("", cat("static ", opt_unused, "int64_t bf_imod(int64_t a, int64_t b) {"));
        ln("  ", "if (!b) { vraise(\"div by zero\"); return 0; }");
        ln("  ", "if (a == INT64_MIN && b == -1) { return 0; }");
        ln("  ", "int64_t r = a % b;");
        ln("  ", "if (r && ((r < 0) != (b < 0))) r += b;");
        ln("  ", "return r;");
        ln("", "}");
    }
    if (need_idiv_nz) {
        ln("", "static BRAT_UNUSED int64_t bf_idiv_nz(int64_t a, int64_t b) {");
        ln("  ", "if (a == INT64_MIN && b == -1) { return INT64_MIN; }");
        ln("  ", "int64_t q = a / b;");
        ln("  ", "if ((a % b) && ((a < 0) != (b < 0))) q--;");
        ln("  ", "return q;");
        ln("", "}");
    }
    if (need_imod_nz) {
        ln("", "static BRAT_UNUSED int64_t bf_imod_nz(int64_t a, int64_t b) {");
        ln("  ", "if (a == INT64_MIN && b == -1) { return 0; }");
        ln("  ", "int64_t r = a % b;");
        ln("  ", "if (r && ((r < 0) != (b < 0))) r += b;");
        ln("  ", "return r;");
        ln("", "}");
    }
    if (need_fdiv) {
        ln("", "static BRAT_UNUSED double bf_fdiv(double a, double b) {");
        ln("  ", "if (b == 0.0) { vraise(\"div by zero\"); return 0.0; }");
        ln("  ", "return a / b;");
        ln("", "}");
    }
    if (need_fmod) {
        ln("", "static BRAT_UNUSED double bf_fmod(double a, double b) {");
        ln("  ", "if (b == 0.0) { vraise(\"div by zero\"); return 0.0; }");
        ln("  ", "double r = fmod(a, b);");
        ln("  ", "if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;");
        ln("  ", "return r;");
        ln("", "}");
    }
    if (need_viat) {
        ln("", cat("static ", opt_unused, "int64_t bf_viat(Vec *v, int64_t i) {"));
        ln("  ", "if (i < 0 || i >= v->len) { vraise(\"index out of range\"); return 0; }");
        ln("  ", "Val x = v->items[i];");
        ln("  ", "if (x.tag != V_INT) { vraise(\"vec elem not int\"); return 0; }");
        ln("  ", "return x.as.ival;");
        ln("", "}");
    }
    if (need_viat_nb) {
        ln("", "static BRAT_UNUSED int64_t bf_viat_nb(Vec *v, int64_t i) {");
        ln("  ", "Val x = v->items[i];");
        ln("  ", "if (x.tag != V_INT) { vraise(\"vec elem not int\"); return 0; }");
        ln("  ", "return x.as.ival;");
        ln("", "}");
    }
    if (need_vset) {
        ln("", cat("static ", opt_unused, "void bf_vset(Vec *v, int64_t i, int64_t x) {"));
        ln("  ", "if (i < 0 || i >= v->len) { vraise(\"index out of range\"); return; }");
        ln("  ", "v->items[i] = vint(x);");
        ln("", "}");
    }
    if (need_vfat) {
        ln("", cat("static ", opt_unused, "double bf_vfat(Vec *v, int64_t i) {"));
        ln("  ", "if (i < 0 || i >= v->len) { vraise(\"index out of range\"); return 0.0; }");
        ln("  ", "Val x = v->items[i];");
        ln("  ", "if (x.tag == V_FLOAT) return x.as.fval;");
        ln("  ", "if (x.tag == V_INT) return (double)x.as.ival;");
        ln("  ", "vraise(\"vec elem not float\"); return 0.0;");
        ln("", "}");
    }
    if (need_vset_f) {
        ln("", cat("static ", opt_unused, "void bf_vset_f(Vec *v, int64_t i, double x) {"));
        ln("  ", "if (i < 0 || i >= v->len) { vraise(\"index out of range\"); return; }");
        ln("  ", "v->items[i] = vfloat(x);");
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
            const std::string key = "fn:" + late[late_declared].name;
            ln("", cat("static Val ", cfn(key), "(Map *cap, Vec *args);"));
            auto it = fns.find(late[late_declared].name);
            if (it != fns.end() && !it->second.ctor && it->second.decl) {
                std::string sig = cat("static Val ", cfn(key), "_p(Map *cap");
                for (size_t i = 0; i < it->second.decl->params.size(); i++) sig += cat(", Val bf_arg_", i);
                ln("", cat(sig, ");"));
            }
            late_declared++;
        }
        std::string saved_src_file = cur_src_file;
        if (!late[late_done].src_file.empty()) cur_src_file = late[late_done].src_file;
        emit_fn_by_key("fn:" + late[late_done].name);
        cur_src_file = saved_src_file;
        late_done++;
    }
    emit_closures_queue();
}

void Emitter::emit_main_open() {
    cur_src_file = root_src_file;
    sym.clear();
    cap.reset();
    cap_slots.clear();
    dfb.clear();
    scopes.clear();
    iter_cleanups.clear();
    err_scope_depth = 0;
    err_iter_depth = 0;
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
        ln("    ", "default: return v.as.tv && v.as.tv->tag ? v.as.tv->tag : \"tag\";");
        ln("  ", "}");
        ln("", "}");
    }
    ln("", "int main(int argc, char **argv) {");
    ln("  ", "vmem_budget_apply();");
    ln("  ", "vargs_set(argc, argv);");
    ln("  ", "bdepth_init();");
    ln("  ", cat("vbase_set(\"", cstr_path(base), "\");"));
    if (!cb_wrappers.empty()) ln("  ", "brat_mt_enable();");
    ln("  ", "Vec *_a = vvec();");
    ln("  ", "bexit_track_args(_a);");
    std::set<std::string> names;
    std::function<void(const Stmt &)> scan = [&](const Stmt &s) {
        if (s.k == SK::Pub) {
            scan(*s.body[0]);
        } else if (s.k == SK::Fn || s.k == SK::Impl || s.k == SK::Trait || s.k == SK::Enum ||
                   s.k == SK::Struct) {
            return;
        } else {
            collect_vars({&s}, names);
        }
    };
    for (const Stmt *s : top_exe) scan(*s);
    dfb_locals = names;
    // Track top-level script `let` / `LetTmp` bindings as Owned so their heap
    // values are released at exit.
    std::set<std::string> script_lets;
    for (const Stmt *s : top_exe)
        if (s->k == SK::Let || s->k == SK::LetTmp) script_lets.insert(s->a);
    for (const auto &n : names) {
        if (global_lets.count(n)) continue;
        sym[n] = Sym{cname(n), script_lets.count(n) ? Own::Owned : Own::Copy, "", ""};
        ln("  ", cat("Val BRAT_UNUSED ", cname(n), " = vnil();"));
        if (script_lets.count(n)) ln("  ", cat("bexit_track_val(&", cname(n), ");"));
    }
    for (const auto &n : global_lets) {
        sym[n] = Sym{cname(n), script_lets.count(n) ? Own::Owned : Own::Copy, "", ""};
        if (script_lets.count(n)) ln("  ", cat("bexit_track_val(&", cname(n), ");"));
    }
    for (const auto &n : relet)
        if (names.count(n)) ln("  ", cat(cname(n), " = vnil();"));
}

void Emitter::emit_main_close() {
    ln("  ", "bexit_untrack_all();");
    if (codes.count("fn:main")) {
        ln("  ", cat("Val __r = ", cfn("fn:main"), "(NULL, _a);"));
        ln("  ", "vrc_dec(vvecv(_a));");
        ln("  ", "if (verr_pending()) { fprintf(stderr, \"unhandled: \"); vprint_err(verr_take()); "
                "fprintf(stderr, \"\\n\"); return 1; }");
    } else {
        // Release `_a` in script mode (no user `main` consumes it).
        ln("  ", "vrc_dec(vvecv(_a));");
    }
    if (move_rt) emit_scope_drops("  ");
    ln("  ", "vjoin_all();");
    ln("  ", "if (verr_pending()) { fprintf(stderr, \"unhandled: \"); vprint_err(verr_take()); "
            "fprintf(stderr, \"\\n\"); return 1; }");
    if (codes.count("fn:main")) {
        ln("  ", "if (__r.tag == V_NIL) return 0;");
        ln("  ", "if (__r.tag == V_TAG) {");
        ln("    ", "if (__r.as.tv && __r.as.tv->tag && strcmp(__r.as.tv->tag, \"err\") == 0) {");
        ln("      ", "fprintf(stderr, \"unhandled: Err(\");");
        ln("      ", "{ Val __p = vnil();");
        ln("        ", "if (__r.as.tv->len > 0) __p = __r.as.tv->items[0];");
        ln("        ", "if (__p.tag == V_STR) fprintf(stderr, \"'%s'\", vstr_c(&__p));");
        ln("        ", "else { Val __s = vstr_of(__p); fprintf(stderr, \"%s\", vstr_c(&__s)); vrelease(__s); } }");
        if (trace_lines) {
            ln("      ", "fprintf(stderr, \")\");");
            ln("      ", "vtrace_print_cur_line();");
            ln("      ", "fprintf(stderr, \"\\n\");");
        } else {
            ln("      ", "fprintf(stderr, \")\\n\");");
        }
        ln("      ", "vrelease(__r);");
        ln("      ", "return 1;");
        ln("    ", "}");
        ln("    ", "if (__r.as.tv && __r.as.tv->tag && strcmp(__r.as.tv->tag, \"ok\") == 0) {");
        ln("      ", "Val __p = (__r.as.tv->len > 0) ? __r.as.tv->items[0] : vnil();");
        ln("      ", "if (__p.tag == V_INT) { int __code = (int)__p.as.ival; vrelease(__r); return __code; }");
        ln("      ", "if (__p.tag == V_NIL) { vrelease(__r); return 0; }");
        ln("      ", "fprintf(stderr, \"TypeError: main must return i64, got Ok(%s)\\n\", __brat_tagname(__p));");
        ln("      ", "vrelease(__r);");
        ln("      ", "return 1;");
        ln("    ", "}");
        ln("    ", "fprintf(stderr, \"TypeError: main must return i64, got %s\\n\", __r.as.tv && __r.as.tv->tag ? __r.as.tv->tag : \"tag\");");
        ln("    ", "vrelease(__r);");
        ln("    ", "return 1;");
        ln("  ", "}");
        // `main` may hand back a container or a string; the process is about to
        // exit, but a leak check still sees it. Read the code first.
        ln("  ", "if (__r.tag == V_INT) { int __code = (int)__r.as.ival; vrelease(__r); return __code; }");
        ln("  ", "fprintf(stderr, \"TypeError: main must return i64, got %s\\n\", __brat_tagname(__r));");
        ln("  ", "vrelease(__r);");
        ln("  ", "return 1;");
    }
    ln("  ", "return 0;");
    ln("", "}");
}

static int64_t parse_ffi_arr_dim(const std::string &s) {
    size_t i = 0, n = s.size();
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    while (n > i && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (i == n) throw EmitError("empty array dimension in extern struct");
    int64_t v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') throw EmitError("invalid array dimension `" + s + "` in extern struct");
        int d = s[i] - '0';
        if (v > (67108864LL - d) / 10)
            throw EmitError("array dimension `" + s + "` exceeds limit in extern struct");
        v = v * 10 + d;
    }
    return v;
}

static std::string trim_ffi_tok(const std::string &s) {
    size_t i = 0, n = s.size();
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    while (n > i && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    return s.substr(i, n - i);
}

std::vector<std::string> Emitter::emit_struct_conv(const std::string &sname) {
    std::vector<std::string> lines;
    const bool is_un = unions.count(sname) > 0;
    const std::string cf = ffi_c(sname);
    lines.push_back(cat("BFN ", cf, " _bv2c_", sname, "(Val _v, int _pin) {"));
    lines.push_back("  (void)_pin;");
    lines.push_back(cat("  ", cf, " _u;"));
    lines.push_back("  memset(&_u, 0, sizeof(_u));");
    lines.push_back(cat("  if (_v.tag != V_STRUCT || !_v.as.map) { vraise(\"ffi: expected struct ", sname, "\"); return _u; }"));
    lines.push_back("  Val *_p;");
    std::vector<StructField> flds = *structs[sname];
    if (is_un) {
        std::stable_sort(flds.begin(), flds.end(), [](const StructField &a, const StructField &b) {
            auto rank = [](const std::optional<std::string> &t) {
                return (t && (*t == "f32" || *t == "f64")) ? 0 : 1;
            };
            return rank(a.ann) < rank(b.ann);
        });
    }
    auto conv_scalar_to_c = [&](const std::string &ft, const std::string &src, const std::string &dst,
                                const std::string &label) -> std::string {
        if (ft == "bool") {
            return cat("if (", src, ".tag != V_BOOL) { vraise(\"ffi: field ", label, " wants bool\"); return _u; } ", dst, " = ", src, ".as.b;");
        }
        if (ft == "str") {
            return cat("if (", src, ".tag != V_STR) { vraise(\"ffi: field ", label, " wants str\"); return _u; } ", dst, " = (_pin ? vstr_pin_ret(", src, ") : vstr_pin(", src, ")); if (verr_pending()) return _u;");
        }
        if (ft == "ptr" || ft.rfind("ptr[", 0) == 0 || ft.rfind("fn", 0) == 0) {
            return cat("if (", src, ".tag != V_INT || ", src, ".as.ival < 0) { vraise(\"ffi: field ", label, " wants ptr\"); return _u; } ", dst, " = (void *)(intptr_t)", src, ".as.ival;");
        }
        if (ft == "f64") {
            return cat("if (", src, ".tag != V_FLOAT && ", src, ".tag != V_INT) { vraise(\"ffi: field ", label, " wants f64\"); return _u; } ", dst, " = (", src, ".tag == V_INT ? (double)", src, ".as.ival : ", src, ".as.fval);");
        }
        if (ft == "f32") {
            return cat("if (", src, ".tag != V_FLOAT && ", src, ".tag != V_INT) { vraise(\"ffi: field ", label, " wants f32\"); return _u; } ", dst, " = (float)(", src, ".tag == V_INT ? (double)", src, ".as.ival : ", src, ".as.fval);");
        }
        if (structs.count(ft)) {
            return cat(dst, " = _bv2c_", ft, "(", src, ", _pin); if (verr_pending()) return _u;");
        }
        std::string ok = cat(src, ".tag == V_INT");
        if (FFI_RANGE.count(ft)) {
            const auto &r = FFI_RANGE.at(ft);
            ok += cat(" && ", src, ".as.ival >= ", r.first, " && ", src, ".as.ival <= ", r.second);
        }
        return cat("if (!(", ok, ")) { vraise(\"ffi: field ", label, " wants ", ft.empty() ? std::string("i64") : ft, "\"); return _u; } ", dst, " = ", src, ".as.ival;");
    };
    for (const auto &sfield : flds) {
        const std::string &f = sfield.name;
        const std::string ft = sfield.ann.value_or("");
        const std::string ffn = ffi_c(f);
        const std::string label = sname + "." + f;
        std::string body;
        if (ft.rfind("ptr[", 0) != 0 && ft.find('[') != std::string::npos && !ft.empty() && ft.back() == ']') {
            auto lb = ft.find('[');
            std::string ael = trim_ffi_tok(ft.substr(0, lb));
            int64_t acnt = parse_ffi_arr_dim(ft.substr(lb + 1, ft.size() - lb - 2));
            std::string step = conv_scalar_to_c(ael, "(*_p).as.vec->items[_i]", cat("_u.", ffn, "[_i]"), label);
            body = cat("if ((*_p).tag != V_VEC || !(*_p).as.vec || (*_p).as.vec->len != ", std::to_string(acnt),
                       ") { vraise(\"ffi: field ", label, " wants vec of length ", std::to_string(acnt),
                       "\"); return _u; } for (int64_t _i = 0; _i < ", std::to_string(acnt), "; _i++) { ", step, " }");
        } else {
            body = conv_scalar_to_c(ft, "(*_p)", cat("_u.", ffn), label);
        }
        if (is_un) {
            lines.push_back(cat("  if ((_p = map_get(_v.as.map, \"", f, "\")) != NULL) { ", body, " }"));
        } else {
            lines.push_back(cat("  if ((_p = map_get(_v.as.map, \"", f, "\")) == NULL) { vraise(\"ffi: struct ", sname, " missing field ", f, "\"); return _u; }"));
            lines.push_back(cat("  ", body));
        }
    }
    lines.push_back("  return _u;");
    lines.push_back("}");

    lines.push_back(cat("BFN Val _bc2v_", sname, "(", cf, " _r) {"));
    lines.push_back(cat("  Val _v = vstruct(vmap(\"", sname, "\"));"));
    auto conv_c_to_val = [&](const std::string &ft, const std::string &src) -> std::string {
        if (ft == "bool") return cat("vbool(", src, ")");
        if (ft == "str") return cat("vstr_ffi_field(", src, ")");
        if (ft == "ptr" || ft.rfind("ptr[", 0) == 0 || ft.rfind("fn", 0) == 0)
            return cat("vint((int64_t)(intptr_t)", src, ")");
        if (ft == "f64") return cat("vfloat(", src, ")");
        if (ft == "f32") return cat("vfloat((double)", src, ")");
        if (structs.count(ft)) return cat("_bc2v_", ft, "(", src, ")");
        return cat("vint((int64_t)", src, ")");
    };
    for (const auto &sfield : *structs[sname]) {
        const std::string &f = sfield.name;
        const std::string ft = sfield.ann.value_or("");
        const std::string fexpr = cat("_r.", ffi_c(f));
        if (ft.rfind("ptr[", 0) != 0 && ft.find('[') != std::string::npos && !ft.empty() && ft.back() == ']') {
            auto lb = ft.find('[');
            std::string ael = trim_ffi_tok(ft.substr(0, lb));
            int64_t acnt = parse_ffi_arr_dim(ft.substr(lb + 1, ft.size() - lb - 2));
            std::string av = conv_c_to_val(ael, cat(fexpr, "[_i]"));
            lines.push_back(cat("  { Vec *_av = vvec_cap(", std::to_string(acnt), "); for (int64_t _i = 0; _i < ",
                                std::to_string(acnt), "; _i++) { vec_push(_av, ", av, "); } map_set(_v.as.map, \"",
                                f, "\", vvecv(_av)); }"));
        } else {
            lines.push_back(cat("  map_set(_v.as.map, \"", f, "\", ", conv_c_to_val(ft, fexpr), ");"));
        }
    }
    lines.push_back("  return _v;");
    lines.push_back("}");
    return lines;
}

std::string Emitter::val_to_c_struct(const std::string &sname, const std::string &valexpr, bool pin_ret) {
    return cat("_bv2c_", sname, "(", valexpr, ", ", pin_ret ? "1" : "0", ")");
}

std::string Emitter::c_struct_to_val(const std::string &sname, const std::string &cexpr) {
    return cat("_bc2v_", sname, "(", cexpr, ")");
}

void Emitter::emit_forward_decls() {
    ln("", "static Val vcall_any(Val fn, Vec *a);");
    ln("", "static Val vcall_method_ic(Val obj, const char *meth, Vec *a, BMethIC *ic);");
    ln("", "static Val vcall_method(Val obj, const char *meth, Vec *a);");
    ln("", "static Val vmap_with(Val obj, Val fn, const char *meth);");
    mic_slot = static_cast<int>(out.size());
    std::vector<std::pair<std::string, int>> cd(codes.begin(), codes.end());
    std::sort(cd.begin(), cd.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    for (const auto &[key, code] : cd) {
        ln("", cat("static Val ", cfn(key), "(Map *cap, Vec *args);"));
        const std::vector<Param> *params = nullptr;
        if (key.rfind("fn:", 0) == 0) {
            auto it = fns.find(key.substr(3));
            if (it != fns.end() && !it->second.ctor && it->second.decl) params = &it->second.decl->params;
        } else if (key.rfind("meth:", 0) == 0) {
            auto it = fns.find(key.substr(5));
            if (it != fns.end() && !it->second.ctor && it->second.decl) params = &it->second.decl->params;
        }
        if (params) {
            std::string sig = cat("static Val ", cfn(key), "_p(Map *cap");
            for (size_t i = 0; i < params->size(); i++) sig += cat(", Val bf_arg_", i);
            ln("", cat(sig, ");"));
        }
    }
    for (const auto &[name, e] : fast) {
        const std::vector<Param> &params = e.decl->params;
        std::string ct = (fastkinds[name] == "f64" || fastkinds[name] == "f64_dyn" ||
                          fastkinds[name] == "f64v")
                             ? "double"
                             : "int64_t";
        std::string types;
        if (fastkinds[name] == "i64v" || fastkinds[name] == "i64v_dyn" ||
            fastkinds[name] == "f64v") {
            const std::set<std::string> &vv = fastvec[name];
            for (const auto &p : params) {
                if (!types.empty()) types += ", ";
                types += vv.count(p.name) ? "Vec *" : ct;
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
        std::string saved_src_file = cur_src_file;
        cur_src_file = !c.src_file.empty() ? c.src_file : root_src_file;
        define_fn(cfn(cat("clo:", clos_done)), *c.params, c.body, &c.caps, nullptr, nullptr, nullptr,
                  false, c.ret_result);
        cur_src_file = saved_src_file;
        clos_done++;
        emit_closure_protos();
    }
}

void Emitter::emit_prelude_headers() {
    const char *case_version = std::getenv("BRAT_CASE_MAP_VERSION");
    ln("", case_version && std::strcmp(case_version, "1600") == 0
              ? "#define BRAT_CASE_MAP_VERSION 1600"
              : "#define BRAT_CASE_MAP_VERSION 1500");
    ln("", "#define BRAT_CRT_MASK " + std::to_string(crt_mask) + "u");
    ln("", "#define BRAT_HAS_VTAB_DISP 1");
    if (trace_lines) ln("", "#define BRAT_TRACE_DEFAULT 1");
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
    last_dbg_file.clear();
    last_dbg_line = -1;
    last_dbg_next_idx = 0;
    run_pipeline();
    cb_wrappers.clear();
    auto ffi_decl_impl = [&](auto &&self, const std::string &ann,
                             const std::string &inner) -> std::string {
        auto attach = [](const std::string &base, const std::string &d) -> std::string {
            if (d.empty()) return base;
            return base + " " + d;
        };
        if (ann.empty() || ann == "none") return attach("void", inner);
        if (structs.count(ann)) return attach(ffi_c(ann), inner);
        if (ann == "ptr") return attach("void *", inner);
        if (ann.rfind("fn", 0) == 0) {
            auto sig = callback_signature(ann);
            if (!sig) throw std::runtime_error("unsupported FFI callback type `" + ann + "`");
            std::string params;
            for (const auto &param : sig->first) {
                if (!params.empty()) params += ", ";
                params += self(self, param, "");
            }
            std::string ptr_decl =
                "(*" + inner + ") (" + (params.empty() ? "void" : params) + ")";
            return self(self, sig->second, ptr_decl);
        }
        if (ann.rfind("ptr[", 0) == 0 && ann.back() == ']') {
            std::string elem = ann.substr(4, ann.size() - 5);
            if (elem.rfind("fn", 0) == 0)
                throw std::runtime_error("pointer to callback is not a supported FFI type");
            return attach(self(self, elem, "") + " *", inner);
        }
        auto it = ffi_scalar_c().find(ann);
        if (it == ffi_scalar_c().end())
            throw std::runtime_error("unsupported FFI type `" + ann + "` in an extern declaration");
        return attach(it->second, inner);
    };
    auto ffi_decl_type = [&](const std::string &ann) {
        return ffi_decl_impl(ffi_decl_impl, ann, "");
    };
    auto ffi_decl_named = [&](const std::string &ann, const std::string &inner) {
        return ffi_decl_impl(ffi_decl_impl, ann, inner);
    };
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
            try {
                t = ffi_decl_named(*ann, arg);
            } catch (const std::runtime_error &) {
                ok = false;
                break;
            }
            cparams.push_back(t);
        }
        if (!ok) continue;
        std::string ret_s = ret ? *ret : std::string("none");
        std::string cret;
        try {
            cret = ffi_decl_type(ret_s);
        } catch (const std::runtime_error &) {
            continue;
        }
        cb_wrappers.push_back(Cb{cbname, cret, ret_s, cparams});
    }
    out.clear();
    last_dbg_file.clear();
    last_dbg_line = -1;
    last_dbg_next_idx = 0;
    ic_n = 0;
    mic_slot = -1;
    nested_seq = 0;
    clos_done = clos_declared = late_done = late_declared = 0;
    tmp = 0;
    // Ownership is re-derived in pass two; a stale `dropped` would suppress it.
    moved.clear();
    captured.clear();
    shared.clear();
    dropped.clear();
    call_cleanup.clear();
    scalar_vars.clear();
    scopes.clear();
    err_scope_depth = 0;
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
            for (const auto &fld : *structs[n]) {
                if (!fld.ann) continue;
                std::string bft = *fld.ann;
                if (bft.rfind("ptr[", 0) != 0) {
                    auto lb = bft.find('[');
                    if (lb != std::string::npos) bft = bft.substr(0, lb);
                }
                if (structs.count(bft) && bft != n) visit(bft);
            }
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
    // corrupting the struct at run time.
    // Use the target's widths, not the host's. An explicit triple determines
    // them below; BRAT_FFI_PTR/BRAT_FFI_A8 are overrides for the no-triple
    // path used by focused tests.
    int ffi_pw = 8, ffi_a8 = 8;
    if (!target_triple.empty()) {
        std::string t = target_triple;
        for (char &ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const bool is_win = t.find("windows") != std::string::npos ||
                            t.find("win32") != std::string::npos ||
                            t.find("msvc") != std::string::npos ||
                            t.find("mingw") != std::string::npos;
        const bool is_ilp32 = !is_win && (
            t.rfind("i386", 0) == 0 || t.rfind("i486", 0) == 0 || t.rfind("i586", 0) == 0 ||
            t.rfind("i686", 0) == 0 || t.rfind("arm", 0) == 0 || t.rfind("armv", 0) == 0 ||
            t.rfind("thumb", 0) == 0 || t.rfind("wasm32", 0) == 0 || t.rfind("mips", 0) == 0 ||
            t.rfind("riscv32", 0) == 0 || t.rfind("powerpc-", 0) == 0 ||
            t.rfind("x86-64-32", 0) == 0 || t.find("-gnux32") != std::string::npos ||
            t.find("-x32") != std::string::npos || t.find("ilp32") != std::string::npos);
        if (is_ilp32) {
            ffi_pw = 4;
            const bool x86_32 = t.rfind("i386", 0) == 0 || t.rfind("i486", 0) == 0 ||
                                t.rfind("i586", 0) == 0 || t.rfind("i686", 0) == 0 ||
                                t.rfind("x86-64-32", 0) == 0 ||
                                t.find("-x32") != std::string::npos ||
                                t.find("gnux32") != std::string::npos;
            ffi_a8 = x86_32 ? 4 : 8;
        }
    } else {
        if (const char *e = std::getenv("BRAT_FFI_PTR")) ffi_pw = std::atoi(e);
        if (const char *e = std::getenv("BRAT_FFI_A8")) ffi_a8 = std::atoi(e);
    }
    auto ffi_field_shape = [ffi_pw, ffi_a8](const std::string &t) -> std::optional<std::pair<int, int>> {
        static const std::map<std::string, std::pair<int, int>> shape = {
            {"bool", {1, 1}},
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
        const bool is_un = unions.count(n) > 0;
        FFILayout lay;
        int off = 0, max_sz = 0, align = 1;
        for (const auto &sfld : *structs[n]) {
            const std::string sty = sfld.ann.value_or("");
            int size = 0, falign = 1;
            std::string arr_elem;
            int arr_cnt = 0;
            if (sty.rfind("ptr[", 0) != 0 && !sty.empty() && sty.back() == ']') {
                auto lb = sty.find('[');
                if (lb != std::string::npos) {
                    std::string cs = sty.substr(lb + 1, sty.size() - lb - 2);
                    bool dig = !cs.empty();
                    for (char ch : cs) if (!std::isdigit(static_cast<unsigned char>(ch))) dig = false;
                    if (dig) { arr_elem = sty.substr(0, lb); arr_cnt = std::stoi(cs); }
                }
            }
            if (arr_cnt > 0) {
                if (structs.count(arr_elem)) {
                    const FFILayout *sub = ffi_of(arr_elem);
                    if (!sub) return nullptr;
                    size = sub->size * arr_cnt;
                    falign = sub->align;
                } else {
                    auto sh = ffi_field_shape(arr_elem);
                    if (!sh) return nullptr;
                    size = sh->first * arr_cnt;
                    falign = sh->second;
                }
            } else if (structs.count(sty)) {
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
            if (is_un) {
                lay.slots.push_back({sfld.name, 0});
                max_sz = std::max(max_sz, size);
                align = std::max(align, falign);
            } else {
                if (pk) falign = 1;
                off = (off + falign - 1) / falign * falign;
                lay.slots.push_back({sfld.name, off});
                off += size;
                align = std::max(align, falign);
            }
        }
        if (is_un) {
            lay.size = (max_sz + align - 1) / align * align;
            lay.align = align;
        } else {
            if (pk) align = 1;
            else off = (off + align - 1) / align * align;
            lay.size = off;
            lay.align = align;
        }
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
            if (sty == "bool") {
                m = cat("_Bool ", sfn, ";");
            } else if (sty == "i64") {
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
            } else if (sty == "ptr" || sty.rfind("ptr[", 0) == 0 || sty.rfind("fn", 0) == 0) {
                m = cat("void *", sfn, ";");
            } else if (sty.find('[') != std::string::npos && !sty.empty() && sty.back() == ']') {
                auto lb = sty.find('[');
                std::string ael = sty.substr(0, lb);
                std::string acnt = sty.substr(lb + 1, sty.size() - lb - 2);
                std::string act = "int64_t";
                if (ael == "bool") act = "_Bool";
                else if (ael == "i64") act = "int64_t";
                else if (ael == "i32") act = "int";
                else if (ael == "u8") act = "unsigned char";
                else if (ael == "i8") act = "signed char";
                else if (ael == "u16") act = "unsigned short";
                else if (ael == "i16") act = "short";
                else if (ael == "u32") act = "unsigned";
                else if (ael == "u64") act = "uint64_t";
                else if (ael == "usize") act = "size_t";
                else if (ael == "f64") act = "double";
                else if (ael == "f32") act = "float";
                else if (ael == "str") act = "const char *";
                else if (ael == "ptr") act = "void *";
                else if (structs.count(ael)) act = ffi_c(ael);
                m = cat(act, " ", sfn, "[", std::to_string(parse_ffi_arr_dim(acnt)), "];");
            } else if (structs.count(sty)) {
                m = cat(ffi_c(sty), " ", sfn, ";");
            } else {
                m = cat("int64_t ", sfn, ";");
            }
            if (!mems.empty()) mems += " ";
            mems += m;
        }
        const bool is_un = unions.count(sname) > 0;
        const char *rk = is_un ? "union" : "struct";
        ln("", cat(is_un ? "typedef union { "
                         : (packed.count(sname) ? "typedef struct __attribute__((packed)) { "
                                                : "typedef struct { "),
                   mems, " } ", ffi_c(sname), ";"));
        const FFILayout *lay = ffi_of(sname);
        if (lay) {
            ln("", cat("_Static_assert(sizeof(", ffi_c(sname), ") == ", std::to_string(lay->size),
                       ", \"extern ", rk, " ", sname,
                       ": size differs from the declared layout\");"));
            for (const auto &slot : lay->slots) {
                ln("", cat("_Static_assert(offsetof(", ffi_c(sname), ", ", ffi_c(slot.first),
                           ") == ", std::to_string(slot.second), ", \"extern ", rk, " ", sname,
                           ".", slot.first, ": offset differs from the declared layout\");"));
            }
        }
        for (const auto &sl : emit_struct_conv(sname)) ln("", sl);
    }
    ln("", "");
    static const std::set<std::string> sys_c_syms = {
        "malloc", "calloc", "realloc", "free", "abs", "labs", "llabs",
        "atoi", "atol", "atoll", "atof", "strtol", "strtoul", "strtoll",
        "strtoull", "strtod", "strtof", "exit", "_exit", "abort", "getenv",
        "setenv", "unsetenv", "system", "qsort", "bsearch", "rand", "srand",
        "strlen", "strnlen", "strcmp", "strncmp", "strcpy", "strncpy",
        "strcat", "strncat", "strchr", "strrchr", "strstr", "strdup",
        "strndup", "strerror", "memcpy", "memmove", "memset", "memcmp",
        "memchr", "printf", "fprintf", "sprintf", "snprintf", "puts",
        "fputs", "putchar", "getchar", "fopen", "fclose", "fread", "fwrite",
        "fflush", "fseek", "ftell", "rewind", "remove", "rename", "htonl",
        "htons", "ntohl", "ntohs", "sin", "cos", "tan", "asin", "acos",
        "atan", "atan2", "sinh", "cosh", "tanh", "sqrt", "cbrt", "hypot",
        "pow", "exp", "log", "log10", "log2", "floor", "ceil", "fabs",
        "fmod", "round", "trunc", "read", "write", "close", "open", "lseek",
        "unlink", "access", "getpid", "sleep", "usleep", "time", "clock",
        "difftime",
    };
    std::set<std::string> emitted_externs;
    for (const auto &e : externs) {
        if (!emitted_externs.insert(e.name).second) continue;
        if (e.lib.empty() && sys_c_syms.count(e.name)) continue;
        if (!e.lib.empty() && (e.lib.find("libc.so") != std::string::npos || e.lib == "libc"))
            continue;
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
        std::string attr;
        if (e.effects) {
            bool has_pure = false, has_nothrow = false, has_reads = false;
            for (const auto &ef : *e.effects) {
                if (ef == "pure") has_pure = true;
                else if (ef == "nothrow") has_nothrow = true;
                else if (ef.rfind("reads:", 0) == 0) has_reads = true;
            }
            if (has_pure) attr = has_reads ? " BRAT_ATTR_PURE" : " BRAT_ATTR_CONST";
            else if (has_nothrow) attr = " BRAT_ATTR_NOTHROW";
        }
        const std::string ext_head =
            cat(e.name, "(", cparams.empty() ? "void" : cparams, ")");
        ln("", cat("extern ", ffi_decl_named(e.ret ? *e.ret : std::string(), ext_head), attr, ";"));
    }
    ln("", "");
    for (const auto &cb : cb_wrappers) {
        std::string joined;
        for (const auto &p : cb.params) {
            if (!joined.empty()) joined += ", ";
            joined += p;
        }
        const std::string cb_head = cat("brat_cb_", cb.name, "(", joined, ")");
        ln("", cat(ffi_decl_named(cb.ret_ann, cb_head), ";"));
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
            } else if (ann && *ann == "bool") {
                pushes.push_back(cat("vec_push(_a, vbool(", arg, "));"));
            } else if (ann && *ann == "str") {
                pushes.push_back(cat("vec_push(_a, vstr_ffi_arg(", arg, "));"));
            } else if (ann && (*ann == "f32" || *ann == "f64")) {
                // `f32` arrives as a C float: widen once, here, because BRAT has
                // a single float type and the callee expects one.
                pushes.push_back(cat("vec_push(_a, vfloat((double)", arg, "));"));
            } else if (ann && (ann->rfind("ptr", 0) == 0 || ann->rfind("fn", 0) == 0)) {
                pushes.push_back(cat("vec_push(_a, vint((int64_t)(intptr_t)", arg, "));"));
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
        const std::string cb_head = cat("brat_cb_", cb.name, "(", joined, ")");
        ln("", cat(ffi_decl_named(cb.ret_ann, cb_head), " {"));
        ln("  ", "brat_mt_enable();");
        ln("  ", "Vec *_a = vvec();");
        for (const auto &p : pushes) ln("  ", p);
        ln("  ", "vcb_check(vnil());");
        ln("  ", cat("Val _r = ", cfn("fn:" + cb.name), "(NULL, _a);"));
        // The packed arguments belong to this call: C holds nothing from them,
        // so a wrapper that skips this leaks a Vec per callback invocation.
        ln("  ", "vrc_dec(vvecv(_a));");
        ln("  ", "vcb_check(_r);");
        ln("  ", "if (vdfsp == 0) { free(vdfr); vdfr = NULL; vdfcap = 0; }");
        if (!ret || *ret == "none") {
            ln("  ", "vrc_dec(_r);");
        } else if (*ret == "bool") {
            ln("  ", "return _r.as.b;");
        } else if (*ret == "str") {
            ln("  ", "const char *_sp = vstr_pin_ret(_r); vrc_dec(_r); vcb_check(vnil()); return _sp;");
        } else if (ret->rfind("ptr", 0) == 0 || ret->rfind("fn", 0) == 0) {
            ln("  ", cat("return (", ffi_decl_type(*ret), ")(intptr_t)_r.as.ival;"));
        } else if (*ret == "f32" || *ret == "f64") {
            // BRAT has one float type; a `float` return read out of `ival` is
            // the float's bit pattern, which is how 3.5 became 4.6e18.
            ln("  ", "return _r.as.fval;");
        } else if (structs.count(*ret)) {
            ln("  ", cat(cb.ret, " _out = ", val_to_c_struct(*ret, "_r", true), ";"));
            ln("  ", "vrc_dec(_r);");
            ln("  ", "vcb_check(vnil());");
            ln("  ", "return _out;");
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
    std::vector<std::pair<std::string, std::string>> abi_entries;
    for (const auto &exname : exported) {
        auto it = fns.find(exname);
        if (it == fns.end() || it->second.ctor) continue;
        const FnDecl &f = *it->second.decl;
        std::optional<std::string> ret;
        auto rit = fn_ret.find(exname);
        if (rit != fn_ret.end()) ret = rit->second;
        std::vector<std::string> cparams, pushes, abi_ps;
        bool ok = true;
        for (size_t i = 0; i < f.params.size(); i++) {
            const auto &ann = f.params[i].ann;
            std::string arg = cat("a", i);
            if (ann && *ann == "bool") {
                cparams.push_back(cat("_Bool ", arg));
                pushes.push_back(cat("vec_push(_a, vbool(", arg, "));"));
                abi_ps.push_back("bool");
            } else if (ann && (*ann == "i64" || *ann == "i32" || *ann == "i16" ||
                               *ann == "i8" || *ann == "isize")) {
                const char *ct = *ann == "i64" ? "int64_t" : (*ann == "i32" ? "int"
                                 : (*ann == "i16" ? "short" : (*ann == "i8" ? "signed char" : "ptrdiff_t")));
                cparams.push_back(cat(ct, " ", arg));
                pushes.push_back(cat("vec_push(_a, vint(", arg, "));"));
                abi_ps.push_back(*ann);
            } else if (ann && *ann == "u8") {
                cparams.push_back(cat("unsigned char ", arg));
                pushes.push_back(cat("vec_push(_a, vint(", arg, "));"));
                abi_ps.push_back("u8");
            } else if (ann && *ann == "u16") {
                cparams.push_back(cat("unsigned short ", arg));
                pushes.push_back(cat("vec_push(_a, vint(", arg, "));"));
                abi_ps.push_back("u16");
            } else if (ann && (*ann == "u32" || *ann == "u64" || *ann == "usize")) {
                cparams.push_back(cat(*ann == "u32" ? "unsigned" : (*ann == "u64" ? "uint64_t"
                                                  : "size_t"), " ", arg));
                pushes.push_back(cat("vec_push(_a, vint(", arg, "));"));
                abi_ps.push_back(*ann);
            } else if (ann && *ann == "f64") {
                cparams.push_back(cat("double ", arg));
                pushes.push_back(cat("vec_push(_a, vfloat(", arg, "));"));
                abi_ps.push_back("f64");
            } else if (ann && *ann == "f32") {
                cparams.push_back(cat("float ", arg));
                pushes.push_back(cat("vec_push(_a, vfloat((double)", arg, "));"));
                abi_ps.push_back("f32");
            } else if (ann && *ann == "str") {
                cparams.push_back(cat("const char * ", arg));
                pushes.push_back(cat("vec_push(_a, vstr_ffi_arg(", arg, "));"));
                abi_ps.push_back("str");
            } else if (ann && (*ann == "ptr" || ann->rfind("ptr[", 0) == 0)) {
                cparams.push_back(cat("void * ", arg));
                pushes.push_back(cat("vec_push(_a, vint((int64_t)", arg, "));"));
                abi_ps.push_back("ptr");
            } else if (ann && structs.count(*ann)) {
                cparams.push_back(cat(ffi_c(*ann), " ", arg));
                pushes.push_back(cat("vec_push(_a, ", c_struct_to_val(*ann, arg), ");"));
                abi_ps.push_back(*ann);
            } else {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        std::string cret, abi_ret;
        if (!ret || *ret == "none") {
            cret = "void";
            abi_ret = "none";
        } else if (*ret == "bool") {
            cret = "_Bool";
            abi_ret = "bool";
        } else if (structs.count(*ret)) {
            cret = ffi_c(*ret);
            abi_ret = *ret;
        } else if (*ret == "i64" || *ret == "i32" || *ret == "i16" || *ret == "i8" ||
                   *ret == "isize" || *ret == "u8" || *ret == "u16" || *ret == "u32" ||
                   *ret == "u64" || *ret == "usize") {
            cret = *ret == "i64" ? "int64_t" : (*ret == "i32" ? "int"
                 : (*ret == "i16" ? "short" : (*ret == "i8" ? "signed char"
                 : (*ret == "isize" ? "ptrdiff_t" : (*ret == "u8" ? "unsigned char"
                 : (*ret == "u16" ? "unsigned short" : (*ret == "u32" ? "unsigned"
                 : (*ret == "u64" ? "uint64_t" : "size_t"))))))));
            abi_ret = *ret;
        } else if (*ret == "f64") {
            cret = "double";
            abi_ret = "f64";
        } else if (*ret == "f32") {
            cret = "float";
            abi_ret = "f32";
        } else if (*ret == "str") {
            cret = "const char *";
            abi_ret = "str";
        } else if (*ret == "ptr" || ret->rfind("ptr[", 0) == 0) {
            cret = "void *";
            abi_ret = "ptr";
        } else {
            continue;
        }
        std::string abi_sig = abi_ret + "(";
        for (size_t i = 0; i < abi_ps.size(); i++) {
            if (i) abi_sig += ",";
            abi_sig += abi_ps[i];
        }
        abi_sig += ")";
        abi_entries.push_back({exname, abi_sig});
        std::string joined;
        for (const auto &p : cparams) {
            if (!joined.empty()) joined += ", ";
            joined += p;
        }
        ln("", "");
        ln("", cat(cret, " ", exname, "(", joined, ") {"));
        ln("  ", "brat_mt_enable();");
        ln("  ", "Vec *_a = vvec();");
        for (const auto &p : pushes) ln("  ", p);
        ln("  ", "vcb_check(vnil());");
        ln("  ", cat("Val _r = ", cfn("fn:" + exname), "(NULL, _a);"));
        ln("  ", "vargs_drop(_a);");
        ln("  ", "vcb_check(_r);");
        ln("  ", "if (vdfsp == 0) { free(vdfr); vdfr = NULL; vdfcap = 0; }");
        if (!ret || *ret == "none") {
            ln("  ", "vrc_dec(_r);");
        } else if (*ret == "str") {
            ln("  ", "const char *_sp = vstr_pin_ret(_r); vrc_dec(_r); vcb_check(vnil()); return _sp;");
        } else if (*ret == "bool") {
            ln("  ", "return _r.as.b;");
        } else if (*ret == "ptr" || ret->rfind("ptr[", 0) == 0) {
            ln("  ", "return (void *)_r.as.ival;");
        } else if (*ret == "f64") {
            ln("  ", "return _r.as.fval;");
        } else if (*ret == "f32") {
            ln("  ", "return (float)_r.as.fval;");
        } else if (structs.count(*ret)) {
            ln("  ", cat(ffi_c(*ret), " _out = ", val_to_c_struct(*ret, "_r", true), ";"));
            ln("  ", "vrc_dec(_r);");
            ln("  ", "vcb_check(vnil());");
            ln("  ", "return _out;");
        } else {
            ln("  ", "return _r.as.ival;");
        }
        ln("", "}");
    }
    if (!abi_entries.empty()) {
        ln("", "");
        ln("", "extern const BPluginAbiEntry brat_plugin_abi_v1[];");
        ln("", "const BPluginAbiEntry brat_plugin_abi_v1[] = {");
        for (const auto &ae : abi_entries)
            ln("  ", cat("{\"", ae.first, "\", \"", ae.second, "\"},"));
        ln("  ", "{NULL, NULL}");
        ln("", "};");
    }
    if (mic_slot >= 0) {
        std::vector<std::string> ic_decls;
        for (size_t sid = 0; sid < shape_defs.size(); sid++) {
            const auto &fnames = shape_defs[sid].second;
            std::string klits;
            for (size_t j = 0; j < fnames.size(); j++) {
                if (j) klits += ", ";
                klits += cat("\"", clit(fnames[j]), "\"");
            }
            ic_decls.push_back(cat("static BRAT_UNUSED const char *const _sk", static_cast<int>(sid), "[",
                                   static_cast<int>(fnames.size()), "] = {", klits, "};"));
        }
        if (!shape_defs.empty())
            ic_decls.push_back(cat("static BShape _shp[", static_cast<int>(shape_defs.size()), "];"));
        if (fic_n > 0)
            ic_decls.push_back(cat("static BTLS BFieldIC _fic[", fic_n, "];"));
        if (ic_n > 0)
            ic_decls.push_back(cat("static BTLS BMethIC _mic[", ic_n, "];"));
        if (!ic_decls.empty())
            out.insert(out.begin() + mic_slot, ic_decls.begin(), ic_decls.end());
    }
    {
        unsigned mask = 0u;
        size_t mask_idx = out.size();
        for (size_t i = 0; i < out.size(); i++) {
            const auto &line = out[i];
            if (mask_idx == out.size() && line.rfind("#define BRAT_CRT_MASK ", 0) == 0) {
                mask_idx = i;
                continue;
            }
            if (line.rfind("#line ", 0) == 0 ||
                line.rfind("/* fn ", 0) == 0 ||
                line.rfind("/* meth ", 0) == 0)
                continue;
            if (line.find("vlib_") != std::string::npos ||
                line.find("vplugin_") != std::string::npos ||
                line.find("BPluginAbiEntry") != std::string::npos)
                mask |= 0x001u | 0x100u;
            if (line.find("vre_") != std::string::npos)
                mask |= 0x002u;
            if (line.find("vtcp_") != std::string::npos)
                mask |= 0x004u;
            if (line.find("vsha256_bytes") != std::string::npos ||
                line.find("vrandom_bytes") != std::string::npos ||
                line.find("vhttps_request_raw") != std::string::npos)
                mask |= 0x008u | 0x004u;
            if (line.find("vsql_") != std::string::npos)
                mask |= 0x010u;
            if (line.find("vproc_run") != std::string::npos)
                mask |= 0x020u;
            if (line.find("vcase(") != std::string::npos || line.find("vcp_") != std::string::npos ||
                line.find("\"lower\"") != std::string::npos || line.find("\"upper\"") != std::string::npos)
                mask |= 0x040u;
            if (line.find("vchan_") != std::string::npos ||
                line.find("\"send\"") != std::string::npos || line.find("\"recv\"") != std::string::npos ||
                line.find("\"close\"") != std::string::npos)
                mask |= 0x080u;
            if (line.find("vspawn(") != std::string::npos ||
                line.find("vparfor(") != std::string::npos ||
                line.find("vscope_") != std::string::npos)
                mask |= 0x080u | 0x100u;
            if (line.find("vjson_parse") != std::string::npos ||
                line.find("vjson_stringify") != std::string::npos)
                mask |= 0x400u;
        }
        if (!externs.empty() || !exported.empty() || !cb_wrappers.empty())
            mask |= 0x100u;
        crt_mask = mask;
        if (mask_idx < out.size())
            out[mask_idx] = "#define BRAT_CRT_MASK " + std::to_string(mask) + "u";
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

std::string Emitter::err_scope_unwind() {
    std::string out;
    const size_t it_base = err_goto ? err_iter_depth : 0;
    for (size_t i = iter_cleanups.size(); i > it_base; --i)
        out += iter_cleanups[i - 1];
    for (size_t i = scopes.size(); i > err_scope_depth; --i)
        out += cat("vscope_end(vint(", scopes[i - 1], ")); ");
    return out;
}

std::string Emitter::err_bail() {
    std::string rel;
    for (auto it = call_cleanup.rbegin(); it != call_cleanup.rend(); ++it) {
        if (it->rfind("@args:", 0) == 0)
            rel += cat("vargs_drop(", it->substr(6), "); ");
        else
            rel += cat("vdrop_temp(", *it, "); ");
    }
    std::string unwind = err_scope_unwind();
    if (err_goto)
        return cat("{ ", rel, unwind, "goto ", *err_goto, "; }");
    if (fin == "__fin_top")
        return cat("{ ", rel, unwind, "fprintf(stderr, \"unhandled: \"); vprint_err(verr_take()); "
                   "fprintf(stderr, \"\\n\"); return 1; }");
    return cat("{ ", rel, unwind, "__ret = verr_leave(); goto ", fin, "; }");
}

std::string Emitter::err_check_line(const std::vector<std::string> &drops) {
    // `drops` are temps this statement built and handed to nothing: on the way to
    // the catch their owner is gone, so they are released here.
    std::string rel;
    std::set<std::string> seen;
    for (auto &d : drops) {
        if (seen.insert(d).second) rel += cat("vdrop_temp(", d, "); ");
    }
    for (auto it = call_cleanup.rbegin(); it != call_cleanup.rend(); ++it) {
        if (!seen.insert(*it).second) continue;
        if (it->rfind("@args:", 0) == 0)
            rel += cat("vargs_drop(", it->substr(6), "); ");
        else
            rel += cat("vdrop_temp(", *it, "); ");
    }
    std::string unwind = err_scope_unwind();
    if (err_goto)
        return cat("if (verr_pending()) { ", rel, unwind, "goto ", *err_goto, "; }");
    if (fin == "__fin_top")
        return cat("if (verr_pending()) { ", rel, unwind,
                   "fprintf(stderr, \"unhandled: \"); vprint_err(verr_take()); "
                   "fprintf(stderr, \"\\n\"); return 1; }");
    return cat("if (verr_pending()) { ", rel, unwind, "__ret = verr_leave(); goto ", fin, "; }");
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
    int width = el == "bool" || el == "u8" || el == "i8" ? 1
              : el == "u16" || el == "i16" ? 2
              : el == "i32" || el == "u32" || el == "f32" ? 4 : 8;
    std::string ctype = el == "bool" ? "_Bool" : el == "i8" ? "int8_t"
                      : el == "u8" ? "uint8_t" : el == "i16" ? "int16_t"
                      : el == "u16" ? "uint16_t" : el == "i32" ? "int32_t"
                      : el == "u32" ? "uint32_t" : el == "i64" ? "int64_t"
                      : el == "u64" ? "uint64_t" : el == "usize" ? "size_t"
                      : el == "ptr" ? "void*" : el == "f32" ? "float" : el == "f64" ? "double"
                      : width == 1 ? "unsigned char" : width == 2 ? "short"
                      : width == 4 ? "int" : "int64_t";
    std::string cn = var_ref(e.a);
    std::string scratch = new_tmp("out");
    std::string initial = el == "bool" ? cat("vis_true(", cn, ")")
                        : (el == "f32" || el == "f64") ? cat("vnum(", cn, ")")
                        : el == "ptr" ? cat("(intptr_t)(", cn, ").as.ival")
                        : cat("(", cn, ").as.ival");
    out_binds.push_back({e.a, cn, scratch, el});
    return {{cat(ctype, " ", scratch, " = (", ctype, ")(", initial, ");")},
            cat("(void *)&", scratch)};
}

std::vector<std::string> Emitter::out_flush() {
    std::vector<std::string> out;
    for (const auto &b : out_binds) {
        out.push_back(cat("vrc_dec(", b.cn, ");"));
        if (b.elem == "f32" || b.elem == "f64")
            out.push_back(cat(b.cn, " = vfloat((double)", b.scratch, ");"));
        else if (b.elem == "bool")
            out.push_back(cat(b.cn, " = vbool(", b.scratch, ");"));
        else if (b.elem == "ptr")
            out.push_back(cat(b.cn, " = vint((int64_t)(intptr_t)", b.scratch, ");"));
        else {
            bool is_unsigned = b.elem == "u8" || b.elem == "u16" || b.elem == "u32"
                            || b.elem == "u64" || b.elem == "usize";
            std::string c = is_unsigned
                ? cat("(int64_t)(uint64_t)", b.scratch)
                : cat("(int64_t)", b.scratch);
            out.push_back(cat(b.cn, " = vint(", c, ");"));
        }
    }
    return out;
}

V Emitter::ex_call(const Expr &e) {
    const std::string &name = e.a;
    ExprList args = ptrs(e.kids);
    if (name == "print") {
        if (args.empty())
            return {{}, "(BRAT_PRINT_LOCK(), vprint_nl(), BRAT_PRINT_UNLOCK(), vint(0))"};
        // vprint reads its argument; a value this statement built is nobody's
        // afterwards, and leaving it was a leak of one container per print.
        // `print(move m)` took `m` out of its binding, which is the same.
        // A later argument that raises skips the print, so the parts built
        // before it ride in `call_cleanup` until the line is out.
        std::vector<std::string> pre_all, parts, mine;
        CleanupGuard guard{call_cleanup, call_cleanup.size()};
        for (const Expr *a : args) {
            V v = ex(*a);
            for (auto &p : v.pre) pre_all.push_back(p);
            std::string t = new_tmp("p");
            pre_all.push_back(cat("Val ", t, " = (", v.val, ");"));
            pre_all.push_back(err_check_line());
            parts.push_back(t);
            if (dec_able(*a, heap_returns()) || moves_owned_local(*a)) {
                mine.push_back(t);
                call_cleanup.push_back(t);
            }
        }
        pre_all.push_back("BRAT_PRINT_LOCK();");
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) pre_all.push_back("printf(\" \");");
            pre_all.push_back(cat("vprint(", parts[i], ");"));
        }
        pre_all.push_back("vprint_nl();");
        pre_all.push_back("BRAT_PRINT_UNLOCK();");
        for (auto &t : mine) pre_all.push_back(cat("vdrop_temp(", t, ");"));
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
        pre.push_back(cat("vargs_drop(", a.val, ");"));
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
        out.push_back(cat("vargs_drop(", a.val, ");"));
        if (body) {
            V b = ex(*body);
            for (auto &p : b.pre) out.push_back(p);
            std::string t = new_tmp("sp"), sv = new_tmp("sv");
            out.push_back(cat("Vec *", sv, " = vvec();"));
            out.push_back(cat("Val ", t, " = vcall_any(", b.val, ", ", sv, ");"));
            out.push_back(cat("vrc_dec(vvecv(", sv, "));"));
            out.push_back(cat("vrc_dec(", b.val, ");"));
            return {out, t};
        }
        return {out, "vint(0)"};
    }
    static const std::map<std::string, std::pair<const char *, const char *>> READ_ONLY_1 = {
        {"len", {"vlen(", ")"}},
        {"str", {"vstr_of(", ")"}},
        {"tagof", {"vtagof(", ")"}},
        {"own", {"vown(", ")"}},
        {"is_ok", {"vbool(vis_ok(", "))"}},
        {"is_err", {"vbool(vis_err(", "))"}},
        {"is_some", {"vbool(vis_some(", "))"}},
        {"is_none", {"vbool(vis_none(", "))"}},
        {"is_vec", {"vbool((", ").tag == V_VEC)"}},
        {"is_str", {"vbool((", ").tag == V_STR)"}},
        {"is_map", {"vbool((", ").tag == V_STRUCT)"}},
        {"is_bool", {"vbool((", ").tag == V_BOOL)"}},
        {"is_int", {"vbool((", ").tag == V_INT)"}},
        {"is_float", {"vbool((", ").tag == V_FLOAT)"}},
    };
    if (auto ro = READ_ONLY_1.find(name); ro != READ_ONLY_1.end()) {
        need_arity(args, 1, name);
        const auto [pre_s, post_s] = ro->second;
        return read_only_call(*args[0], [=](const std::string &v) { return cat(pre_s, v, post_s); });
    }
    if (name == "__at") {
        need_arity(args, 2, name);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        return {pre, cat("vtag_at_owned(", a.val, ", ", b.val, ")")};
    }
    if (name == "__stag_is") {
        need_arity(args, 2, name);
        std::string tname;
        if (plain_cstr_key(*args[1], tname)) {
            return read_only_call(*args[0], [&](const std::string &v) {
                return cat("vstag_is(", v, ", \"", clit(tname), "\")");
            });
        }
        throw EmitError("__stag_is requires string literal tag");
    }
    if (name == "__shas") {
        need_arity(args, 2, name);
        std::string fname;
        if (plain_cstr_key(*args[1], fname)) {
            return read_only_call(*args[0], [&](const std::string &v) {
                return cat("vshas_ic(", v, ", \"", clit(fname), "\", ", next_fic(), ")");
            });
        }
        throw EmitError("__shas requires string literal field");
    }
    if (name == "Ok" || name == "Err" || name == "Some") {
        need_arity(args, 1, name);
        return ex_tag(lower(name), args);
    }
    if (name == "unwrap_or") {
        need_arity(args, 2, name);
        V a = ex(*args[0]);
        V b = ex(*args[1]);
        std::string ta = new_tmp("uo");
        std::string tr = new_tmp("ur");
        std::string st = new_tmp("us");
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("Val ", ta, " = (", a.val, ");"));
        pre.push_back(cat("int ", st, " = vunwrap_or_state(", ta, ");"));
        const bool drop_a = dec_able(*args[0], heap_returns());
        if (drop_a)
            pre.push_back(cat("if (", st, " < 0) { vdrop_temp(", ta, "); ", err_bail(), " }"));
        else
            pre.push_back(cat("if (", st, " < 0) { ", err_bail(), " }"));
        pre.push_back(cat("Val ", tr, " = vnil();"));
        pre.push_back(cat("if (", st, ") { ", tr, " = vtag_payload(", ta, ");",
                          drop_a ? cat(" vdrop_temp(", ta, ");") : "", " } else {"));
        if (drop_a) pre.push_back(cat("    vdrop_temp(", ta, ");"));
        for (auto &p : b.pre) pre.push_back(cat("    ", p));
        pre.push_back(cat("    ", tr, " = (", own_operand(*args[1], b.val), ");"));
        pre.push_back("}");
        return {pre, tr};
    }
    if (name == "unwrap") {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return part_of_temp(a, *args[0], "vunwrap(", ")");
    }
    if (name == "assert") {
        if (args.empty() || args.size() > 2) throw EmitError("assert arity");
        V a = ex(*args[0]);
        std::string bail = err_bail();
        if (args.size() == 2) {
            V b = ex(*args[1]);
            std::vector<std::string> pre = a.pre;
            for (auto &p : b.pre) pre.push_back(p);
            pre.push_back(cat("if (!vassert2(", keep(*args[0], a.val), ", ", keep(*args[1], b.val), ")) ", bail));
            return {pre, "vbool(1)"};
        }
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("if (!vassert1(", keep(*args[0], a.val), ")) ", bail));
        return {pre, "vbool(1)"};
    }
    static const std::map<std::string, std::pair<const char *, size_t>> BORROW_CALLS = {
        {"read_file", {"vreadfile", 1}}, {"read_bytes", {"vreadbytes", 1}},
        {"utf8_bytes", {"vutf8_bytes", 1}}, {"utf8_str", {"vutf8_str", 1}},
        {"json_parse", {"vjson_parse", 1}}, {"fs_exists", {"vfs_exists", 1}},
        {"fs_is_dir", {"vfs_is_dir", 1}}, {"fs_file_size", {"vfs_file_size", 1}},
        {"fs_mkdir_all", {"vfs_mkdir_all", 1}}, {"fs_list_dir", {"vfs_list_dir", 1}},
        {"fs_remove_dir", {"vfs_remove_dir", 1}}, {"fs_open", {"vfs_open", 1}},
        {"fs_read_line", {"vfs_read_line", 1}}, {"fs_close", {"vfs_close", 1}},
        {"fs_remove", {"vfs_remove", 1}}, {"sha256_bytes", {"vsha256_bytes", 1}},
        {"random_bytes", {"vrandom_bytes", 1}}, {"sql_open", {"vsql_open", 1}},
        {"sql_close", {"vsql_close", 1}}, {"tcp_listen", {"vtcp_listen", 1}},
        {"tcp_connect", {"vtcp_connect", 1}}, {"tcp_accept", {"vtcp_accept", 1}},
        {"tcp_close", {"vtcp_close", 1}}, {"sbuf_len", {"vsbuf_len", 1}},
        {"sbuf_str", {"vsbuf_str", 1}}, {"env_get", {"venv_get", 1}},
        {"env_get_bytes", {"venv_get_bytes", 1}},
        {"write_file", {"vwritefile", 2}}, {"append_file", {"vappendfile", 2}},
        {"write_bytes", {"vwritebytes", 2}}, {"proc_run", {"vproc_run", 2}},
        {"fs_rename", {"vfs_rename", 2}}, {"json_stringify", {"vjson_stringify", 2}},
        {"sql_exec", {"vsql_exec", 2}}, {"sbuf_add", {"vsbuf_add", 2}},
        {"tcp_recv", {"vtcp_recv", 2}}, {"tcp_send", {"vtcp_send", 2}},
        {"sql_query", {"vsql_query", 3}},
        {"https_request_raw", {"vhttps_request_raw", 4}},
    };
    if (name == "proc_run" && args.size() == 3)
        return borrow_builtin("vproc_run_to", args);
    if (auto bc = BORROW_CALLS.find(name); bc != BORROW_CALLS.end()) {
        need_arity(args, bc->second.second, name);
        return borrow_builtin(bc->second.first, args);
    }
    static const std::map<std::string, const char *> ZERO_ARG_CALLS = {
        {"sbuf", "vsbuf()"}, {"stdin_read_line", "vstdin_read_line()"},
        {"join_all", "(vjoin_all(), vint(0))"}, {"args", "vargs()"},
        {"args_bytes", "vargs_bytes()"}, {"time_ms", "vtime_ms()"},
        {"mono_ns", "vmono_ns()"}, {"ptr_null", "vint(0)"},
        {"errno", "vint(errno)"},
    };
    if (auto zc = ZERO_ARG_CALLS.find(name); zc != ZERO_ARG_CALLS.end()) {
        need_arity(args, 0, name);
        return {{}, zc->second};
    }
    static const std::map<std::string, std::pair<const char *, const char *>> UNARY_EXPR_CALLS = {
        {"chan", {"vchan_new(", ")"}},
        {"exit", {"(vexit(", "), vint(0))"}},
        {"sleep_ms", {"(vsleep_ms(", "), vint(0))"}},
        {"ptr_is_null", {"vbool((", ").as.ival == 0)"}},
        {"ptr_of_str", {"vptr_of_str(", ")"}},
        {"strerror", {"vstr_c_checked(strerror((int)(",
                      ").as.ival), \"strerror: NULL message\", \"strerror: invalid UTF-8\")"}},
    };
    if (auto uc = UNARY_EXPR_CALLS.find(name); uc != UNARY_EXPR_CALLS.end()) {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return {a.pre, cat(uc->second.first, a.val, uc->second.second)};
    }
    if (name == "eprint") {
        V a = args_vec(args);
        std::vector<std::string> pre = a.pre;
        pre.push_back(cat("veprint(", a.val, ");"));
        pre.push_back(cat("vargs_drop(", a.val, ");"));
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
    if (name == "$arena_enter" || name == "$arena_leave") {
        need_arity(args, 0, name);
        return {{cat(name == "$arena_enter" ? "varena_enter" : "varena_leave", "();")}, "vint(0)"};
    }
    if (name == "slice_of" || name == "slice_of_ptr" || name == "slice_len" || name == "slice_get" ||
        name == "slice_set" || name == "str_from_ptr" || name == "slice_of_struct" ||
        name == "slice_struct_of")
        return ex_slice(name, args);
    if (name == "lib_open" || name == "lib_sym" || name == "lib_close" || name == "call")
        return ex_plugin(name, args);
    if (name == "ptr_offset") {
        need_arity(args, 2, name);
        V a = ex(*args[0]), b = ex(*args[1]);
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        return {pre, cat("vint((int64_t)((char *)(", a.val, ").as.ival + (", b.val, ").as.ival))")};
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
        V a = ex(*args[0]), b = ex(*args[1]);
        const Expr &tn = *args[2];
        if (tn.k != EK::Str) throw EmitError("ptr_write_struct wants a struct name as a string literal");
        if (!structs.count(tn.a)) throw EmitError("ptr_write_struct: " + tn.a + " is not an extern struct");
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        if (dec_able(*args[1], heap_returns())) {
            std::string ws = new_tmp("ws"), wr = new_tmp("wr");
            pre.push_back(cat("Val ", ws, " = (", b.val, ");"));
            pre.push_back(cat("Val ", wr, " = vunwind;"));
            pre.push_back(cat("if (vptr_live(", a.val, ")) { *(bf_", tn.a, " *)(", a.val, ").as.ival = ",
                              val_to_c_struct(tn.a, ws, true), "; ", wr, " = (verr_pending() ? vunwind : vint(0)); }"));
            pre.push_back(cat("vdrop_temp(", ws, ");"));
            return {pre, wr};
        }
        std::string stmt = cat("*(", "bf_", tn.a, " *)(", a.val, ").as.ival = ", val_to_c_struct(tn.a, b.val, true));
        return {pre, cat("((vptr_live(", a.val, ")) ? ((", stmt, "), (verr_pending() ? vunwind : vint(0))) : vunwind)")};
    }
    static const std::map<std::string, std::pair<const char *, const char *>> PTR_READS = {
        {"ptr_read_i64", {"vint(*(int64_t *)(", ").as.ival)"}},
        {"ptr_read_f32", {"vfloat((double)*(float *)(", ").as.ival)"}},
        {"ptr_read_f64", {"vfloat(*(double *)(", ").as.ival)"}},
        {"ptr_read_ptr", {"vint((int64_t)*(void **)(", ").as.ival)"}},
        {"ptr_read_str", {"vstr_ptr_read((const char *)(", ").as.ival)"}},
    };
    if (auto pr = PTR_READS.find(name); pr != PTR_READS.end()) {
        need_arity(args, 1, name);
        V a = ex(*args[0]);
        return {a.pre, cat("((vptr_live(", a.val, ")) ? ", pr->second.first, a.val, pr->second.second, " : vunwind)")};
    }
    static const std::map<std::string, std::pair<const char *, const char *>> PTR_WRITES = {
        {"ptr_write_i64", {"(*(int64_t *)(", ").as.ival)"}},
        {"ptr_write_f32", {"(*(float *)(", ").as.fval)"}},
        {"ptr_write_f64", {"(*(double *)(", ").as.fval)"}},
        {"ptr_write_ptr", {"(*(void **)(", ").as.ival)"}},
    };
    if (name == "ptr_write_str" || PTR_WRITES.count(name)) {
        need_arity(args, 2, name);
        V a = ex(*args[0]), b = ex(*args[1]);
        std::vector<std::string> pre = a.pre;
        for (auto &p : b.pre) pre.push_back(p);
        if (name == "ptr_write_str")
            return {pre, cat("vptr_write_str(", a.val, ", ", b.val, ")")};
        const char *rhs_cast = name == "ptr_write_f32" ? "(float)(" : name == "ptr_write_ptr" ? "(void *)(" : "(";
        const auto [lhs_pre, rhs_suf] = PTR_WRITES.at(name);
        return {pre, cat("((vptr_live(", a.val, ")) ? ((", lhs_pre, a.val, ").as.ival = ",
                         rhs_cast, b.val, rhs_suf, "), vint(0)) : vunwind)")};
    }
    if (name == "re_match" || name == "re_find" || name == "re_captures" || name == "re_replace") {
        int want_n = (name == "re_replace") ? 3 : 2;
        need_arity(args, want_n, name);
        std::vector<std::string> pre;
        std::vector<std::string> vals;
        std::vector<std::string> drops;
        for (const Expr *arg_node : args) {
            V a = ex(*arg_node);
            for (auto &p : a.pre) pre.push_back(p);
            if (dec_able(*arg_node, heap_returns())) {
                if (a.val.empty() || a.val[0] != '_') {
                    std::string t = new_tmp("v");
                    pre.push_back(cat("Val ", t, " = (", a.val, ");"));
                    a.val = t;
                }
                drops.push_back(a.val);
            }
            vals.push_back(a.val);
        }
        std::string expr;
        if (name == "re_match") expr = cat("vbool(vre_match(", vals[0], ", ", vals[1], "))");
        else if (name == "re_find") expr = cat("vre_find(", vals[0], ", ", vals[1], ")");
        else if (name == "re_captures") expr = cat("vre_captures(", vals[0], ", ", vals[1], ")");
        else expr = cat("vre_replace(", vals[0], ", ", vals[1], ", ", vals[2], ")");
        if (drops.empty()) return {pre, expr};
        std::string r = new_tmp("a");
        pre.push_back(cat("Val ", r, " = ", expr, ";"));
        for (auto &d : drops) pre.push_back(cat("vdrop_temp(", d, ");"));
        return {pre, r};
    }
    if (fns.count(fn_key(name))) return ex_direct(fn_key(name), args);
    if (name.find('.') != std::string::npos) return ex_dotted(name, args);
    if (args.empty() && !name.empty() && name[0] >= 'A' && name[0] <= 'Z' &&
        (sym.count(name) || (cap && cap->second.count(name)))) {
        // Bare uppercase identifiers parse as zero-arg calls; resolve to local
        // or captured bindings when one is in scope.
        ExprP var = mk_var(name);
        return ex(*var);
    }
    auto ext = ext_ix.find(name);
    if (ext != ext_ix.end()) {
        const Ext &x = externs[ext->second];
        std::vector<std::string> pa_all;
        std::string cargs;
        auto saved_out = out_binds;
        out_binds.clear();
        size_t clean_start = call_cleanup.size();
        std::vector<std::string> held;
        struct ExtGuard {
            Emitter *em;
            std::vector<OutBind> saved;
            size_t cs;
            ~ExtGuard() {
                em->out_binds = std::move(saved);
                if (em->call_cleanup.size() > cs) em->call_cleanup.resize(cs);
            }
        } guard{this, std::move(saved_out), clean_start};
        const auto &hr = heap_returns();
        for (size_t i = 0; i < args.size(); i++) {
            std::optional<std::string> ann;
            if (i < x.params->size()) {
                ann = (*x.params)[i].ann;
            } else {
                ann = x.varargs;
            }
            const Expr &a = *args[i];
            if (ann && ann->rfind("fn", 0) == 0) {
                bool sentinel = (a.k == EK::Num && !a.is_float && a.i == 0) ||
                                (a.k == EK::Un && a.a == "-" && a.kids.size() == 1 &&
                                 a.kids[0] && a.kids[0]->k == EK::Num &&
                                 !a.kids[0]->is_float && a.kids[0]->i == 1);
                if (!sentinel) {
                    check_cb_arg(*ann, a, name, static_cast<int>(i) + 1);
                    cargs += cat(cargs.empty() ? "" : ", ", "brat_cb_", a.a);
                    continue;
                }
            }
            if (ann && !FFI_TYPES.count(*ann) && !structs.count(*ann) && ann->rfind("fn", 0) != 0 &&
                ann->rfind("ptr[", 0) != 0)
                throw EmitError(cat("unsupported FFI argument type `", *ann, "` in extern fn ", name));
            if (ann && ann->rfind("ptr", 0) == 0 && a.k == EK::Un && a.a == "&") {
                in_out_arg = true;
                auto o = out_addr(*a.kids[0], ann ? &*ann : nullptr);
                in_out_arg = false;
                for (auto &p : o.first) pa_all.push_back(p);
                cargs += cat(cargs.empty() ? "" : ", ", o.second);
                continue;
            }
            V v = ex(a);
            for (auto &p : v.pre) pa_all.push_back(p);
            const bool owned_arg = dec_able(a, hr);
            if (ann && structs.count(*ann)) {
                std::string tv = new_tmp("fa");
                std::string ts = new_tmp("fs");
                pa_all.push_back(cat("Val ", tv, " = ", v.val, ";"));
                if (owned_arg) {
                    held.push_back(tv);
                    call_cleanup.push_back(tv);
                }
                pa_all.push_back(cat(ffi_c(*ann), " ", ts, " = ", val_to_c_struct(*ann, tv), ";"));
                pa_all.push_back(cat("if (verr_pending()) ", err_bail()));
                cargs += cat(cargs.empty() ? "" : ", ", ts);
                continue;
            }
            std::string t = new_tmp("fa");
            pa_all.push_back(cat("Val ", t, " = ", v.val, ";"));
            if (owned_arg) {
                held.push_back(t);
                call_cleanup.push_back(t);
            }
            std::string ok;
            bool var_slot = i >= x.params->size() && ann && *ann == "ptr";
            if (ann && *ann == "bool") {
                ok = cat(t, ".tag == V_BOOL");
            } else if (ann && *ann == "str") {
                ok = cat(t, ".tag == V_STR");
            } else if (var_slot) {
                ok = cat("((", t, ".tag == V_INT && ", t, ".as.ival >= 0) || ", t, ".tag == V_STR)");
            } else if (ann && (*ann == "f64" || *ann == "f32")) {
                ok = cat("(", t, ".tag == V_FLOAT || ", t, ".tag == V_INT)");
            } else {
                ok = cat(t, ".tag == V_INT");
                if (ann && FFI_RANGE.count(*ann)) {
                    const auto &r = FFI_RANGE.at(*ann);
                    ok += cat(" && ", t, ".as.ival >= ", r.first, " && ", t, ".as.ival <= ", r.second);
                } else if (ann && (*ann == "ptr" || ann->rfind("ptr[", 0) == 0)) {
                    ok += cat(" && ", t, ".as.ival >= 0");
                }
            }
            pa_all.push_back(cat("if (", t, ".tag == V_MOVED) { vraise(\"use of moved value\"); ",
                                 err_bail(), " }"));
            pa_all.push_back(cat("if (!(", ok, ")) { vraise(\"ffi: arg ", i + 1, " of ", name, " wants ",
                                 ann.value_or("value"), "\"); ", err_bail(), " }"));
            if (ann && *ann == "bool") {
                cargs += cat(cargs.empty() ? "" : ", ", t, ".as.b");
            } else if (ann && *ann == "str") {
                std::string ps = new_tmp("ps");
                pa_all.push_back(cat("const char *", ps, " = vstr_pin(", t, ");"));
                pa_all.push_back(cat("if (verr_pending()) { ", err_bail(), " }"));
                cargs += cat(cargs.empty() ? "" : ", ", ps);
            } else if (var_slot) {
                std::string pv = new_tmp("pv");
                pa_all.push_back(cat("void *", pv, " = (", t, ".tag == V_STR ? (void *)vstr_pin(", t,
                                     ") : (void *)", t, ".as.ival);"));
                pa_all.push_back(cat("if (verr_pending()) { ", err_bail(), " }"));
                cargs += cat(cargs.empty() ? "" : ", ", pv);
            } else if (ann && ann->rfind("fn", 0) == 0) {
                cargs += cat(cargs.empty() ? "" : ", ", "(void *)(intptr_t)", t, ".as.ival");
            } else if (ann && (*ann == "ptr" || ann->rfind("ptr[", 0) == 0)) {
                cargs += cat(cargs.empty() ? "" : ", ", "(void *)", t, ".as.ival");
            } else if (ann && *ann == "f64") {
                cargs += cat(cargs.empty() ? "" : ", ", "(", t, ".tag == V_INT ? (double)", t,
                             ".as.ival : ", t, ".as.fval)");
            } else if (ann && *ann == "f32") {
                cargs += cat(cargs.empty() ? "" : ", ", "(float)(", t, ".tag == V_INT ? (double)", t,
                             ".as.ival : ", t, ".as.fval)");
            } else {
                cargs += cat(cargs.empty() ? "" : ", ", t, ".as.ival");
            }
        }
        std::string call = cat(name, "(", cargs, ")");
        std::string ret = x.ret.value_or("");
        std::vector<std::string> fl = out_flush();
        out_binds.clear();
        if (call_cleanup.size() > clean_start) call_cleanup.resize(clean_start);
        auto finish_post_call = [&]() {
            for (auto &f : fl) pa_all.push_back(f);
            for (auto &d : held) pa_all.push_back(cat("vdrop_temp(", d, ");"));
        };
        if (errno_externs.count(name)) {
            std::string r = new_tmp("er");
            std::string eno = new_tmp("eno");
            std::string sentinel = ret == "ptr" ? "0" : "-1";
            pa_all.push_back("errno = 0;");
            pa_all.push_back(cat("int64_t ", r, " = (int64_t)", call, ";"));
            pa_all.push_back(cat("int ", eno, " = errno;"));
            finish_post_call();
            pa_all.push_back(cat("if (", r, " == ", sentinel, ") { errno = ", eno, "; vraise_errno(\"", name, "\"); ",
                                 err_bail(), " }"));
            return {pa_all, cat("vint(", r, ")")};
        }
        if (ret == "bool") {
            if (fl.empty() && held.empty()) return {pa_all, cat("vbool(", call, ")")};
            std::string r = new_tmp("br");
            pa_all.push_back(cat("int ", r, " = (int)(", call, ");"));
            finish_post_call();
            return {pa_all, cat("vbool(", r, ")")};
        }
        if (ffi_int_result(ret)) {
            std::string r = new_tmp("ir");
            pa_all.push_back(cat("int64_t ", r, " = (int64_t)", call, ";"));
            finish_post_call();
            return {pa_all, cat("vint(", r, ")")};
        }
        if (ret == "u64" || ret == "usize") {
            std::string r = new_tmp("ur");
            pa_all.push_back(cat("uint64_t ", r, " = ", call, ";"));
            finish_post_call();
            pa_all.push_back(cat("if (", r, " > 9223372036854775807ULL) { vraise(\"ffi: result of ", name,
                                 " does not fit i64\"); ", err_bail(), " }"));
            return {pa_all, cat("vint((int64_t)", r, ")")};
        }
        if (ret == "str") {
            std::string t = new_tmp("fr");
            pa_all.push_back(cat("Val ", t, " = vstr_ffi(", call, ");"));
            finish_post_call();
            pa_all.push_back(cat("if (verr_pending()) ", err_bail()));
            return {pa_all, t};
        }
        if (ret == "ptr" || ret.rfind("ptr[", 0) == 0 || ret.rfind("fn", 0) == 0) {
            if (fl.empty() && held.empty()) return {pa_all, cat("vint((int64_t)(intptr_t)(", call, "))")};
            std::string r = new_tmp("pr");
            pa_all.push_back(cat("int64_t ", r, " = (int64_t)(intptr_t)(", call, ");"));
            finish_post_call();
            return {pa_all, cat("vint(", r, ")")};
        }
        if (ret == "f64") {
            if (fl.empty() && held.empty()) return {pa_all, cat("vfloat(", call, ")")};
            std::string r = new_tmp("fr");
            pa_all.push_back(cat("double ", r, " = ", call, ";"));
            finish_post_call();
            return {pa_all, cat("vfloat(", r, ")")};
        }
        if (ret == "f32") {
            if (fl.empty() && held.empty()) return {pa_all, cat("vfloat((double)", call, ")")};
            std::string r = new_tmp("fr");
            pa_all.push_back(cat("float ", r, " = ", call, ";"));
            finish_post_call();
            return {pa_all, cat("vfloat((double)", r, ")")};
        }
        if (structs.count(ret)) {
            std::string rt = new_tmp("cs");
            pa_all.push_back(cat(ffi_c(ret), " ", rt, " = ", call, ";"));
            finish_post_call();
            return {pa_all, c_struct_to_val(ret, rt)};
        }
        if (!x.ret || *x.ret == "none") {
            pa_all.push_back(cat(call, ";"));
            finish_post_call();
            return {pa_all, "vnil()"};
        }
        throw EmitError(cat("unsupported FFI return type `", ret, "` in extern fn ", name));
    }
    if (sym.count(name)) return ex_vcall(name, args);
    if (cap && cap->second.count(name)) {
        V a = args_vec(args);
        return call_result(a.pre, a.val,
                           cat("vcall_any((*vclosure_env_at(", cap->first, ", ", cap_slots[name], ")), ", a.val, ")"));
    }
    throw EmitError("unresolved call " + name);
}

}  // namespace

std::string emit_program(const std::vector<StmtP> &prog, const std::string &base_, int opt,
                         std::vector<ir::Proof> *proofs, const std::string &target,
                         bool trace_lines, bool debug_lines, const std::string &root_file,
                         const std::map<const Stmt *, std::string> *stmt_files,
                         const std::map<std::string, std::string> *fn_files) {
    Emitter em;
    em.base = base_;
    em.opt = opt;
    em.proofs = proofs;
    em.target_triple = target;
    em.trace_lines = trace_lines;
    em.debug_lines = debug_lines;
    em.root_src_file = root_file;
    em.cur_src_file = root_file;
    em.stmt_files = stmt_files;
    em.fn_files = fn_files;
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
