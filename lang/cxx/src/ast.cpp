#include "ast.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

const char *mode_str(Mode m) {
    switch (m) {
    case Mode::Ref: return "ref";
    case Mode::Mut: return "mut";
    case Mode::Move: return "move";
    }
    return "ref";
}

std::optional<Mode> mode_of(const std::string &s) {
    if (s == "ref") return Mode::Ref;
    if (s == "mut") return Mode::Mut;
    if (s == "move") return Mode::Move;
    return std::nullopt;
}


static ExprP bare(EK k) {
    auto e = std::make_unique<Expr>();
    e->k = k;
    return e;
}

ExprP mk_num(int64_t v) {
    auto e = bare(EK::Num);
    e->i = v;
    return e;
}

ExprP mk_float(double v) {
    auto e = bare(EK::Num);
    e->f = v;
    e->is_float = true;
    return e;
}

ExprP mk_str(std::string v) {
    auto e = bare(EK::Str);
    e->a = std::move(v);
    return e;
}

ExprP mk_rstr(std::string v) {
    auto e = bare(EK::RStr);
    e->a = std::move(v);
    return e;
}

ExprP mk_bool(bool v) {
    auto e = bare(EK::Bool);
    e->flag = v;
    return e;
}

ExprP mk_none() { return bare(EK::None); }

ExprP mk_var(std::string v) {
    auto e = bare(EK::Var);
    e->a = std::move(v);
    return e;
}

ExprP mk_bin(const char *op, ExprP l, ExprP r) {
    auto e = bare(EK::Bin);
    e->a = op;
    e->kids.push_back(std::move(l));
    e->kids.push_back(std::move(r));
    return e;
}

ExprP mk_un(const char *op, ExprP x) {
    auto e = bare(EK::Un);
    e->a = op;
    e->kids.push_back(std::move(x));
    return e;
}

ExprP mk_call(std::string name, std::vector<ExprP> args) {
    auto e = bare(EK::Call);
    e->a = std::move(name);
    e->kids = std::move(args);
    return e;
}

ExprP mk_mcall(ExprP obj, std::string meth, std::vector<ExprP> args) {
    auto e = bare(EK::MCall);
    e->b = std::move(meth);
    e->kids = std::move(args);
    e->kids.insert(e->kids.begin(), std::move(obj));
    return e;
}

ExprP mk_field(ExprP obj, std::string name) {
    auto e = bare(EK::Field);
    e->b = std::move(name);
    e->kids.push_back(std::move(obj));
    return e;
}

ExprP mk_idx(ExprP obj, ExprP ix) {
    auto e = bare(EK::Idx);
    e->kids.push_back(std::move(obj));
    e->kids.push_back(std::move(ix));
    return e;
}

ExprP mk_veclit(std::vector<ExprP> items) {
    auto e = bare(EK::VecLit);
    e->kids = std::move(items);
    return e;
}

ExprP mk_quest(ExprP x) {
    auto e = bare(EK::Quest);
    e->kids.push_back(std::move(x));
    return e;
}

ExprP mk_move(ExprP x) {
    auto e = bare(EK::Move);
    e->kids.push_back(std::move(x));
    return e;
}

ExprP mk_or(ExprP l, ExprP r) {
    auto e = bare(EK::Or);
    e->kids.push_back(std::move(l));
    e->kids.push_back(std::move(r));
    return e;
}

static StmtP bare_stmt(SK k) {
    auto s = std::make_unique<Stmt>();
    s->k = k;
    return s;
}

StmtP mk_let(std::string name, ExprP init, bool mut, bool konst,
             std::optional<std::string> ann) {
    auto s = bare_stmt(SK::Let);
    s->a = std::move(name);
    s->e1 = std::move(init);
    s->mut = mut;
    s->konst = konst;
    s->ann = std::move(ann);
    return s;
}

StmtP mk_let_tmp(std::string name, ExprP init, bool mut) {
    auto s = bare_stmt(SK::LetTmp);
    s->a = std::move(name);
    s->e1 = std::move(init);
    s->mut = mut;
    return s;
}

StmtP mk_expr(ExprP e) {
    auto s = bare_stmt(SK::Expr);
    s->e1 = std::move(e);
    return s;
}

StmtP mk_block(std::vector<StmtP> v) {
    auto s = bare_stmt(SK::Block);
    s->body = std::move(v);
    return s;
}

StmtP mk_if(ExprP cond, std::vector<StmtP> then_body, std::optional<StmtP> els) {
    auto s = bare_stmt(SK::If);
    s->e1 = std::move(cond);
    s->body = std::move(then_body);
    s->els = std::move(els).value_or(nullptr);
    return s;
}

StmtP mk_while(ExprP cond, std::vector<StmtP> body) {
    auto s = bare_stmt(SK::While);
    s->e1 = std::move(cond);
    s->body = std::move(body);
    return s;
}


namespace {

void space(std::string &o) { o.push_back(' '); }

void quoted(std::string &o, const std::string &s) {
    o.push_back('"');
    for (char c : s) {
        switch (c) {
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\t': o += "\\t"; break;
        case '\r': o += "\\r"; break;
        case '"': o += "\\\""; break;
        default: o.push_back(c);
        }
    }
    o.push_back('"');
}

// A tag, an identifier, an operator: every one of them is a string in the
// reference dump, and so every one of them is quoted.
void open(std::string &o, const char *tag) {
    o.push_back('(');
    quoted(o, tag);
}

void leaf(std::string &o, const char *tag) {
    open(o, tag);
    o.push_back(')');
}

void word(std::string &o, const std::string &s) {
    space(o);
    quoted(o, s);
}

void opt(std::string &o, const std::optional<std::string> &s) {
    space(o);
    if (s) {
        quoted(o, *s);
    } else {
        o += "None";
    }
}

void boolean(std::string &o, bool v) {
    space(o);
    o += (v ? "true" : "false");
}

void stmts(const std::vector<StmtP> &v, std::string &o);
void stmt(const Stmt &s, std::string &o);

void words(const std::vector<std::string> &v, std::string &o) {
    o.push_back('[');
    for (size_t i = 0; i < v.size(); i++) {
        if (i) space(o);
        quoted(o, v[i]);
    }
    o.push_back(']');
}

void params(const std::vector<Param> &v, std::string &o) {
    o.push_back('[');
    for (size_t i = 0; i < v.size(); i++) {
        if (i) space(o);
        o.push_back('(');
        quoted(o, v[i].name);
        word(o, mode_str(v[i].mode));
        opt(o, v[i].ann);
        if (v[i].def) {
            space(o);
            print_expr(*v[i].def, o);
        } else {
            space(o);
            o += "None";
        }
        o.push_back(')');
    }
    o.push_back(']');
}

void exprs(const std::vector<ExprP> &v, std::string &o) {
    o.push_back('[');
    for (size_t i = 0; i < v.size(); i++) {
        if (i) space(o);
        print_expr(*v[i], o);
    }
    o.push_back(']');
}

// The arguments of a method call: the receiver is element zero.
void tail_exprs(const std::vector<ExprP> &v, std::string &o) {
    o.push_back('[');
    for (size_t i = 1; i < v.size(); i++) {
        if (i > 1) space(o);
        print_expr(*v[i], o);
    }
    o.push_back(']');
}

void fn_body(const FnDecl &f, std::string &o);

void stmt(const Stmt &s, std::string &o);

void pat(const Pat &p, std::string &o) {
    switch (p.k) {
    case PK::Wild: leaf(o, "wild"); break;
    case PK::None: leaf(o, "none"); break;
    case PK::Lit: open(o, "lit"); space(o); print_expr(*p.lit, o); o.push_back(')'); break;
    case PK::Bind: open(o, "bind"); word(o, p.a); o.push_back(')'); break;
    case PK::Variant:
        open(o, "variant");
        word(o, p.a);
        space(o);
        o.push_back('[');
        for (size_t i = 0; i < p.subs.size(); i++) {
            if (i) space(o);
            pat(*p.subs[i], o);
        }
        o += "])";
        break;
    }
}

}  // namespace

std::string repr_f64(double v) {
    // The reference is `repr(float)` in `codegen_c.py`: the emitter contract is
    // byte-identical C, and `std::to_chars` shortest form is not Python's. Two
    // rules differ. Python switches to scientific notation outside
    // [1e-4, 1e16) -- `to_chars` uses its own threshold -- and Python writes
    // `1e-320`, not `1.0e-320`. Checked against 60 000 doubles, including
    // subnormals: identical on every one.
    if (v != v) return "NAN";                                  // vfloat(inf) is not C
    if (std::isinf(v)) return v > 0 ? "INFINITY" : "-INFINITY";
    bool neg = std::signbit(v);
    double a = neg ? -v : v;
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, a, std::chars_format::scientific);
    std::string sci(buf, res.ptr);
    auto e = sci.find('e');
    std::string mant = sci.substr(0, e);
    int exp10 = std::atoi(sci.c_str() + e + 1);
    std::string digits;
    for (char c : mant) {
        if (c != '.') digits.push_back(c);
    }
    std::string out;
    if (exp10 < -4 || exp10 >= 16) {
        out += digits[0];
        std::string rest = digits.substr(1);
        while (!rest.empty() && rest.back() == '0') rest.pop_back();
        if (!rest.empty()) out += "." + rest;
        char eb[16];
        std::snprintf(eb, sizeof eb, "e%c%02d", exp10 < 0 ? '-' : '+',
                      exp10 < 0 ? -exp10 : exp10);
        out += eb;
    } else {
        std::string frac;
        if (exp10 >= 0) {
            std::string ip = digits.substr(0, std::min<size_t>(digits.size(), size_t(exp10) + 1));
            while (int(ip.size()) < exp10 + 1) ip.push_back('0');
            frac = digits.size() > size_t(exp10) + 1 ? digits.substr(exp10 + 1) : "";
            out = ip;
        } else {
            frac = std::string(-exp10 - 1, '0') + digits;
            out = "0";
        }
        while (!frac.empty() && frac.back() == '0') frac.pop_back();
        out += "." + (frac.empty() ? std::string("0") : frac);
    }
    return neg ? "-" + out : out;
}

void print_expr(const Expr &e, std::string &o) {
    switch (e.k) {
    case EK::Num:
        open(o, "num");
        space(o);
        if (e.is_float) {
            o += repr_f64(e.f);
        } else {
            o += std::to_string(e.i);
        }
        o.push_back(')');
        break;
    case EK::Str: open(o, "str"); word(o, e.a); o.push_back(')'); break;
    case EK::RStr: open(o, "rstr"); word(o, e.a); o.push_back(')'); break;
    case EK::Bool: open(o, "bool"); boolean(o, e.flag); o.push_back(')'); break;
    case EK::None: leaf(o, "none"); break;
    case EK::Var: open(o, "var"); word(o, e.a); o.push_back(')'); break;
    case EK::Bin:
        open(o, "bin");
        word(o, e.a);
        space(o);
        print_expr(*e.kids[0], o);
        space(o);
        print_expr(*e.kids[1], o);
        o.push_back(')');
        break;
    case EK::Un:
        open(o, "un");
        word(o, e.a);
        space(o);
        print_expr(*e.kids[0], o);
        o.push_back(')');
        break;
    case EK::Call: open(o, "call"); word(o, e.a); space(o); exprs(e.kids, o); o.push_back(')'); break;
    case EK::MCall:
        open(o, "mcall");
        space(o);
        print_expr(*e.kids[0], o);
        word(o, e.b);
        space(o);
        tail_exprs(e.kids, o);
        o.push_back(')');
        break;
    case EK::Field:
        open(o, "field");
        space(o);
        print_expr(*e.kids[0], o);
        word(o, e.b);
        o.push_back(')');
        break;
    case EK::Idx:
        open(o, "idx");
        space(o);
        print_expr(*e.kids[0], o);
        space(o);
        print_expr(*e.kids[1], o);
        o.push_back(')');
        break;
    case EK::VecLit: open(o, "vec"); space(o); exprs(e.kids, o); o.push_back(')'); break;
    case EK::MapLit:
        open(o, "maplit");
        space(o);
        o.push_back('[');
        for (size_t i = 0; i + 1 < e.kids.size(); i += 2) {
            if (i) space(o);
            o.push_back('(');
            print_expr(*e.kids[i], o);
            space(o);
            print_expr(*e.kids[i + 1], o);
            o.push_back(')');
        }
        o += "])";
        break;
    case EK::StructLit:
        open(o, "struct");
        word(o, e.a);
        space(o);
        o.push_back('[');
        for (size_t i = 0; i < e.names.size(); i++) {
            if (i) space(o);
            o.push_back('(');
            quoted(o, e.names[i]);
            space(o);
            print_expr(*e.kids[i], o);
            o.push_back(')');
        }
        o += "])";
        break;
    case EK::Closure:
        open(o, "closure");
        opt(o, e.fn->name);
        space(o);
        params(e.fn->params, o);
        space(o);
        stmts(e.fn->body, o);
        o.push_back(')');
        break;
    case EK::ParFor:
        open(o, "parfor");
        space(o);
        quoted(o, e.a);
        space(o);
        print_expr(*e.kids[0], o);
        space(o);
        quoted(o, e.b);
        space(o);
        print_expr(*e.kids[1], o);
        space(o);
        stmts(e.fn->body, o);
        o.push_back(')');
        break;
    case EK::Quest: open(o, "quest"); space(o); print_expr(*e.kids[0], o); o.push_back(')'); break;
    case EK::Move: open(o, "move"); space(o); print_expr(*e.kids[0], o); o.push_back(')'); break;
    case EK::Or:
        open(o, "or");
        space(o);
        print_expr(*e.kids[0], o);
        space(o);
        print_expr(*e.kids[1], o);
        o.push_back(')');
        break;
    }
}

namespace {

void fn_body(const FnDecl &f, std::string &o) {
    if (f.name) {
        quoted(o, *f.name);
    } else {
        o += "None";
    }
    space(o);
    params(f.params, o);
    space(o);
    stmts(f.body, o);
    space(o);
    words(f.tparams, o);
    opt(o, f.ret);
}

void stmts(const std::vector<StmtP> &v, std::string &o) {
    o.push_back('[');
    for (size_t i = 0; i < v.size(); i++) {
        if (i) space(o);
        stmt(*v[i], o);
    }
    o.push_back(']');
}

void stmt(const Stmt &s, std::string &o) {
    switch (s.k) {
    case SK::Let:
        open(o, "let");
        word(o, s.a);
        space(o);
        print_expr(*s.e1, o);
        boolean(o, s.mut);
        boolean(o, s.konst);
        opt(o, s.ann);
        o.push_back(')');
        break;
    case SK::LetTmp:
        open(o, "let");
        word(o, s.a);
        space(o);
        print_expr(*s.e1, o);
        boolean(o, s.mut);
        o.push_back(')');
        break;
    case SK::Set: open(o, "set"); word(o, s.a); space(o); print_expr(*s.e1, o); o.push_back(')'); break;
    case SK::SetIdx:
        open(o, "setidx");
        space(o);
        print_expr(*s.e1, o);
        space(o);
        print_expr(*s.e2, o);
        space(o);
        print_expr(*s.e3, o);
        o.push_back(')');
        break;
    case SK::SetField:
        open(o, "setfield");
        space(o);
        print_expr(*s.e1, o);
        word(o, s.a);
        space(o);
        print_expr(*s.e2, o);
        o.push_back(')');
        break;
    case SK::Expr: open(o, "expr"); space(o); print_expr(*s.e1, o); o.push_back(')'); break;
    case SK::Block: stmts(s.body, o); break;
    case SK::If:
        open(o, "if");
        space(o);
        print_expr(*s.e1, o);
        space(o);
        stmts(s.body, o);
        space(o);
        if (s.els) {
            stmt(*s.els, o);
        } else {
            o += "None";
        }
        o.push_back(')');
        break;
    case SK::While:
        open(o, "while");
        space(o);
        print_expr(*s.e1, o);
        space(o);
        stmts(s.body, o);
        o.push_back(')');
        break;
    case SK::ForRange:
        open(o, "forrange");
        word(o, s.a);
        space(o);
        print_expr(*s.e1, o);
        space(o);
        print_expr(*s.e2, o);
        space(o);
        stmts(s.body, o);
        o.push_back(')');
        break;
    case SK::ForIter:
        open(o, "foriter");
        word(o, s.a);
        space(o);
        print_expr(*s.e1, o);
        space(o);
        stmts(s.body, o);
        o.push_back(')');
        break;
    case SK::Return: open(o, "return"); space(o); print_expr(*s.e1, o); o.push_back(')'); break;
    case SK::Break: leaf(o, "break"); break;
    case SK::Continue: leaf(o, "continue"); break;
    case SK::Spawn: open(o, "spawn"); space(o); stmts(s.body, o); o.push_back(')'); break;
    case SK::Scope: open(o, "scope"); space(o); stmts(s.body, o); o.push_back(')'); break;
    case SK::Unsafe: open(o, "unsafe"); space(o); stmts(s.body, o); o.push_back(')'); break;
    case SK::Defer: open(o, "defer"); space(o); print_expr(*s.e1, o); o.push_back(')'); break;
    case SK::Try:
        open(o, "try");
        space(o);
        stmts(s.body, o);
        space(o);
        stmts(s.body2, o);
        opt(o, s.opt1);
        o.push_back(')');
        break;
    case SK::Fn: open(o, "fn"); space(o); fn_body(s.fns[0], o); o.push_back(')'); break;
    case SK::ExportFn: open(o, "exportfn"); space(o); fn_body(s.fns[0], o); o.push_back(')'); break;
    case SK::Impl:
        open(o, "impl");
        opt(o, s.opt1);
        word(o, s.a);
        space(o);
        o.push_back('[');
        for (size_t i = 0; i < s.fns.size(); i++) {
            if (i) space(o);
            open(o, "fn");
            space(o);
            fn_body(s.fns[i], o);
            o.push_back(')');
        }
        o += "])";
        break;
    case SK::Trait:
        open(o, "trait");
        word(o, s.a);
        space(o);
        o.push_back('[');
        for (size_t i = 0; i < s.sigs.size(); i++) {
            const Sig &g = s.sigs[i];
            if (i) space(o);
            open(o, "sig");
            word(o, g.name);
            space(o);
            o.push_back('[');
            for (size_t j = 0; j < g.params.size(); j++) {
                if (j) space(o);
                o.push_back('(');
                quoted(o, g.params[j].name);
                word(o, mode_str(g.params[j].mode));
                opt(o, g.params[j].ann);
                o.push_back(')');
            }
            o.push_back(']');
            opt(o, g.ret);
            o.push_back(')');
        }
        o += "])";
        break;
    case SK::Enum:
        open(o, "enum");
        word(o, s.a);
        space(o);
        o.push_back('[');
        for (size_t i = 0; i < s.variants.size(); i++) {
            if (i) space(o);
            o.push_back('(');
            quoted(o, s.variants[i].name);
            space(o);
            words(s.variants[i].fields, o);
            o.push_back(')');
        }
        o += "])";
        break;
    case SK::Pub:
        open(o, "pub");
        space(o);
        stmt(*s.body[0], o);
        o.push_back(')');
        break;
    case SK::PubUse: open(o, "pubuse"); word(o, s.a); o.push_back(')'); break;
    case SK::Import: open(o, "import"); word(o, s.a); o.push_back(')'); break;
    case SK::Extern:
        open(o, "extern");
        opt(o, s.ext.lib);
        word(o, s.ext.name);
        space(o);
        params(s.ext.params, o);
        opt(o, s.ext.ret);
        opt(o, s.ext.varargs);
        boolean(o, s.ext.owned);
        boolean(o, s.ext.checked);
        // A tuple in the reference printer: `("pure" "nothrow")`, and `()` when
        // the declaration promises nothing. `quoted`, not `word` -- `word`
        // prepends the space that separates fields, and the open paren is
        // already the separator.
        space(o);
        o.push_back('(');
        for (size_t i = 0; i < s.ext.effects.size(); i++) {
            if (i) space(o);
            quoted(o, s.ext.effects[i]);
        }
        o.push_back(')');
        space(o);
        o.push_back('(');
        for (size_t i = 0; i < s.ext.contracts.size(); i++) {
            if (i) space(o);
            quoted(o, s.ext.contracts[i]);
        }
        o.push_back(')');
        o.push_back(')');
        break;
    case SK::ExternStruct:
        open(o, "externstruct");
        word(o, s.a);
        space(o);
        o.push_back('[');
        for (size_t i = 0; i < s.fields.size(); i++) {
            if (i) space(o);
            o.push_back('(');
            quoted(o, s.fields[i].name);
            opt(o, s.fields[i].ann);
            o.push_back(')');
        }
        o += "])";
        break;
    }
}

}  // namespace

std::string dump(const std::vector<StmtP> &prog) {
    std::string o;
    o.reserve(prog.size() * 96 + 16);
    open(o, "prog");
    space(o);
    stmts(prog, o);
    o.push_back(')');
    return o;
}

std::string pat_text(const Pat &p) {
    std::string o;
    pat(p, o);
    return o;
}
