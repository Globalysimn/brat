// Intraprocedural ownership, move, and borrow checker.
// Every function keeps the reference's name and its order of checks, because
// the two are compared diagnostic for diagnostic on the whole corpus: the
// order of `warns` and `errs` is part of the output.
#include "borrow.h"

#include "capability.h"
#include "plugin.h"
#include "caps.h"
#include "session.h"
#include "parfor.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {


const std::set<std::string> BUILTINS = {
    "print", "log.info", "log.warn", "log.error",
    "trace.span",
    "join_all",
    "len", "__at", "__stag_is", "__shas", "Ok", "Err", "Some", "unwrap", "unwrap_or",
    "is_ok", "is_err", "is_some", "is_none", "is_vec",
    "is_str", "is_map", "is_bool", "is_int", "is_float", "tagof",
    "str", "read_file", "write_file", "append_file", "read_bytes", "write_bytes", "proc_run",
    "utf8_bytes", "utf8_str", "json_parse", "json_stringify", "fs_exists", "fs_rename", "fs_remove", "assert",
    "mono_ns", "sha256_bytes", "random_bytes", "https_request_raw",
    "sql_open", "sql_close", "sql_exec", "sql_query",
    "args", "args_bytes", "exit", "eprint", "raise", "time_ms", "sleep_ms", "env_get", "env_get_bytes",
    "ptr_null", "ptr_is_null", "ptr_offset", "ptr_read_i64", "ptr_write_i64",
    "ptr_read_f64", "ptr_write_f64", "ptr_read_f32", "ptr_write_f32",
    "ptr_read_struct", "ptr_write_struct",
    "ptr_read_ptr", "ptr_read_str",
    "ptr_write_ptr", "ptr_of_str", "ptr_write_str", "errno", "strerror",
    "$arena_enter", "$arena_leave", "own",
    "slice_of", "slice_of_ptr", "slice_of_struct", "slice_struct_of",
    "slice_len", "slice_get", "slice_set", "str_from_ptr",
    "lib_open", "lib_sym", "lib_close", "call",
    "re_match", "re_find", "re_captures", "re_replace",
    "tcp_listen", "tcp_connect", "tcp_accept",
    "tcp_send", "tcp_recv", "tcp_close",
    "sbuf", "sbuf_add", "sbuf_len", "sbuf_str",
    "chan",
};

std::set<std::string> builtin_heads() {
    std::set<std::string> out;
    for (const std::string &b : BUILTINS) {
        size_t dot = b.find('.');
        if (dot != std::string::npos) out.insert(b.substr(0, dot));
    }
    return out;
}

const std::set<std::string> BUILTIN_HEADS = builtin_heads();
const std::set<std::string> MUTATORS = {"push", "pop", "sort", "sort_by", "ins", "del", "set"};
const std::set<std::string> COPY_ANNS = {"i8", "u8", "i16", "u16", "i32", "u32",
                                         "i64", "u64", "usize", "isize", "f32", "f64", "bool"};
const std::set<std::string> TYPE_WORDS = {"i8", "u8", "i16", "u16", "i32", "u32",
                                          "i64", "u64", "usize", "isize", "f32", "f64", "bool", "str"};

// A handle that is taken away by a consuming builtin: closing a socket twice is
// a use of a moved value, not a runtime guess.
const std::map<std::string, std::string> CONSUMES = {{"tcp_close", "closed"}};


std::string root_of(const std::string &name) {
    size_t dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

bool starts_with(const std::string &s, const std::string &p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string &s, const std::string &p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool has(const std::set<std::string> &set, const std::optional<std::string> &v) {
    return v && set.count(*v) > 0;
}


// One binding as the pass sees it. `src`/`srcs` name the storage a `let y = x`
// aliases, `dl` is the line that should have said `mut`.
struct Decl {
    bool mut = false;
    bool ref = false;
    std::optional<std::string> src;
    std::vector<std::string> srcs;
    bool moved = false;
    int moved_line = 0;
    std::string moved_verb = "moved";
    std::set<std::string> moved_paths;
    bool konst = false;
    bool alias = false;
    bool copy = false;
    bool pinned = false;
    std::optional<std::string> ann;
    int dl = 0;
    bool param = false;
    std::optional<Mode> mode;
    bool owned = false;
    bool borrowed_ptr = false;
    bool spawn_frozen = false;
    std::optional<std::pair<std::string, int>> frozen_by;
};

// Insertion-ordered, like the dict it replaces: a leaked-pointer report or a
// frozen-borrow note is emitted in declaration order, and that order shows.
// Entries are shared pointers because the reference distinguishes two kinds of
// copy: `copy_decl` clones every entry, `dict(decl)` shares them, and a walk
// over a shared table sees the moves its caller made.
struct Decls {
    std::vector<std::pair<std::string, std::shared_ptr<Decl>>> v;

    Decl *find(const std::string &n) {
        for (auto &kv : v) {
            if (kv.first == n) return kv.second.get();
        }
        return nullptr;
    }
    const Decl *find(const std::string &n) const {
        for (const auto &kv : v) {
            if (kv.first == n) return kv.second.get();
        }
        return nullptr;
    }
    bool has(const std::string &n) const { return find(n) != nullptr; }
    Decl &get(const std::string &n) {
        for (auto &kv : v) {
            if (kv.first == n) return *kv.second;
        }
        v.emplace_back(n, std::make_shared<Decl>());
        return *v.back().second;
    }
    void set(const std::string &n, Decl d) {
        auto sp = std::make_shared<Decl>(std::move(d));
        for (auto &kv : v) {
            if (kv.first == n) {
                kv.second = std::move(sp);
                return;
            }
        }
        v.emplace_back(n, std::move(sp));
    }
    void erase(const std::string &n) {
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i].first == n) {
                v.erase(v.begin() + static_cast<long>(i));
                return;
            }
        }
    }
};

struct Borrow {
    std::string name;
    bool mut = false;
    int line = 0;
    int born = 0;                      // statement index the borrow was taken in
};

struct Borrows {
    std::vector<std::pair<std::string, std::vector<Borrow>>> v;

    std::vector<Borrow> *find(const std::string &n) {
        for (auto &kv : v) {
            if (kv.first == n) return &kv.second;
        }
        return nullptr;
    }
    const std::vector<Borrow> *peek(const std::string &n) const {
        for (const auto &kv : v) {
            if (kv.first == n) return &kv.second;
        }
        return nullptr;
    }
    std::vector<Borrow> &get(const std::string &n) {
        for (auto &kv : v) {
            if (kv.first == n) return kv.second;
        }
        v.emplace_back(n, std::vector<Borrow>{});
        return v.back().second;
    }
    bool has(const std::string &n) const { return peek(n) != nullptr; }
    void erase(const std::string &n) {
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i].first == n) {
                v.erase(v.begin() + static_cast<long>(i));
                return;
            }
        }
    }
};


struct Errs {
    std::vector<BorrowDiag> v;
    int line = 0;
    int idx = 0;                       // index of the statement being walked
    int in_spawn = 0;
    // Roots of the containers a `for` is walking right now, innermost last:
    // `(root, line_of_the_for)`. Keyed on `(root, for_line)`.
    std::vector<std::pair<std::string, int>> iters;
    // Moves executed anywhere inside an active `try` body: an exception after a
    // move reaches `catch` with that binding already consumed.
    std::vector<Decls> try_sinks;
    // Names from outside the current function/closure frame (not owned by its
    // `return`), and active `defer` expressions `(expr, line)` in registration order.
    std::set<std::string> fn_floor;
    std::vector<std::pair<const Expr *, int>> active_defers;

    void note(const std::string &msg) { v.push_back({line, msg, std::nullopt}); }
    void note(const std::string &msg, std::optional<BorrowNote> rel) {
        v.push_back({line, msg, std::move(rel)});
    }
};

std::optional<BorrowNote> rel_at(int line, const std::string &msg) {
    return BorrowNote{line, msg};
}

using Body = std::vector<const Stmt *>;
Body wrap(const std::vector<StmtP> &v);
void used_in_stmt(const Stmt &s, std::vector<std::string> &acc);
void cut_stmt(const Stmt &s, std::vector<BorrowDiag> &warns);
std::map<std::string, int> walk_block(const Body &body, Decls &decl, Borrows &borrows, Errs &errs,
                                      FnTable &funcs);
const char *advice(const std::string &verb);

Decl bare_decl() { return Decl{}; }

Decl param_decl(const Param &p) {
    Decl d;
    d.mut = p.mode == Mode::Mut;
    d.copy = has(COPY_ANNS, p.ann);
    d.ann = p.ann;
    d.param = true;
    d.mode = p.mode;
    return d;
}

std::vector<ParamMode> fn_modes(const std::vector<Param> &params) {
    std::vector<ParamMode> out;
    out.reserve(params.size());
    for (const Param &p : params) out.push_back({p.name, p.mode});
    return out;
}

// Every entry cloned: the reference's `copy_decl`, used where a branch must
// not see the other branch's moves.
Decls copy_decl(const Decls &d) {
    Decls out;
    out.v.reserve(d.v.size());
    for (const auto &kv : d.v) out.v.emplace_back(kv.first, std::make_shared<Decl>(*kv.second));
    return out;
}
Borrows copy_borrows(const Borrows &b) { return b; }


const Expr *skip_quest(const Expr *e) {
    while (e && e->k == EK::Quest && !e->kids.empty()) e = e->kids[0].get();
    return e;
}

// Path naming the storage `e` denotes, or nothing when `e` computes a value.
std::optional<std::string> place_of(const Expr *e) {
    e = skip_quest(e);
    if (!e) return std::nullopt;
    switch (e->k) {
    case EK::Var: return e->a;
    case EK::Move: return place_of(e->kids[0].get());
    case EK::Field: {
        auto b = place_of(e->kids[0].get());
        if (!b) return std::nullopt;
        return *b + "." + e->b;
    }
    case EK::Idx: {
        auto b = place_of(e->kids[0].get());
        if (!b) return std::nullopt;
        return *b + ".*";
    }
    default: return std::nullopt;
    }
}

std::optional<std::string> path_of(const Expr *e) {
    if (!e) return std::nullopt;
    if (e->k == EK::Var) return e->a;
    if (e->k == EK::Field) {
        auto b = path_of(e->kids[0].get());
        if (!b) return std::nullopt;
        return *b + "." + e->b;
    }
    if (e->k == EK::Idx) {
        auto b = path_of(e->kids[0].get());
        if (!b) return std::nullopt;
        return *b + ".*";
    }
    return std::nullopt;
}

// Dotted path of a pure name chain. A bare uppercase name arrives from the
// parser as a zero-arg call, and is the same chain; `f().b` is not.
std::optional<std::string> write_path(const Expr *e) {
    if (!e) return std::nullopt;
    if (e->k == EK::Var) return e->a;
    if (e->k == EK::Field) {
        auto b = write_path(e->kids[0].get());
        if (!b) return std::nullopt;
        return *b + "." + e->b;
    }
    if (e->k == EK::Call && e->kids.empty() && e->a.find('.') == std::string::npos) {
        if (!e->a.empty() && e->a[0] >= 'A' && e->a[0] <= 'Z') return e->a;
    }
    return std::nullopt;
}

// The parser reads any bare uppercase identifier as a zero-arg call, so a
// local named `C` arrives as a call. Naming a local in scope with no arguments
// is a read of that local, and the ownership rules have to see the place.
std::optional<std::string> deref_var(const Expr *e, const Decls &decl, FnTable &funcs) {
    if (e && e->k == EK::Call && e->kids.empty() && e->a.find('.') == std::string::npos &&
        decl.has(e->a) && funcs.sigs.count(e->a) == 0) {
        return e->a;
    }
    return std::nullopt;
}

bool paths_conflict(const std::string &p, const std::string &q) {
    if (p == q) return true;
    if (starts_with(p, q + ".") || starts_with(q, p + ".")) return true;
    if (q == p + ".*" || p == q + ".*") return true;
    if (ends_with(q, ".*") && starts_with(p, q.substr(0, q.size() - 1))) return true;
    if (ends_with(p, ".*") && starts_with(q, p.substr(0, p.size() - 1))) return true;
    return false;
}


std::string trim_ann(const std::string &raw) {
    size_t b = raw.find_first_not_of(" \t\r\n");
    size_t z = raw.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? std::string() : raw.substr(b, z - b + 1);
}

std::string unwrap_adt_ann(const std::string &raw) {
    std::string a = trim_ann(raw);
    if (starts_with(a, "Option[") && ends_with(a, "]"))
        return trim_ann(a.substr(7, a.size() - 8));
    if (starts_with(a, "Result[") && ends_with(a, "]")) {
        std::string inner = a.substr(7, a.size() - 8);
        int depth = 0;
        for (size_t i = 0; i < inner.size(); i++) {
            if (inner[i] == '[' || inner[i] == '(') depth++;
            else if (inner[i] == ']' || inner[i] == ')') depth--;
            else if (inner[i] == ',' && depth == 0)
                return trim_ann(inner.substr(0, i));
        }
        return trim_ann(inner);
    }
    return a;
}

std::optional<std::string> infer_expr_ann(const Expr *e, const Decls &decl,
                                          const FnTable *funcs = nullptr) {
    if (!e) return std::nullopt;
    switch (e->k) {
    case EK::Quest: {
        auto sub = infer_expr_ann(e->kids.empty() ? nullptr : e->kids[0].get(), decl, funcs);
        if (!sub) return std::nullopt;
        std::string u = unwrap_adt_ann(*sub);
        return u.empty() ? std::nullopt : std::optional<std::string>(std::move(u));
    }
    case EK::Move:
        return infer_expr_ann(e->kids.empty() ? nullptr : e->kids[0].get(), decl, funcs);
    case EK::Var: {
        const Decl *d = decl.find(e->a);
        if (!d || !d->ann) return std::nullopt;
        return trim_ann(*d->ann);
    }
    case EK::StructLit:
        if (funcs && funcs->struct_fields.count(e->a)) return e->a;
        return std::nullopt;
    case EK::Call: {
        if (e->a == "own" && e->kids.size() == 1)
            return infer_expr_ann(e->kids[0].get(), decl, funcs);
        if (e->a == "is_ok" || e->a == "is_err" || e->a == "is_some" || e->a == "is_none")
            return std::string("bool");
        if (e->a == "Some" && e->kids.size() == 1) {
            auto in = infer_expr_ann(e->kids[0].get(), decl, funcs);
            if (in && !in->empty()) return "Option[" + *in + "]";
            return std::nullopt;
        }
        if (e->a == "Ok" && e->kids.size() == 1) {
            auto in = infer_expr_ann(e->kids[0].get(), decl, funcs);
            if (in && !in->empty()) return "Result[" + *in + ", dyn]";
            return std::nullopt;
        }
        if (e->a == "unwrap" && e->kids.size() == 1) {
            auto in = infer_expr_ann(e->kids[0].get(), decl, funcs);
            if (!in) return std::nullopt;
            std::string u = unwrap_adt_ann(*in);
            return (u.empty() || u == *in) ? std::nullopt : std::optional<std::string>(std::move(u));
        }
        if (e->a == "unwrap_or" && e->kids.size() == 2) {
            auto in = infer_expr_ann(e->kids[0].get(), decl, funcs);
            if (in) {
                std::string u = unwrap_adt_ann(*in);
                if (!u.empty() && u != *in && u != "dyn") return u;
            }
            return infer_expr_ann(e->kids[1].get(), decl, funcs);
        }
        if (e->a == "fs_exists") return std::string("bool");
        if (e->a == "read_file") return std::string("Result[str, str]");
        if (e->a == "append_file" || e->a == "write_file") return std::string("Result[i64, str]");
        if (e->a == "re_find") return std::string("Option[str]");
        if (!funcs) return std::nullopt;
        auto rit = funcs->fn_rets.find(e->a);
        return rit == funcs->fn_rets.end() ? std::nullopt : std::optional<std::string>(trim_ann(rit->second));
    }
    case EK::MCall: {
        if (e->kids.empty()) return std::nullopt;
        if (e->b == "is_ok" || e->b == "is_err" || e->b == "is_some" || e->b == "is_none")
            return std::string("bool");
        if (e->b == "parse_int") return std::string("Result[i64, str]");
        if (e->b == "parse_float") return std::string("Result[f64, str]");
        if (funcs && ((e->kids[0]->k == EK::Call && e->kids[0]->kids.empty()) ||
                      (e->kids[0]->k == EK::Var && !decl.has(e->kids[0]->a)))) {
            auto sit = funcs->fn_rets.find(e->kids[0]->a + "." + e->b);
            if (sit != funcs->fn_rets.end()) return trim_ann(sit->second);
        }
        auto recv_ann = infer_expr_ann(e->kids[0].get(), decl, funcs);
        if (recv_ann) {
            if (e->b == "copy") return *recv_ann;
            if (e->b == "unwrap") {
                std::string u = unwrap_adt_ann(*recv_ann);
                if (!u.empty() && u != *recv_ann) return u;
            }
            if (e->b == "unwrap_or") {
                std::string u = unwrap_adt_ann(*recv_ann);
                if (!u.empty() && u != *recv_ann && u != "dyn") return u;
                if (e->kids.size() >= 2) return infer_expr_ann(e->kids[1].get(), decl, funcs);
            }
            if (funcs) {
                auto mit = funcs->fn_rets.find(*recv_ann + "." + e->b);
                if (mit != funcs->fn_rets.end()) return trim_ann(mit->second);
            }
        }
        return std::nullopt;
    }
    case EK::Idx: {
        auto ba = infer_expr_ann(e->kids.empty() ? nullptr : e->kids[0].get(), decl, funcs);
        if (!ba) return std::nullopt;
        const std::string &a = *ba;
        if (starts_with(a, "vec[") && ends_with(a, "]"))
            return trim_ann(a.substr(4, a.size() - 5));
        if (starts_with(a, "map[") && ends_with(a, "]")) {
            std::string inner = a.substr(4, a.size() - 5);
            int depth = 0;
            size_t split = std::string::npos;
            for (size_t i = 0; i < inner.size(); i++) {
                if (inner[i] == '[') depth++;
                else if (inner[i] == ']') depth--;
                else if (inner[i] == ',' && depth == 0) split = i;
            }
            return trim_ann(split == std::string::npos ? inner : inner.substr(split + 1));
        }
        return std::nullopt;
    }
    case EK::Field: {
        if (!funcs) return std::nullopt;
        auto ba = infer_expr_ann(e->kids.empty() ? nullptr : e->kids[0].get(), decl, funcs);
        if (!ba) return std::nullopt;
        auto sit = funcs->struct_fields.find(*ba);
        if (sit == funcs->struct_fields.end()) return std::nullopt;
        auto fit = sit->second.find(e->b);
        if (fit == sit->second.end()) return std::nullopt;
        return trim_ann(fit->second);
    }
    default:
        return std::nullopt;
    }
}

bool expr_is_copy_slot(const Expr *e, const Decls &decl, const FnTable *funcs = nullptr) {
    auto a = infer_expr_ann(e, decl, funcs);
    return a && COPY_ANNS.count(*a) > 0;
}

bool elem_copy(const Decls &decl, const Expr *base);

bool is_copy_expr(const Expr *e, const Decls &decl, const FnTable *funcs = nullptr) {
    if (!e) return false;
    switch (e->k) {
    case EK::Num:
    case EK::Bool: return true;
    case EK::Var: {
        const Decl *d = decl.find(e->a);
        return d && d->copy;
    }
    case EK::Call:
        return (funcs != nullptr && funcs->scalar_ret.count(e->a) > 0) ||
               expr_is_copy_slot(e, decl, funcs);
    case EK::Quest:
    case EK::MCall:
        return expr_is_copy_slot(e, decl, funcs);
    case EK::Idx:
    case EK::Field:
        return expr_is_copy_slot(e, decl, funcs) ||
               (e->k == EK::Idx && elem_copy(decl, e->kids.empty() ? nullptr : e->kids[0].get()));
    case EK::Un: return is_copy_expr(e->kids[0].get(), decl, funcs);
    case EK::Move: return is_copy_expr(e->kids[0].get(), decl, funcs);
    case EK::Bin:
        return is_copy_expr(e->kids[0].get(), decl, funcs) &&
               is_copy_expr(e->kids[1].get(), decl, funcs);
    default: return false;
    }
}

// The same question, asked with the function table in hand: a call to an extern
// whose *declared* result is a scalar slot produces a fresh value.
static bool is_copy_expr(const Expr *e, const Decls &decl, const FnTable &funcs) {
    return is_copy_expr(e, decl, &funcs);
}

// Only an annotation settles whether an element is Copy: `vec[i64]` reads an
// element as a value, an unannotated binding is dynamically typed and `v[0]`
// hands back a borrow of the slot.
bool elem_copy(const Decls &decl, const Expr *base) {
    base = skip_quest(base);
    if (!base) return false;
    switch (base->k) {
    case EK::Move:
    case EK::Field:
    case EK::Idx: return elem_copy(decl, base->kids[0].get());
    case EK::Var: break;
    default: return false;
    }
    const Decl *d = decl.find(base->a);
    if (!d || !d->ann) return false;
    std::string a = *d->ann;
    // strip surrounding spaces, as `ann.strip()` does
    size_t b = a.find_first_not_of(" \t\r\n");
    size_t z = a.find_last_not_of(" \t\r\n");
    a = (b == std::string::npos) ? std::string() : a.substr(b, z - b + 1);
    if (starts_with(a, "vec[") && ends_with(a, "]")) {
        a = a.substr(4, a.size() - 5);
        size_t bb = a.find_first_not_of(" \t\r\n");
        size_t zz = a.find_last_not_of(" \t\r\n");
        a = (bb == std::string::npos) ? std::string() : a.substr(bb, zz - bb + 1);
    } else if (starts_with(a, "map[") && ends_with(a, "]")) {
        std::string inner = a.substr(4, a.size() - 5);
        size_t comma = std::string::npos;
        int depth = 0;
        for (size_t i = 0; i < inner.size(); i++) {
            char c = inner[i];
            if (c == '[' || c == '(') depth++;
            else if ((c == ']' || c == ')') && depth > 0) depth--;
            else if (c == ',' && depth == 0) { comma = i; break; }
        }
        a = (comma == std::string::npos) ? inner : inner.substr(comma + 1);
        size_t bb = a.find_first_not_of(" \t\r\n");
        size_t zz = a.find_last_not_of(" \t\r\n");
        a = (bb == std::string::npos) ? std::string() : a.substr(bb, zz - bb + 1);
    } else {
        return false;
    }
    return COPY_ANNS.count(a) > 0;
}

// Locals whose storage `e` aliases, so `let y = e` borrows them. Only container
// wrappers propagate; anything that computes a fresh value yields nothing.
void add_unique(std::vector<std::string> &out, const std::vector<std::string> &more) {
    for (const std::string &x : more) {
        if (std::find(out.begin(), out.end(), x) == out.end()) out.push_back(x);
    }
}

bool is_borrowed_ptr_expr(const Expr *e, const Decls &decl, FnTable &funcs,
                          std::set<std::string> &seen) {
    while (e && (e->k == EK::Quest || e->k == EK::Move) && !e->kids.empty())
        e = e->kids[0].get();
    if (!e) return false;
    if (auto v = deref_var(e, decl, funcs)) {
        if (!seen.insert(*v).second) return false;
        const Decl *d = decl.find(*v);
        return d && d->borrowed_ptr;
    }
    if (e->k == EK::Var) {
        if (!seen.insert(e->a).second) return false;
        const Decl *d = decl.find(e->a);
        return d && d->borrowed_ptr;
    }
    if (e->k == EK::Call) {
        if (funcs.borrows_ret.count(e->a) > 0) return true;
        if (e->a == "ptr_of_str" && funcs.sigs.count(e->a) == 0 && !e->kids.empty())
            return true;
        if (e->a == "ptr_offset" && !e->kids.empty())
            return is_borrowed_ptr_expr(e->kids[0].get(), decl, funcs, seen);
    }
    return false;
}

bool is_borrowed_ptr_expr(const Expr *e, const Decls &decl, FnTable &funcs) {
    std::set<std::string> seen;
    return is_borrowed_ptr_expr(e, decl, funcs, seen);
}

std::vector<std::string> borrow_srcs(const Expr *e, const Decls &decl, FnTable &funcs);
std::vector<std::string> borrow_srcs_at(const std::string &name, const Decls &decl,
                                        FnTable &funcs);

std::vector<std::string> borrow_srcs_at(const std::string &name, const Decls &decl,
                                        FnTable &funcs) {
    std::vector<std::string> out;
    const Decl *d = decl.find(name);
    if (!d || d->moved || d->copy) return out;
    if (!d->srcs.empty()) {
        for (const std::string &s : d->srcs) {
            if (s != name) add_unique(out, borrow_srcs_at(s, decl, funcs));
        }
        return out;
    }
    if (d->src && *d->src != name) return borrow_srcs_at(*d->src, decl, funcs);
    out.push_back(name);
    return out;
}

std::vector<std::string> borrow_srcs(const Expr *e, const Decls &decl, FnTable &funcs) {
    std::vector<std::string> out;
    if (expr_is_copy_slot(e, decl, &funcs)) return out;
    e = skip_quest(e);
    if (!e) return out;
    if (auto v = deref_var(e, decl, funcs)) return borrow_srcs_at(*v, decl, funcs);
    switch (e->k) {
    case EK::Var: return borrow_srcs_at(e->a, decl, funcs);
    case EK::Move: {
        if (auto p = path_of(e->kids[0].get())) {
            std::string r = root_of(*p);
            const Decl *d = decl.find(r);
            if (!d || d->copy) return out;
            if (!d->srcs.empty()) {
                for (const std::string &s : d->srcs) {
                    if (s != r) add_unique(out, borrow_srcs_at(s, decl, funcs));
                }
                return out;
            }
            if (d->src && *d->src != r) return borrow_srcs_at(*d->src, decl, funcs);
            return out;
        }
        return borrow_srcs(e->kids[0].get(), decl, funcs);
    }
    case EK::Idx:
        if (elem_copy(decl, e->kids[0].get()) || expr_is_copy_slot(e, decl, &funcs)) return out;
        return borrow_srcs(e->kids[0].get(), decl, funcs);
    case EK::Field:
        if (elem_copy(decl, e->kids[0].get()) || expr_is_copy_slot(e, decl, &funcs)) return out;
        return borrow_srcs(e->kids[0].get(), decl, funcs);
    case EK::VecLit:
        for (const auto &x : e->kids) add_unique(out, borrow_srcs(x.get(), decl, funcs));
        return out;
    case EK::MapLit:
        for (const auto &x : e->kids) add_unique(out, borrow_srcs(x.get(), decl, funcs));
        return out;
    case EK::StructLit:
        for (const auto &x : e->kids) add_unique(out, borrow_srcs(x.get(), decl, funcs));
        return out;
    case EK::Or:
        add_unique(out, borrow_srcs(e->kids[0].get(), decl, funcs));
        add_unique(out, borrow_srcs(e->kids[1].get(), decl, funcs));
        return out;
    case EK::Call: {
        if ((e->a == "ref" || e->a == "borrow" || e->a == "ptr_of_str") &&
            funcs.sigs.count(e->a) == 0 && !e->kids.empty()) {
            return borrow_srcs(e->kids[0].get(), decl, funcs);
        }
        auto bit = funcs.borrows_ret.find(e->a);
        if (bit != funcs.borrows_ret.end()) {
            for (size_t ix : bit->second)
                if (ix < e->kids.size())
                    add_unique(out, borrow_srcs(e->kids[ix].get(), decl, funcs));
            return out;
        }
        if (e->a == "ptr_offset" && !e->kids.empty() &&
            is_borrowed_ptr_expr(e->kids[0].get(), decl, funcs)) {
            return borrow_srcs(e->kids[0].get(), decl, funcs);
        }
        return out;
    }
    default: return out;
    }
}


Body wrap(const std::vector<StmtP> &v) {
    Body out;
    out.reserve(v.size());
    for (const auto &s : v) out.push_back(s.get());
    return out;
}

// Walk branch body statements.
Body else_body(const Stmt &s) {
    if (s.els->k == SK::Block) return wrap(s.els->body);
    return Body{s.els.get()};
}

void vars_in(const Expr *e, std::vector<std::string> &acc);

// `{name}` placeholders left in a `str`: the parser lowers interpolation, so
// this only sees text the lowering left alone.
void interp_names(const std::string &text, std::vector<std::string> &acc) {
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '{') continue;
        size_t j = i + 1;
        if (j >= text.size()) continue;
        char c = text[j];
        bool start_ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (!start_ok) continue;
        while (j < text.size() && ((text[j] >= 'A' && text[j] <= 'Z') ||
                                   (text[j] >= 'a' && text[j] <= 'z') ||
                                   (text[j] >= '0' && text[j] <= '9') || text[j] == '_')) {
            j++;
        }
        while (j < text.size() && text[j] == '.') {
            size_t k = j + 1;
            char d = k < text.size() ? text[k] : '\0';
            bool ok = (d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || d == '_';
            if (!ok) break;
            size_t m = k;
            while (m < text.size() && ((text[m] >= 'A' && text[m] <= 'Z') ||
                                       (text[m] >= 'a' && text[m] <= 'z') ||
                                       (text[m] >= '0' && text[m] <= '9') || text[m] == '_')) {
                m++;
            }
            j = m;
        }
        if (j < text.size() && text[j] == '}') acc.push_back(root_of(text.substr(i + 1, j - i - 1)));
    }
}

// The reference returns `acc` for chaining; returning a copy of the whole
// accumulated list at every level is the one thing that must not be translated.
void vars_in(const Expr *e, std::vector<std::string> &acc) {
    if (!e) return;
    switch (e->k) {
    case EK::Var: acc.push_back(e->a); break;
    case EK::Num:
    case EK::Bool:
    case EK::None: break;
    case EK::Str: interp_names(e->a, acc); break;
    case EK::RStr: break;
    case EK::Un:
    case EK::Move:
    case EK::Quest: vars_in(e->kids[0].get(), acc); break;
    case EK::Closure: {
        // A closure body reads the enclosing scope: a captured name is a use of
        // the binding it names. Its own parameters are bound by the body, so a
        // parameter of the same spelling is not a use of the outer binding.
        std::vector<std::string> inner;
        for (const auto &st : e->fn->body) used_in_stmt(*st, inner);
        std::set<std::string> params;
        for (const auto &p : e->fn->params) params.insert(p.name);
        for (const auto &n : inner)
            if (!params.count(n)) acc.push_back(n);
        break;
    }
    case EK::ParFor: {
        // The iterator, the unit and the operation are uses of the outer scope;
        // the loop variable is bound by the body, so its uses are not.
        vars_in(e->kids[0].get(), acc);
        vars_in(e->kids[1].get(), acc);
        acc.push_back(e->b);
        std::vector<std::string> inner;
        for (const auto &st : e->fn->body) used_in_stmt(*st, inner);
        for (const auto &n : inner)
            if (n != e->a) acc.push_back(n);
        break;
    }
    case EK::Bin:
    case EK::Or:
    case EK::Idx:
        vars_in(e->kids[0].get(), acc);
        vars_in(e->kids[1].get(), acc);
        break;
    case EK::MCall:
        vars_in(e->kids[0].get(), acc);
        for (size_t i = 1; i < e->kids.size(); i++) vars_in(e->kids[i].get(), acc);
        break;
    case EK::VecLit:
    case EK::MapLit:
    case EK::StructLit:
        for (const auto &x : e->kids) vars_in(x.get(), acc);
        break;
    case EK::Field: vars_in(e->kids[0].get(), acc); break;
    case EK::Call:
        acc.push_back(root_of(e->a));
        for (const auto &x : e->kids) vars_in(x.get(), acc);
        break;
    }
}

void used_in_stmt(const Stmt &s, std::vector<std::string> &acc);

void used_in_block(const Body &body, std::vector<std::string> &acc) {
    for (const Stmt *s : body) used_in_stmt(*s, acc);
}

void used_in_stmt(const Stmt &s, std::vector<std::string> &acc) {
    switch (s.k) {
    case SK::Let:
    case SK::LetTmp: vars_in(s.e1.get(), acc); break;
    case SK::Set:
        acc.push_back(root_of(s.a));
        vars_in(s.e1.get(), acc);
        break;
    case SK::SetIdx:
        vars_in(s.e1.get(), acc);
        vars_in(s.e2.get(), acc);
        vars_in(s.e3.get(), acc);
        break;
    case SK::SetField:
        vars_in(s.e1.get(), acc);
        vars_in(s.e2.get(), acc);
        break;
    case SK::Return:
    case SK::Expr:
    case SK::Defer: vars_in(s.e1.get(), acc); break;
    case SK::If:
        vars_in(s.e1.get(), acc);
        used_in_block(wrap(s.body), acc);
        if (s.els) used_in_block(else_body(s), acc);
        break;
    case SK::While:
        vars_in(s.e1.get(), acc);
        used_in_block(wrap(s.body), acc);
        break;
    case SK::ForIter:
        vars_in(s.e1.get(), acc);
        used_in_block(wrap(s.body), acc);
        break;
    case SK::ForRange:
        vars_in(s.e1.get(), acc);
        vars_in(s.e2.get(), acc);
        used_in_block(wrap(s.body), acc);
        break;
    case SK::Try:
        used_in_block(wrap(s.body), acc);
        used_in_block(wrap(s.body2), acc);
        break;
    case SK::Spawn:
    case SK::Scope:
    case SK::Unsafe: used_in_block(wrap(s.body), acc); break;
    case SK::Impl:
        for (const FnDecl &f : s.fns) used_in_block(wrap(f.body), acc);
        break;
    case SK::Fn:
    case SK::ExportFn: used_in_block(wrap(s.fns[0].body), acc); break;
    case SK::Pub: used_in_stmt(*s.body[0], acc); break;
    case SK::Block: used_in_block(wrap(s.body), acc); break;
    case SK::Enum:
    case SK::Struct:
    case SK::Trait:
    case SK::Import:
    case SK::Extern:
    case SK::ExternStruct:
    case SK::PubUse:
    case SK::Break:
    case SK::Continue: break;
    }
}


bool callee_known(const std::string &name, const Decls &decl, FnTable &funcs) {
    if (funcs.sigs.count(name) || BUILTINS.count(name)) return true;
    if (name.find('.') == std::string::npos) return decl.has(name);
    std::string head = root_of(name);
    if (decl.has(head) || funcs.sigs.count(head) || BUILTIN_HEADS.count(head)) return true;
    return funcs.heads.count(head) > 0;
}

std::set<const Expr *> CUT_PARTS;

int edit_distance(const std::string &a, const std::string &b) {
    std::vector<std::vector<int>> rows(a.size() + 1,
                                       std::vector<int>(b.size() + 1));
    for (size_t i = 0; i <= a.size(); i++) rows[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= b.size(); j++) rows[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); i++) {
        for (size_t j = 1; j <= b.size(); j++) {
            rows[i][j] = std::min({rows[i - 1][j] + 1, rows[i][j - 1] + 1,
                                   rows[i - 1][j - 1] + (a[i - 1] != b[j - 1])});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] &&
                a[i - 2] == b[j - 1]) {
                rows[i][j] = std::min(rows[i][j], rows[i - 2][j - 2] + 1);
            }
        }
    }
    return rows.back().back();
}

std::optional<std::string> name_suggestion(const std::string &name,
                                           const Decls *decl,
                                           const FnTable *funcs) {
    if (name.size() < 3 || name.size() > 128) return std::nullopt;
    int limit = name.size() <= 4 ? 1 : 2;
    std::set<std::string> candidates = BUILTINS;
    if (decl) {
        for (const auto &[candidate, _] : decl->v) candidates.insert(candidate);
    }
    if (funcs) {
        for (const auto &[candidate, _] : funcs->sigs) candidates.insert(candidate);
    }
    std::vector<std::pair<int, std::string>> ranked;
    for (const std::string &candidate : candidates) {
        if (candidate == name) continue;
        const size_t diff = name.size() > candidate.size()
                                ? (name.size() - candidate.size())
                                : (candidate.size() - name.size());
        if (diff > static_cast<size_t>(limit)) continue;
        ranked.emplace_back(edit_distance(name, candidate), candidate);
    }
    if (ranked.empty()) return std::nullopt;
    std::sort(ranked.begin(), ranked.end());
    if (ranked[0].first > limit ||
        (ranked.size() > 1 && ranked[1].first == ranked[0].first)) {
        return std::nullopt;
    }
    return ranked[0].second;
}

std::optional<BorrowNote> undeclared_rel(const std::string &name,
                                         const Expr *node = nullptr,
                                         const Decls *decl = nullptr,
                                         const FnTable *funcs = nullptr) {
    if (node && CUT_PARTS.count(node)) return std::nullopt;
    if (TYPE_WORDS.count(name)) {
        return BorrowNote{0, "help: `" + name + "` is a type name for annotations (`let n: " +
                                 name + " = ...`), not a value"};
    }
    if (name == "enum" || name == "trait" || name == "impl" || name == "struct") {
        return BorrowNote{0, "help: `" + name + "` starts a declaration, so it cannot be used "
                             "inside an expression"};
    }
    if (auto suggestion = name_suggestion(name, decl, funcs)) {
        return BorrowNote{0, "help: did you mean `" + *suggestion + "`?"};
    }
    return BorrowNote{0,
                      "help: in scope here are `let` bindings, fn parameters, `for` bindings and "
                      "`match` arm bindings; functions come from `fn`/`impl`; a name from another "
                      "file needs `import that.file`"};
}


// A fn name is a callback, not a value: it is spelled bare only in these argument
// slots, which both backends turn into a call. Method -> argument index.
const std::unordered_map<std::string, size_t> HOF_CB_ARG = {
    {"map", 0}, {"filter", 0}, {"any", 0}, {"all", 0}, {"find", 0},
    {"sort_by", 0}, {"fold", 1},
};

void check_expr(const Expr *e, Decls &decl, Errs &errs, FnTable &funcs, bool inpath = false,
                bool callable_ok = false);
void walk_closure_arg(const Expr &a, Decls &decl, Errs &errs, FnTable &funcs);

void check_expr(const Expr *e, Decls &decl, Errs &errs, FnTable &funcs, bool inpath,
                bool callable_ok) {
    if (!e) {
        errs.note("bad expr node");
        return;
    }
    switch (e->k) {
    case EK::Var: {
        const Decl *d = decl.has(e->a) ? decl.find(e->a) : nullptr;
        if (!d && funcs.sigs.count(e->a) == 0) {
            errs.note("use of undeclared var `" + e->a + "`",
                      undeclared_rel(e->a, e, &decl, &funcs));
        } else if (!d && funcs.sigs.count(e->a) && !callable_ok) {
            errs.note("use of `" + e->a + "` as a value: a fn name is a callback, not a value "
                      "(hint: call it: `" + e->a + "()`)",
                      BorrowNote{0,
                                 "help: a bare fn name is a callback argument to `map`, "
                                 "`filter`, `fold`, `any`, `all`, `find` and `sort_by` only"});
        } else if (d && d->moved) {
            errs.note("use of " + d->moved_verb + " `" + e->a + "` (hint: " +
                          advice(d->moved_verb) + ")",
                      rel_at(d->moved_line, "`" + e->a + "` " + d->moved_verb + " here"));
        } else if (d && d->frozen_by) {
            errs.note("cannot use `" + e->a + "` while mutably borrowed by `" +
                          d->frozen_by->first + "` (hint: end the borrow first)",
                      rel_at(d->frozen_by->second, "mut borrow here"));
        }
        break;
    }
    case EK::Num:
    case EK::Str:
    case EK::RStr:
    case EK::Bool:
    case EK::None: break;
    case EK::Un:
    case EK::Move:
    case EK::Quest: check_expr(e->kids[0].get(), decl, errs, funcs); break;
    case EK::Closure: walk_closure_arg(*e, decl, errs, funcs); break;
    case EK::ParFor: {
        check_expr(e->kids[0].get(), decl, errs, funcs);
        check_expr(e->kids[1].get(), decl, errs, funcs);
        Decls sub = decl;
        sub.set(e->a, bare_decl());     // the loop variable is bound by the body
        Borrows none;
        walk_block(wrap(e->fn->body), sub, none, errs, funcs);
        break;
    }
    case EK::Bin:
    case EK::Or:
        check_expr(e->kids[0].get(), decl, errs, funcs);
        check_expr(e->kids[1].get(), decl, errs, funcs);
        break;
    case EK::MCall: {
        std::optional<std::string> bp;
        if (auto v = deref_var(e->kids[0].get(), decl, funcs)) {
            bp = *v;
        } else {
            bp = write_path(e->kids[0].get());
        }
        if (!bp || !callee_known(*bp + "." + e->b, decl, funcs)) {
            check_expr(e->kids[0].get(), decl, errs, funcs);
        }
        auto cb = HOF_CB_ARG.find(e->b);
        for (size_t i = 1; i < e->kids.size(); i++) {
            check_expr(e->kids[i].get(), decl, errs, funcs, false,
                       cb != HOF_CB_ARG.end() && cb->second == i - 1);
        }
        break;
    }
    case EK::VecLit:
    case EK::MapLit:
    case EK::StructLit:
        for (const auto &x : e->kids) check_expr(x.get(), decl, errs, funcs);
        break;
    case EK::Field: {
        check_expr(e->kids[0].get(), decl, errs, funcs, true);
        auto p = path_of(e);
        if (p && !inpath) {
            const Decl *d = decl.find(root_of(*p));
            if (d && !d->moved) {
                for (const std::string &mp : d->moved_paths) {
                    if (paths_conflict(*p, mp)) {
                        errs.note("use of moved `" + *p + "`");
                        break;
                    }
                }
            }
        }
        break;
    }
    case EK::Idx: {
        check_expr(e->kids[0].get(), decl, errs, funcs, true);
        check_expr(e->kids[1].get(), decl, errs, funcs);
        auto p = path_of(e);
        if (p && !inpath) {
            const Decl *d = decl.find(root_of(*p));
            if (d && !d->moved) {
                for (const std::string &mp : d->moved_paths) {
                    if (paths_conflict(*p, mp)) {
                        errs.note("use of moved `" + *p + "`");
                        break;
                    }
                }
            }
        }
        break;
    }
    case EK::Call:
        for (const auto &x : e->kids) check_expr(x.get(), decl, errs, funcs);
        if (!callee_known(e->a, decl, funcs)) {
            errs.note("use of undeclared var `" + root_of(e->a) + "`",
                      undeclared_rel(root_of(e->a), nullptr, &decl, &funcs));
        } else if (e->a == "ptr_of_str" && funcs.sigs.count(e->a) == 0 && !e->kids.empty()) {
            const Expr *a0 = skip_quest(e->kids[0].get());
            if (!(a0 && (a0->k == EK::Str || a0->k == EK::RStr)) &&
                borrow_srcs(a0, decl, funcs).empty()) {
                errs.note("`ptr_of_str` wants a string binding or string literal, not a temporary");
            }
        }
        break;
    }
}

void walk_closure_arg(const Expr &a, Decls &decl, Errs &errs, FnTable &funcs) {
    if (a.k != EK::Closure || !a.fn) return;
    Decls sub = decl;                  // shallow, like `dict(decl)`: same Decl objects
    auto saved_floor = errs.fn_floor;
    auto saved_defers = errs.active_defers;
    errs.fn_floor.clear();
    for (const auto &kv : decl.v) errs.fn_floor.insert(kv.first);
    if (a.fn->name) {
        sub.set(*a.fn->name, bare_decl());
        errs.fn_floor.insert(*a.fn->name);
    }
    for (const Param &p : a.fn->params) sub.set(p.name, param_decl(p));
    errs.active_defers.clear();
    Borrows none;
    walk_block(wrap(a.fn->body), sub, none, errs, funcs);
    errs.fn_floor = std::move(saved_floor);
    errs.active_defers = std::move(saved_defers);
}


const char *advice(const std::string &verb) {
    if (verb == "closed") return "the handle was closed; open a new one instead of reusing this";
    if (verb == "sent") {
        return "a channel send hands the value to the receiver; send a copy if you need it back";
    }
    return "the value was moved; use a copy or re-bind";
}

void moved_targets(const Expr *e, std::vector<std::pair<std::string, std::string>> &acc) {
    if (!e) return;
    if (e->k == EK::Move && !e->kids.empty()) {
        auto p = path_of(e->kids[0].get());
        if (p) {
            acc.emplace_back(*p, "moved");
            return;
        }
    }
    if (e->k == EK::Call) {
        auto it = CONSUMES.find(e->a);
        if (it != CONSUMES.end() && !e->kids.empty()) {
            auto p = path_of(e->kids[0].get());
            if (p) {
                acc.emplace_back(*p, it->second);
                return;
            }
        }
    }
    if (e->k == EK::MCall && e->b == "send" && e->kids.size() > 1) {
        auto p = path_of(e->kids[1].get());
        if (p) {
            acc.emplace_back(*p, "sent");
            return;
        }
    }
    for (const auto &sub : e->kids) moved_targets(sub.get(), acc);
}

std::optional<std::string> borrowed_param_source(const std::string &name,
                                                 const Decls &decl,
                                                 std::set<std::string> &seen) {
    if (!seen.insert(name).second) return std::nullopt;
    const Decl *d = decl.find(name);
    if (!d || d->copy) return std::nullopt;
    if (d->param) {
        if (d->mode && *d->mode == Mode::Move) return std::nullopt;
        return name;
    }
    std::vector<std::string> sources = d->srcs;
    if (sources.empty() && d->src) sources.push_back(*d->src);
    for (const std::string &source : sources) {
        if (auto borrowed = borrowed_param_source(source, decl, seen)) return borrowed;
    }
    return std::nullopt;
}

void note_try_move(Errs &errs, const std::string &root, const std::string &path,
                   const std::string &verb) {
    for (Decls &sink : errs.try_sinks) {
        Decl *td = sink.find(root);
        if (!td) {
            Decl fresh;
            sink.set(root, std::move(fresh));
            td = sink.find(root);
        }
        if (path == root) {
            if (!td->moved) {
                td->moved = true;
                td->moved_line = errs.line;
                td->moved_verb = verb;
            }
        } else {
            td->moved_paths.insert(path);
        }
    }
}

void scan_moves(const Expr *e, Decls &decl, Borrows &borrows, Errs &errs) {
    std::vector<std::pair<std::string, std::string>> targets;
    moved_targets(e, targets);
    for (const auto &[v, verb] : targets) {
        std::string root = root_of(v);
        Decl *d = decl.find(root);
        if (!d) continue;
        if (verb == "moved") {
            std::set<std::string> seen;
            if (auto borrowed = borrowed_param_source(root, decl, seen)) {
                errs.note("cannot move borrowed parameter `" + *borrowed +
                          "`: declare it `move` to take ownership");
            } else if (d->borrowed_ptr) {
                errs.note("cannot move borrowed pointer `" + root +
                          "`: only its owner may be moved or freed");
            }
        }
        if (verb == "sent" && d->copy) continue;
        if (d->moved) {
            // A consuming call reads its argument, and the read is already
            // reported with the right verb; reporting again is the same error
            // twice on one line.
            if (verb != "closed") {
                errs.note("use of " + d->moved_verb + " `" + root + "` (hint: " +
                              advice(d->moved_verb) + ")",
                          rel_at(d->moved_line, "`" + root + "` " + d->moved_verb + " here"));
            }
            continue;
        }
        bool conflict = false;
        for (const std::string &mp : d->moved_paths) {
            if (paths_conflict(v, mp)) {
                conflict = true;
                break;
            }
        }
        if (conflict) {
            errs.note("use of moved `" + v + "`");
            continue;
        }
        const std::vector<Borrow> *live = borrows.peek(root);
        if (live && !live->empty()) {
            const Borrow &b = live->back();
            errs.note("cannot move `" + v + "` while borrowed by `" + b.name + "`",
                      rel_at(b.line, "borrowed by `" + b.name + "` here"));
            continue;
        }
        if (v == root) {
            d->moved = true;
            d->moved_line = errs.line;
            d->moved_verb = verb;
        } else {
            d->moved_paths.insert(v);
        }
        note_try_move(errs, root, v, verb);
    }
}


std::string alias_owner(const std::string &name, const Decls &decl, int depth = 0) {
    if (depth > 32) return name;
    const Decl *d = decl.find(name);
    if (!d || !d->alias) return name;
    if (d->src && *d->src != name) return alias_owner(*d->src, decl, depth + 1);
    return name;
}

std::optional<BorrowNote> decl_hint(const Decl *d, const std::string &root) {
    if (!d || !d->dl) return std::nullopt;
    if (d->konst) return BorrowNote{d->dl, "`" + root + "` declared here as const"};
    return BorrowNote{d->dl, "`" + root + "` declared here without `mut` (help: `let mut " + root +
                                 "`)"};
}

std::optional<std::string> mutate_kind(const std::string &meth, FnTable &funcs) {
    auto it = funcs.umeth.find(meth);
    if (it != funcs.umeth.end()) return it->second ? std::optional<std::string>("mut self")
                                                   : std::nullopt;
    return MUTATORS.count(meth) ? std::optional<std::string>("builtin") : std::nullopt;
}

bool alias_place_conflict(const std::string &root, const std::string &path, Decls &decl,
                          Errs &errs, const std::string &who) {
    std::string own = alias_owner(root, decl);
    if (own == root) return false;
    const Decl *od = decl.find(own);
    if (!od) return false;
    if (od->konst) {
        errs.note("assign to const `" + path + "` through alias `" + root + "`");
        return true;
    }
    if (!od->mut) {
        errs.note("write through `" + root + "` mutates immutable `" + own + "`");
        return true;
    }
    errs.note("double mut borrow of `" + own + "`: `" + who + "` through alias `" + root +
                  "` is a second mutable place",
              rel_at(0, "help: `" + root + "` is a second mutable place over `" + own +
                           "`; keep one writer -- write through the owner (`" + own +
                           "[key][...] = ...`) instead of holding a mutating copy of one element"));
    return true;
}

// `(line, text)` for the `for` that is walking `root`, or nullopt.
std::optional<BorrowNote> iterated_by(const std::string &root, Errs &errs) {
    for (auto it = errs.iters.rbegin(); it != errs.iters.rend(); ++it) {
        if (it->first == root) return rel_at(it->second, "iterated here by this `for`");
    }
    return std::nullopt;
}

// A `for` holds a view of the container for the whole of its body; growing,
// shrinking or rewriting that container underneath the walk is the write the
// loop cannot survive. A rule, not a runtime guard: the runtime cannot see the
// loop. Check mutation/move of active loop collection.
bool iter_conflict(const std::string &root, Errs &errs) {
    std::optional<BorrowNote> rel = iterated_by(root, errs);
    if (!rel) return false;
    errs.note("cannot mutate `" + root + "` while iterating it (hint: collect the "
              "writes and apply them after the loop)", rel);
    return true;
}

bool check_write(const std::string &path, Decls &decl, Borrows &borrows, Errs &errs,
                 bool rebind = false, bool strict = true) {
    std::string root = root_of(path);
    Decl *d = decl.find(root);
    if (!d) return true;
    if (!rebind && iter_conflict(root, errs)) return false;
    if (d->konst) {
        errs.note("assign to const `" + path + "`");
        return false;
    }
    if (d->moved && !(rebind && path == root)) {
        errs.note("use of moved `" + root + "`",
                  rel_at(d->moved_line, "`" + root + "` moved here"));
        return false;
    }
    for (const std::string &mp : d->moved_paths) {
        if (starts_with(path, mp + ".")) {
            errs.note("use of moved `" + mp + "`");
            return false;
        }
    }
    if (d->spawn_frozen && rebind) {
        errs.note("assign to captured `" + root +
                  "` in spawn (captured by copy, write is lost)");
        return false;
    }
    if (strict && !d->mut) {
        errs.note("assign to immutable `" + path + "` (want `let mut` or mut param)",
                  decl_hint(d, root));
        return false;
    }
    if (!rebind && alias_place_conflict(root, path, decl, errs, root)) return false;
    const std::vector<Borrow> *live = borrows.peek(root);
    if (live && !live->empty()) {
        const Borrow &b = live->back();
        errs.note("cannot assign `" + root + "` while borrowed by `" + b.name + "`",
                  rel_at(b.line, "borrowed by `" + b.name + "` here"));
        return false;
    }
    return true;
}

bool check_mutate(const std::string &path, Decls &decl, Borrows &borrows, Errs &errs,
                  const std::optional<std::string> &kind, const std::string &who) {
    std::string root = root_of(path);
    Decl *d = decl.find(root);
    if (!d) return false;
    if (iter_conflict(root, errs)) return false;
    if (d->konst) {
        errs.note("assign to const `" + path + "`");
        return false;
    }
    if (d->moved) {
        errs.note("use of moved `" + root + "`",
                  rel_at(d->moved_line, "`" + root + "` moved here"));
        return false;
    }
    if (alias_place_conflict(root, path, decl, errs, who)) return false;
    if (!d->mut) {
        auto hint = decl_hint(d, root);
        if (kind && *kind == "mut self") {
            errs.note("cannot mutate immutable `" + root + "`: `" + who + "` takes mut self", hint);
        } else {
            errs.note("cannot mutate immutable `" + root + "`: `" + who + "` writes through it",
                      hint);
        }
        return false;
    }
    const std::vector<Borrow> *live = borrows.peek(root);
    if (live && !live->empty()) {
        const Borrow &b = live->back();
        errs.note("cannot mutate immutable `" + root + "` while borrowed by `" + b.name + "`",
                  rel_at(b.line, "borrowed by `" + b.name + "` here"));
        return false;
    }
    return true;
}

bool claim_loe(const std::string &path,
               const std::vector<std::pair<std::string, std::string>> &claimed, Errs &errs) {
    for (const auto &other : claimed) {
        if (paths_conflict(path, other.first)) {
            errs.note("law of exclusivity: `" + path + "` passed as mut twice in one expression "
                      "(clashes with `" + other.first + "` used by `" + other.second + "`)");
            return false;
        }
    }
    return true;
}

void scan_calls(const Expr *e, Decls &decl, Borrows &borrows, Errs &errs, FnTable &funcs,
                std::vector<std::pair<std::string, std::string>> &claimed) {
    if (!e) return;
    if (e->k == EK::Call) {
        // `&x` is an exclusive loan. C receives a writable pointer, so BRAT's
        // aliasing rule has to hold at the boundary exactly as it does inside a
        // function: two `&x` in one call means C may write the same place through
        // two pointers (Rust refuses `f(&mut x, &mut x)`), and `&x` of a binding
        // declared without `mut` is a write the program never allowed.
        std::map<std::string, int> amp_seen;
        for (const auto &kid : e->kids) {
            const Expr *arg = skip_quest(kid.get());
            if (!arg || arg->k != EK::Un || arg->a != "&" || arg->kids.empty()) continue;
            const Expr *inner = arg->kids[0].get();
            if (!inner || inner->k != EK::Var) continue;
            const std::string v = inner->a;
            auto seen = amp_seen.find(v);
            if (seen != amp_seen.end()) {
                errs.note("two `&" + v + "` in one call to `" + e->a +
                              "`: C may write the same place through both",
                          rel_at(seen->second, "the first `&" + v + "` here"));
                continue;
            }
            amp_seen[v] = errs.line;
            Decl *d = decl.find(v);
            if (d && d->moved) {
                errs.note("use of moved `" + v + "`",
                          rel_at(d->moved_line, "`" + v + "` moved here"));
            } else if (d && !d->mut) {
                errs.note("pass immutable `" + v + "` by address: C may write through `&" +
                          v + "`, so declare it `let mut " + v + "`");
            } else {
                const std::vector<Borrow> *live = borrows.peek(v);
                if (live && !live->empty()) {
                    const Borrow &b = live->back();
                    errs.note("cannot pass `&" + v + "` while borrowed by `" + b.name + "`",
                              rel_at(b.line, "borrowed by `" + b.name + "` here"));
                }
            }
        }
        auto it = funcs.sigs.find(e->a);
        if (it != funcs.sigs.end()) {
            const std::vector<ParamMode> &sig = it->second;
            for (size_t i = 0; i < sig.size() && i < e->kids.size(); i++) {
                const Expr *arg = skip_quest(e->kids[i].get());
                const ParamMode &pm = sig[i];
                if (pm.mode == Mode::Ref || (pm.mode != Mode::Mut && pm.mode != Mode::Move)) {
                    continue;
                }
                if (!arg) continue;
                auto p = place_of(arg);
                if (p && claim_loe(*p, claimed, errs)) claimed.emplace_back(*p, pm.name);
                std::string v;
                bool whole = arg->k == EK::Var;
                if (whole) {
                    v = arg->a;
                } else if (arg->k == EK::Field && p) {
                    v = root_of(*p);
                } else {
                    continue;
                }
                Decl *d = decl.find(v);
                const std::vector<Borrow> *live = borrows.peek(v);
                if (pm.mode == Mode::Mut) {
                    if (iter_conflict(v, errs)) {
                        continue;
                    } else if (d && !d->mut) {
                        errs.note("pass immutable `" + v + "` as mut param `" + pm.name + "`");
                    } else if (d && d->moved) {
                        errs.note("use of moved `" + v + "`",
                                  rel_at(d->moved_line, "`" + v + "` moved here"));
                    } else if (live && !live->empty()) {
                        const Borrow &b = live->back();
                        errs.note("cannot pass `" + v + "` as mut while borrowed by `" + b.name +
                                      "`",
                                  rel_at(b.line, "borrowed by `" + b.name + "` here"));
                    }
                    continue;
                }
                if (d && d->moved) {
                    errs.note("use of moved `" + v + "`",
                              rel_at(d->moved_line, "`" + v + "` moved here"));
                    continue;
                }
                if (live && !live->empty()) {
                    const Borrow &b = live->back();
                    errs.note("cannot move `" + v + "` while borrowed by `" + b.name + "`",
                              rel_at(b.line, "borrowed by `" + b.name + "` here"));
                    continue;
                }
                if (whole && d) {
                    bool conflict = false;
                    for (const std::string &mp : d->moved_paths) {
                        if (paths_conflict(v, mp)) {
                            conflict = true;
                            break;
                        }
                    }
                    if (conflict) {
                        errs.note("use of moved `" + v + "`");
                        continue;
                    }
                    d->moved = true;
                    d->moved_line = errs.line;
                    note_try_move(errs, v, v, "moved");
                }
            }
        }
    } else if (e->k == EK::MCall) {
        if (e->b == "send" && e->kids.size() > 1) {
            const Expr *arg = e->kids[1].get();
            if (arg && arg->k == EK::Var) {
                const Decl *d = decl.find(arg->a);
                // A parameter is a view of the caller's storage, so handing it
                // to a channel would leave the receiver reading a dead frame.
                if (d && d->param && d->mode.value_or(Mode::Ref) != Mode::Move) {
                    errs.note("cannot send `" + arg->a +
                              "`: a parameter is a view of the caller's storage; declare it "
                              "`move` or send a copy");
                }
            }
        }
        auto kind = mutate_kind(e->b, funcs);
        const Expr *recv = e->kids[0].get();
        std::optional<std::string> bpath;
        std::optional<std::string> bwrite;
        if (auto v = deref_var(recv, decl, funcs)) {
            bpath = *v;
            bwrite = *v;
        } else {
            bpath = path_of(recv);
            bwrite = write_path(recv);
        }
        std::optional<std::string> p;
        std::string who;
        if (kind && bwrite) {
            // A receiver that is a plain name chain reads the way a dotted
            // spelling did: the write lands on the root binding, and the full
            // path names the call.
            p = root_of(*bwrite);
            who = *bwrite + "." + e->b;
        } else {
            p = kind ? bpath : std::nullopt;
            who = e->b;
        }
        if (p) {
            if (check_mutate(*p, decl, borrows, errs, kind, who) && claim_loe(*p, claimed, errs)) {
                claimed.emplace_back(*p, who);
            }
        }
    }
    for (const auto &sub : e->kids) scan_calls(sub.get(), decl, borrows, errs, funcs, claimed);
}


void walk_stmt(const Stmt &s, Decls &decl, Borrows &borrows, Errs &errs, FnTable &funcs);

// Fold a block's exit state back in: a path taken once may run, or branch, twice.
void merge_moved(Decls &decl, const Decls &post) {
    for (const auto &[n, d] : post.v) {
        Decl *t = decl.find(n);
        if (!t) continue;
        if (d->moved && !t->moved) {
            t->moved = true;
            t->moved_line = d->moved_line;
            t->moved_verb = d->moved_verb;
        }
        if (!d->moved_paths.empty()) {
            for (const std::string &p : d->moved_paths) t->moved_paths.insert(p);
        }
    }
}

// True when control cannot fall out of the body at the bottom: a `tcp_close` on
// an error branch that returns must not count as a second close.
bool always_exits(const Body &body) {
    if (body.empty()) return false;
    const Stmt &last = *body.back();
    // Only `return` leaves the function: a `break` path still ends after the loop.
    if (last.k == SK::Return) return true;
    if (last.k == SK::If) {
        if (!last.els) return false;
        return always_exits(wrap(last.body)) && always_exits(else_body(last));
    }
    if (last.k == SK::Try) {
        return always_exits(wrap(last.body)) && always_exits(wrap(last.body2));
    }
    return false;
}

bool is_raw_ptr_decl(const Decl &d) {
    if (d.owned || d.borrowed_ptr) return true;
    if (!d.ann) return false;
    return *d.ann == "ptr" || d.ann->rfind("ptr[", 0) == 0;
}

void scan_defer_call_moves(const Expr *e, Decls &decl, Borrows &borrows, Errs &errs,
                           FnTable &funcs, std::set<std::string> &move_roots) {
    if (!e) return;
    if (e->k == EK::Call) {
        auto it = funcs.sigs.find(e->a);
        if (it != funcs.sigs.end()) {
            const std::vector<ParamMode> &sig = it->second;
            for (size_t i = 0; i < sig.size() && i < e->kids.size(); i++) {
                if (sig[i].mode != Mode::Move) continue;
                const Expr *arg = skip_quest(e->kids[i].get());
                if (!arg || arg->k != EK::Var) continue;
                const std::string &v = arg->a;
                move_roots.insert(v);
                Decl *d = decl.find(v);
                const std::vector<Borrow> *live = borrows.peek(v);
                if (d && d->moved) {
                    errs.note("use of moved `" + v + "`",
                              rel_at(d->moved_line, "`" + v + "` moved here"));
                } else if (live && !live->empty()) {
                    const Borrow &b = live->back();
                    errs.note("cannot move `" + v + "` while borrowed by `" + b.name + "`",
                              rel_at(b.line, "borrowed by `" + b.name + "` here"));
                } else if (d) {
                    d->moved = true;
                    d->moved_line = errs.line;
                    note_try_move(errs, v, v, "moved");
                }
            }
        }
    }
    for (const auto &sub : e->kids)
        scan_defer_call_moves(sub.get(), decl, borrows, errs, funcs, move_roots);
}

void run_defers(const std::vector<std::pair<const Expr *, int>> &defers, Decls &decl,
                Borrows &borrows, Errs &errs, FnTable &funcs) {
    int saved_line = errs.line;
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
        const Expr *de = it->first;
        errs.line = it->second;
        std::set<std::string> move_roots;
        std::vector<std::pair<std::string, std::string>> targets;
        moved_targets(de, targets);
        for (const auto &[p, _] : targets) move_roots.insert(root_of(p));
        scan_defer_call_moves(de, decl, borrows, errs, funcs, move_roots);
        scan_moves(de, decl, borrows, errs);
        std::vector<std::string> used;
        vars_in(de, used);
        std::set<std::string> seen;
        for (const std::string &v : used) {
            if (move_roots.count(v) || !seen.insert(v).second) continue;
            const Decl *d = decl.find(v);
            if (d && is_raw_ptr_decl(*d) && d->moved) {
                errs.note("use of " + d->moved_verb + " `" + v + "` (hint: " +
                              advice(d->moved_verb) + ")",
                          rel_at(d->moved_line, "`" + v + "` " + d->moved_verb + " here"));
            }
        }
    }
    errs.line = saved_line;
}

std::map<std::string, int> walk_block(const Body &body, Decls &decl, Borrows &borrows, Errs &errs,
                                      FnTable &funcs) {
    std::map<std::string, int> last;
    for (size_t i = 0; i < body.size(); i++) {
        std::vector<std::string> used;
        used_in_stmt(*body[i], used);
        int death = (body[i]->k == SK::Defer) ? static_cast<int>(body.size())
                                              : static_cast<int>(i);
        for (const std::string &v : used) {
            auto it = last.find(v);
            if (it == last.end() || death > it->second) last[v] = death;
        }
    }
    std::set<std::string> outer_names;
    for (const auto &kv : decl.v) outer_names.insert(kv.first);
    size_t defer_base = errs.active_defers.size();
    std::vector<std::pair<const Expr *, int>> defers;
    for (size_t i = 0; i < body.size(); i++) {
        const Stmt &s = *body[i];
        errs.idx = static_cast<int>(i);
        for (size_t b = 0; b < borrows.v.size();) {
            std::vector<Borrow> kept;
            for (const Borrow &bm : borrows.v[b].second) {
                auto it = last.find(bm.name);
                int dies = (it == last.end()) ? bm.born : it->second;
                if (!decl.has(bm.name) || dies >= static_cast<int>(i)) kept.push_back(bm);
            }
            if (kept.empty()) {
                borrows.v.erase(borrows.v.begin() + static_cast<long>(b));
            } else {
                borrows.v[b].second = std::move(kept);
                b++;
            }
        }
        for (auto &kv : decl.v) kv.second->frozen_by.reset();
        for (const auto &kv : borrows.v) {
            for (const Borrow &bm : kv.second) {
                Decl *d = decl.find(kv.first);
                if (bm.mut && d) d->frozen_by = std::make_pair(bm.name, bm.line);
            }
        }
        walk_stmt(s, decl, borrows, errs, funcs);
        if (s.k == SK::Defer) {
            int dln = s.line > 0 ? s.line : errs.line;
            defers.emplace_back(s.e1.get(), dln);
            errs.active_defers.emplace_back(s.e1.get(), dln);
        }
    }
    errs.active_defers.resize(defer_base);
    if (!always_exits(body)) {
        run_defers(defers, decl, borrows, errs, funcs);
        for (const auto &[n, d] : decl.v) {
            if (!outer_names.count(n) && d->owned && !d->moved) {
                errs.note("leaked owned pointer `" + n + "`: free it or return it",
                          rel_at(d->dl, "`" + n + "` allocated here"));
            }
        }
    }
    return last;
}

bool ref_escapes_local(const std::string &name, const Decls &decl, int depth = 0) {
    if (depth > 32) return false;
    const Decl *rd = decl.find(name);
    if (!rd) return true;
    if (rd->param) return false;
    std::vector<std::string> srcs = rd->srcs;
    if (srcs.empty() && rd->src) srcs.push_back(*rd->src);
    std::vector<std::string> next;
    for (const std::string &s : srcs) {
        if (!s.empty() && s != name) next.push_back(s);
    }
    if (next.empty()) return true;
    for (const std::string &s : next) {
        if (ref_escapes_local(s, decl, depth + 1)) return true;
    }
    return false;
}

void escaping_refs_name(const std::string &name, const Decls &decl, Errs &errs);

// A borrow of a *parameter's* storage may escape: the caller owns that storage
// and it outlives the call. A borrow of a local's storage still cannot.
void escaping_refs(const Expr *e, const Decls &decl, FnTable &funcs, Errs &errs) {
    e = skip_quest(e);
    if (!e) return;
    if (e->k == EK::Var) {
        escaping_refs_name(e->a, decl, errs);
        return;
    }
    if (e->k == EK::Call) {
        if ((e->a == "ref" || e->a == "borrow") && funcs.sigs.count(e->a) == 0) {
            for (const std::string &src : borrow_srcs(e, decl, funcs)) {
                if (ref_escapes_local(src, decl))
                    errs.note("ref `" + src + "` escapes via return");
            }
            return;
        }
        if (is_borrowed_ptr_expr(e, decl, funcs)) {
            for (const std::string &src : borrow_srcs(e, decl, funcs)) {
                if (ref_escapes_local(src, decl))
                    errs.note("ref `" + src + "` escapes via return");
            }
        }
        return;
    }
    if (e->k == EK::Move || e->k == EK::Field || e->k == EK::Idx || e->k == EK::Or ||
        e->k == EK::VecLit || e->k == EK::MapLit || e->k == EK::StructLit) {
        for (const auto &sub : e->kids) escaping_refs(sub.get(), decl, funcs, errs);
        return;
    }
}

void escaping_refs_name(const std::string &name, const Decls &decl, Errs &errs) {
    const Decl *d = decl.find(name);
    if (!d) return;
    if (d->ref) {
        if (ref_escapes_local(name, decl)) {
            errs.note("ref `" + name + "` escapes via return");
        }
        return;
    }
    if (!d->srcs.empty()) {
        for (const std::string &s : d->srcs) {
            if (s != name) escaping_refs_name(s, decl, errs);
        }
        return;
    }
    if (d->src && *d->src != name) escaping_refs_name(*d->src, decl, errs);
}

// Walk a loop body twice: a `move` legal on pass one is a use-after-move on two.
void walk_loop(const Body &body, Decls &decl, Borrows &borrows, Errs &errs, FnTable &funcs,
               const std::optional<std::string> &varname = std::nullopt) {
    auto one = [&]() {
        Decls sub = copy_decl(decl);
        if (varname) sub.set(*varname, bare_decl());
        Borrows bs = copy_borrows(borrows);
        walk_block(body, sub, bs, errs, funcs);
        return sub;
    };
    auto fingerprint = [&]() {
        std::map<std::string, std::pair<bool, std::set<std::string>>> out;
        for (const auto &[n, d] : decl.v) out[n] = {d->moved, d->moved_paths};
        return out;
    };
    auto before = fingerprint();
    merge_moved(decl, one());
    if (fingerprint() != before) merge_moved(decl, one());
}

void walk_set(const std::string &path, const Expr *value, Decls &decl, Borrows &borrows,
              Errs &errs, FnTable &funcs) {
    check_expr(value, decl, errs, funcs);
    scan_moves(value, decl, borrows, errs);
    std::vector<std::pair<std::string, std::string>> claimed;
    scan_calls(value, decl, borrows, errs, funcs, claimed);
    std::string tgt = root_of(path);
    if (!decl.has(tgt)) {
        errs.note("use of undeclared var `" + path + "`",
                  undeclared_rel(tgt, nullptr, &decl, &funcs));
        return;
    }
    if (!check_write(path, decl, borrows, errs, true)) return;
    Decl *d = decl.find(tgt);
    if (path == tgt) {
        if (d->owned && !d->moved) {
            errs.note("leaked owned pointer `" + tgt + "`: free it or return it",
                      rel_at(d->dl, "`" + tgt + "` allocated here"));
        }
        d->owned = value && value->k == EK::Call && funcs.owned_ret.count(value->a) > 0;
        if (d->owned) d->dl = errs.line;
    }
    d->moved = false;
    if (d->pinned && is_copy_expr(value, decl, funcs)) {
        d->copy = true;
    } else {
        d->copy = is_copy_expr(value, decl, funcs);
        d->pinned = false;
    }
    if (path.find('.') == std::string::npos) {
        d->moved_paths.clear();
    } else {
        std::set<std::string> kept;
        for (const std::string &mp : d->moved_paths) {
            if (mp != path && !starts_with(mp, path + ".")) kept.insert(mp);
        }
        d->moved_paths = std::move(kept);
    }
}

void walk_stmt(const Stmt &s, Decls &decl, Borrows &borrows, Errs &errs, FnTable &funcs) {
    if (s.line > 0) errs.line = s.line;
    switch (s.k) {
    case SK::Let:
    case SK::LetTmp: {
        const Expr *e = s.e1.get();
        check_expr(e, decl, errs, funcs);
        scan_moves(e, decl, borrows, errs);
        std::vector<std::pair<std::string, std::string>> claimed;
        scan_calls(e, decl, borrows, errs, funcs, claimed);
        std::vector<std::string> srcs = borrow_srcs(e, decl, funcs);
        auto place = place_of(e);
        bool elem = place && place->find('.') != std::string::npos;
        bool hm = s.mut && !elem;
        for (const std::string &src : srcs) {
            std::vector<Borrow> &prev = borrows.get(src);
            if (!prev.empty() && (hm || prev.back().mut)) {
                const Borrow &b = prev.back();
                errs.note("double mut borrow of `" + src + "`: `" + s.a + "` clashes with `" +
                              b.name + "`",
                          rel_at(b.line, "first borrow `" + b.name + "` here"));
            }
            prev.push_back(Borrow{s.a, hm, errs.line, errs.idx});
        }
        if (s.mut && !elem) {
            for (const std::string &src : srcs) {
                Decl *sd = decl.find(src);
                if (!sd) continue;
                if (sd->konst) {
                    errs.note("assign to const `" + src + "` through mut borrow `" + s.a + "`");
                } else if (!sd->mut) {
                    errs.note("cannot borrow immutable `" + src + "` as mut (`let mut " + s.a +
                              "`)");
                }
            }
        }
        Decl d;
        d.mut = s.mut;
        d.ref = !srcs.empty() && !elem;
        if (!srcs.empty()) d.src = srcs[0];
        d.srcs = srcs;
        d.konst = s.konst;
        d.alias = srcs.size() == 1 && place.has_value();
        bool ann_copy = has(COPY_ANNS, s.ann);
        d.copy = ann_copy || is_copy_expr(e, decl, funcs);
        d.pinned = ann_copy;
        d.ann = s.ann ? s.ann : infer_expr_ann(e, decl, &funcs);
        d.dl = errs.line;
        bool owned_call = e && e->k == EK::Call && funcs.owned_ret.count(e->a) > 0;
        if (owned_call) d.owned = true;
        if (is_borrowed_ptr_expr(e, decl, funcs)) d.borrowed_ptr = true;
        decl.set(s.a, std::move(d));
        return;
    }
    case SK::Set: walk_set(s.a, s.e1.get(), decl, borrows, errs, funcs); return;
    case SK::SetIdx: {
        check_expr(s.e1.get(), decl, errs, funcs);
        check_expr(s.e2.get(), decl, errs, funcs);
        check_expr(s.e3.get(), decl, errs, funcs);
        scan_moves(s.e3.get(), decl, borrows, errs);
        std::vector<std::pair<std::string, std::string>> claimed;
        scan_calls(s.e3.get(), decl, borrows, errs, funcs, claimed);
        std::optional<std::string> base;
        if (auto v = deref_var(s.e1.get(), decl, funcs)) {
            base = *v;
        } else {
            base = path_of(s.e1.get());
        }
        if (!base) return;
        std::string root = root_of(*base);
        if (!decl.has(root)) {
            errs.note("use of undeclared var `" + *base + "`",
                      undeclared_rel(root, nullptr, &decl, &funcs));
        } else {
            check_write(*base, decl, borrows, errs, false, true);
        }
        return;
    }
    case SK::SetField: {
        std::optional<std::string> bwrite;
        std::optional<std::string> bpath;
        if (auto v = deref_var(s.e1.get(), decl, funcs)) {
            bwrite = *v;
            bpath = *v;
        } else {
            bwrite = write_path(s.e1.get());
            bpath = path_of(s.e1.get());
        }
        if (bwrite) {
            // A write through a name chain is the statement a dotted spelling
            // used to build, so it takes the plain-`set` checks and bookkeeping.
            walk_set(*bwrite + "." + s.a, s.e2.get(), decl, borrows, errs, funcs);
            return;
        }
        check_expr(s.e1.get(), decl, errs, funcs, true);
        check_expr(s.e2.get(), decl, errs, funcs);
        scan_moves(s.e2.get(), decl, borrows, errs);
        std::vector<std::pair<std::string, std::string>> claimed;
        scan_calls(s.e2.get(), decl, borrows, errs, funcs, claimed);
        if (!bpath) return;
        std::string p = *bpath + "." + s.a;
        std::string root = root_of(p);
        if (!decl.has(root)) {
            errs.note("use of undeclared var `" + *bpath + "`",
                      undeclared_rel(root, nullptr, &decl, &funcs));
        } else {
            check_write(p, decl, borrows, errs, false, false);
        }
        return;
    }
    case SK::Return: {
        if (errs.in_spawn) errs.note("return escapes spawn");
        check_expr(s.e1.get(), decl, errs, funcs);
        scan_moves(s.e1.get(), decl, borrows, errs);
        std::vector<std::pair<std::string, std::string>> claimed;
        scan_calls(s.e1.get(), decl, borrows, errs, funcs, claimed);
        if (s.e1 && s.e1->k == EK::Var) {
            Decl *d = decl.find(s.e1->a);
            if (d && d->owned) {
                d->moved = true;               // returning it hands it to the caller
                d->moved_line = errs.line;
                note_try_move(errs, s.e1->a, s.e1->a, "moved");
            }
        }
        escaping_refs(s.e1.get(), decl, funcs, errs);
        {
            Decls rdecl = copy_decl(decl);
            Borrows rbs = copy_borrows(borrows);
            run_defers(errs.active_defers, rdecl, rbs, errs, funcs);
            for (const auto &[n, d] : rdecl.v) {
                if (!errs.fn_floor.count(n) && d->owned && !d->moved) {
                    errs.note("leaked owned pointer `" + n + "`: free it or return it",
                              rel_at(d->dl, "`" + n + "` allocated here"));
                }
            }
        }
        return;
    }
    case SK::Expr: {
        check_expr(s.e1.get(), decl, errs, funcs);
        scan_moves(s.e1.get(), decl, borrows, errs);
        std::vector<std::pair<std::string, std::string>> claimed;
        scan_calls(s.e1.get(), decl, borrows, errs, funcs, claimed);
        return;
    }
    case SK::Fn:
    case SK::ExportFn: {
        const FnDecl &f = s.fns[0];
        Decls sub = decl;                  // shallow, like `dict(decl)`
        auto saved_floor = errs.fn_floor;
        auto saved_defers = errs.active_defers;
        auto saved_sinks = std::move(errs.try_sinks);
        errs.fn_floor.clear();
        for (const auto &kv : decl.v) errs.fn_floor.insert(kv.first);
        for (const Param &p : f.params) sub.set(p.name, param_decl(p));
        errs.active_defers.clear();
        errs.try_sinks.clear();
        Borrows none;
        walk_block(wrap(f.body), sub, none, errs, funcs);
        errs.fn_floor = std::move(saved_floor);
        errs.active_defers = std::move(saved_defers);
        errs.try_sinks = std::move(saved_sinks);
        return;
    }
    case SK::If: {
        check_expr(s.e1.get(), decl, errs, funcs);
        scan_moves(s.e1.get(), decl, borrows, errs);
        std::vector<std::pair<std::string, std::string>> claimed;
        scan_calls(s.e1.get(), decl, borrows, errs, funcs, claimed);
        Decls pre = copy_decl(decl);
        {
            Borrows bs = copy_borrows(borrows);
            walk_block(wrap(s.body), pre, bs, errs, funcs);
        }
        if (s.els) {
            Body ebody = else_body(s);
            Decls post = copy_decl(decl);
            {
                Borrows bs = copy_borrows(borrows);
                walk_block(ebody, post, bs, errs, funcs);
            }
            // Only an arm that can fall through carries its moves onward; an
            // arm that returns takes them out of the function.
            if (!always_exits(wrap(s.body))) merge_moved(decl, pre);
            if (!always_exits(ebody)) merge_moved(decl, post);
            return;
        }
        if (!always_exits(wrap(s.body))) merge_moved(decl, pre);
        return;
    }
    case SK::While: {
        check_expr(s.e1.get(), decl, errs, funcs);
        scan_moves(s.e1.get(), decl, borrows, errs);
        std::vector<std::pair<std::string, std::string>> claimed;
        scan_calls(s.e1.get(), decl, borrows, errs, funcs, claimed);
        walk_loop(wrap(s.body), decl, borrows, errs, funcs);
        return;
    }
    case SK::ForIter: {
        check_expr(s.e1.get(), decl, errs, funcs);
        scan_moves(s.e1.get(), decl, borrows, errs);
        std::vector<std::pair<std::string, std::string>> claimed;
        scan_calls(s.e1.get(), decl, borrows, errs, funcs, claimed);
        // Only a *place* pins the walk: `for x in v.copy()` walks a fresh
        // container and `path_of` answers nullopt for a call; a range pins
        // nothing, so `for i in 0..3 { v.push(i) }` stays legal.
        std::optional<std::string> ip = path_of(s.e1.get());
        if (!ip) {
            walk_loop(wrap(s.body), decl, borrows, errs, funcs, s.a);
            return;
        }
        errs.iters.emplace_back(root_of(*ip), errs.line);
        walk_loop(wrap(s.body), decl, borrows, errs, funcs, s.a);
        errs.iters.pop_back();
        return;
    }
    case SK::ForRange: {
        for (const Expr *b : {s.e1.get(), s.e2.get()}) {
            check_expr(b, decl, errs, funcs);
            scan_moves(b, decl, borrows, errs);
            std::vector<std::pair<std::string, std::string>> claimed;
            scan_calls(b, decl, borrows, errs, funcs, claimed);
        }
        walk_loop(wrap(s.body), decl, borrows, errs, funcs, s.a);
        return;
    }
    case SK::Break:
    case SK::Continue: return;
    case SK::Try: {
        Body tbody = wrap(s.body);
        Body cbody = wrap(s.body2);
        Decls pre = copy_decl(decl);
        errs.try_sinks.emplace_back();
        {
            Borrows bs = copy_borrows(borrows);
            walk_block(tbody, pre, bs, errs, funcs);
        }
        Decls sink = std::move(errs.try_sinks.back());
        errs.try_sinks.pop_back();
        Decls hdecl = copy_decl(decl);
        merge_moved(hdecl, sink);
        if (s.opt1 && !s.opt1->empty()) hdecl.set(*s.opt1, bare_decl());
        {
            Borrows bs = copy_borrows(borrows);
            walk_block(cbody, hdecl, bs, errs, funcs);
        }
        if (!always_exits(tbody)) merge_moved(decl, pre);
        if (!always_exits(cbody)) merge_moved(decl, hdecl);
        return;
    }
    case SK::Scope: {
        // The block joins rather than branches: it walks the caller's own table.
        walk_block(wrap(s.body), decl, borrows, errs, funcs);
        merge_moved(decl, decl);
        return;
    }
    case SK::Spawn: {
        std::set<std::string> seen;
        std::vector<std::string> used;
        used_in_block(wrap(s.body), used);
        for (const std::string &v : used) {
            const Decl *d = decl.find(v);
            if (d && d->ref && !seen.count(v)) {
                seen.insert(v);
                errs.note("ref `" + v + "` escapes via spawn");
            }
        }
        Decls sub = copy_decl(decl);
        for (auto &kv : sub.v) kv.second->spawn_frozen = true;
        auto saved_floor = errs.fn_floor;
        auto saved_defers = errs.active_defers;
        errs.fn_floor.clear();
        for (const auto &kv : decl.v) errs.fn_floor.insert(kv.first);
        errs.active_defers.clear();
        errs.in_spawn++;
        {
            Borrows bs = copy_borrows(borrows);
            walk_block(wrap(s.body), sub, bs, errs, funcs);
        }
        errs.in_spawn--;
        errs.fn_floor = std::move(saved_floor);
        errs.active_defers = std::move(saved_defers);
        merge_moved(decl, sub);
        return;
    }
    case SK::Defer:
        check_expr(s.e1.get(), decl, errs, funcs);
        {
            Decls snap = copy_decl(decl);
            auto saved_sinks = std::move(errs.try_sinks);
            errs.try_sinks.clear();
            std::vector<std::pair<std::string, std::string>> claimed;
            scan_calls(s.e1.get(), snap, borrows, errs, funcs, claimed);
            errs.try_sinks = std::move(saved_sinks);
        }
        return;
    case SK::Unsafe: {
        Decls sub = copy_decl(decl);
        Borrows bs = copy_borrows(borrows);
        // `unsafe` is for raw pointers and FFI; it does not buy a way around
        // ownership. 
        walk_block(wrap(s.body), sub, bs, errs, funcs);
        return;
    }
    case SK::Impl: {
        std::set<std::string> seen;
        for (const FnDecl &f : s.fns) {
            std::string name = f.name.value_or("");
            if (seen.count(name)) {
                errs.note("duplicate method `" + s.a + "." + name + "`");
            }
            seen.insert(name);
            Decls sub = decl;              // shallow, like `dict(decl)`
            auto saved_floor = errs.fn_floor;
            auto saved_defers = errs.active_defers;
            auto saved_sinks = std::move(errs.try_sinks);
            errs.fn_floor.clear();
            for (const auto &kv : decl.v) errs.fn_floor.insert(kv.first);
            for (const Param &p : f.params) {
                Decl pd = param_decl(p);
                if (p.name == "self" && !pd.ann && funcs.struct_fields.count(s.a))
                    pd.ann = s.a;
                sub.set(p.name, std::move(pd));
            }
            errs.active_defers.clear();
            errs.try_sinks.clear();
            Borrows none;
            walk_block(wrap(f.body), sub, none, errs, funcs);
            errs.fn_floor = std::move(saved_floor);
            errs.active_defers = std::move(saved_defers);
            errs.try_sinks = std::move(saved_sinks);
        }
        return;
    }
    case SK::Enum:
        for (const Variant &v : s.variants) funcs.sigs[v.name] = {};
        return;
    case SK::Block:
        // Never a statement in a tree the parser builds; walked anyway so that
        // -Werror=switch keeps the enumeration honest.
        walk_block(wrap(s.body), decl, borrows, errs, funcs);
        return;
    case SK::Trait:
    case SK::Struct:
    case SK::Import:
    case SK::Extern:
    case SK::ExternStruct:
    case SK::PubUse: return;
    case SK::Pub: walk_stmt(*s.body[0], decl, borrows, errs, funcs); return;
    }
}


void unused_in(const Body &body, std::vector<BorrowDiag> &warns) {
    std::vector<std::pair<std::string, int>> declared;
    for (const Stmt *sp : body) {
        const Stmt &s = *sp;
        if (s.k == SK::Let || s.k == SK::LetTmp) {
            declared.emplace_back(s.a, s.line);
        } else if (s.k == SK::Pub && !s.body.empty()) {
            const Stmt &in = *s.body[0];
            if ((in.k == SK::Let || in.k == SK::LetTmp) && !in.konst)
                declared.emplace_back(in.a, in.line);
        }
    }
    std::vector<std::string> used_list;
    used_in_block(body, used_list);
    std::set<std::string> used(used_list.begin(), used_list.end());
    for (const auto &[name, ln] : declared) {
        if (!name.empty() && name[0] == '_') continue;
        if (!used.count(name)) warns.push_back(BorrowDiag{ln, "unused var `" + name + "`", std::nullopt});
    }
}

// A lexer ends a literal at the first `"` outside parentheses, so `"{e["hits"]}"`
// arrives as a literal piece, a name and another piece. The fingerprint is a
// piece ending in `{name` or `{name.`: a JSON blob keeps its braces paired.
bool unclosed_brace(const std::string &s) {
    std::string t;
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 2, "{{") == 0) {
            i += 2;
            continue;
        }
        t.push_back(s[i++]);
    }
    std::string u;
    for (size_t i = 0; i < t.size();) {
        if (t.compare(i, 2, "}}") == 0) {
            i += 2;
            continue;
        }
        u.push_back(t[i++]);
    }
    long opens = 0, closes = 0;
    for (char c : u) {
        if (c == '{') opens++;
        if (c == '}') closes++;
    }
    if (opens == closes) return false;
    for (size_t i = 0; i < u.size(); i++) {
        if (u[i] != '{') continue;
        size_t j = i + 1;
        if (j >= u.size()) continue;
        char c = u[j];
        bool ident = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (!ident) continue;
        while (j < u.size() && ((u[j] >= 'A' && u[j] <= 'Z') || (u[j] >= 'a' && u[j] <= 'z') ||
                                (u[j] >= '0' && u[j] <= '9') || u[j] == '_')) {
            j++;
        }
        if (j < u.size() && u[j] == '.') j++;
        while (j < u.size() && (u[j] == '[' || u[j] == '"' || u[j] == '\'')) j++;
        if (u.find('}', j) == std::string::npos) return true;
    }
    return false;
}

bool cut_string_ops(const std::vector<const Expr *> &ops) {
    if (ops.size() < 2) return false;
    bool any_rstr = false, any_other = false;
    for (const Expr *o : ops) {
        if (o->k == EK::RStr) any_rstr = true;
        if (o->k != EK::RStr && o->k != EK::Str) any_other = true;
    }
    if (!any_rstr || !any_other) return false;
    for (const Expr *o : ops) {
        if (o->k == EK::RStr && unclosed_brace(o->a)) return true;
    }
    return false;
}

void cut_warn(const std::vector<const Expr *> &ops, int line, std::vector<BorrowDiag> &warns) {
    for (const Expr *o : ops) CUT_PARTS.insert(o);
    warns.push_back(BorrowDiag{
        line,
        R"BRAT(string literal looks cut short: a `{...}` placeholder stops at a bare `"`, so `"{e[\"hits\"]}"` is not the placeholder it looks like (help: `"{e.hits}"`, or `let hits = e["hits"]` then `"{hits}"`; a quote inside a call is fine: `"{m.get(\"hits\")}"`))BRAT",
        std::nullopt});
}

void plus_operands(const Expr *e, std::vector<const Expr *> &out) {
    if (e->k == EK::Bin && e->a == "+") {
        plus_operands(e->kids[0].get(), out);
        plus_operands(e->kids[1].get(), out);
        return;
    }
    out.push_back(e);
}

void cut_body(const Body &body, std::vector<BorrowDiag> &warns);
void cut_expr(const Expr &e, int line, std::vector<BorrowDiag> &warns);

// The list rule: a call's arguments, a vec's items, a `+`-split literal.
void cut_list(const std::vector<const Expr *> &items, int line, std::vector<BorrowDiag> &warns) {
    if (cut_string_ops(items)) cut_warn(items, line, warns);
    for (const Expr *x : items) cut_expr(*x, line, warns);
}

void cut_expr(const Expr &e, int line, std::vector<BorrowDiag> &warns) {
    if (e.k == EK::Bin && e.a == "+") {
        std::vector<const Expr *> ops;
        plus_operands(&e, ops);
        if (cut_string_ops(ops)) {
            cut_warn(ops, line, warns);
            return;
        }
    }
    switch (e.k) {
    case EK::Call: {
        std::vector<const Expr *> items;
        for (const auto &k : e.kids) items.push_back(k.get());
        cut_list(items, line, warns);
        return;
    }
    case EK::ParFor: {
        cut_expr(*e.kids[0], line, warns);
        cut_expr(*e.kids[1], line, warns);
        for (const auto &st : e.fn->body) cut_stmt(*st, warns);
        return;
    }
    case EK::MCall: {
        cut_expr(*e.kids[0], line, warns);
        std::vector<const Expr *> items;
        for (size_t i = 1; i < e.kids.size(); i++) items.push_back(e.kids[i].get());
        cut_list(items, line, warns);
        return;
    }
    case EK::VecLit: {
        std::vector<const Expr *> items;
        for (const auto &k : e.kids) items.push_back(k.get());
        cut_list(items, line, warns);
        return;
    }
    case EK::MapLit:
        // The reference walks the pair list, whose entries are `(key, value)`
        // tuples: the key is read as a node tag and never matches, the value is
        // reached as the tuple's second element. Keys are left alone.
        for (size_t i = 1; i < e.kids.size(); i += 2) cut_expr(*e.kids[i], line, warns);
        return;
    case EK::StructLit:
        for (const auto &k : e.kids) cut_expr(*k, line, warns);
        return;
    case EK::Closure:
        if (e.fn) {
            for (const Param &p : e.fn->params) {
                if (p.def) cut_expr(*p.def, line, warns);
            }
            cut_body(wrap(e.fn->body), warns);
        }
        return;
    case EK::Bin:
    case EK::Un:
    case EK::Move:
    case EK::Quest:
    case EK::Field:
    case EK::Idx:
    case EK::Or:
        for (const auto &k : e.kids) cut_expr(*k, line, warns);
        return;
    case EK::Num:
    case EK::Str:
    case EK::RStr:
    case EK::Bool:
    case EK::None:
    case EK::Var: return;
    }
}

void cut_stmt(const Stmt &s, std::vector<BorrowDiag> &warns) {
    int line = s.line;
    for (const Expr *e : {s.e1.get(), s.e2.get(), s.e3.get()}) {
        if (e) cut_expr(*e, line, warns);
    }
    for (const Param &p : s.params) {
        if (p.def) cut_expr(*p.def, line, warns);
    }
    for (const Param &p : s.ext.params) {
        if (p.def) cut_expr(*p.def, line, warns);
    }
    for (const FnDecl &f : s.fns) {
        for (const Param &p : f.params) {
            if (p.def) cut_expr(*p.def, line, warns);
        }
        cut_body(wrap(f.body), warns);
    }
    cut_body(wrap(s.body), warns);
    cut_body(wrap(s.body2), warns);
    if (s.els) cut_stmt(*s.els, warns);
}

// A statement list can never be the cut-literal shape: a statement's head is a
// tag, never `rstr`, so the list rule has nothing to fire on there.
void cut_body(const Body &body, std::vector<BorrowDiag> &warns) {
    for (const Stmt *s : body) cut_stmt(*s, warns);
}


void scan_type_names_expr(const Expr &e, FnTable &funcs);

void scan_type_names_stmt(const Stmt &s, FnTable &funcs) {
    for (const Expr *e : {s.e1.get(), s.e2.get(), s.e3.get()}) {
        if (e) scan_type_names_expr(*e, funcs);
    }
    for (const Param &p : s.params) {
        if (p.def) scan_type_names_expr(*p.def, funcs);
    }
    for (const Param &p : s.ext.params) {
        if (p.def) scan_type_names_expr(*p.def, funcs);
    }
    for (const FnDecl &f : s.fns) {
        for (const Param &p : f.params) {
            if (p.def) scan_type_names_expr(*p.def, funcs);
        }
        for (const auto &b : f.body) scan_type_names_stmt(*b, funcs);
    }
    for (const auto &b : s.body) scan_type_names_stmt(*b, funcs);
    for (const auto &b : s.body2) scan_type_names_stmt(*b, funcs);
    if (s.els) scan_type_names_stmt(*s.els, funcs);
}

void scan_type_names_expr(const Expr &e, FnTable &funcs) {
    if (e.k == EK::StructLit) funcs.heads.insert(e.a);
    for (const auto &k : e.kids) scan_type_names_expr(*k, funcs);
    if (e.k == EK::Closure && e.fn) {
        for (const Param &p : e.fn->params) {
            if (p.def) scan_type_names_expr(*p.def, funcs);
        }
        for (const auto &b : e.fn->body) scan_type_names_stmt(*b, funcs);
    }
}

const Stmt *pub_inner(const Stmt &s) {
    if (s.k == SK::Pub && !s.body.empty()) return s.body[0].get();
    return &s;
}

void collect_heads(const std::vector<StmtP> &body, FnTable &funcs) {
    for (const auto &sp : body) {
        const Stmt &s = *sp;
        if (s.k == SK::Import) {
            std::string path = s.a;
            size_t at = 0;
            while ((at = path.find('.', at)) != std::string::npos) {
                funcs.heads.insert(path.substr(0, at));
                at++;
            }
            funcs.heads.insert(path);
        }
        const Stmt *node = pub_inner(s);
        if (node->k == SK::Impl) {
            funcs.heads.insert(node->a);
            for (const FnDecl &f : node->fns) {
                if (f.params.empty() || f.params[0].name != "self") continue;
                bool is_self_mut = f.params[0].mode == Mode::Mut;
                if (MUTATORS.count(f.name.value_or("")) && !is_self_mut) continue;
                bool &slot = funcs.umeth[f.name.value_or("")];
                slot = slot || is_self_mut;
            }
        } else if (node->k == SK::Enum || node->k == SK::Struct) {
            funcs.heads.insert(node->a);
        }
    }
    for (const auto &s : body) scan_type_names_stmt(*s, funcs);
}

}  // namespace

// A `fn` inside a body is callable from that body. The pass above only sees
// top-level declarations, so a nested `fn` used to read as undeclared.
static void register_nested(const std::vector<StmtP> &body, FnTable &funcs);

static void register_nested_stmt(const Stmt &s, FnTable &funcs) {
    if (s.k == SK::Fn || s.k == SK::ExportFn) {
        const FnDecl &f = s.fns[0];
        funcs.sigs[f.name.value_or("")] = fn_modes(f.params);
    }
    register_nested(s.body, funcs);
    register_nested(s.body2, funcs);
    if (s.els) register_nested_stmt(*s.els, funcs);
    for (const FnDecl &f : s.fns) register_nested(f.body, funcs);
}

static void register_nested(const std::vector<StmtP> &body, FnTable &funcs) {
    for (const auto &sp : body) {
        if (sp) register_nested_stmt(*sp, funcs);
    }
}


static std::vector<size_t> extern_borrows_indices(const ExternDecl &ext) {
    std::vector<size_t> out;
    for (const std::string &eff : ext.effects) {
        if (eff.rfind("borrows:", 0) == 0) {
            std::string bp = eff.substr(8);
            for (size_t i = 0; i < ext.params.size(); i++) {
                if (ext.params[i].name == bp &&
                    std::find(out.begin(), out.end(), i) == out.end())
                    out.push_back(i);
            }
        }
    }
    return out;
}

FnTable pub_sigs(const std::vector<StmtP> &body,
                 const std::set<std::string> &exp_fns) {
    static const std::set<std::string> copy_rets = {
        "bool", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "isize",
        "f32", "f64",
    };
    FnTable out;
    for (const auto &sp : body) {
        if (sp->k == SK::Extern) {
            out.sigs[sp->ext.name] = fn_modes(sp->ext.params);
            if (sp->ext.ret && copy_rets.count(*sp->ext.ret)) out.scalar_ret.insert(sp->ext.name);
            else if (sp->ext.owned) out.owned_ret.insert(sp->ext.name);
            auto bix = extern_borrows_indices(sp->ext);
            if (!bix.empty()) out.borrows_ret[sp->ext.name] = std::move(bix);
            continue;
        }
        if (sp->k == SK::Fn && !sp->fns.empty()) {
            const std::string fnm = sp->fns[0].name.value_or("");
            if (exp_fns.count(fnm)) {
                out.sigs[fnm] = fn_modes(sp->fns[0].params);
                if (sp->fns[0].ret) out.fn_rets[fnm] = *sp->fns[0].ret;
            }
            continue;
        }
        if (sp->k != SK::Pub || sp->body.empty()) continue;
        const Stmt &in = *sp->body[0];
        if (in.k == SK::Fn) {
            const std::string fnm = in.fns[0].name.value_or("");
            out.sigs[fnm] = fn_modes(in.fns[0].params);
            if (in.fns[0].ret) out.fn_rets[fnm] = *in.fns[0].ret;
        } else if (in.k == SK::Extern) {
            out.sigs[in.ext.name] = fn_modes(in.ext.params);
            if (in.ext.ret && copy_rets.count(*in.ext.ret)) out.scalar_ret.insert(in.ext.name);
            else if (in.ext.owned) out.owned_ret.insert(in.ext.name);
            auto bix = extern_borrows_indices(in.ext);
            if (!bix.empty()) out.borrows_ret[in.ext.name] = std::move(bix);
        } else if (in.k == SK::Enum) {
            out.heads.insert(in.a);
            auto &ef = out.struct_fields["__enum__:" + in.a];
            size_t slot = 0;
            for (const Variant &v : in.variants) {
                out.sigs[v.name] = {};
                if (v.name != "Ok" && v.name != "Err" && v.name != "Some" && v.name != "None")
                    out.fn_rets[v.name] = in.a;
                for (size_t fi = 0; fi < v.fields.size(); fi++) {
                    std::string fann = (fi < v.field_anns.size() && v.field_anns[fi] &&
                                        !v.field_anns[fi]->empty())
                                           ? *v.field_anns[fi]
                                           : "dyn";
                    ef["f" + std::to_string(slot++)] = std::move(fann);
                }
            }
        } else if (in.k == SK::Struct) {
            out.heads.insert(in.a);
            for (const auto &f : in.fields) {
                if (f.ann) out.struct_fields[in.a][f.name] = *f.ann;
            }
        } else if (in.k == SK::Impl) {
            out.heads.insert(in.a);
            for (const FnDecl &f : in.fns) {
                if (f.name && f.ret) out.fn_rets[in.a + "." + *f.name] = *f.ret;
            }
        } else if ((in.k == SK::Let || in.k == SK::LetTmp) && in.konst) {
            out.pub_consts.insert(in.a);
        }
    }
    return out;
}


// A slot of an `extern` declaration: the widths the C emitter can spell, a
// pointer, a callback, an `extern struct`, or `none`/absent for void. Anything
// else is refused here, at `check`, so `check` stays the verdict it claims to
// be: a program whose signature the emitter cannot spell used to pass `check`
// and die in `build`. 
const std::set<std::string> FFI_SLOTS = {"bool", "i8", "u8", "i16", "u16", "i32", "u32",
                                         "i64", "u64", "usize", "f32", "f64",
                                         "str", "ptr"};

const char *FFI_HINT =
    "a supported extern slot is bool, i8..u64, usize, f32, f64, str, ptr, "
    "ptr[T], fn(...), an `extern struct`, or `none`";

std::string trim_ffi_type(const std::string &text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> split_ffi_types(const std::string &text) {
    std::vector<std::string> parts;
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '(' || text[i] == '[') depth++;
        else if (text[i] == ')' || text[i] == ']') depth--;
        else if (text[i] == ',' && depth == 0) {
            std::string part = trim_ffi_type(text.substr(start, i - start));
            if (!part.empty()) parts.push_back(part);
            start = i + 1;
        }
    }
    std::string tail = trim_ffi_type(text.substr(start));
    if (!tail.empty()) parts.push_back(tail);
    return parts;
}

std::optional<std::pair<std::vector<std::string>, std::string>> callback_parts(const std::string &ann) {
    std::string text = trim_ffi_type(ann);
    if (text.rfind("fn", 0) != 0) return std::nullopt;
    size_t open_at = 2;
    while (open_at < text.size() && std::isspace(static_cast<unsigned char>(text[open_at]))) open_at++;
    if (open_at >= text.size() || text[open_at] != '(') return std::nullopt;
    int depth = 0;
    size_t close_at = std::string::npos;
    for (size_t i = open_at; i < text.size(); i++) {
        if (text[i] == '(') depth++;
        else if (text[i] == ')' && --depth == 0) {
            close_at = i;
            break;
        }
    }
    if (close_at == std::string::npos) return std::nullopt;
    auto params = split_ffi_types(text.substr(open_at + 1, close_at - open_at - 1));
    std::string tail = trim_ffi_type(text.substr(close_at + 1));
    if (tail.empty()) return std::make_pair(params, std::string("none"));
    if (tail.rfind("->", 0) == 0) {
        std::string result = trim_ffi_type(tail.substr(2));
        if (!result.empty()) return std::make_pair(params, result);
    }
    return std::nullopt;
}

bool ffi_nested_ok(const std::string &ann, const std::set<std::string> &structs,
                   bool allow_none = false, bool allow_fn = true) {
    std::string a = trim_ffi_type(ann);
    if (a == "none") return allow_none;
    if (FFI_SLOTS.count(a) || structs.count(a)) return true;
    if (a.rfind("ptr[", 0) == 0 && a.size() >= 5 && a.back() == ']') {
        std::string inner = trim_ffi_type(a.substr(4, a.size() - 5));
        return !inner.empty() && ffi_nested_ok(inner, structs, false, false);
    }
    if (allow_fn && a.rfind("fn", 0) == 0) {
        auto parts = callback_parts(a);
        if (!parts) return false;
        for (const auto &param : parts->first)
            if (!ffi_nested_ok(param, structs, false, false)) return false;
        return ffi_nested_ok(parts->second, structs, true, false);
    }
    return false;
}

bool ffi_slot_ok(const std::optional<std::string> &ann, const std::set<std::string> &structs,
                 bool allow_array = false) {
    if (!ann || *ann == "none") return true;
    const std::string &a = *ann;
    if (allow_array && a.rfind("ptr[", 0) != 0 && a.find('[') != std::string::npos && !a.empty() && a.back() == ']') {
        auto lb = a.find('[');
        std::string elem = a.substr(0, lb);
        auto ef = elem.find_first_not_of(" \t");
        auto el = elem.find_last_not_of(" \t");
        elem = (ef == std::string::npos) ? "" : elem.substr(ef, el - ef + 1);
        std::string cnt = a.substr(lb + 1, a.size() - lb - 2);
        bool dig = !cnt.empty() && cnt.size() <= 7;
        for (char ch : cnt) if (!std::isdigit(static_cast<unsigned char>(ch))) dig = false;
        if (dig) {
            long long n = std::stoll(cnt);
            if (n >= 1 && n <= 1048576) return FFI_SLOTS.count(elem) > 0 || structs.count(elem) > 0;
        }
    }
    return ffi_nested_ok(a, structs, true);
}

void ffi_signatures(const std::vector<StmtP> &body, Errs &errs) {
    std::set<std::string> structs;
    for (const auto &sp : body) {
        const Stmt *node = pub_inner(*sp);
        if (node->k == SK::ExternStruct) structs.insert(node->a);
    }
    for (const auto &sp : body) {
        const Stmt *node = pub_inner(*sp);
        int line = node->line > 0 ? node->line : sp->line;
        if (node->k == SK::Extern) {
            const ExternDecl &e = node->ext;
            for (const Param &p : e.params) {
                if (ffi_slot_ok(p.ann, structs)) continue;
                errs.line = line;
                errs.note("unsupported FFI type `" + *p.ann + "` for parameter `" + p.name +
                          "` of extern fn `" + e.name + "` (hint: " + FFI_HINT + ")");
            }
            if (!ffi_slot_ok(e.ret, structs)) {
                errs.line = line;
                errs.note("unsupported FFI type `" + *e.ret + "` as the result of extern fn `" +
                          e.name + "` (hint: " + FFI_HINT + ")");
            }
            if (!ffi_slot_ok(e.varargs, structs)) {
                errs.line = line;
                errs.note("unsupported FFI type `" + *e.varargs +
                          "` in the variadic tail of extern fn `" + e.name + "` (hint: " +
                          FFI_HINT + ")");
            }
            // A C scalar is a copy: `move` on one transfers nothing, and the
            // ownership pass would chase an owned value that has no storage --
            // `-> move i64` was reported as "leaked owned pointer", a leak that
            // cannot exist. Refuse the declaration instead of inventing a leak.
            auto is_copy = [](const std::optional<std::string> &t) {
                if (!t) return false;
                static const std::set<std::string> k = {
                    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
                    "usize", "isize", "f32", "f64", "bool"};
                return k.count(*t) > 0;
            };
            for (const Param &p : e.params) {
                if (p.mode != Mode::Move || !is_copy(p.ann)) continue;
                errs.line = line;
                errs.note("`move` on `" + *p.ann + "` for parameter `" + p.name +
                          "` of extern fn `" + e.name +
                          "`: a scalar is copied, there is nothing to own (drop the mode)");
            }
            if (e.owned && is_copy(e.ret)) {
                errs.line = line;
                errs.note("`-> move` on `" + *e.ret + "` as the result of extern fn `" +
                          e.name + "`: a scalar is copied, there is nothing to own "
                          "(drop the mode)");
            }
        } else if (node->k == SK::ExternStruct) {
            for (const StructField &f : node->fields) {
                if (ffi_slot_ok(f.ann, structs, true)) continue;
                errs.line = line;
                errs.note("unsupported FFI type `" + *f.ann + "` for field `" + f.name +
                          "` of extern struct `" + node->a + "` (hint: " + FFI_HINT + ")");
            }
        }
    }
}

BorrowOut borrow_check(const std::vector<StmtP> &body,
                       const std::map<std::string, std::vector<ParamMode>> &extra,
                       const std::vector<std::pair<uint32_t, std::string>> &stray,
                       const std::set<std::string> &module_heads,
                       const std::set<std::string> &extra_consts,
                       const std::set<std::string> &extra_scalar_rets,
                       const std::map<std::string, std::vector<size_t>> &extra_borrows_rets,
                       const std::set<std::string> &extra_owned_rets,
                       const std::map<std::string, std::map<std::string, std::string>> &extra_struct_fields,
                       const std::map<std::string, std::string> &extra_fn_rets) {
    BorrowOut out;
    Errs errs;
    std::vector<BorrowDiag> warns;
    CUT_PARTS.clear();
    ffi_signatures(body, errs);

    FnTable funcs;
    for (const auto &kv : extra) funcs.sigs[kv.first] = kv.second;
    funcs.scalar_ret.insert(extra_scalar_rets.begin(), extra_scalar_rets.end());
    for (const auto &kv : extra_borrows_rets) funcs.borrows_ret[kv.first] = kv.second;
    funcs.owned_ret.insert(extra_owned_rets.begin(), extra_owned_rets.end());
    funcs.struct_fields = extra_struct_fields;
    funcs.fn_rets = extra_fn_rets;
    std::map<std::string, std::vector<std::string>> traits;
    for (const auto &sp : body) {
        const Stmt *node = pub_inner(*sp);
        if (node->k == SK::Struct || node->k == SK::ExternStruct) {
            for (const auto &f : node->fields) {
                if (f.ann) funcs.struct_fields[node->a][f.name] = *f.ann;
            }
        } else if ((node->k == SK::Fn || node->k == SK::ExportFn) && !node->fns.empty()) {
            const FnDecl &f = node->fns[0];
            if (f.name && f.ret) funcs.fn_rets[*f.name] = *f.ret;
        } else if (node->k == SK::Impl) {
            for (const FnDecl &f : node->fns) {
                if (f.name && f.ret) funcs.fn_rets[node->a + "." + *f.name] = *f.ret;
            }
        }
    }
    for (const auto &sp : body) {
        const Stmt *node = pub_inner(*sp);
        if (node->k == SK::Fn || node->k == SK::ExportFn) {
            const FnDecl &f = node->fns[0];
            funcs.sigs[f.name.value_or("")] = fn_modes(f.params);
        } else if (node->k == SK::Extern) {
            funcs.sigs[node->ext.name] = fn_modes(node->ext.params);
            auto bix = extern_borrows_indices(node->ext);
            if (!bix.empty()) funcs.borrows_ret[node->ext.name] = std::move(bix);
            // An owned result the declaration already refused must not also
            // become a phantom leak.
            {
                static const std::set<std::string> copies = {
                    "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64",
                    "usize", "isize", "f32", "f64", "bool"};
                if (node->ext.ret && copies.count(*node->ext.ret))
                    funcs.scalar_ret.insert(node->ext.name);
            }
            if (node->ext.owned) {
                static const std::set<std::string> copies = {
                    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
                    "usize", "isize", "f32", "f64", "bool"};
                if (!(node->ext.ret && copies.count(*node->ext.ret)))
                    funcs.owned_ret.insert(node->ext.name);
            }
        } else if (node->k == SK::Enum) {
            auto &ef = funcs.struct_fields["__enum__:" + node->a];
            size_t slot = 0;
            for (const Variant &v : node->variants) {
                funcs.sigs[v.name] = {};
                if (v.name != "Ok" && v.name != "Err" && v.name != "Some" && v.name != "None")
                    funcs.fn_rets[v.name] = node->a;
                for (size_t fi = 0; fi < v.fields.size(); fi++) {
                    std::string fann = (fi < v.field_anns.size() && v.field_anns[fi] &&
                                        !v.field_anns[fi]->empty())
                                           ? *v.field_anns[fi]
                                           : "dyn";
                    ef["f" + std::to_string(slot++)] = std::move(fann);
                }
            }
        } else if (node->k == SK::Trait) {
            std::vector<std::string> names;
            for (const Sig &g : node->sigs) names.push_back(g.name);
            traits[node->a] = names;
        }
    }
    collect_heads(body, funcs);
    funcs.heads.insert(module_heads.begin(), module_heads.end());

    // A method name the builtin mutators already own cannot be spelled without
    // `mut self`: the receiver would be written through silently.
    for (const auto &sp : body) {
        const Stmt *node = pub_inner(*sp);
        if (node->k != SK::Impl) continue;
        for (const FnDecl &f : node->fns) {
            bool is_mut = !f.params.empty() && f.params[0].name == "self" &&
                          f.params[0].mode == Mode::Mut;
            std::string name = f.name.value_or("");
            if (MUTATORS.count(name) && !is_mut) {
                errs.line = node->line > 0 ? node->line : sp->line;
                errs.note("method name " + name + " is reserved (built-in mutator)");
            }
        }
    }
    for (const auto &sp : body) {
        const Stmt *node = pub_inner(*sp);
        if (node->k != SK::Impl) continue;
        if (node->opt1 && !traits.count(*node->opt1)) {
            errs.line = node->line > 0 ? node->line : sp->line;
            errs.note("unknown trait " + *node->opt1 + " (declare it with `trait " + *node->opt1 +
                      " { ... }`)");
            continue;
        }
        std::vector<std::string> have;
        for (const FnDecl &f : node->fns) have.push_back(f.name.value_or(""));
        if (node->opt1) {
            std::vector<std::string> missing;
            for (const std::string &m : traits[*node->opt1]) {
                if (std::find(have.begin(), have.end(), m) == have.end()) missing.push_back(m);
            }
            if (!missing.empty()) {
                errs.line = node->line > 0 ? node->line : sp->line;
                for (const std::string &m : missing) {
                    errs.note("impl " + *node->opt1 + " for " + node->a + " missing " + m);
                }
            }
        }
    }

    cut_body(wrap(body), warns);
    Decls decl;
    for (const std::string &cn : extra_consts) {
        Decl d;
        d.konst = true;
        d.copy = true;
        d.pinned = true;
        decl.set(cn, std::move(d));
    }
    for (const auto &sp : body) {
        const Stmt *node = pub_inner(*sp);
        if ((node->k == SK::Let || node->k == SK::LetTmp) && node->konst) {
            Decl d;
            d.konst = true;
            d.copy = true;
            d.pinned = true;
            d.ann = node->ann;
            d.dl = node->line;
            decl.set(node->a, std::move(d));
        }
    }
    Borrows borrows;
    register_nested(body, funcs);
    walk_block(wrap(body), decl, borrows, errs, funcs);
    unused_in(wrap(body), warns);
    for (const auto &[ln, text] : stray) {
        warns.push_back(BorrowDiag{static_cast<int>(ln),
                                   "placeholder `{" + text +
                                       "}` is not an expression and stays text: check for a typo",
                                   std::nullopt});
    }
    for (const auto &sp : body) {
        const Stmt *node = pub_inner(*sp);
        if (node->k == SK::Fn || node->k == SK::ExportFn) {
            unused_in(wrap(node->fns[0].body), warns);
        } else if (node->k == SK::Impl) {
            for (const FnDecl &f : node->fns) unused_in(wrap(f.body), warns);
        }
    }

    // Sharing: cycles (a refcount cannot reclaim them) and writes that escape a
    // spawn. Same order as the reference: functions first, then the script body.
    for (const BorrowDiag &d : sharing_check(body, funcs.struct_fields)) errs.v.push_back(d);
    // Channels: a straight-line pipe whose ends do not match cannot finish.
    for (const BorrowDiag &d : session_check(body)) errs.v.push_back(d);
    // A `par for` is only parallel-safe when its operation and body are pure.
    for (const BorrowDiag &d : parfor_rules(body, funcs)) errs.v.push_back(d);
    // Plugins: a symbol or a pointer belongs to the library that made it, so a
    // `lib_close` ends it -- and the literal signature in `call` is compared
    // with the `extern fn` of the same name instead of being believed.
    for (const BorrowDiag &d : plugin_check(body)) errs.v.push_back(d);
    // Capability lattice: what each place still allows, joined at every branch.
    // A line the lexical walk already judged is left alone, so these verdicts
    // only ever add what a list of live borrows cannot see.
    for (const BorrowDiag &d : cap_verdicts(body, funcs.sigs, funcs.struct_fields, funcs.fn_rets)) {
        bool seen = false;
        for (const BorrowDiag &e : errs.v) {
            if (e.line == d.line) {
                seen = true;
                break;
            }
        }
        if (!seen) errs.v.push_back(d);
    }

    for (const BorrowDiag &d : errs.v) {
        if (std::find(out.errs.begin(), out.errs.end(), d) == out.errs.end()) out.errs.push_back(d);
    }
    for (const BorrowDiag &d : warns) {
        if (std::find(out.warns.begin(), out.warns.end(), d) == out.warns.end()) {
            out.warns.push_back(d);
        }
    }
    return out;
}
