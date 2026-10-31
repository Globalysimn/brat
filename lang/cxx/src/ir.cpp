#include "ir.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace ir {
namespace {

//
// An interval over i64 with wrapping arithmetic, the way both backends define
// `+`, `-` and `*`: `((int64_t)((uint64_t)a + (uint64_t)b))`. When the exact
// range of a result leaves i64 the value wraps, so the interval of the wrapped
// result is not the interval of the exact result; the pass answers "unknown"
// there instead of pretending. A missed proof costs nothing, a wrong one
// deletes a check that fires.

struct IV {
    bool top = true;
    int64_t lo = 0, hi = 0;
};

IV unknown_iv() { return IV{}; }
IV cst(int64_t v) { IV x; x.top = false; x.lo = x.hi = v; return x; }
IV rng(int64_t lo, int64_t hi) { IV x; x.top = false; x.lo = lo; x.hi = hi; return x; }
bool is_empty(const IV &a) { return !a.top && a.lo > a.hi; }

std::string iv_text(const IV &a) {
    if (a.top) return "unknown";
    if (a.lo == a.hi) return std::to_string(a.lo);
    return "[" + std::to_string(a.lo) + ", " + std::to_string(a.hi) + "]";
}

bool fits(__int128 v) {
    return v >= static_cast<__int128>(INT64_MIN) && v <= static_cast<__int128>(INT64_MAX);
}

IV add_iv(const IV &a, const IV &b) {
    if (a.top || b.top) return unknown_iv();
    __int128 l = static_cast<__int128>(a.lo) + b.lo;
    __int128 h = static_cast<__int128>(a.hi) + b.hi;
    if (!fits(l) || !fits(h)) return unknown_iv();
    return rng(static_cast<int64_t>(l), static_cast<int64_t>(h));
}

IV sub_iv(const IV &a, const IV &b) {
    if (a.top || b.top) return unknown_iv();
    __int128 l = static_cast<__int128>(a.lo) - b.hi;
    __int128 h = static_cast<__int128>(a.hi) - b.lo;
    if (!fits(l) || !fits(h)) return unknown_iv();
    return rng(static_cast<int64_t>(l), static_cast<int64_t>(h));
}

IV mul_iv(const IV &a, const IV &b) {
    if (a.top || b.top) return unknown_iv();
    __int128 p1 = static_cast<__int128>(a.lo) * b.lo;
    __int128 p2 = static_cast<__int128>(a.lo) * b.hi;
    __int128 p3 = static_cast<__int128>(a.hi) * b.lo;
    __int128 p4 = static_cast<__int128>(a.hi) * b.hi;
    __int128 l = std::min(std::min(p1, p2), std::min(p3, p4));
    __int128 h = std::max(std::max(p1, p2), std::max(p3, p4));
    if (!fits(l) || !fits(h)) return unknown_iv();
    return rng(static_cast<int64_t>(l), static_cast<int64_t>(h));
}

IV neg_iv(const IV &a) {
    if (a.top || a.lo == INT64_MIN) return unknown_iv();
    return rng(-a.hi, -a.lo);
}

IV meet_iv(const IV &a, const IV &b) {
    if (a.top) return b;
    if (b.top) return a;
    return rng(std::max(a.lo, b.lo), std::min(a.hi, b.hi));
}

IV hull_iv(const IV &a, const IV &b) {
    if (a.top || b.top) return unknown_iv();
    return rng(std::min(a.lo, b.lo), std::max(a.hi, b.hi));
}

bool nonneg(const IV &a) { return !a.top && a.lo >= 0; }
bool positive(const IV &a) { return !a.top && a.lo >= 1; }
bool nonpos(const IV &a) { return !a.top && a.hi <= 0; }
bool negative(const IV &a) { return !a.top && a.hi <= -1; }

bool excludes_div_overflow(const IV &a, const IV &b) {
    if (a.top || b.top) return false;
    const bool may_min = a.lo <= INT64_MIN && a.hi >= INT64_MIN;
    const bool may_neg_one = b.lo <= -1 && b.hi >= -1;
    return !may_min || !may_neg_one;
}

// Only reached once `a` is non-negative and `b` positive, where truncating and
// Euclidean division agree, so plain C arithmetic gives the exact bounds.
IV div_iv(const IV &a, const IV &b) { return rng(a.lo / b.hi, a.hi / b.lo); }
IV mod_iv(const IV &a, const IV &b) {
    int64_t hi = std::min(a.hi, b.hi - 1);
    return rng(0, hi < 0 ? 0 : hi);
}


enum class VK { Const, Param, Arith, Cmp, Narrow, Phi, Call, Div, Mod };

struct Val {
    VK k = VK::Call;
    IV iv;
    int a = -1, b = -1;
    std::string name;
    std::string why;
    char op = 0;
    int line = 0;
    std::string cond;   // Narrow: the guard it came from
};

// The source text of a guard, for the proof. Only the shapes the pass reads are
// rendered here; anything else falls back to the tree printer.
std::string src_text(const Expr &e) {
    switch (e.k) {
    case EK::Num:
        return std::to_string(e.i);
    case EK::Bool:
        return e.flag ? "true" : "false";
    case EK::Var:
        return e.a;
    case EK::Move:
        return "move " + src_text(*e.kids[0]);
    case EK::Un:
        return e.a + src_text(*e.kids[0]);
    case EK::Bin:
        return src_text(*e.kids[0]) + " " + e.a + " " + src_text(*e.kids[1]);
    case EK::Call: {
        std::string s = e.a + "(";
        for (size_t i = 0; i < e.kids.size(); i++) {
            if (i) s += ", ";
            s += src_text(*e.kids[i]);
        }
        return s + ")";
    }
    default: {
        std::string s;
        print_expr(e, s);
        return s;
    }
    }
}


struct Builder {
    std::string fname;
    std::vector<Val> vals;
    std::map<std::string, int> env;
    Result out;
    bool bad = false;
    int seq = 0;

    int push(Val v) {
        int id = static_cast<int>(vals.size());
        vals.push_back(std::move(v));
        return id;
    }

    int konst(int64_t c) {
        Val v;
        v.k = VK::Const;
        v.iv = cst(c);
        v.name = "%c" + std::to_string(vals.size());
        v.why = "literal " + std::to_string(c);
        return push(std::move(v));
    }

    int call(const std::string &what, int line) {
        Val v;
        v.k = VK::Call;
        v.name = "%t" + std::to_string(seq++);
        v.why = what;
        v.line = line;
        return push(std::move(v));
    }

    int arith(char op, int a, int b, int line) {
        Val v;
        v.k = VK::Arith;
        v.op = op;
        v.a = a;
        v.b = b;
        v.line = line;
        v.iv = op == '+' ? add_iv(vals[a].iv, vals[b].iv)
               : op == '-' ? sub_iv(vals[a].iv, vals[b].iv)
                           : mul_iv(vals[a].iv, vals[b].iv);
        v.name = "%t" + std::to_string(seq++);
        v.why = "`" + vals[a].name + " " + op + " " + vals[b].name + "` at line " +
                std::to_string(line);
        return push(std::move(v));
    }

    int neg(int a, int line) {
        Val v;
        v.k = VK::Arith;
        v.op = '~';
        v.a = a;
        v.line = line;
        v.iv = neg_iv(vals[a].iv);
        v.name = "%t" + std::to_string(seq++);
        v.why = "`-" + vals[a].name + "` at line " + std::to_string(line);
        return push(std::move(v));
    }

    int cmp(int a, int b, const std::string &op, int line) {
        Val v;
        v.k = VK::Cmp;
        v.a = a;
        v.b = b;
        v.line = line;
        v.iv = rng(0, 1);
        v.name = "%t" + std::to_string(seq++);
        v.why = "`" + vals[a].name + " " + op + " " + vals[b].name + "` at line " +
                std::to_string(line);
        return push(std::move(v));
    }

    int narrow(int a, const IV &iv, const std::string &cond, int line) {
        Val v;
        v.k = VK::Narrow;
        v.a = a;
        v.line = line;
        v.iv = iv;
        v.cond = cond;
        v.name = "%n" + std::to_string(seq++);
        v.why = "narrow of " + vals[a].name + " by `" + cond + "` at line " +
                std::to_string(line);
        return push(std::move(v));
    }

    int phi(int a, int b, int line) {
        Val v;
        v.k = VK::Phi;
        v.a = a;
        v.b = b;
        v.line = line;
        v.iv = hull_iv(vals[a].iv, vals[b].iv);
        v.name = "%p" + std::to_string(seq++);
        v.why = "join of " + vals[a].name + " and " + vals[b].name;
        return push(std::move(v));
    }

    // A `/` or `%`. `safe` is the whole point: with a non-negative dividend and
    // a positive divisor, C's truncating operator is the Euclidean one the
    // checked helpers implement and cannot trap, so the check has nothing left
    // to catch.
    int divmod(bool is_div, int a, int b, int line, bool &safe) {
        Val v;
        v.k = is_div ? VK::Div : VK::Mod;
        v.a = a;
        v.b = b;
        v.line = line;
        v.iv = unknown_iv();
        const IV &av = vals[a].iv;
        const IV &bv = vals[b].iv;
        const bool positive_operands = nonneg(av) && positive(bv);
        const bool negative_operands = nonpos(av) && negative(bv) && excludes_div_overflow(av, bv);
        safe = positive_operands || negative_operands;
        if (positive_operands)
            v.iv = is_div ? div_iv(av, bv) : mod_iv(av, bv);
        v.name = "%t" + std::to_string(seq++);
        v.why = "`" + vals[a].name + (is_div ? " / " : " % ") + vals[b].name + "` at line " +
                std::to_string(line);
        return push(std::move(v));
    }

    void prove(int id, bool is_div, const Expr *node) {
        const Val &v = vals[id];
        const Val &a = vals[v.a];
        const Val &b = vals[v.b];
        Proof p;
        p.fn = fname;
        p.line = v.line;
        std::string t = "fn " + fname + ", line " + std::to_string(v.line) +
                        ": check dropped on `" + a.name + (is_div ? " / " : " % ") + b.name +
                        "`\n";
        t += "  " + a.name + " in " + iv_text(a.iv) + ": " + a.why + "\n";
        t += "  " + b.name + " in " + iv_text(b.iv) + ": " + b.why + "\n";
        t += "  divisor is nonzero, operand signs match, and `INT64_MIN / -1` is impossible, so `";
        t += is_div ? "/" : "%";
        t += "` matches the checked helper without a runtime check\n";
        p.text = t;
        out.unchecked.insert(node);
        out.proofs.push_back(std::move(p));
    }
};

struct Refine {
    std::string var;
    bool has_lo = false, has_hi = false;
    int64_t lo = 0, hi = 0;
};

std::string flip(const std::string &op) {
    if (op == "<") return ">";
    if (op == ">") return "<";
    if (op == "<=") return ">=";
    if (op == ">=") return "<=";
    return op;
}

// What a guard says about one variable, on the branch where it holds (or where
// it fails). `&&` splits when it holds; a negated `&&` does not, because the
// branch then only says "one of them is false".
void refines(const Expr &cond, bool positive, std::vector<Refine> &out) {
    if (cond.k != EK::Bin) return;
    const std::string &op0 = cond.a;
    if (op0 == "&&" && positive) {
        refines(*cond.kids[0], true, out);
        refines(*cond.kids[1], true, out);
        return;
    }
    if (op0 != "<" && op0 != "<=" && op0 != ">" && op0 != ">=" && op0 != "==" && op0 != "!=")
        return;
    const Expr &l = *cond.kids[0], &r = *cond.kids[1];
    const Expr *var = nullptr;
    int64_t c = 0;
    std::string op = op0;
    if (l.k == EK::Var && r.k == EK::Num && !r.is_float) {
        var = &l;
        c = r.i;
    } else if (r.k == EK::Var && l.k == EK::Num && !l.is_float) {
        var = &r;
        c = l.i;
        op = flip(op);
    } else {
        return;
    }
    Refine f;
    f.var = var->a;
    if (positive) {
        if (op == "<") {
            if (c == INT64_MIN) return;
            f.has_hi = true;
            f.hi = c - 1;
        } else if (op == "<=") {
            f.has_hi = true;
            f.hi = c;
        } else if (op == ">") {
            if (c == INT64_MAX) return;
            f.has_lo = true;
            f.lo = c + 1;
        } else if (op == ">=") {
            f.has_lo = true;
            f.lo = c;
        } else if (op == "==") {
            f.has_lo = f.has_hi = true;
            f.lo = f.hi = c;
        } else {
            return;
        }
    } else {
        if (op == "<") {
            f.has_lo = true;
            f.lo = c;
        } else if (op == "<=") {
            if (c == INT64_MAX) return;
            f.has_lo = true;
            f.lo = c + 1;
        } else if (op == ">") {
            f.has_hi = true;
            f.hi = c;
        } else if (op == ">=") {
            if (c == INT64_MIN) return;
            f.has_hi = true;
            f.hi = c - 1;
        } else if (op == "!=") {
            f.has_lo = f.has_hi = true;
            f.lo = f.hi = c;
        } else {
            return;
        }
    }
    out.push_back(std::move(f));
}

std::vector<const Stmt *> view(const std::vector<StmtP> &v) {
    std::vector<const Stmt *> out;
    out.reserve(v.size());
    for (const auto &p : v) out.push_back(p.get());
    return out;
}

const Stmt *real_else(const Stmt &s) {
    if (!s.els) return nullptr;
    if (s.els->k == SK::Block && s.els->body.empty()) return nullptr;
    return s.els.get();
}

void collect_assigned(const Stmt &s, std::set<std::string> &out);
void collect_vars(const Expr &e, std::set<std::string> &out);

// True when this statement is the only shape the counter step accepts:
// `counter = counter + 1`, standing last in the body. Anywhere else in the body
// and the statements after it would read a counter one past the head range,
// which is where an unsound interval comes from.
bool is_step(const Stmt &s, const std::string &name) {
    if (s.k != SK::Set || s.a != name || !s.e1) return false;
    const Expr &e = *s.e1;
    if (e.k != EK::Bin || e.a != "+" || e.kids.size() != 2) return false;
    if (e.kids[0]->k != EK::Var || e.kids[0]->a != name) return false;
    const Expr &one = *e.kids[1];
    return one.k == EK::Num && !one.is_float && one.i == 1;
}

// Every write to `name` in the body, and whether one of them is a step.
struct Writes {
    int count = 0;
    bool all_steps = true;
    bool nested_loop = false;
};

void scan_writes(const Stmt &s, const std::string &name, Writes &w) {
    switch (s.k) {
    case SK::Let:
    case SK::LetTmp:
    case SK::Set:
        if (s.a == name) {
            w.count++;
            if (!is_step(s, name)) w.all_steps = false;
        }
        break;
    case SK::While:
    case SK::ForRange:
    case SK::ForIter:
        w.nested_loop = true;
        break;
    default:
        break;
    }
    for (const Stmt *t : view(s.body)) scan_writes(*t, name, w);
    for (const Stmt *t : view(s.body2)) scan_writes(*t, name, w);
    if (s.els) scan_writes(*s.els, name, w);
}

struct Walker {
    Builder &b;
    int line = 0;
    int loops = 0;

    bool bail() {
        b.bad = true;
        return false;
    }

    int val(const Expr &e);
    bool block(const std::vector<const Stmt *> &body);
    void apply(const std::vector<Refine> &rs, const std::string &cond, int at);
};

void Walker::apply(const std::vector<Refine> &rs, const std::string &cond, int at) {
    for (const auto &r : rs) {
        auto it = b.env.find(r.var);
        if (it == b.env.end()) continue;
        IV want = rng(r.has_lo ? r.lo : INT64_MIN, r.has_hi ? r.hi : INT64_MAX);
        IV iv = meet_iv(b.vals[it->second].iv, want);
        if (is_empty(iv)) continue;   // the branch cannot be reached: say nothing
        b.env[r.var] = b.narrow(it->second, iv, cond, at);
    }
}

int Walker::val(const Expr &e) {
    switch (e.k) {
    case EK::Num:
        return e.is_float ? -1 : b.konst(e.i);
    case EK::Bool:
        return b.konst(e.flag ? 1 : 0);
    case EK::Var: {
        auto it = b.env.find(e.a);
        return it == b.env.end() ? -1 : it->second;
    }
    case EK::Move:
        return val(*e.kids[0]);
    case EK::Un: {
        int x = val(*e.kids[0]);
        if (x < 0) return -1;
        if (e.a == "-") return b.neg(x, line);
        if (e.a == "!") return b.cmp(x, b.konst(0), "!", line);
        return -1;
    }
    case EK::Bin: {
        const std::string &op = e.a;
        bool cmp = op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" ||
                   op == "!=" || op == "&&" || op == "||";
        bool div = op == "/" || op == "%";
        bool ar = op == "+" || op == "-" || op == "*";
        if (!cmp && !div && !ar) return -1;
        int a = val(*e.kids[0]);
        if (a < 0) return -1;
        int c = val(*e.kids[1]);
        if (c < 0) return -1;
        if (ar) return b.arith(op[0], a, c, line);
        if (cmp) return b.cmp(a, c, op, line);
        bool safe = false;
        int id = b.divmod(op == "/", a, c, line, safe);
        if (safe) b.prove(id, op == "/", &e);
        return id;
    }
    case EK::Call:
        return b.call("call `" + e.a + "()` at line " + std::to_string(line), line);
    default:
        return -1;
    }
}

bool Walker::block(const std::vector<const Stmt *> &body) {
    for (const Stmt *s : body) {
        line = s->line;
        switch (s->k) {
        case SK::Let:
        case SK::LetTmp:
        case SK::Set: {
            if (s->k == SK::Set && s->a.find('.') != std::string::npos) return bail();
            int v = val(*s->e1);
            if (v < 0) return bail();
            b.env[s->a] = v;
            break;
        }
        case SK::Return: {
            int v = val(*s->e1);
            if (v < 0) return bail();
            return true;   // nothing after this statement runs
        }
        case SK::Expr: {
            if (s->e1->k == EK::Call && s->e1->a == "print") break;   // fprint, no value
            int v = val(*s->e1);
            if (v < 0) return bail();
            break;
        }
        case SK::If: {
            int c = val(*s->e1);
            if (c < 0) return bail();
            std::vector<Refine> rt, re;
            refines(*s->e1, true, rt);
            refines(*s->e1, false, re);
            const std::string guard = src_text(*s->e1);
            std::map<std::string, int> saved = b.env;
            int at = line;
            apply(rt, guard, at);
            bool ex_t = block(view(s->body));
            if (b.bad) return false;
            std::map<std::string, int> env_t = b.env;
            b.env = saved;
            apply(re, "!(" + guard + ")", at);
            const Stmt *els = real_else(*s);
            bool ex_e = false;
            if (els) {
                ex_e = els->k == SK::Block ? block(view(els->body)) : block({els});
                if (b.bad) return false;
            }
            std::map<std::string, int> env_e = b.env;
            if (ex_t && ex_e) {
                b.env = saved;
                return true;
            }
            if (ex_t) {
                b.env = env_e;
                break;
            }
            if (ex_e) {
                b.env = env_t;
                break;
            }
            b.env = saved;
            std::vector<std::string> keys;
            for (const auto &kv : env_t) keys.push_back(kv.first);
            for (const auto &kv : env_e)
                if (!env_t.count(kv.first)) keys.push_back(kv.first);
            for (const auto &k : keys) {
                int it = env_t.count(k) ? env_t[k] : (saved.count(k) ? saved[k] : -1);
                int ie = env_e.count(k) ? env_e[k] : (saved.count(k) ? saved[k] : -1);
                if (it >= 0 && ie >= 0) {
                    b.env[k] = it == ie ? it : b.phi(it, ie, at);
                } else if (it >= 0 || ie >= 0) {
                    b.env[k] = b.call("bound on one branch only", at);
                } else {
                    b.env.erase(k);
                }
            }
            break;
        }
        case SK::Break:
        case SK::Continue:
            // Legal only inside the loop this step models: the body runs once per
            // head range and what it writes is forgotten afterwards, so a jump out
            // of the middle of it cannot make a later interval wrong.
            if (!loops) return bail();
            break;
        case SK::While: {
            const Expr &cond = *s->e1;
            if (cond.k != EK::Bin || cond.a != "<" || cond.kids[0]->k != EK::Var) return bail();
            const std::string &counter = cond.kids[0]->a;
            auto it = b.env.find(counter);
            if (it == b.env.end()) return bail();
            const int entry = it->second;
            const IV entry_iv = b.vals[entry].iv;
            if (entry_iv.top) return bail();        // no lower bound to carry into the body

            std::vector<const Stmt *> body = view(s->body);
            if (body.empty() || !is_step(*body.back(), counter)) return bail();
            Writes w;
            for (const Stmt *t : body) scan_writes(*t, counter, w);
            if (w.count != 1 || !w.all_steps || w.nested_loop) return bail();

            // The bound must not move while the body runs, and must not mention the
            // counter itself, or the range derived here would not hold on the second
            // iteration.
            std::set<std::string> assigned, guard_vars;
            for (const Stmt *t : body) collect_assigned(*t, assigned);
            collect_vars(*cond.kids[1], guard_vars);
            if (guard_vars.count(counter)) return bail();
            for (const auto &g : guard_vars)
                if (assigned.count(g)) return bail();

            int bid = val(*cond.kids[1]);
            if (bid < 0) return bail();
            const IV biv = b.vals[bid].iv;
            if (!biv.top && biv.hi == INT64_MIN) break;      // `i < i64::MIN`: never runs
            const int64_t head_hi = biv.top ? INT64_MAX : biv.hi - 1;
            if (head_hi < entry_iv.lo) break;                // guard false on entry
            const std::string guard = src_text(cond);

            b.env[counter] = b.narrow(entry, rng(entry_iv.lo, head_hi), guard, line);
            for (const auto &name : assigned)
                if (name != counter)
                    b.env[name] =
                        b.call("assigned inside the loop at line " + std::to_string(line), line);
            loops++;
            block(body);
            loops--;
            if (b.bad) return false;
            // The body ran zero or more times: everything it wrote is unknown
            // afterwards. The counter keeps the lower bound the entry and the step
            // give it, and no upper bound, since the loop may stop anywhere.
            for (const auto &name : assigned)
                b.env[name] = b.call("assigned inside the loop at line " + std::to_string(line), line);
            b.env[counter] = b.narrow(entry, rng(entry_iv.lo, INT64_MAX), guard, line);
            break;
        }
        case SK::ForRange: {
            // `for i in lo..hi` is the same counter loop as `while i < hi`: the
            // counter is fresh, the step is +1 and the head runs to `hi - 1`.
            const std::string &counter = s->a;
            std::vector<const Stmt *> body = view(s->body);
            Writes w;
            for (const Stmt *t : body) scan_writes(*t, counter, w);
            if (w.count || w.nested_loop) return bail();

            std::set<std::string> assigned, bound_vars;
            for (const Stmt *t : body) collect_assigned(*t, assigned);
            collect_vars(*s->e1, bound_vars);
            collect_vars(*s->e2, bound_vars);
            if (bound_vars.count(counter)) return bail();
            for (const auto &g : bound_vars)
                if (assigned.count(g)) return bail();

            int lo_id = val(*s->e1);
            if (lo_id < 0) return bail();
            const IV lo_iv = b.vals[lo_id].iv;
            if (lo_iv.top) return bail();          // no lower bound to carry into the body
            int hi_id = val(*s->e2);
            if (hi_id < 0) return bail();
            const IV hi_iv = b.vals[hi_id].iv;
            const std::string guard =
                "for " + counter + " in " + src_text(*s->e1) + ".." + src_text(*s->e2);

            const int entry = lo_id;
            if (!hi_iv.top && hi_iv.hi == INT64_MIN) {
                b.env.erase(counter);              // an empty range: the body never runs
                break;
            }
            const int64_t head_hi = hi_iv.top ? INT64_MAX : hi_iv.hi - 1;
            if (head_hi < lo_iv.lo) {
                b.env.erase(counter);
                break;
            }
            b.env[counter] = b.narrow(entry, rng(lo_iv.lo, head_hi), guard, line);
            for (const auto &name : assigned)
                if (name != counter)
                    b.env[name] =
                        b.call("assigned inside the loop at line " + std::to_string(line), line);
            loops++;
            block(body);
            loops--;
            if (b.bad) return false;
            // The counter is bound by the loop, not before it, and everything the
            // body wrote is unknown afterwards.
            b.env.erase(counter);
            for (const auto &name : assigned)
                b.env[name] = b.call("assigned inside the loop at line " + std::to_string(line), line);
            break;
        }
        default:
            // Ownership, defer, try, spawn, nested loops: outside this step.
            return bail();
        }
    }
    return false;
}

void collect_assigned(const Stmt &s, std::set<std::string> &out) {
    if (s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::Set || s.k == SK::ForRange ||
        s.k == SK::ForIter)
        out.insert(s.a);
    for (const Stmt *t : view(s.body)) collect_assigned(*t, out);
    for (const Stmt *t : view(s.body2)) collect_assigned(*t, out);
    if (s.els) collect_assigned(*s.els, out);
}

void collect_vars(const Expr &e, std::set<std::string> &out) {
    if (e.k == EK::Var) out.insert(e.a);
    for (const auto &k : e.kids) collect_vars(*k, out);
}

}  // namespace

bool analyze(const std::string &fn, const std::vector<Param> &params,
             const std::vector<StmtP> &body, Result &out) {
    out = Result{};
    Builder b;
    b.fname = fn;
    for (const auto &p : params) {
        if (p.def) return false;   // a default is filled in at the call site, not here
        Val v;
        v.k = VK::Param;
        v.iv = unknown_iv();
        v.name = "%" + p.name + ".0";
        v.why = "parameter `" + p.name + "`";
        b.env[p.name] = b.push(std::move(v));
    }
    Walker w{b};
    w.block(view(body));
    if (b.bad) return false;
    out = std::move(b.out);
    return true;
}

}  // namespace ir
