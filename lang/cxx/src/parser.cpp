#include "parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <unordered_map>
#include <unordered_set>

#include <map>

#include "lexer.h"

namespace {

bool is_name(const Tok &t) { return t.kind == TK::Id || t.kind == TK::Kw; }

std::string to_str(std::string_view v) { return std::string(v); }

std::string join(const std::vector<std::string> &v) {
    std::string out;
    for (const auto &s : v) out += s;
    return out;
}

// Drops a leading ownership word and glues the rest: `mut vec[i64]` -> `vec[i64]`.
std::optional<std::string> ann_text(std::vector<std::string> toks) {
    if (!toks.empty() && (toks[0] == "move" || toks[0] == "mut" || toks[0] == "ref")) {
        toks.erase(toks.begin());
    }
    if (toks.empty()) return std::nullopt;
    return join(toks);
}

// C spellings for a slot whose width BRAT already names, so a signature copied
// out of a header is accepted as written instead of being refused for its
// spelling. Only names where both tables agree on the width are here: a
// `int16_t` is deliberately absent, because `brat bindgen` widens it to `i32`
// and a hand-written `i16` would then disagree with a generated one.
const std::map<std::string, std::string> C_FFI_ALIAS = {
    {"size_t", "usize"},
    {"ssize_t", "i64"}, {"ptrdiff_t", "i64"}, {"intptr_t", "i64"}, {"uintptr_t", "u64"},
    {"int64_t", "i64"}, {"uint64_t", "u64"}, {"int32_t", "i32"}, {"uint32_t", "u32"},
};

std::optional<std::string> canon_ann(std::optional<std::string> ann) {
    if (!ann) return ann;
    auto it = C_FFI_ALIAS.find(*ann);
    if (it == C_FFI_ALIAS.end()) return ann;
    return it->second;
}

// Names the C emitter spells into generated C as written (an `extern` symbol is
// the library's symbol), so they have to be identifiers C can spell too.
const std::unordered_set<std::string> C_RESERVED = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if", "int",
    "long", "register", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "typedef", "union", "unsigned", "void", "volatile",
    "while",
};

bool is_c_ident(std::string_view s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                        (i > 0 && c >= '0' && c <= '9');
        if (!ok) return false;
    }
    return true;
}

std::string unquoted(std::string_view s) {
    while (!s.empty() && (s.front() == '"' || s.front() == ' ')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == '"' || s.back() == ' ')) s.remove_suffix(1);
    return std::string(s);
}

std::string unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out.push_back(s[i]);
            continue;
        }
        char c = s[++i];
        switch (c) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        case '\'': out.push_back('\''); break;
        case '0': out.push_back('\0'); break;
        default: out.push_back('\\'); out.push_back(c);
        }
    }
    return out;
}

std::string lower_ascii(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

}  // namespace

namespace {

// A node is owned exactly once; where the grammar needs the same expression
// twice (`v[i] += x` reads and writes `v[i]`), it is copied here and nowhere
// else.
ExprP clone_expr(const Expr &e) {
    auto c = std::make_unique<Expr>();
    c->k = e.k;
    c->i = e.i;
    c->f = e.f;
    c->is_float = e.is_float;
    c->flag = e.flag;
    c->a = e.a;
    c->b = e.b;
    c->names = e.names;
    for (const auto &k : e.kids) c->kids.push_back(clone_expr(*k));
    if (e.fn) {
        auto f = std::make_unique<FnDecl>();
        f->name = e.fn->name;
        f->tparams = e.fn->tparams;
        f->ret = e.fn->ret;
        for (const auto &p : e.fn->params) {
            Param np;
            np.name = p.name;
            np.mode = p.mode;
            np.ann = p.ann;
            if (p.def) np.def = clone_expr(*p.def);
            f->params.push_back(std::move(np));
        }
        c->fn = std::move(f);
    }
    return c;
}

struct Arm {
    std::optional<Pat> pat;
    ExprP guard;
    std::vector<StmtP> body;
};

StmtP clone_stmt(const Stmt &s);

PatP clone_pat(const Pat &p) {
    auto c = std::make_unique<Pat>();
    c->k = p.k;
    c->a = p.a;
    if (p.lit) c->lit = clone_expr(*p.lit);
    for (const auto &sub : p.subs) c->subs.push_back(clone_pat(*sub));
    return c;
}

StmtP clone_stmt(const Stmt &s) {
    auto c = std::make_unique<Stmt>();
    c->k = s.k;
    c->line = s.line;
    c->a = s.a;
    c->ann = s.ann;
    c->mut = s.mut;
    c->konst = s.konst;
    if (s.e1) c->e1 = clone_expr(*s.e1);
    if (s.e2) c->e2 = clone_expr(*s.e2);
    if (s.e3) c->e3 = clone_expr(*s.e3);
    for (const auto &b : s.body) c->body.push_back(clone_stmt(*b));
    for (const auto &b : s.body2) c->body2.push_back(clone_stmt(*b));
    if (s.els) c->els = clone_stmt(*s.els);
    for (const auto &p : s.params) {
        Param np;
        np.name = p.name;
        np.mode = p.mode;
        np.ann = p.ann;
        if (p.def) np.def = clone_expr(*p.def);
        c->params.push_back(std::move(np));
    }
    c->names = s.names;
    c->variants = s.variants;
    c->sigs = s.sigs;
    c->fields = s.fields;
    c->opt1 = s.opt1;
    c->ext.lib = s.ext.lib;
    c->ext.name = s.ext.name;
    c->ext.ret = s.ext.ret;
    c->ext.varargs = s.ext.varargs;
    c->ext.owned = s.ext.owned;
    c->ext.checked = s.ext.checked;
    for (const auto &f : s.fns) {
        FnDecl nf;
        nf.name = f.name;
        nf.tparams = f.tparams;
        nf.ret = f.ret;
        for (const auto &p : f.params) {
            Param np;
            np.name = p.name;
            np.mode = p.mode;
            np.ann = p.ann;
            if (p.def) np.def = clone_expr(*p.def);
            nf.params.push_back(std::move(np));
        }
        for (const auto &b : f.body) nf.body.push_back(clone_stmt(*b));
        c->fns.push_back(std::move(nf));
    }
    for (const auto &p : s.ext.params) {
        Param np;
        np.name = p.name;
        np.mode = p.mode;
        np.ann = p.ann;
        if (p.def) np.def = clone_expr(*p.def);
        c->ext.params.push_back(std::move(np));
    }
    return c;
}

class Parser {
public:
    explicit Parser(std::string_view src) : t_(lex(src)) {}

    // One frame of the iterative tree walk: exactly one of the three pointers
    // is set, and `d` is the depth of that node from the statement's root.
    struct DepthNode {
        const Stmt *s = nullptr;
        const Expr *e = nullptr;
        const FnDecl *f = nullptr;
        size_t d = 0;
    };

    static void push_expr(std::vector<DepthNode> &stack, const Expr &e, size_t d) {
        for (const auto &kid : e.kids)
            if (kid) stack.push_back({nullptr, kid.get(), nullptr, d + 1});
        if (e.fn) stack.push_back({nullptr, nullptr, e.fn.get(), d + 1});
    }

    static void push_stmt(std::vector<DepthNode> &stack, const Stmt &s, size_t d) {
        for (const ExprP *e : {&s.e1, &s.e2, &s.e3})
            if (*e) stack.push_back({nullptr, e->get(), nullptr, d + 1});
        if (s.els) stack.push_back({s.els.get(), nullptr, nullptr, d + 1});
        for (const auto &b : s.body)
            if (b) stack.push_back({b.get(), nullptr, nullptr, d + 1});
        for (const auto &b : s.body2)
            if (b) stack.push_back({b.get(), nullptr, nullptr, d + 1});
        for (const auto &p : s.params)
            if (p.def) stack.push_back({nullptr, p.def.get(), nullptr, d + 1});
        for (const auto &f : s.fns) push_fn(stack, f, d + 1);
    }

    static void push_fn(std::vector<DepthNode> &stack, const FnDecl &f, size_t d) {
        for (const auto &p : f.params)
            if (p.def) stack.push_back({nullptr, p.def.get(), nullptr, d + 1});
        for (const auto &b : f.body)
            if (b) stack.push_back({b.get(), nullptr, nullptr, d + 1});
    }

    static size_t tree_depth(const Stmt &root) {
        size_t deepest = 0;
        std::vector<DepthNode> stack{{&root, nullptr, nullptr, 1}};
        while (!stack.empty()) {
            DepthNode n = stack.back();
            stack.pop_back();
            if (n.d > deepest) deepest = n.d;
            if (n.s) push_stmt(stack, *n.s, n.d);
            else if (n.e) push_expr(stack, *n.e, n.d);
            else if (n.f) push_fn(stack, *n.f, n.d);
        }
        return deepest;
    }

    void refuse_deep_tree(const std::vector<StmtP> &body) {
        // One statement at a time, so the refusal can name the line the deepest
        // chain starts on -- which is the line a reader needs and the only one
        // a pass downstream would have reported.
        size_t worst = 0;
        uint32_t worst_line = 1;
        for (const auto &s : body) {
            if (!s) continue;
            size_t d = tree_depth(*s);
            if (d > worst) {
                worst = d;
                worst_line = s->line ? static_cast<uint32_t>(s->line) : 1u;
            }
        }
        if (worst > kMaxTreeDepth) {
            fail("expression tree too deep (" + std::to_string(worst) + " levels, limit " +
                     std::to_string(kMaxTreeDepth) + ")",
                 worst_line);
        }
    }

    ParseOut run() {
        std::vector<StmtP> body;
        while (peek().kind != TK::Eof) {
            for (auto &s : stmt()) body.push_back(std::move(s));
        }
        refuse_deep_tree(body);
        return ParseOut{std::move(body), std::move(stray_)};
    }

private:
    std::vector<Tok> t_;
    size_t i_ = 0;
    uint32_t seq_ = 0;
    uint32_t curline_ = 1;
    std::unordered_map<std::string, std::vector<std::string>> enums_;
    std::unordered_map<std::string, std::optional<std::string>> enums_rev_;
    std::vector<std::pair<uint32_t, std::string>> stray_;

    const Tok &peek() const { return t_[i_]; }
    const Tok &peek(size_t k) const { return i_ + k < t_.size() ? t_[i_ + k] : t_.back(); }
    bool at(std::string_view v) const { return peek().text == v; }
    bool next_is(std::string_view v) const { return peek(1).text == v; }
    uint32_t line() const { return peek().line; }

    [[noreturn]] void fail(std::string msg, uint32_t ln) const { throw ParseError(std::move(msg), ln); }
    [[noreturn]] void fail(std::string msg) const { fail(std::move(msg), line()); }

    // A struct name and a field name are written as `bf_<name>`, so only a name
    // the BRAT lexer refuses matters there. A function name is the C symbol
    // itself, so a C keyword cannot be spelled and is refused as well.
    std::string c_ident(const char *what, bool c_kw = false) {
        const Tok &t = peek();
        if (t.kind != TK::Id || (c_kw && C_RESERVED.count(std::string(t.text))))
            fail(std::string(what) + " wants a plain name, got '" + std::string(t.text) + "'",
                 t.line);
        return to_str(step().text);
    }

    Tok step() {
        const Tok &c = peek();
        if (c.kind == TK::Eof) fail("unexpected end of file", c.line);
        return t_[i_++];
    }

    Tok want(std::string_view v) {
        const Tok &c = peek();
        if (c.text != v) {
            if (c.kind == TK::Eof) fail("unexpected end of file", c.line);
            fail("want '" + std::string(v) + "' have '" + to_str(c.text) + "'", c.line);
        }
        return step();
    }

    Tok want_id(const char *what) {
        const Tok &c = peek();
        if (!is_name(c)) {
            if (c.kind == TK::Eof) fail("unexpected end of file", c.line);
            fail(std::string("want ") + what + " (identifier) have '" + to_str(c.text) + "'", c.line);
        }
        return step();
    }

    bool eat(std::string_view v) {
        if (!at(v)) return false;
        step();
        return true;
    }

    std::string fresh(char tag) { return std::string("@") + tag + std::to_string(++seq_); }

    // A vector of one statement: a braced-init-list would try to copy the
    // unique_ptr, so the one-element list is built by hand.
    static std::vector<StmtP> one(StmtP s) {
        std::vector<StmtP> v;
        v.push_back(std::move(s));
        return v;
    }

    static const char *compound_op(std::string_view s) {
        if (s == "+=") return "+";
        if (s == "-=") return "-";
        if (s == "*=") return "*";
        if (s == "/=") return "/";
        if (s == "%=") return "%";
        return nullptr;
    }


    // The reference stamps every node with the line its statement started on,
    // innermost first, so an error inside a nested block keeps the inner line.
    static void stamp(Stmt &s, uint32_t ln) {
        if (!s.line) s.line = static_cast<int>(ln);
        for (auto &b : s.body) stamp(*b, ln);
        for (auto &b : s.body2) stamp(*b, ln);
        if (s.els) stamp(*s.els, ln);
        for (auto &f : s.fns)
            for (auto &b : f.body) stamp(*b, ln);
    }

    std::vector<StmtP> stmt() {
        // The line is a local: parsing the body overwrites the member, and a
        // `for` would then be stamped with the line of its last statement.
        uint32_t ln = peek().line;
        curline_ = ln;
        std::vector<StmtP> v = stmt_inner();
        for (auto &s : v) stamp(*s, ln);
        return v;
    }

    std::vector<StmtP> block() {
        // `if { if { if { … } } }` recurses through the statement parser, so the
        // same guard as expressions: refuse the shape instead of blowing the
        // stack, which no `catch` would see.
        Depth guard(*this, "block");
        std::vector<StmtP> out;
        while (!at("}")) {
            for (auto &s : stmt()) out.push_back(std::move(s));
        }
        return out;
    }

    template <class... Args>
    static StmtP make(SK k, Args &&...args) {
        auto s = std::make_unique<Stmt>();
        s->k = k;
        (void)k;
        return s;
    }

    std::vector<StmtP> stmt_inner() {
        const Tok c = peek();

        if (c.kind == TK::Id && c.text == "arena" && next_is("{")) return one(arena());

        if (c.kind == TK::Kw) {
            const std::string kw = to_str(c.text);
            if (kw == "let") return let_stmt(false);
            if (kw == "const") { step(); return let_stmt(true); }
            if (kw == "extern") { step(); return extern_block(); }
            if (kw == "export") {
                step();
                if (!(peek().kind == TK::Kw && at("fn"))) fail("export wants fn");
                auto s = std::make_unique<Stmt>();
                s->k = SK::ExportFn;
                s->fns.push_back(fn_decl());
                const std::string exname = s->fns[0].name.value_or("");
                if (!is_c_ident(exname) || C_RESERVED.count(exname))
                    fail(std::string("export fn wants a plain name, got '") + exname + "'");
                return one(std::move(s));
            }
            if (kw == "fn") {
                auto s = std::make_unique<Stmt>();
                s->k = SK::Fn;
                s->fns.push_back(fn_decl());
                return one(std::move(s));
            }
            if (kw == "unsafe") {
                step();
                want("{");
                auto s = std::make_unique<Stmt>();
                s->k = SK::Unsafe;
                s->body = block();
                want("}");
                return one(std::move(s));
            }
            if (kw == "if") return if_stmt();
            if (kw == "while") return while_stmt();
            if (kw == "for") return for_stmt();
            if (kw == "impl") return one(impl_stmt());
            if (kw == "trait") return one(trait_stmt());
            if (kw == "enum") return one(enum_stmt());
            if (kw == "pub") return one(pub_stmt());
            if (kw == "use") fail("use wants pub");
            if (kw == "break" || kw == "continue") {
                step();
                eat(";");
                auto s = std::make_unique<Stmt>();
                s->k = (kw == "break") ? SK::Break : SK::Continue;
                return one(std::move(s));
            }
            if (kw == "match") return match_stmt();
            if (kw == "return") {
                step();
                auto s = std::make_unique<Stmt>();
                s->k = SK::Return;
                if (at("}") || at(";")) {
                    eat(";");
                    s->e1 = mk_none();
                } else {
                    s->e1 = expr();
                    eat(";");
                }
                return one(std::move(s));
            }
            if (kw == "try") return one(try_stmt());
            if (kw == "spawn") {
                step();
                want("{");
                auto s = std::make_unique<Stmt>();
                s->k = SK::Spawn;
                s->body = block();
                want("}");
                return one(std::move(s));
            }
            if (kw == "defer") {
                step();
                auto s = std::make_unique<Stmt>();
                s->k = SK::Defer;
                s->e1 = expr();
                eat(";");
                return one(std::move(s));
            }
            if (kw == "import") {
                step();
                auto s = std::make_unique<Stmt>();
                s->k = SK::Import;
                s->a = dotted_path();
                return one(std::move(s));
            }
        }

        // `scope { }` is a block with a join, not a keyword: the name stays
        // usable as an ordinary binding.
        if (c.kind == TK::Id && c.text == "scope" && next_is("{")) {
            step();
            step();
            auto s = std::make_unique<Stmt>();
            s->k = SK::Scope;
            s->body = block();
            want("}");
            return one(std::move(s));
        }

        if (c.kind == TK::Id && next_is("=")) {
            std::string name = to_str(step().text);
            step();
            auto s = std::make_unique<Stmt>();
            s->k = SK::Set;
            s->a = std::move(name);
            s->e1 = expr();
            eat(";");
            return one(std::move(s));
        }
        if (c.kind == TK::Id && compound_op(peek(1).text)) {
            std::string name = to_str(step().text);
            const char *op = compound_op(peek().text);
            step();
            auto s = std::make_unique<Stmt>();
            s->k = SK::Set;
            s->a = name;
            s->e1 = mk_bin(op, mk_var(name), expr());
            eat(";");
            return one(std::move(s));
        }

        auto e = expr();
        const char *op = at("=") ? "=" : compound_op(peek().text);
        if (op) {
            step();
            auto val = expr();
            eat(";");
            const bool plain = op[0] == '=' && op[1] == 0;
            if (e->k == EK::Idx) {
                auto s = std::make_unique<Stmt>();
                s->k = SK::SetIdx;
                auto lhs = mk_idx(clone_expr(*e->kids[0]), clone_expr(*e->kids[1]));
                s->e1 = std::move(e->kids[0]);
                s->e2 = std::move(e->kids[1]);
                s->e3 = plain ? std::move(val) : mk_bin(op, std::move(lhs), std::move(val));
                return one(std::move(s));
            }
            if (e->k == EK::Field) {
                auto s = std::make_unique<Stmt>();
                s->k = SK::SetField;
                auto lhs = mk_field(clone_expr(*e->kids[0]), e->b);
                s->a = e->b;
                s->e1 = std::move(e->kids[0]);
                s->e2 = plain ? std::move(val) : mk_bin(op, std::move(lhs), std::move(val));
                return one(std::move(s));
            }
            fail("bad assign target");
        }
        eat(";");
        auto s = std::make_unique<Stmt>();
        s->k = SK::Expr;
        s->e1 = std::move(e);
        return one(std::move(s));
    }

    std::string dotted_path() {
        std::string out = to_str(step().text);
        while (at(".")) {
            step();
            out += ".";
            out += to_str(step().text);
        }
        eat(";");
        return out;
    }

    std::vector<StmtP> let_stmt(bool konst) {
        if (!konst) want("let");
        bool mut = eat("mut");
        if (konst && mut) fail("const cannot be mut");
        if (at("[")) {
            if (konst) fail("const destructure unsupported");
            step();
            std::vector<std::string> names;
            while (!at("]")) {
                names.push_back(to_str(step().text));
                eat(",");
            }
            want("]");
            want("=");
            auto e = expr();
            eat(";");
            std::string tm = fresh('d');
            std::vector<StmtP> out;
            out.push_back(mk_let_tmp(tm, std::move(e), false));
            for (size_t k = 0; k < names.size(); k++) {
                if (names[k] == "_") continue;
                out.push_back(mk_let_tmp(names[k], mk_idx(mk_var(tm), mk_num(static_cast<int64_t>(k))), mut));
            }
            return out;
        }
        std::string name = to_str(step().text);
        std::optional<std::string> ann;
        if (at(":")) {
            step();
            std::vector<std::string> toks;
            int depth = 0;
            for (;;) {
                const Tok &x = peek();
                if (x.text == "=" && depth == 0) break;
                if (x.text == "[") depth++;
                if (x.text == "]") depth--;
                toks.push_back(to_str(x.text));
                step();
            }
            ann = ann_text(toks);
        }
        want("=");
        auto e = expr();
        eat(";");
        auto s = std::make_unique<Stmt>();
        s->k = SK::Let;
        s->a = std::move(name);
        s->e1 = std::move(e);
        s->mut = konst ? false : mut;
        s->konst = konst;
        s->ann = std::move(ann);
        return one(std::move(s));
    }

    StmtP arena() {
        step();
        want("{");
        auto body = block();
        want("}");
        std::vector<StmtP> then;
        then.push_back(mk_expr(mk_call("__arena_enter", {})));
        auto def = std::make_unique<Stmt>();
        def->k = SK::Defer;
        def->e1 = mk_call("__arena_leave", {});
        then.push_back(std::move(def));
        for (auto &s : body) then.push_back(std::move(s));
        return mk_if(mk_bool(true), std::move(then), mk_block({}));
    }

    std::optional<StmtP> else_block() {
        if (!at("else")) return std::nullopt;
        step();
        if (at("if")) return mk_block(if_stmt());
        want("{");
        auto b = block();
        want("}");
        return mk_block(std::move(b));
    }

    std::vector<StmtP> if_stmt() {
        want("if");
        if (peek().kind == TK::Kw && at("let")) {
            step();
            auto pat = pattern();
            want("=");
            auto scrut = expr();
            want("{");
            auto then = block();
            want("}");
            auto els = else_block();
            std::string tm = fresh('m');
            StmtP rest = els ? std::move(*els) : mk_block({});
            StmtP node = pat ? build_if(tm, std::move(*pat), std::move(then), std::move(rest), nullptr)
                             : mk_if(mk_bool(true), std::move(then), std::move(rest));
            std::vector<StmtP> out;
            out.push_back(mk_let_tmp(tm, std::move(scrut), false));
            out.push_back(std::move(node));
            return out;
        }
        auto cond = expr();
        want("{");
        auto then = block();
        want("}");
        auto els = else_block();
        return one(mk_if(std::move(cond), std::move(then), std::move(els)));
    }

    std::vector<StmtP> while_stmt() {
        want("while");
        if (peek().kind == TK::Kw && at("let")) {
            step();
            auto pat = pattern();
            want("=");
            auto scrut = expr();
            want("{");
            auto body = block();
            want("}");
            std::string tm = fresh('m');
            std::vector<StmtP> brk_one;
            auto b = std::make_unique<Stmt>();
            b->k = SK::Break;
            brk_one.push_back(std::move(b));
            StmtP brk = mk_block(std::move(brk_one));
            StmtP inner = pat ? build_if(tm, std::move(*pat), std::move(body), std::move(brk), nullptr)
                              : mk_if(mk_bool(true), std::move(body), std::move(brk));
            std::vector<StmtP> wbody;
            wbody.push_back(mk_let_tmp(tm, std::move(scrut), false));
            wbody.push_back(std::move(inner));
            return one(mk_while(mk_bool(true), std::move(wbody)));
        }
        auto cond = expr();
        want("{");
        auto body = block();
        want("}");
        return one(mk_while(std::move(cond), std::move(body)));
    }

    StmtP try_stmt() {
        want("try");
        want("{");
        auto body = block();
        want("}");
        want("catch");
        std::optional<std::string> err;
        if (!at("{")) err = to_str(step().text);
        want("{");
        auto handler = block();
        want("}");
        auto s = std::make_unique<Stmt>();
        s->k = SK::Try;
        s->body = std::move(body);
        s->body2 = std::move(handler);
        s->opt1 = std::move(err);
        return s;
    }

    std::vector<StmtP> for_stmt() {
        want("for");
        Tok v = step();
        if (v.kind != TK::Id) fail("want loop var have '" + to_str(v.text) + "'", v.line);
        want("in");
        auto start = expr();
        if (at("..")) {
            step();
            auto end = expr();
            want("{");
            auto body = block();
            want("}");
            auto s = std::make_unique<Stmt>();
            s->k = SK::ForRange;
            s->a = to_str(v.text);
            s->e1 = std::move(start);
            s->e2 = std::move(end);
            s->body = std::move(body);
            return one(std::move(s));
        }
        want("{");
        auto body = block();
        want("}");
        auto s = std::make_unique<Stmt>();
        s->k = SK::ForIter;
        s->a = to_str(v.text);
        s->e1 = std::move(start);
        s->body = std::move(body);
        return one(std::move(s));
    }

    StmtP pub_stmt() {
        want("pub");
        const Tok c = peek();
        if (c.kind == TK::Kw) {
            if (at("fn")) {
                auto s = std::make_unique<Stmt>();
                s->k = SK::Fn;
                s->fns.push_back(fn_decl());
                return wrap_pub(std::move(s));
            }
            if (at("enum")) return wrap_pub(enum_stmt());
            if (at("trait")) return wrap_pub(trait_stmt());
            if (at("impl")) return wrap_pub(impl_stmt());
            if (at("use")) {
                step();
                auto s = std::make_unique<Stmt>();
                s->k = SK::PubUse;
                s->a = dotted_path();
                return s;
            }
        }
        fail("pub wants fn/enum/trait/impl/use");
    }

    static StmtP wrap_pub(StmtP inner) {
        auto s = std::make_unique<Stmt>();
        s->k = SK::Pub;
        s->body.push_back(std::move(inner));
        return s;
    }

    StmtP impl_stmt() {
        want("impl");
        std::string first = to_str(want_id("impl name").text);
        std::optional<std::string> trait;
        std::string type = first;
        if (at("for")) {
            step();
            trait = type;
            type = to_str(want_id("impl type name").text);
        }
        want("{");
        std::vector<FnDecl> fns;
        while (!at("}")) fns.push_back(fn_decl());
        want("}");
        auto s = std::make_unique<Stmt>();
        s->k = SK::Impl;
        s->opt1 = std::move(trait);
        s->a = std::move(type);
        s->fns = std::move(fns);
        return s;
    }

    StmtP trait_stmt() {
        want("trait");
        std::string name = to_str(step().text);
        want("{");
        std::vector<Sig> sigs;
        while (!at("}")) sigs.push_back(sig());
        want("}");
        auto s = std::make_unique<Stmt>();
        s->k = SK::Trait;
        s->a = std::move(name);
        s->sigs = std::move(sigs);
        return s;
    }

    Sig sig() {
        want("fn");
        Sig g;
        g.name = to_str(step().text);
        want("(");
        while (!at(")")) {
            SigParam p;
            p.name = to_str(step().text);
            p.mode = Mode::Ref;
            if (at(":")) {
                step();
                std::vector<std::string> toks;
                int depth = 0;
                std::optional<std::string> first;
                for (;;) {
                    const Tok &x = peek();
                    if (depth == 0 && (x.text == "," || x.text == ")")) break;
                    if (x.text == "[" || x.text == "(") depth++;
                    if (x.text == "]" || x.text == ")") depth--;
                    if (!first && depth == 0 && is_name(x)) first = to_str(x.text);
                    toks.push_back(to_str(x.text));
                    step();
                }
                if (first) {
                    if (auto m = mode_of(*first)) p.mode = *m;
                }
                p.ann = ann_text(toks);
            }
            g.params.push_back(std::move(p));
            eat(",");
        }
        want(")");
        if (at("->")) {
            step();
            bool move_word = at("move") || at("ref");
            if (move_word && step().text == "ref") fail("ref returns dangle; return owned");
            std::vector<std::string> toks;
            while (!at("}") && !(peek().kind == TK::Kw && at("fn"))) toks.push_back(to_str(step().text));
            g.ret = ann_text(toks);
        }
        return g;
    }

    StmtP enum_stmt() {
        want("enum");
        std::string tname = to_str(want_id("enum name").text);
        want("{");
        std::vector<Variant> variants;
        while (!at("}")) {
            Tok v = step();
            if (!is_name(v)) fail("want variant have '" + to_str(v.text) + "'", v.line);
            Variant var{to_str(v.text), {}};
            if (at("(")) {
                step();
                while (!at(")")) {
                    Tok f = step();
                    if (!is_name(f)) fail("want field have '" + to_str(f.text) + "'", f.line);
                    var.fields.push_back(to_str(f.text));
                    eat(",");
                }
                want(")");
            }
            variants.push_back(std::move(var));
            eat(",");
        }
        want("}");
        std::vector<std::string> tags;
        for (const auto &var : variants) {
            std::string tag = (var.name == "None" && var.fields.empty()) ? "none" : lower_ascii(var.name);
            auto it = enums_rev_.find(tag);
            if (it != enums_rev_.end()) {
                if (it->second && *it->second != tname) it->second = std::nullopt;
            } else {
                enums_rev_[tag] = tname;
            }
            tags.push_back(tag);
        }
        enums_[tname] = std::move(tags);
        auto s = std::make_unique<Stmt>();
        s->k = SK::Enum;
        s->a = std::move(tname);
        s->variants = std::move(variants);
        return s;
    }

    std::vector<StmtP> extern_block() {
        std::optional<std::string> block_lib;
        if (peek().kind == TK::Str) block_lib = unquoted(step().text);
        bool packed = false;
        if (peek().kind == TK::Id && at("packed") && peek(1).text == "struct") {
            step();          // `extern packed struct`: no padding, 1-byte alignment
            packed = true;
        }
        if (peek().kind == TK::Id && at("struct")) {
            step();
            std::string name = c_ident("extern struct name");
            want("{");
            std::vector<StructField> fields;
            while (!at("}")) {
                StructField f;
                f.name = c_ident("extern struct field");
                want(":");
                std::vector<std::string> toks;
                int depth = 0;
                for (;;) {
                    const Tok &x = peek();
                    if (depth == 0 && (x.text == "," || x.text == "}")) break;
                    if (x.text == "[") depth++;
                    if (x.text == "]") depth--;
                    toks.push_back(to_str(x.text));
                    step();
                }
                f.ann = canon_ann(ann_text(toks));
                fields.push_back(std::move(f));
                eat(",");
            }
            want("}");
            auto s = std::make_unique<Stmt>();
            s->k = SK::ExternStruct;
            s->a = std::move(name);
            s->fields = std::move(fields);
            s->opt1 = packed ? std::optional<std::string>("packed") : std::nullopt;
            return one(std::move(s));
        }
        if (at("{")) {
            step();
            std::vector<StmtP> out;
            while (!at("}")) {
                for (auto &node : stmt()) out.push_back(with_lib(std::move(node), block_lib));
            }
            want("}");
            return out;
        }
        if (!(peek().kind == TK::Str || (peek().kind == TK::Kw && at("fn")))) fail("extern wants fn");
        return one(with_lib(extern_fn(), block_lib));
    }

    static StmtP with_lib(StmtP node, const std::optional<std::string> &lib) {
        if (node->k == SK::Extern && !node->ext.lib && lib) node->ext.lib = lib;
        return node;
    }

    // `pure`, `nothrow`, `reads`, `writes` -- the vocabulary `parser.py` calls
    // EFFECTS. Kept identical here on purpose: an effect one front end knows and
    // the other does not is a program that means two things.
    static bool is_effect(std::string_view w) {
        return w == "pure" || w == "nothrow" || w == "reads" || w == "writes";
    }

    // Same table as `CONTRACTS` in `parser.py`.
    static const std::vector<std::pair<std::string, std::vector<std::string>>> &contracts() {
        static const std::vector<std::pair<std::string, std::vector<std::string>>> kTable{
            {"thread", {"same", "any"}}, {"reenter", {"yes", "no"}}};
        return kTable;
    }

    // `fn(i64)->i64frobnicate(x)` -- a trailing `word(value)` that is not a
    // contract is a typo, and a typo that became part of a type name would reach
    // the C prototype. Empty when the tail is a plausible type.
    static std::string unknown_contract(const std::string &ann) {
        if (ann.rfind("fn(", 0) != 0) return {};
        if (ann.find("->") == std::string::npos) return {};
        if (!ann.empty() && ann.back() == '>') return {};
        const size_t close = ann.rfind(')');
        if (close != ann.size() - 1) return {};
        const size_t open = ann.rfind('(');
        if (open == std::string::npos || open == 0) return {};
        const std::string word = ann.substr(0, open);
        const size_t wstart = word.find_last_not_of("abcdefghijklmnopqrstuvwxyz_");
        const std::string w = wstart == std::string::npos ? word : word.substr(wstart + 1);
        const std::string value = ann.substr(open + 1, close - open - 1);
        if (w.empty() || value.empty()) return {};
        for (char c : value)
            if (!(islower(static_cast<unsigned char>(c)) || c == '_')) return {};
        return w + "(" + value + ")";
    }

    static bool points_at_memory(const std::string &ann) {
        if (ann.empty()) return false;
        if (ann.rfind("ptr", 0) == 0 || ann.rfind("str", 0) == 0) return true;
        return ann.back() == ']';
    }

    static bool is_contract(std::string_view w) {
        for (const auto &[word, values] : contracts())
            if (word == w) return true;
        return false;
    }

    static bool is_addressed(std::string_view w) { return w == "reads" || w == "writes"; }

    static std::string effect_text(const std::string &eff) {
        const size_t colon = eff.find(':');
        return colon == std::string::npos ? eff : eff.substr(0, colon) + "(" + eff.substr(colon + 1) + ")";
    }

    // `pure nothrow writes(buf)`: parsed and validated at the same stage as in
    // the Python front end, so a contradictory declaration is refused with one
    // message by both. A promise that cannot hold is a refusal, not a warning.
    std::vector<std::string> effects_of(const std::string &name, const std::vector<Param> &params,
                                        bool owned, bool checked) {
        std::vector<std::string> out;
        std::vector<std::string> pnames;
        for (const auto &p : params) {
            pnames.push_back(p.name);
            // `p: ptr pure` canonicalises to the type `ptrpure` and the promise
            // would be dropped on the floor -- same refusal as `parser.py`.
            for (const char *w : {"pure", "nothrow", "reads", "writes"})
                if (p.ann && p.ann->find(w) != std::string::npos)
                    fail(std::string("`") + w + "` is an effect of " + p.name +
                         ", not a parameter type");
        }
        while (peek().kind == TK::Id && is_effect(peek().text)) {
            const std::string word = to_str(step().text);
            if (is_addressed(word)) {
                if (!at("(")) fail(word + " wants a parameter name, `" + word + "(p)`");
                step();
                const std::string arg_name = to_str(peek().text);
                bool declared = false;
                for (const auto &n : pnames)
                    if (n == arg_name) declared = true;
                if (peek().kind != TK::Id || !declared) {
                    std::string list;
                    for (const auto &n : pnames) {
                        if (!list.empty()) list += ", ";
                        list += n;
                    }
                    fail("`" + word + "(" + arg_name + ")` names no parameter of " + name +
                         " (params: " + (list.empty() ? "none" : list) + ")");
                }
                // An effect on an `i64` is a promise about nothing: the callee
                // has no address to reach through it. Same rule as `parser.py`.
                std::string ann;
                for (const auto &p : params)
                    if (p.name == arg_name) ann = p.ann.value_or("");
                if (!points_at_memory(ann))
                    fail("`" + word + "(" + arg_name + ")` wants a pointer parameter, `" +
                         arg_name + "` is " + (ann.empty() ? "untyped" : ann));
                step();
                want(")");
                out.push_back(word + ":" + arg_name);
            } else {
                out.push_back(word);
            }
        }
        if (std::count(out.begin(), out.end(), std::string("pure"))) {
            for (const auto &eff : out)
                if (eff.rfind("writes:", 0) == 0)
                    fail("`pure` contradicts `" + effect_text(eff) + "` on " + name);
            if (owned) fail("`pure` contradicts a `move` parameter of " + name);
            if (checked) fail("`pure` contradicts `errno` on " + name);
        }
        for (const auto &eff : out)
            if (eff.rfind("reads:", 0) == 0)
                if (std::count(out.begin(), out.end(), "writes:" + eff.substr(6)))
                    fail("`" + name + "` both reads and writes `" + eff.substr(6) + "`");
        for (const auto &eff : out)
            if (std::count(out.begin(), out.end(), eff) > 1)
                fail("`" + effect_text(eff) + "` declared twice on " + name);
        // A contract belongs on the callback parameter: at the end of the
        // declaration it would be read as the start of the next statement, and
        // the diagnostic would be about a name that does not exist.
        for (const auto &[word, values] : contracts())
            if (peek().kind == TK::Id && to_str(peek().text) == word)
                fail("`" + word + "(...)` is written on the callback parameter of " + name +
                     ", not at the end of the declaration");
        return out;
    }

    // The callback contracts, matched as literal suffixes for the same reason
    // `parser.py` matches them that way: canonical annotations have no spaces
    // in them (`fn()->none` followed by `thread(same)` reads
    // `fn()->nonethread(same)`), so a word boundary cannot be recovered.
    std::vector<std::string> take_contracts(const std::string &name, std::vector<Param> &params) {
        std::vector<std::string> out;
        for (auto &p : params) {
            if (!p.ann) continue;
            std::string ann = *p.ann;
            std::vector<std::string> found;
            for (;;) {
                std::string word, value, suffix;
                for (const auto &[w, values] : contracts()) {
                    for (const auto &v : values) {
                        const std::string tail = w + "(" + v + ")";
                        if (ann.size() >= tail.size() &&
                            ann.compare(ann.size() - tail.size(), tail.size(), tail) == 0) {
                            word = w;
                            value = v;
                            suffix = tail;
                            break;
                        }
                    }
                    if (!suffix.empty()) break;
                }
                if (suffix.empty()) break;
                const std::string rest = ann.substr(0, ann.size() - suffix.size());
                if (rest.rfind("fn(", 0) != 0)
                    fail("`" + word + "(" + value + ")` wants a callback parameter, `" +
                         p.name + "` is " + rest);
                found.push_back(word + ":" + value);
                ann = rest;
            }
            const std::string leftover = unknown_contract(ann);
            if (!leftover.empty()) fail("`" + leftover + "` is not a callback contract (`thread`, `reenter`) on " + name);
            for (auto it = found.rbegin(); it != found.rend(); ++it) {
                const std::string key = it->substr(0, it->find(':'));
                for (const auto &seen : out)
                    if (seen.substr(0, seen.find(':')) == key)
                        fail("`" + key + "` declared twice on " + name);
                out.push_back(*it);
            }
            p.ann = ann;
        }
        return out;
    }

    StmtP extern_fn() {
        std::optional<std::string> lib;
        if (peek().kind == TK::Str) lib = unquoted(step().text);
        if (!(peek().kind == TK::Kw && at("fn"))) fail("extern wants fn");
        step();
        std::string name = c_ident("extern fn name", true);
        want("(");
        auto pl = params_list(true);
        want(")");
        std::optional<std::string> ret;
        bool owned = false, checked = false;
        if (at("->")) {
            step();
            std::vector<std::string> toks;
            while (peek().kind != TK::Eof
                   && !(peek().kind == TK::Kw && (at("fn") || at("extern") || at("let") || at("if")))
                   && !at("{") && !at("}") && !at(";")
                   && !(peek().kind == TK::Id && (is_effect(peek().text) || is_contract(peek().text)))) {
                toks.push_back(to_str(step().text));
            }
            owned = !toks.empty() && toks.front() == "move";
            if (owned) toks.erase(toks.begin());
            checked = !toks.empty() && toks.back() == "errno";
            if (checked) {
                toks.pop_back();
                auto t = ann_text(toks);
                if (!t || (*t != "i32" && *t != "i64" && *t != "ptr")) {
                    fail("errno wants an i32, i64 or ptr result");
                }
            }
            if (!toks.empty()) ret = canon_ann(ann_text(toks));
        }
        eat(";");
        for (auto &p : pl.first) p.ann = canon_ann(p.ann);
        pl.second = canon_ann(pl.second);
        std::vector<std::string> contracts = take_contracts(name, pl.first);
        std::vector<std::string> effects =
            effects_of(name, pl.first, owned, checked);
        auto s = std::make_unique<Stmt>();
        s->k = SK::Extern;
        s->ext = ExternDecl{std::move(lib), std::move(name), std::move(pl.first),
                            std::move(ret), std::move(pl.second), owned, checked,
                            std::move(effects), std::move(contracts)};
        return s;
    }

    FnDecl fn_decl() {
        want("fn");
        FnDecl f;
        f.name = to_str(step().text);
        if (at("<")) {
            step();
            while (!at(">")) {
                Tok t = step();
                if (!is_name(t)) fail("want type param have '" + to_str(t.text) + "'", t.line);
                f.tparams.push_back(to_str(t.text));
                eat(",");
            }
            want(">");
        }
        std::unordered_set<std::string> seen;
        for (const auto &t : f.tparams) {
            if (!seen.insert(t).second) fail("duplicate type param");
        }
        want("(");
        f.params = params_list(false).first;
        want(")");
        if (at("->")) {
            step();
            bool move_word = at("move") || at("ref");
            if (move_word && step().text == "ref") fail("ref returns dangle; return owned");
            std::vector<std::string> toks;
            while (!at("{")) toks.push_back(to_str(step().text));
            f.ret = ann_text(toks);
        }
        want("{");
        f.body = block();
        want("}");
        return f;
    }

    std::pair<std::vector<Param>, std::optional<std::string>> params_list(bool allow_varargs) {
        std::vector<Param> out;
        std::optional<std::string> varargs;
        while (!at(")")) {
            if (allow_varargs && at("..")) {
                step();
                std::vector<std::string> toks;
                int depth = 0;
                while (!(depth == 0 && (at(",") || at(")")))) {
                    const Tok &x = peek();
                    if (x.text == "[" || x.text == "(") depth++;
                    if (x.text == "]" || x.text == ")") depth--;
                    toks.push_back(to_str(x.text));
                    step();
                }
                varargs = ann_text(toks);
                eat(",");
                continue;
            }
            Param p;
            p.name = to_str(step().text);
            if (auto m = mode_of(p.name)) {
                if (!at(":")) {
                    p.mode = *m;
                    p.name = to_str(step().text);
                }
            }
            if (at(":")) {
                step();
                std::vector<std::string> toks;
                int depth = 0;
                std::optional<std::string> first;
                for (;;) {
                    const Tok &x = peek();
                    if (depth == 0 && (x.text == "," || x.text == ")" || x.text == "=")) break;
                    if (x.text == "[" || x.text == "(") depth++;
                    if (x.text == "]" || x.text == ")") depth--;
                    if (!first && depth == 0 && is_name(x)) first = to_str(x.text);
                    toks.push_back(to_str(x.text));
                    step();
                }
                if (first) {
                    if (auto m = mode_of(*first)) p.mode = *m;
                }
                p.ann = ann_text(toks);
            }
            if (at("=")) {
                step();
                p.def = expr();
            }
            out.push_back(std::move(p));
            eat(",");
        }
        bool seen_default = false;
        for (const auto &p : out) {
            if (p.def) {
                seen_default = true;
            } else if (seen_default) {
                fail("param without default after default");
            }
        }
        return {std::move(out), std::move(varargs)};
    }


    std::optional<Pat> pattern() {
        const Tok c = peek();
        if (c.kind == TK::Id && c.text == "_") {
            step();
            return std::nullopt;
        }
        auto p = pat_arg();
        if (p->k == PK::Bind && !p->a.empty() && p->a[0] >= 'A' && p->a[0] <= 'Z') {
            auto v = std::make_unique<Pat>();
            v->k = PK::Variant;
            v->a = lower_ascii(p->a);
            Pat out = std::move(*v);
            return out;
        }
        Pat out = std::move(*p);
        return out;
    }

    PatP pat_arg() {
        const Tok c = peek();
        if (c.kind == TK::Id && c.text == "_") {
            step();
            auto p = std::make_unique<Pat>();
            p->k = PK::Wild;
            return p;
        }
        if (c.kind == TK::Id && c.text == "None") {
            step();
            auto p = std::make_unique<Pat>();
            p->k = PK::None;
            return p;
        }
        if (c.kind == TK::Kw && (c.text == "true" || c.text == "false")) {
            step();
            auto p = std::make_unique<Pat>();
            p->k = PK::Lit;
            p->lit = mk_bool(c.text == "true");
            return p;
        }
        if (c.kind == TK::Id && next_is("(")) {
            step();
            auto p = std::make_unique<Pat>();
            p->k = PK::Variant;
            p->a = lower_ascii(to_str(c.text));
            want("(");
            while (!at(")")) {
                p->subs.push_back(pat_arg());
                eat(",");
            }
            want(")");
            return p;
        }
        if (c.kind == TK::Num) {
            step();
            auto p = std::make_unique<Pat>();
            p->k = PK::Lit;
            p->lit = num_val(c.text);
            return p;
        }
        if (c.kind == TK::Str) {
            step();
            auto p = std::make_unique<Pat>();
            p->k = PK::Lit;
            p->lit = expand_str(std::string(c.text.substr(1, c.text.size() - 2)));
            // The literal's own line: `line()` is already the token after it.
            check_nul(*p->lit, c.line);
            return p;
        }
        if (c.kind == TK::Id) {
            step();
            auto p = std::make_unique<Pat>();
            p->k = PK::Bind;
            p->a = to_str(c.text);
            return p;
        }
        fail("bad pattern arg '" + to_str(c.text) + "'", c.line);
    }

    // One pattern becomes a chain of `if`s over the temporary `tm`. A plain
    // binding becomes a block. `rest` is what a failed test falls through to.
    StmtP build_if(const std::string &tm, Pat pat, std::vector<StmtP> body, StmtP rest,
                   ExprP guard) {
        // A wildcard with a guard is one `if`: nothing else tests the value.
        if (pat.k == PK::Wild) {
            if (guard) return mk_if(std::move(guard), std::move(body), std::move(rest));
            if (body.size() == 1) return std::move(body[0]);
            return mk_block(std::move(body));
        }
        // A guard on a real pattern tests first, and falls through to the same
        // `rest` the pattern itself falls through to; the copy is what makes the
        // two owners of that shape independent.
        if (guard) {
            std::vector<StmtP> wrapped;
            wrapped.push_back(mk_if(std::move(guard), std::move(body), clone_stmt(*rest)));
            body = std::move(wrapped);
        }
        switch (pat.k) {
        case PK::Wild:
            return mk_block(std::move(body));
        case PK::Lit:
            return mk_if(mk_bin("==", mk_var(tm), std::move(pat.lit)), std::move(body), std::move(rest));
        case PK::None: {
            std::vector<ExprP> args;
            args.push_back(mk_var(tm));
            return mk_if(mk_call("is_none", std::move(args)), std::move(body), std::move(rest));
        }
        case PK::Bind: {
            std::vector<StmtP> out;
            out.push_back(mk_let_tmp(pat.a, mk_var(tm), false));
            for (auto &s : body) out.push_back(std::move(s));
            return mk_block(std::move(out));
        }
        case PK::Variant: {
            std::vector<ExprP> tagargs;
            tagargs.push_back(mk_var(tm));
            auto cond = mk_bin("==", mk_call("tagof", std::move(tagargs)), mk_str(pat.a));
            std::vector<StmtP> pre;
            std::vector<StmtP> inner = std::move(body);
            for (size_t j = pat.subs.size(); j-- > 0;) {
                std::vector<ExprP> args;
                args.push_back(mk_var(tm));
                args.push_back(mk_num(static_cast<int64_t>(j + 1)));
                auto acc = mk_call("__at", std::move(args));
                Pat &sp = *pat.subs[j];
                if (sp.k == PK::Bind) {
                    pre.push_back(mk_let_tmp(sp.a, std::move(acc), false));
                } else if (sp.k == PK::Wild) {
                    continue;
                } else {
                    std::string sub_tmp = fresh('p');
                    pre.push_back(mk_let_tmp(sub_tmp, std::move(acc), false));
                    StmtP sub = build_if(sub_tmp, std::move(sp), std::move(inner), clone_stmt(*rest), nullptr);
                    inner = one(std::move(sub));
                }
            }
            for (auto &s : inner) pre.push_back(std::move(s));
            return mk_if(std::move(cond), std::move(pre), std::move(rest));
        }
        }
        return nullptr;
    }

    std::vector<StmtP> match_stmt() {
        uint32_t ml = peek().line;
        want("match");
        auto scrut = expr();
        want("{");
        std::vector<Arm> arms;
        std::vector<std::string> seen;
        while (!at("}")) {
            Arm arm;
            arm.pat = pattern();
            if (peek().kind == TK::Kw && at("if")) {
                step();
                arm.guard = expr();
            }
            want("=>");
            if (arm.pat) {
                std::string key = pat_text(*arm.pat);
                if (arm.guard) {
                    std::string g;
                    print_expr(*arm.guard, g);
                    key += g;
                }
                for (const auto &s : seen) {
                    if (s == key) fail("duplicate match pattern '" + key + "'");
                }
                seen.push_back(std::move(key));
            }
            if (at("{")) {
                step();
                arm.body = block();
                want("}");
            } else {
                arm.body = stmt();
            }
            arms.push_back(std::move(arm));
            eat(",");
        }
        want("}");
        if (arms.empty()) fail("match needs arms");
        check_exhaustive(arms, ml);

        const std::string tm = fresh('m');
        StmtP node;
        for (size_t k = arms.size(); k-- > 0;) {
            Arm &a = arms[k];
            StmtP rest = node ? std::move(node) : mk_block({});
            if (!a.pat) {
                if (!a.guard) {
                    if (node) fail("_ must be last");
                    node = mk_block(std::move(a.body));
                } else {
                    node = build_if(tm, Pat{PK::Wild, {}, nullptr, {}}, std::move(a.body),
                                    std::move(rest), std::move(a.guard));
                }
                continue;
            }
            if (a.pat->k == PK::Bind && !a.guard) {
                if (node) fail("_ must be last");
                std::vector<StmtP> v;
                v.push_back(mk_let_tmp(a.pat->a, mk_var(tm), false));
                for (auto &s : a.body) v.push_back(std::move(s));
                node = mk_block(std::move(v));
                continue;
            }
            node = build_if(tm, std::move(*a.pat), std::move(a.body), std::move(rest),
                            std::move(a.guard));
        }
        if (!node) return one(mk_expr(std::move(scrut)));
        if (node->k == SK::Block && node->body.empty()) return one(mk_expr(std::move(scrut)));
        std::vector<StmtP> out;
        out.push_back(mk_let_tmp(tm, std::move(scrut), false));
        if (node->k == SK::Block) {
            for (auto &s : node->body) out.push_back(std::move(s));
        } else {
            out.push_back(std::move(node));
        }
        return out;
    }

    static bool space_total(const Pat &p) {
        return p.k == PK::Wild || p.k == PK::Bind;
    }

    void check_exhaustive(const std::vector<Arm> &arms, uint32_t ml) {
        struct Tags {
            std::string owner;
            std::vector<std::pair<std::string, std::pair<std::vector<const Pat *>, bool>>> seen;
        };
        std::vector<Tags> owners;
        bool has_wild = false;
        for (const auto &a : arms) {
            if (!a.pat || a.pat->k == PK::Wild || a.pat->k == PK::Bind) {
                has_wild = true;
                continue;
            }
            const Pat &p = *a.pat;
            std::string tag;
            std::vector<const Pat *> subs;
            if (p.k == PK::Variant) {
                tag = p.a;
                for (const auto &s : p.subs) subs.push_back(s.get());
            } else if (p.k == PK::None) {
                tag = "none";
            } else {
                continue;
            }
            auto it = enums_rev_.find(tag);
            if (it == enums_rev_.end() || !it->second) continue;
            const std::string &owner = *it->second;
            Tags *slot = nullptr;
            for (auto &o : owners) {
                if (o.owner == owner) slot = &o;
            }
            if (!slot) {
                owners.push_back(Tags{owner, {}});
                slot = &owners.back();
            }
            bool known = false;
            for (const auto &s : slot->seen) {
                if (s.first == tag) known = true;
            }
            if (!known || !a.guard) {
                auto &v = slot->seen;
                for (size_t j = 0; j < v.size();) {
                    if (v[j].first == tag) {
                        v.erase(v.begin() + static_cast<long>(j));
                    } else {
                        j++;
                    }
                }
                v.push_back({tag, {subs, a.guard != nullptr}});
            }
        }
        if (owners.size() == 1 && !has_wild) {
            const Tags &o = owners[0];
            std::vector<std::string> total;
            for (const auto &s : o.seen) {
                bool all = true;
                for (const Pat *sp : s.second.first) {
                    if (!space_total(*sp)) all = false;
                }
                if (!s.second.second && all) total.push_back(s.first);
            }
            auto it = enums_.find(o.owner);
            if (it != enums_.end()) {
                std::vector<std::string> missing;
                for (const auto &t : it->second) {
                    bool have = false;
                    for (const auto &s : total) {
                        if (s == t) have = true;
                    }
                    if (!have) missing.push_back(t);
                }
                if (!missing.empty()) {
                    std::string names;
                    for (size_t j = 0; j < missing.size(); j++) {
                        if (j) names += ", ";
                        names += missing[j];
                    }
                    fail("non-exhaustive match: missing " + names + " in enum " + o.owner, ml);
                }
            }
        }
        if (owners.empty() && !has_wild) {
            bool onlybool = true, saw_true = false, saw_false = false;
            for (const auto &a : arms) {
                if (a.pat && a.pat->k == PK::Lit && a.pat->lit->k == EK::Bool && !a.guard) {
                    if (a.pat->lit->flag) {
                        saw_true = true;
                    } else {
                        saw_false = true;
                    }
                } else {
                    onlybool = false;
                }
            }
            if (onlybool && !(saw_true && saw_false)) {
                fail(std::string("non-exhaustive match: missing ") + (saw_true ? "false" : "true"), ml);
            }
        }
    }


    // The grammar is recursive (`f(g(h(...)))`, `((((1))))`), and the text comes
    // from a file or an editor. Deep nesting is a stack overflow, which is a
    // SIGSEGV: it cannot be caught, so it has to be refused here instead. 256
    // levels is far past anything hand-written and far under the smallest stack.
    static constexpr int kMaxNest = 200;

    // Depth of the tree, not of the nesting in the text. `kMaxNest` bounds the
    // parser's own stack; it says nothing about `1+1+...+1`, which is one
    // nesting level and ten thousand nodes. Every pass after the parser walks
    // the tree recursively, so an unbounded depth is a stack overflow in
    // whichever pass is deepest -- a SIGSEGV, which cannot be caught and so has
    // to be refused here. Keep this equal to `MAX_TREE_DEPTH` in `parser.py`:
    // the two front ends must refuse the same input at the same depth.
    static constexpr size_t kMaxTreeDepth = 1000;
    int nest_expr_ = 0;
    int nest_block_ = 0;

    struct Depth {
        Parser &p;
        bool block;
        Depth(Parser &self, const char *what) : p(self), block(std::string(what) == "block") {
            int &n = block ? p.nest_block_ : p.nest_expr_;
            // The line is reported as `file:line:` by both front ends; the
            // Python message used to be the only one without it, so the two
            // disagreed on a program `--json` could not attribute to a line.
            if (++n > kMaxNest) p.fail(std::string(what) + " nested too deeply", p.line());
        }
        ~Depth() {
            int &n = block ? p.nest_block_ : p.nest_expr_;
            --n;
        }
    };

    ExprP expr() {
        Depth guard(*this, "expression");
        return or_expr();
    }

    ExprP or_expr() {
        auto e = lor_expr();
        while (at("or")) {
            step();
            e = mk_or(std::move(e), lor_expr());
        }
        return e;
    }

    ExprP lor_expr() {
        auto e = land_expr();
        while (at("||")) {
            step();
            e = mk_bin("||", std::move(e), land_expr());
        }
        return e;
    }

    ExprP land_expr() {
        auto e = cmp_expr();
        while (at("&&")) {
            step();
            e = mk_bin("&&", std::move(e), cmp_expr());
        }
        return e;
    }

    static const char *cmp_op(std::string_view t) {
        if (t == "==") return "==";
        if (t == "!=") return "!=";
        if (t == "<") return "<";
        if (t == ">") return ">";
        if (t == "<=") return "<=";
        if (t == ">=") return ">=";
        if (t == "in") return "in";
        return nullptr;
    }

    ExprP cmp_expr() {
        auto e = add_expr();
        while (const char *op = cmp_op(peek().text)) {
            step();
            e = mk_bin(op, std::move(e), add_expr());
        }
        return e;
    }

    ExprP add_expr() {
        auto e = mul_expr();
        for (;;) {
            std::string_view t = peek().text;
            if (t != "+" && t != "-") break;
            const char *op = t == "+" ? "+" : "-";
            step();
            e = mk_bin(op, std::move(e), mul_expr());
        }
        return e;
    }

    ExprP mul_expr() {
        auto e = un_expr();
        for (;;) {
            std::string_view t = peek().text;
            if (t != "*" && t != "/" && t != "%") break;
            const char *op = t == "*" ? "*" : (t == "/" ? "/" : "%");
            step();
            e = mk_bin(op, std::move(e), un_expr());
        }
        return e;
    }

    ExprP un_expr() {
        std::string_view t = peek().text;
        if (t == "&") {
            // `&x` -- the address of a binding: how an out-parameter is written.
            step();
            return mk_un("&", un_expr());
        }
        if (t == "!" || t == "-") {
            step();
            return mk_un(t == "!" ? "!" : "-", un_expr());
        }
        if (peek().kind == TK::Kw && t == "move") {
            step();
            return mk_move(un_expr());
        }
        return post_expr();
    }

    ExprP post_expr() {
        auto e = prim_expr();
        for (;;) {
            if (at("?")) {
                step();
                e = mk_quest(std::move(e));
            } else if (at("[")) {
                step();
                auto ix = expr();
                want("]");
                e = mk_idx(std::move(e), std::move(ix));
            } else if (at("(")) {
                // Same rule, same sentence as `parser.py`: `v[0](10)` is two
                // arguments of the enclosing call, not a call of a value.
                fail("a call needs a name, a function value is not first class", peek().line);
            } else if (at(".")) {
                step();
                std::string meth = to_str(want_id("method or field name").text);
                if (at("(")) {
                    step();
                    std::vector<ExprP> args;
                    while (!at(")")) {
                        args.push_back(expr());
                        eat(",");
                    }
                    want(")");
                    if (e->k == EK::Var && e->a == "trace" && at("{")) {
                        step();
                        auto closure = std::make_unique<Expr>();
                        closure->k = EK::Closure;
                        closure->fn = std::make_unique<FnDecl>();
                        closure->fn->body = block();
                        want("}");
                        args.push_back(std::move(closure));
                    }
                    e = mk_mcall(std::move(e), std::move(meth), std::move(args));
                } else {
                    e = mk_field(std::move(e), std::move(meth));
                }
            } else {
                return e;
            }
        }
    }

    // `par for x in v reduce(op, unit) { return e }`. The loop body lives in the
    // node's FnDecl, so the closure machinery can emit it unchanged; `i` carries
    // the line, because a diagnostic about the fold needs one and an expression
    // has no line field of its own.
    ExprP par_for(int line_no) {
        want("for");
        Tok v = step();
        if (v.kind != TK::Id) fail("want loop var have '" + to_str(v.text) + "'", v.line);
        want("in");
        ExprP it = expr();
        want("reduce");
        want("(");
        Tok op = step();
        want(",");
        ExprP unit = expr();
        want(")");
        want("{");
        auto e = std::make_unique<Expr>();
        e->k = EK::ParFor;
        e->a = to_str(v.text);
        e->b = to_str(op.text);
        e->i = line_no;
        e->kids.push_back(std::move(it));
        e->kids.push_back(std::move(unit));
        e->fn = std::make_unique<FnDecl>();
        e->fn->params.emplace_back();
        e->fn->params.back().name = e->a;
        e->fn->params.back().mode = Mode::Ref;
        e->fn->body = block();
        want("}");
        if (e->fn->body.empty() || e->fn->body.back()->k != SK::Return) {
            fail("a `par for` body ends with `return <value>`");
        }
        return e;
    }

    ExprP prim_expr() {
        const Tok t = step();
        if (t.kind == TK::Num) return num_val(t.text);
        if (t.kind == TK::Id && t.text == "par" && peek().kind == TK::Kw &&
            peek().text == "for") {
            return par_for(t.line);
        }
        if (t.kind == TK::Str) {
            auto e = expand_str(std::string(t.text.substr(1, t.text.size() - 2)));
            check_nul(*e, t.line);
            return e;
        }
        if (t.kind == TK::Kw && (t.text == "true" || t.text == "false")) return mk_bool(t.text == "true");
        if (t.kind == TK::Kw && t.text == "fn") {
            auto closure = std::make_unique<Expr>();
            closure->k = EK::Closure;
            closure->fn = std::make_unique<FnDecl>();
            if (peek().kind == TK::Id && next_is("(")) closure->fn->name = to_str(step().text);
            want("(");
            closure->fn->params = params_list(false).first;
            want(")");
            want("{");
            closure->fn->body = block();
            want("}");
            return closure;
        }
        return prim_tail(t);
    }

    ExprP prim_tail(const Tok &t) {
        if (t.text == "[") {
            std::vector<ExprP> items;
            while (!at("]")) {
                items.push_back(expr());
                eat(",");
            }
            want("]");
            return mk_veclit(std::move(items));
        }
        if (t.text == "{") {
            auto m = std::make_unique<Expr>();
            m->k = EK::MapLit;
            while (!at("}")) {
                m->kids.push_back(expr());
                want(":");
                m->kids.push_back(expr());
                eat(",");
            }
            want("}");
            return m;
        }
        if (t.text == "(") {
            auto e = expr();
            want(")");
            return e;
        }
        if (t.kind == TK::Id && t.text == "None") return mk_none();
        if (!is_name(t)) fail("stuck at '" + to_str(t.text) + "'", t.line);
        const std::string name = to_str(t.text);
        if (at("(")) {
            step();
            std::vector<ExprP> args;
            while (!at(")")) {
                args.push_back(expr());
                eat(",");
            }
            want(")");
            return mk_call(name, std::move(args));
        }
        const bool upper = !name.empty() && name[0] >= 'A' && name[0] <= 'Z';
        if (at("{") && t.kind == TK::Id && upper && struct_literal_ahead()) {
            step();
            auto s = std::make_unique<Expr>();
            s->k = EK::StructLit;
            s->a = name;
            while (!at("}")) {
                s->names.push_back(to_str(want_id("field name").text));
                want(":");
                s->kids.push_back(expr());
                eat(",");
            }
            want("}");
            return s;
        }
        if (t.kind == TK::Id && upper) return mk_call(name, {});
        return mk_var(name);
    }

    // `Point { x: 1 }` is a struct literal; `if x {` is a block. The lookahead
    // after `{` decides: `}` or `name :` means a literal.
    bool struct_literal_ahead() const {
        const Tok &first = peek(1);
        if (first.text == "}") return true;
        return first.kind == TK::Id && peek(2).text == ":";
    }

    // `"a{x}b"` becomes `("bin" "+" ("rstr" "a") ("bin" "+" x ("rstr" "b")))`.
    // A placeholder that does not parse as a whole expression stays text, which
    // is what keeps JSON templates working.
    ExprP expand_str(const std::string &raw) {
        if (raw.find('{') == std::string::npos) return mk_str(unescape(raw));
        struct Part {
            bool is_text;
            std::string text;
            ExprP sub;
        };
        std::vector<Part> parts;
        std::string buf;
        size_t i = 0;
        while (i < raw.size()) {
            const size_t len = utf8_len(static_cast<unsigned char>(raw[i]));
            if (raw[i] == '{') {
                size_t j = i + 1;
                int depth = 1;
                while (j < raw.size() && depth > 0) {
                    if (raw[j] == '\\' && j + 1 < raw.size()) {
                        j += 2;
                        continue;
                    }
                    if (raw[j] == '{') depth++;
                    else if (raw[j] == '}') depth--;
                    j++;
                }
                if (depth == 0) {
                    std::string inner = unescape(raw.substr(i + 1, j - 1 - (i + 1)));
                    if (auto sub = sub_expr(inner)) {
                        if (!buf.empty()) {
                            parts.push_back({true, unescape(buf), nullptr});
                            buf.clear();
                        }
                        parts.push_back({false, {}, std::move(sub)});
                        i = j;
                        continue;
                    }
                    note_stray(raw.substr(i + 1, j - 1 - (i + 1)));
                }
            }
            buf.append(raw, i, len);
            i += len;
        }
        if (!buf.empty()) parts.push_back({true, unescape(buf), nullptr});
        if (parts.empty()) return mk_str("");
        if (parts.size() == 1) {
            if (parts[0].is_text) return mk_rstr(parts[0].text);
            return std::move(parts[0].sub);
        }
        ExprP node = parts[0].is_text ? mk_rstr(parts[0].text) : std::move(parts[0].sub);
        for (size_t j = 1; j < parts.size(); j++) {
            ExprP rhs = parts[j].is_text ? mk_rstr(parts[j].text) : std::move(parts[j].sub);
            node = mk_bin("+", std::move(node), std::move(rhs));
        }
        return node;
    }

    // A `{...}` that stayed literal but looks like a failed placeholder is
    // reported: JSON has a top-level colon and is left alone, so is `{}`.
    void note_stray(const std::string &text) {
        std::string body = text;
        while (!body.empty() && (body.front() == ' ' || body.front() == '\t')) body.erase(body.begin());
        while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) body.pop_back();
        if (body.empty()) return;
        int depth = 0;
        for (char c : body) {
            if (c == '(' || c == '[' || c == '{') {
                depth++;
            } else if (c == ')' || c == ']' || c == '}') {
                depth--;
            } else if (c == ':' && depth == 0) {
                return;
            }
        }
        bool any = false;
        for (char c : body) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') any = true;
        }
        if (!any) return;
        stray_.push_back({curline_, body});
    }

    static size_t utf8_len(unsigned char b) {
        if (b < 0x80) return 1;
        if (b < 0xe0) return 2;
        if (b < 0xf0) return 3;
        return 4;
    }

    static ExprP sub_expr(const std::string &text) {
        try {
            Parser p(text);
            auto e = p.expr();
            if (p.peek().kind != TK::Eof) return nullptr;
            return e;
        } catch (const ParseError &) {
            return nullptr;
        }
    }

    static ExprP num_val(std::string_view tok) {
        std::string t(tok);
        // `_` separates digits in a literal (`1_000`); neither `int`-accumulation
        // below nor `strtod` accepts it, so it is dropped before the value is read.
        for (size_t j = 0; j < t.size();) {
            if (t[j] == '_') t.erase(j, 1); else j++;
        }
        if (t.find('.') != std::string::npos || t.find('e') != std::string::npos
            || t.find('E') != std::string::npos) {
            // `std::stod` throws out_of_range where libstdc++ sees ERANGE - for
            // subnormals (`1e-320`) and for anything that overflows to infinity -
            // while the Python front end keeps `float(tok)`. strtod returns the
            // same value without throwing.
            return mk_float(std::strtod(t.c_str(), nullptr));
        }
        // `parser.py` parses with arbitrary precision and `codegen_c.py` wraps to
        // i64, so `9223372036854775808` is a legal literal there (it is what
        // `-9223372036854775808` is built from). Accumulate modulo 2^64.
        uint64_t v = 0;
        for (char ch : t) {
            if (ch < '0' || ch > '9') continue;
            v = v * 10u + static_cast<uint64_t>(ch - '0');
        }
        return mk_num(static_cast<int64_t>(v));
    }

    static void check_nul(const Expr &root, uint32_t line) {
        // Iterative for the same reason the depth bound exists: `"a"+"b"+...`
        // is a left-deep Bin chain, so the recursive form of this walk reached
        // one frame per term and crashed the front end on a file it had already
        // tokenised. A stack of pointers costs one heap vector per literal.
        std::vector<const Expr *> stack{&root};
        while (!stack.empty()) {
            const Expr &e = *stack.back();
            stack.pop_back();
            switch (e.k) {
            case EK::Str:
            case EK::RStr:
                if (e.a.find('\0') != std::string::npos) throw ParseError("string literal contains NUL", line);
                break;
            case EK::Bin:
                for (size_t k = e.kids.size(); k-- > 0;)
                    if (e.kids[k]) stack.push_back(e.kids[k].get());
                break;
            default:
                break;
            }
        }
    }
};

}  // namespace

ParseOut parse(std::string_view src) {
    Parser p(src);
    return p.run();
}
