#include "plugin.h"

#include <map>
#include <optional>
#include <set>

namespace {

using View = std::vector<const Stmt *>;

const std::string kOpen = "lib_open";
const std::string kSym = "lib_sym";
const std::string kClose = "lib_close";
const std::string kCall = "call";

std::string dead(const std::string &value, const std::string &lib, int line) {
    return "`" + value + "` is from library `" + lib + "`, closed on line " +
           std::to_string(line) +
           " (hint: close it after the last call, or open the library again)";
}

std::string closed_value(const std::string &name, int line) {
    return "`" + name + "` is closed on line " + std::to_string(line) +
           " (hint: open the library again)";
}

std::string twice(const std::string &name, int line) {
    return "`" + name + "` is closed twice (the first close is on line " +
           std::to_string(line) + ")";
}

std::string arity(const std::string &name, size_t want, size_t got) {
    return "call: `" + name + "` is declared to take " + std::to_string(want) +
           " argument(s), the signature has " + std::to_string(got);
}

std::string param(const std::string &name, size_t i, const std::string &want,
                  const std::string &got) {
    return "call: `" + name + "` parameter " + std::to_string(i) + " is " + want +
           ", the signature says " + got;
}

std::string ret(const std::string &name, const std::string &want,
                const std::string &got) {
    return "call: `" + name + "` returns " + want + ", the signature says " + got;
}

// BRAT annotation -> the C type the call signature must name. `str` crosses the
// boundary as `ptr`, and every integer width crosses as `i64`, so a mismatch in
// the number of bytes is not silently blessed.
std::string c_type(const std::optional<std::string> &ann) {
    const std::string t = ann ? *ann : "";
    if (t == "none") return "none";
    if (t == "str" || t == "ptr") return "ptr";
    if (t == "f32" || t == "f64") return "f64";
    if (t == "i8" || t == "i16" || t == "i32" || t == "i64" || t == "u8" ||
        t == "u16" || t == "u32" || t == "u64" || t == "isize" || t == "usize" ||
        t == "bool")
        return "i64";
    return t;
}

struct Sig {
    std::string ret;
    std::vector<std::string> params;
};

std::optional<Sig> parse_sig(const std::string &text) {
    std::string t;
    for (char c : text)
        if (c != ' ') t.push_back(c);
    const size_t open = t.find('(');
    if (open == std::string::npos || t.empty() || t.back() != ')') return std::nullopt;
    Sig s;
    s.ret = t.substr(0, open);
    const std::string rest = t.substr(open + 1, t.size() - open - 2);
    size_t at = 0;
    while (at <= rest.size()) {
        const size_t comma = rest.find(',', at);
        const std::string piece =
            rest.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!piece.empty()) s.params.push_back(piece);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return s;
}

struct Declared {
    std::vector<std::string> params;
    std::string ret;
};

const std::set<SK> kDecls{SK::Fn, SK::Impl, SK::Trait, SK::Enum, SK::Extern,
                          SK::ExternStruct, SK::ExportFn};

struct State {
    int next_id = 0;
    std::map<std::string, int> handle;         // binding -> library id
    std::map<int, int> closed;                 // library id -> line of the close
    std::map<std::string, int> tied;           // binding -> library id
    std::map<std::string, std::string> sym;    // binding -> declared name
};

View view(const std::vector<StmtP> &body) {
    View out;
    out.reserve(body.size());
    for (const auto &s : body) out.push_back(s.get());
    return out;
}

View else_of(const Stmt &s) {
    if (!s.els) return {};
    if (s.els->k == SK::Block) return view(s.els->body);
    return {s.els.get()};
}

std::string lib_name(const State &st, int id) {
    for (const auto &entry : st.handle)
        if (entry.second == id) return entry.first;
    return "?";
}

bool is_var(const Expr *e) { return e && e->k == EK::Var; }

void bind(const std::string &name, const Expr *expr, State &st) {
    if (!expr || expr->k != EK::Call) return;
    const std::string &callee = expr->a;
    const std::vector<ExprP> &args = expr->kids;
    if (callee == kOpen) {
        st.handle[name] = st.next_id++;
        return;
    }
    if (callee == kSym && args.size() == 2 && is_var(args[0].get())) {
        auto h = st.handle.find(args[0]->a);
        if (h != st.handle.end()) {
            st.tied[name] = h->second;
            if (args[1] && args[1]->k == EK::Str) st.sym[name] = args[1]->a;
        }
        return;
    }
    if (callee == kCall && args.size() >= 2 && is_var(args[0].get())) {
        auto t = st.tied.find(args[0]->a);
        if (t == st.tied.end()) return;
        if (!args[1] || args[1]->k != EK::Str) return;
        auto sig = parse_sig(args[1]->a);
        if (sig && sig->ret == "ptr") st.tied[name] = t->second;
    }
}

void body_check(const View &body, const std::map<std::string, Declared> &decls,
                std::vector<BorrowDiag> &out, State &st, int hint);

void dead_use(const std::string &var, std::vector<BorrowDiag> &out, const State &st,
              int line) {
    auto t = st.tied.find(var);
    if (t == st.tied.end()) return;
    auto c = st.closed.find(t->second);
    if (c == st.closed.end()) return;
    out.push_back(BorrowDiag{line, dead(var, lib_name(st, t->second), c->second),
                             std::nullopt});
}

void plugin_call(const std::vector<ExprP> &args,
                 const std::map<std::string, Declared> &decls,
                 std::vector<BorrowDiag> &out, State &st, int line) {
    if (args.size() < 2 || !is_var(args[0].get())) return;
    const std::string &var = args[0]->a;
    auto t = st.tied.find(var);
    if (t == st.tied.end()) return;
    auto c = st.closed.find(t->second);
    if (c != st.closed.end()) {
        out.push_back(BorrowDiag{line, dead(var, lib_name(st, t->second), c->second),
                                 std::nullopt});
        return;
    }
    if (!args[1] || args[1]->k != EK::Str) return;
    auto known = st.sym.find(var);
    if (known == st.sym.end()) return;
    auto declared = decls.find(known->second);
    auto sig = parse_sig(args[1]->a);
    if (declared == decls.end() || !sig) return;
    const Declared &want = declared->second;
    if (sig->params.size() != want.params.size()) {
        out.push_back(BorrowDiag{
            line, arity(known->second, want.params.size(), sig->params.size()),
            std::nullopt});
        return;
    }
    for (size_t i = 0; i < sig->params.size(); i++) {
        const std::string got = c_type(sig->params[i]);
        if (got != want.params[i]) {
            out.push_back(BorrowDiag{
                line, param(known->second, i + 1, want.params[i], got), std::nullopt});
            return;
        }
    }
    const std::string got = c_type(sig->ret);
    if (got != want.ret)
        out.push_back(BorrowDiag{line, ret(known->second, want.ret, got), std::nullopt});
}

void expr_check(const Expr *e, const std::map<std::string, Declared> &decls,
                std::vector<BorrowDiag> &out, State &st, int line) {
    if (!e) return;
    if (e->k == EK::Var) {
        dead_use(e->a, out, st, line);
        return;
    }
    if (e->k != EK::Call) {
        for (const auto &kid : e->kids) expr_check(kid.get(), decls, out, st, line);
        return;
    }
    const std::string &callee = e->a;
    const std::vector<ExprP> &args = e->kids;
    if (callee == kClose) {
        if (args.size() == 1 && is_var(args[0].get())) {
            auto h = st.handle.find(args[0]->a);
            if (h != st.handle.end()) {
                auto c = st.closed.find(h->second);
                if (c != st.closed.end())
                    out.push_back(BorrowDiag{line, twice(args[0]->a, c->second),
                                             std::nullopt});
                else
                    st.closed[h->second] = line;
            }
        }
        for (const auto &a : args) expr_check(a.get(), decls, out, st, line);
        return;
    }
    if (callee == kSym) {
        // A handle that was closed cannot produce a symbol.
        if (!args.empty() && is_var(args[0].get())) {
            auto h = st.handle.find(args[0]->a);
            if (h != st.handle.end()) {
                auto c = st.closed.find(h->second);
                if (c != st.closed.end())
                    out.push_back(BorrowDiag{line, closed_value(args[0]->a, c->second),
                                             std::nullopt});
            }
        }
        for (const auto &a : args) expr_check(a.get(), decls, out, st, line);
        return;
    }
    if (callee == kCall && args.size() >= 2) {
        plugin_call(args, decls, out, st, line);
        for (size_t i = 2; i < args.size(); i++)
            expr_check(args[i].get(), decls, out, st, line);
        return;
    }
    for (const auto &a : args) expr_check(a.get(), decls, out, st, line);
}

void body_check(const View &body, const std::map<std::string, Declared> &decls,
                std::vector<BorrowDiag> &out, State &st, int hint) {
    for (const Stmt *s : body) {
        if (!s) continue;
        const int line = s->line ? s->line : hint;
        switch (s->k) {
        case SK::Let:
        case SK::LetTmp:
            expr_check(s->e1.get(), decls, out, st, line);
            bind(s->a, s->e1.get(), st);
            break;
        case SK::Set:
        case SK::SetField:
            expr_check(s->e2.get(), decls, out, st, line);
            break;
        case SK::SetIdx:
            expr_check(s->e1.get(), decls, out, st, line);
            expr_check(s->e2.get(), decls, out, st, line);
            expr_check(s->e3.get(), decls, out, st, line);
            break;
        case SK::Expr:
        case SK::Return:
        case SK::Defer:
            expr_check(s->e1.get(), decls, out, st, line);
            break;
        case SK::If:
            expr_check(s->e1.get(), decls, out, st, line);
            body_check(view(s->body), decls, out, st, line);
            body_check(else_of(*s), decls, out, st, line);
            break;
        case SK::While:
            expr_check(s->e1.get(), decls, out, st, line);
            body_check(view(s->body), decls, out, st, line);
            break;
        case SK::ForRange:
            expr_check(s->e1.get(), decls, out, st, line);
            expr_check(s->e2.get(), decls, out, st, line);
            body_check(view(s->body), decls, out, st, line);
            break;
        case SK::ForIter:
            expr_check(s->e1.get(), decls, out, st, line);
            body_check(view(s->body), decls, out, st, line);
            break;
        case SK::Spawn:
        case SK::Scope:
        case SK::Unsafe:
        case SK::Block:
            body_check(view(s->body), decls, out, st, line);
            break;
        case SK::Try:
            body_check(view(s->body), decls, out, st, line);
            body_check(view(s->body2), decls, out, st, line);
            break;
        default:
            break;
        }
    }
}

const Stmt *inner(const Stmt &s) {
    if (s.k == SK::Pub && !s.body.empty()) return s.body[0].get();
    return &s;
}

}  // namespace

std::vector<BorrowDiag> plugin_check(const std::vector<StmtP> &body) {
    std::vector<BorrowDiag> out;
    std::map<std::string, Declared> decls;
    std::vector<View> bodies;
    View top;
    for (const auto &sp : body) {
        const Stmt *node = inner(*sp);
        if (node->k == SK::Extern) {
            Declared d;
            for (const Param &p : node->ext.params) d.params.push_back(c_type(p.ann));
            d.ret = c_type(node->ext.ret);
            decls[node->ext.name] = d;
            continue;
        }
        if (node->k == SK::ExportFn) {
            if (node->fns.empty()) continue;
            Declared d;
            const FnDecl &f = node->fns[0];
            for (const Param &p : f.params) d.params.push_back(c_type(p.ann));
            d.ret = c_type(f.ret);
            decls[f.name ? *f.name : node->a] = d;
            continue;
        }
        if (node->k == SK::Fn) {
            if (!node->fns.empty()) bodies.push_back(view(node->fns[0].body));
            continue;
        }
        if (node->k == SK::Impl) {
            for (const FnDecl &f : node->fns) bodies.push_back(view(f.body));
            continue;
        }
        if (kDecls.count(node->k)) continue;
        top.push_back(node);
    }
    bodies.push_back(top);   // a script is one body, like `main`
    for (const View &b : bodies) {
        State st;
        body_check(b, decls, out, st, 0);
    }
    return out;
}
