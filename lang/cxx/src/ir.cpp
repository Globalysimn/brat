#include "ir.h"

#include <algorithm>
#include <cstdint>
#include <functional>
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
        if (e.a == "~") return b.call("`~` at line " + std::to_string(line), line);
        return -1;
    }
    case EK::Bin: {
        const std::string &op = e.a;
        bool cmp = op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" ||
                   op == "!=" || op == "&&" || op == "||";
        bool div = op == "/" || op == "%";
        bool ar = op == "+" || op == "-" || op == "*";
        bool bw = op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>";
        if (!cmp && !div && !ar && !bw) return -1;
        int a = val(*e.kids[0]);
        if (a < 0) return -1;
        int c = val(*e.kids[1]);
        if (c < 0) return -1;
        if (bw) {
            int id = b.call("`" + op + "` at line " + std::to_string(line), line);
            if (op == "&") {
                const IV &av = b.vals[a].iv, &cv = b.vals[c].iv;
                if (nonneg(cv)) b.vals[id].iv = rng(0, nonneg(av) ? std::min(av.hi, cv.hi) : cv.hi);
                else if (nonneg(av)) b.vals[id].iv = rng(0, av.hi);
            }
            return id;
        }
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

            const int saved_counter = b.env.count(counter) ? b.env[counter] : -1;
            const int entry = lo_id;
            if (!hi_iv.top && hi_iv.hi == INT64_MIN) {
                if (saved_counter >= 0) b.env[counter] = saved_counter;
                else b.env.erase(counter);
                break;
            }
            const int64_t head_hi = hi_iv.top ? INT64_MAX : hi_iv.hi - 1;
            if (head_hi < lo_iv.lo) {
                if (saved_counter >= 0) b.env[counter] = saved_counter;
                else b.env.erase(counter);
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
            // In C emission, `for` reuses an outer binding of the same name when
            // one exists, so an outer `counter` is clobbered whenever the loop
            // may run at least one iteration.
            if (saved_counter >= 0)
                b.env[counter] = b.call("for-loop counter at line " + std::to_string(line), line);
            else
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

// `-O2` reduced-product relational abstract interpreter:
// wrapping interval `IV` x Granger congruence `mZ + r` x potential-graph DBM
// (`v_i - v_j <= c`), plus interprocedural return summaries.
namespace o2 {

struct Cong {
    bool bot = false;
    uint64_t m = 1;   // 0 => singleton constant (int64_t)r; 1 => top; >=2 => mZ + r (0 <= r < m)
    uint64_t r = 0;
};

Cong top_cong() { return Cong{false, 1, 0}; }
Cong cst_cong(int64_t v) { return Cong{false, 0, static_cast<uint64_t>(v)}; }

uint64_t gcd64(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

uint64_t euclid_mod(__int128 x, uint64_t m) {
    if (m <= 1) return 0;
    __int128 rem = x % static_cast<__int128>(m);
    if (rem < 0) rem += static_cast<__int128>(m);
    return static_cast<uint64_t>(rem);
}

Cong norm_cong(uint64_t m, __int128 r) {
    if (m == 0) return Cong{false, 0, static_cast<uint64_t>(static_cast<int64_t>(r))};
    if (m == 1) return top_cong();
    return Cong{false, m, euclid_mod(r, m)};
}

// Project congruence across potential 2^64 wrap: only gcd(m, 2^64) = lowbit(m) survives wrap.
Cong wrap_cong(const Cong &c) {
    if (c.bot || c.m <= 1) return c;
    uint64_t pow2 = c.m & (~c.m + 1ULL);
    if (pow2 <= 1) return top_cong();
    return Cong{false, pow2, c.r % pow2};
}

std::string cong_text(const Cong &c) {
    if (c.bot) return "empty";
    if (c.m == 0) return "== " + std::to_string(static_cast<int64_t>(c.r));
    if (c.m == 1) return "Z";
    return std::to_string(c.m) + "Z + " + std::to_string(c.r);
}

Cong add_cong(const Cong &a, const Cong &b, bool wrapped) {
    if (a.bot || b.bot) return Cong{true, 1, 0};
    if (a.m == 0 && b.m == 0) {
        uint64_t res = a.r + b.r;
        return Cong{false, 0, res};
    }
    uint64_t g = gcd64(a.m, b.m);
    __int128 ra = (a.m == 0) ? static_cast<__int128>(static_cast<int64_t>(a.r)) : static_cast<__int128>(a.r);
    __int128 rb = (b.m == 0) ? static_cast<__int128>(static_cast<int64_t>(b.r)) : static_cast<__int128>(b.r);
    Cong out = norm_cong(g, ra + rb);
    return wrapped ? wrap_cong(out) : out;
}

Cong sub_cong(const Cong &a, const Cong &b, bool wrapped) {
    if (a.bot || b.bot) return Cong{true, 1, 0};
    if (a.m == 0 && b.m == 0) {
        uint64_t res = a.r - b.r;
        return Cong{false, 0, res};
    }
    uint64_t g = gcd64(a.m, b.m);
    __int128 ra = (a.m == 0) ? static_cast<__int128>(static_cast<int64_t>(a.r)) : static_cast<__int128>(a.r);
    __int128 rb = (b.m == 0) ? static_cast<__int128>(static_cast<int64_t>(b.r)) : static_cast<__int128>(b.r);
    Cong out = norm_cong(g, ra - rb);
    return wrapped ? wrap_cong(out) : out;
}

Cong neg_cong(const Cong &a, bool wrapped) {
    return sub_cong(cst_cong(0), a, wrapped);
}

uint64_t safe_mul_gcd(uint64_t x, uint64_t y) {
    if (x == 0 || y == 0) return 0;
    unsigned __int128 p = static_cast<unsigned __int128>(x) * static_cast<unsigned __int128>(y);
    if (p > (1ULL << 62)) return 1;
    return static_cast<uint64_t>(p);
}

Cong mul_cong(const Cong &a, const Cong &b, bool wrapped) {
    if (a.bot || b.bot) return Cong{true, 1, 0};
    if (a.m == 0 && b.m == 0) return Cong{false, 0, a.r * b.r};
    __int128 ra = (a.m == 0) ? static_cast<__int128>(static_cast<int64_t>(a.r)) : static_cast<__int128>(a.r);
    __int128 rb = (b.m == 0) ? static_cast<__int128>(static_cast<int64_t>(b.r)) : static_cast<__int128>(b.r);
    uint64_t ara = static_cast<uint64_t>(ra < 0 ? -ra : ra);
    uint64_t arb = static_cast<uint64_t>(rb < 0 ? -rb : rb);
    uint64_t t1 = safe_mul_gcd(a.m, b.m);
    uint64_t t2 = safe_mul_gcd(a.m, arb);
    uint64_t t3 = safe_mul_gcd(b.m, ara);
    if (t1 == 1 || t2 == 1 || t3 == 1) {
        Cong fb = top_cong();
        return wrapped ? wrap_cong(fb) : fb;
    }
    uint64_t g = gcd64(t1, gcd64(t2, t3));
    __int128 prod = ra * rb;
    if (!fits(prod) && !wrapped) return wrap_cong(top_cong());
    Cong out = (g == 0) ? Cong{false, 0, static_cast<uint64_t>(static_cast<int64_t>(prod))}
                        : norm_cong(g, prod);
    return wrapped ? wrap_cong(out) : out;
}

Cong join_cong(const Cong &a, const Cong &b) {
    if (a.bot) return b;
    if (b.bot) return a;
    __int128 ra = (a.m == 0) ? static_cast<__int128>(static_cast<int64_t>(a.r))
                             : static_cast<__int128>(a.r);
    __int128 rb = (b.m == 0) ? static_cast<__int128>(static_cast<int64_t>(b.r))
                             : static_cast<__int128>(b.r);
    __int128 diff = ra >= rb ? (ra - rb) : (rb - ra);
    if (diff > static_cast<__int128>(1ULL << 62)) return top_cong();
    uint64_t g = gcd64(gcd64(a.m, b.m), static_cast<uint64_t>(diff));
    return norm_cong(g, ra);
}

// Chinese Remainder Theorem meet `(m1*Z + r1) ⊓ (m2*Z + r2)`
Cong meet_cong(const Cong &a, const Cong &b) {
    if (a.bot || b.bot) return Cong{true, 1, 0};
    if (a.m == 1) return b;
    if (b.m == 1) return a;
    if (a.m == 0) {
        int64_t ca = static_cast<int64_t>(a.r);
        if (b.m == 0) return ca == static_cast<int64_t>(b.r) ? a : Cong{true, 1, 0};
        return euclid_mod(ca, b.m) == b.r ? a : Cong{true, 1, 0};
    }
    if (b.m == 0) return meet_cong(b, a);
    uint64_t g = gcd64(a.m, b.m);
    if ((a.r % g) != (b.r % g)) return Cong{true, 1, 0};
    unsigned __int128 lcm128 = static_cast<unsigned __int128>(a.m / g) * b.m;
    if (lcm128 > (1ULL << 62)) return a.m >= b.m ? a : b;
    uint64_t lcm = static_cast<uint64_t>(lcm128);
    // Extended Euclidean solve `(a.m / g) * k === (b.r - a.r) / g (mod b.m / g)`
    int64_t m1 = static_cast<int64_t>(a.m / g);
    int64_t m2 = static_cast<int64_t>(b.m / g);
    __int128 old_r = m1, r = m2;
    __int128 old_s = 1, s = 0;
    while (r != 0) {
        __int128 q = old_r / r;
        __int128 tmp_r = old_r - q * r; old_r = r; r = tmp_r;
        __int128 tmp_s = old_s - q * s; old_s = s; s = tmp_s;
    }
    __int128 diff = (static_cast<__int128>(b.r) - static_cast<__int128>(a.r)) / static_cast<__int128>(g);
    uint64_t k = euclid_mod(diff * old_s, static_cast<uint64_t>(m2));
    __int128 combined = static_cast<__int128>(a.r) + static_cast<__int128>(a.m) * static_cast<__int128>(k);
    return norm_cong(lcm, combined);
}

// Reduced product ρ(IV, Cong)
void reduce_iv_cong(IV &iv, Cong &cg) {
    if (cg.bot) {
        iv = rng(1, 0);
        return;
    }
    if (is_empty(iv)) {
        cg.bot = true;
        return;
    }
    if (iv.top) {
        if (cg.m == 0) iv = cst(static_cast<int64_t>(cg.r));
        return;
    }
    if (cg.m == 0) {
        iv = meet_iv(iv, cst(static_cast<int64_t>(cg.r)));
        if (is_empty(iv)) cg.bot = true;
        return;
    }
    if (cg.m == 1) {
        if (iv.lo == iv.hi) cg = cst_cong(iv.lo);
        return;
    }
    uint64_t r_lo = euclid_mod(iv.lo, cg.m);
    uint64_t d_lo = (cg.r >= r_lo) ? (cg.r - r_lo) : (cg.m - (r_lo - cg.r));
    __int128 nlo = static_cast<__int128>(iv.lo) + static_cast<__int128>(d_lo);

    uint64_t r_hi = euclid_mod(iv.hi, cg.m);
    uint64_t d_hi = (r_hi >= cg.r) ? (r_hi - cg.r) : (cg.m - (cg.r - r_hi));
    __int128 nhi = static_cast<__int128>(iv.hi) - static_cast<__int128>(d_hi);

    if (nlo > nhi || !fits(nlo) || !fits(nhi)) {
        iv = rng(1, 0);
        cg.bot = true;
        return;
    }
    iv = rng(static_cast<int64_t>(nlo), static_cast<int64_t>(nhi));
    if (iv.lo == iv.hi) cg = cst_cong(iv.lo);
}

bool cong_nonzero(const Cong &c) {
    if (c.bot) return true;
    if (c.m == 0) return static_cast<int64_t>(c.r) != 0;
    return c.m >= 2 && c.r != 0;
}

// Difference-Bound Matrix (Potential Graph Zone Domain) over __int128
static constexpr int MAX_DBM = 48;
static const __int128 DBM_INF = (static_cast<__int128>(1) << 110);

struct DBM {
    int n = 1;   // node 0 is constant zero `z0 = 0`
    std::map<std::string, int> ids;
    std::vector<std::string> names{"0"};
    __int128 m[MAX_DBM][MAX_DBM];
    bool inconsistent = false;

    DBM() {
        for (int i = 0; i < MAX_DBM; ++i)
            for (int j = 0; j < MAX_DBM; ++j)
                m[i][j] = (i == j) ? 0 : DBM_INF;
    }

    int node(const std::string &nm, bool nonneg_init = false) {
        auto it = ids.find(nm);
        if (it != ids.end()) return it->second;
        if (n >= MAX_DBM) return -1;
        int id = n++;
        ids[nm] = id;
        names.push_back(nm);
        for (int k = 0; k < n; ++k) {
            m[id][k] = (id == k) ? 0 : DBM_INF;
            m[k][id] = (id == k) ? 0 : DBM_INF;
        }
        // Every int64_t variable `v` satisfies INT64_MIN <= v <= INT64_MAX:
        // v - 0 <= INT64_MAX, and 0 - v <= -INT64_MIN (= 2^63)
        m[id][0] = static_cast<__int128>(INT64_MAX);
        m[0][id] = nonneg_init ? 0 : (-static_cast<__int128>(INT64_MIN));
        return id;
    }

    void forget(const std::string &nm) {
        auto it = ids.find(nm);
        if (it == ids.end()) return;
        int id = it->second;
        for (int k = 0; k < n; ++k) {
            m[id][k] = (id == k) ? 0 : DBM_INF;
            m[k][id] = (id == k) ? 0 : DBM_INF;
        }
        m[id][0] = static_cast<__int128>(INT64_MAX);
        m[0][id] = -static_cast<__int128>(INT64_MIN);
    }

    // Add `u - w <= c`
    void add_diff(int u, int w, __int128 c) {
        if (u < 0 || w < 0 || inconsistent) return;
        if (c < m[u][w]) m[u][w] = c;
    }

    void set_iv(int u, const IV &iv) {
        if (u <= 0 || inconsistent || iv.top) return;
        if (is_empty(iv)) { inconsistent = true; return; }
        add_diff(u, 0, static_cast<__int128>(iv.hi));
        add_diff(0, u, -static_cast<__int128>(iv.lo));
    }

    // Canonical shortest-path closure (Floyd-Warshall)
    void close() {
        if (inconsistent) return;
        for (int k = 0; k < n; ++k) {
            for (int i = 0; i < n; ++i) {
                if (m[i][k] >= DBM_INF) continue;
                for (int j = 0; j < n; ++j) {
                    if (m[k][j] >= DBM_INF) continue;
                    __int128 cand = m[i][k] + m[k][j];
                    if (cand < m[i][j]) m[i][j] = cand;
                }
            }
        }
        for (int i = 0; i < n; ++i) {
            if (m[i][i] < 0) {
                inconsistent = true;
                return;
            }
        }
    }

    bool diff_le(int u, int w, __int128 bound) const {
        if (inconsistent) return true;
        if (u < 0 || w < 0 || u >= n || w >= n) return false;
        return m[u][w] <= bound;
    }

    IV extract_iv(int u) const {
        if (u <= 0 || u >= n || inconsistent) return unknown_iv();
        __int128 lo = -m[0][u];
        __int128 hi = m[u][0];
        if (!fits(lo) || !fits(hi)) return unknown_iv();
        return rng(static_cast<int64_t>(lo), static_cast<int64_t>(hi));
    }

    // Exact range of `u - w` when guaranteed non-wrapping in int64_t.
    IV extract_diff_iv(int u, int w) const {
        if (u < 0 || w < 0 || u >= n || w >= n || inconsistent) return unknown_iv();
        __int128 lo = -m[w][u];
        __int128 hi = m[u][w];
        if (m[w][u] >= DBM_INF || m[u][w] >= DBM_INF) return unknown_iv();
        if (!fits(lo) || !fits(hi)) return unknown_iv();
        return rng(static_cast<int64_t>(lo), static_cast<int64_t>(hi));
    }

    static DBM join(const DBM &a, const DBM &b) {
        if (a.inconsistent) return b;
        if (b.inconsistent) return a;
        DBM res;
        // Align shared node universe
        for (int i = 1; i < a.n; ++i) res.node(a.names[i]);
        for (int i = 1; i < b.n; ++i) res.node(b.names[i]);
        for (int i = 0; i < res.n; ++i) {
            for (int j = 0; j < res.n; ++j) {
                if (i == j) { res.m[i][j] = 0; continue; }
                auto ia = (i == 0) ? 0 : (a.ids.count(res.names[i]) ? a.ids.at(res.names[i]) : -1);
                auto ja = (j == 0) ? 0 : (a.ids.count(res.names[j]) ? a.ids.at(res.names[j]) : -1);
                auto ib = (i == 0) ? 0 : (b.ids.count(res.names[i]) ? b.ids.at(res.names[i]) : -1);
                auto jb = (j == 0) ? 0 : (b.ids.count(res.names[j]) ? b.ids.at(res.names[j]) : -1);
                __int128 va = (ia >= 0 && ja >= 0) ? a.m[ia][ja] : DBM_INF;
                __int128 vb = (ib >= 0 && jb >= 0) ? b.m[ib][jb] : DBM_INF;
                res.m[i][j] = std::max(va, vb);
            }
        }
        return res;
    }
};

struct Val2 {
    VK k = VK::Call;
    IV iv;
    Cong cg;
    int dbm_node = -1;
    int origin = -1;
    int a = -1, b = -1;
    std::string name;
    std::string src_var;
    std::string why;
    char op = 0;
    int line = 0;
};

std::string dom_text(const Val2 &v) {
    std::string s = iv_text(v.iv);
    if (!v.cg.bot && v.cg.m >= 2) s += " ⊓ (" + cong_text(v.cg) + ")";
    return s;
}

struct Builder2 {
    std::string fname;
    std::vector<Val2> vals;
    std::map<std::string, int> env;
    std::set<std::string> vecs;
    const std::map<std::string, FnSummary> &summaries;
    DBM dbm;
    Result out;
    bool bad = false;
    int seq = 0;
    bool saw_return = false;
    IV ret_iv;
    Cong ret_cg;

    explicit Builder2(const std::map<std::string, FnSummary> &sums) : summaries(sums) {
        ret_iv = rng(1, 0);
        ret_cg.bot = true;
    }

    int push(Val2 v) {
        reduce_iv_cong(v.iv, v.cg);
        if (v.dbm_node > 0) dbm.set_iv(v.dbm_node, v.iv);
        int id = static_cast<int>(vals.size());
        if (v.origin < 0) v.origin = id;
        vals.push_back(std::move(v));
        return id;
    }

    void sync_from_dbm() {
        dbm.close();
        if (dbm.inconsistent) return;
        for (auto &[vname, vid] : env) {
            int dn = vals[vid].dbm_node;
            if (dn > 0) {
                IV div = dbm.extract_iv(dn);
                vals[vid].iv = meet_iv(vals[vid].iv, div);
                reduce_iv_cong(vals[vid].iv, vals[vid].cg);
                dbm.set_iv(dn, vals[vid].iv);
            }
        }
    }

    int konst(int64_t c) {
        Val2 v;
        v.k = VK::Const;
        v.iv = cst(c);
        v.cg = cst_cong(c);
        v.name = "%c" + std::to_string(vals.size());
        v.why = "literal " + std::to_string(c);
        if (dbm.n < MAX_DBM - 8) {
            v.dbm_node = dbm.node(v.name);
            dbm.add_diff(v.dbm_node, 0, c);
            dbm.add_diff(0, v.dbm_node, -static_cast<__int128>(c));
        }
        return push(std::move(v));
    }

    int len_of(const std::string &vname, int line) {
        std::string key = "len(" + vname + ")";
        int dn = dbm.node(key, /*nonneg_init=*/true);
        Val2 v;
        v.k = VK::Call;
        v.iv = rng(0, INT64_MAX);
        v.cg = top_cong();
        v.dbm_node = dn;
        v.line = line;
        v.name = "%" + key;
        v.why = "`" + key + "` (non-negative vector length)";
        return push(std::move(v));
    }

    int call_val(const std::string &callee, int line) {
        Val2 v;
        v.k = VK::Call;
        v.line = line;
        v.name = "%t" + std::to_string(seq++);
        auto it = summaries.find(callee);
        if (it != summaries.end() && it->second.valid) {
            const FnSummary &s = it->second;
            v.iv = s.top ? unknown_iv() : rng(s.lo, s.hi);
            v.cg = Cong{false, s.mod, s.rem};
            reduce_iv_cong(v.iv, v.cg);
            v.why = "interprocedural summary of `" + callee + "()` in " + dom_text(v);
        } else {
            v.iv = unknown_iv();
            v.cg = top_cong();
            v.why = "call `" + callee + "()` at line " + std::to_string(line);
        }
        if (dbm.n < MAX_DBM - 6) v.dbm_node = dbm.node(v.name);
        return push(std::move(v));
    }

    int arith(char op, int a, int b, int line) {
        Val2 v;
        v.k = VK::Arith;
        v.op = op;
        v.a = a;
        v.b = b;
        v.line = line;
        const Val2 &va = vals[a];
        const Val2 &vb = vals[b];
        IV raw_iv = op == '+' ? add_iv(va.iv, vb.iv)
                  : op == '-' ? sub_iv(va.iv, vb.iv)
                              : mul_iv(va.iv, vb.iv);
        // Relational DBM sharpening for `u - w`.
        if (op == '-' && va.dbm_node >= 0 && vb.dbm_node >= 0) {
            dbm.close();
            IV diff_iv = dbm.extract_diff_iv(va.dbm_node, vb.dbm_node);
            raw_iv = meet_iv(raw_iv, diff_iv);
        }
        bool wrapped = raw_iv.top;
        v.iv = raw_iv;
        v.cg = op == '+' ? add_cong(va.cg, vb.cg, wrapped)
             : op == '-' ? sub_cong(va.cg, vb.cg, wrapped)
                         : mul_cong(va.cg, vb.cg, wrapped);
        v.name = "%t" + std::to_string(seq++);
        if (!wrapped && (op == '+' || op == '-') && dbm.n < MAX_DBM - 4) {
            if (vb.k == VK::Const && va.dbm_node > 0) {
                int dn = dbm.node(v.name);
                __int128 c = (op == '+') ? static_cast<__int128>(vb.iv.lo)
                                         : -static_cast<__int128>(vb.iv.lo);
                dbm.add_diff(dn, va.dbm_node, c);
                dbm.add_diff(va.dbm_node, dn, -c);
                v.dbm_node = dn;
            }
        }
        std::string extra;
        if (op == '-' && va.dbm_node >= 0 && vb.dbm_node >= 0 && !raw_iv.top &&
            (va.iv.top || vb.iv.top || sub_iv(va.iv, vb.iv).lo < raw_iv.lo)) {
            extra = " (DBM zone bound `" + dbm.names[va.dbm_node] + " - " +
                    dbm.names[vb.dbm_node] + "` in " + iv_text(raw_iv) + ")";
        }
        v.why = "`" + va.name + " " + op + " " + vb.name + "` at line " +
                std::to_string(line) + extra;
        return push(std::move(v));
    }

    int neg(int a, int line) {
        Val2 v;
        v.k = VK::Arith;
        v.op = '~';
        v.a = a;
        v.line = line;
        v.iv = neg_iv(vals[a].iv);
        v.cg = neg_cong(vals[a].cg, v.iv.top);
        v.name = "%t" + std::to_string(seq++);
        v.why = "`-" + vals[a].name + "` at line " + std::to_string(line);
        return push(std::move(v));
    }

    int cmp(int a, int b, const std::string &op, int line) {
        Val2 v;
        v.k = VK::Cmp;
        v.a = a;
        v.b = b;
        v.line = line;
        v.iv = rng(0, 1);
        v.cg = top_cong();
        v.name = "%t" + std::to_string(seq++);
        v.why = "`" + vals[a].name + " " + op + " " + vals[b].name + "` at line " +
                std::to_string(line);
        return push(std::move(v));
    }

    int divmod(bool is_div, int a, int b, int line, bool &safe, bool &nz_only, std::string &reason) {
        Val2 v;
        v.k = is_div ? VK::Div : VK::Mod;
        v.a = a;
        v.b = b;
        v.line = line;
        v.iv = unknown_iv();
        v.cg = top_cong();
        const Val2 &av = vals[a];
        const Val2 &bv = vals[b];
        const bool pos_ops = nonneg(av.iv) && positive(bv.iv);
        const bool neg_ops = nonpos(av.iv) && negative(bv.iv) && excludes_div_overflow(av.iv, bv.iv);
        bool exact_mult = false;
        if (bv.k == VK::Const && bv.iv.lo != 0 && excludes_div_overflow(av.iv, bv.iv)) {
            uint64_t abs_b = static_cast<uint64_t>(bv.iv.lo < 0 ? -static_cast<__int128>(bv.iv.lo)
                                                                : static_cast<__int128>(bv.iv.lo));
            if (abs_b >= 1 && !av.cg.bot) {
                if (av.cg.m == 0 && (static_cast<int64_t>(av.cg.r) % bv.iv.lo) == 0)
                    exact_mult = true;
                else if (av.cg.m >= 2 && av.cg.r == 0 && (av.cg.m % abs_b) == 0 &&
                         (!av.iv.top || (abs_b & (abs_b - 1)) == 0))
                    exact_mult = true;
            }
        }
        safe = pos_ops || neg_ops || exact_mult;
        nz_only = false;
        if (pos_ops) {
            v.iv = is_div ? div_iv(av.iv, bv.iv) : mod_iv(av.iv, bv.iv);
            if (!is_div && bv.k == VK::Const && bv.iv.lo > 0 && !av.cg.bot && !av.iv.top) {
                uint64_t kb = static_cast<uint64_t>(bv.iv.lo);
                if (av.cg.m == 0 || (av.cg.m >= 2 && (av.cg.m % kb) == 0)) {
                    int64_t rem = static_cast<int64_t>(av.cg.r % kb);
                    v.iv = cst(rem);
                    v.cg = cst_cong(rem);
                }
            }
            reason = "divisor > 0 and dividend >= 0";
        } else if (neg_ops) {
            reason = "divisor < 0, dividend <= 0, and `INT64_MIN / -1` excluded";
        } else if (exact_mult) {
            if (!is_div) { v.iv = cst(0); v.cg = cst_cong(0); }
            reason = "Granger congruence proves exact divisibility (`a % b == 0`) with nonzero divisor";
        } else {
            const bool b_nz = positive(bv.iv) || negative(bv.iv) ||
                              (!bv.cg.bot && bv.cg.m >= 2 && bv.cg.r != 0 && !bv.iv.top);
            if (b_nz) {
                nz_only = true;
                reason = "divisor proven non-zero (div-by-zero trap elided)";
            }
        }
        v.name = "%t" + std::to_string(seq++);
        v.why = "`" + av.name + (is_div ? " / " : " % ") + bv.name + "` at line " +
                std::to_string(line);
        return push(std::move(v));
    }

    void prove_div(int id, bool is_div, const Expr *node, const std::string &reason, bool nz_only = false) {
        if (dbm.inconsistent) return;
        const Val2 &v = vals[id];
        const Val2 &a = vals[v.a];
        const Val2 &b = vals[v.b];
        Proof p;
        p.fn = fname;
        p.line = v.line;
        std::string t = "fn " + fname + ", line " + std::to_string(v.line) +
                        (nz_only ? ": div-by-zero check dropped on `" : ": check dropped on `") +
                        a.name + (is_div ? " / " : " % ") + b.name + "`\n";
        t += "  " + a.name + " in " + dom_text(a) + ": " + a.why + "\n";
        t += "  " + b.name + " in " + dom_text(b) + ": " + b.why + "\n";
        t += "  " + reason + ", so `" + (is_div ? "/" : "%") + "` needs no runtime check\n";
        p.text = t;
        if (nz_only) out.nonzero_div.insert(node);
        else out.unchecked.insert(node);
        out.proofs.push_back(std::move(p));
    }

    bool check_vec_bounds(const std::string &vname, int idx_val, int line,
                          const Expr *idx_node, const Stmt *set_node) {
        if (!vecs.count(vname) || idx_val < 0) return false;
        dbm.close();
        if (dbm.inconsistent) return false;
        const Val2 &iv = vals[idx_val];
        int len_dn = dbm.node("len(" + vname + ")", /*nonneg_init=*/true);
        bool ge_zero = nonneg(iv.iv) || (iv.dbm_node > 0 && dbm.diff_le(0, iv.dbm_node, 0));
        bool lt_len = (iv.dbm_node > 0 && dbm.diff_le(iv.dbm_node, len_dn, -1));
        if (!ge_zero || !lt_len) return false;
        Proof p;
        p.fn = fname;
        p.line = line;
        std::string kind = set_node ? ("`" + vname + "[...] = ...`") : ("`" + vname + "[...]`");
        std::string t = "fn " + fname + ", line " + std::to_string(line) +
                        ": bounds check dropped on " + kind + "\n";
        t += "  index " + iv.name + " in " + dom_text(iv) + ": " + iv.why + "\n";
        t += "  DBM zone proves `0 <= " + iv.name + "` and `" + iv.name + " - len(" + vname +
             ") <= -1`\n";
        p.text = t;
        if (idx_node) out.unchecked_idx.insert(idx_node);
        if (set_node) out.unchecked_setidx.insert(set_node);
        out.proofs.push_back(std::move(p));
        return true;
    }

    void record_return(int vid) {
        const Val2 &v = vals[vid];
        if (!saw_return) {
            saw_return = true;
            ret_iv = v.iv;
            ret_cg = v.cg;
        } else {
            ret_iv = hull_iv(ret_iv, v.iv);
            ret_cg = join_cong(ret_cg, v.cg);
        }
    }
};

// Apply guard `cond` (holding when `pos == true`, negated when `pos == false`)
// to `Builder2` (Intervals + Congruences + Relational DBM).
void apply_guard_o2(Builder2 &b, const Expr &cond, bool pos, int line) {
    if (b.dbm.inconsistent) return;
    if (cond.k == EK::Un && cond.a == "!") {
        apply_guard_o2(b, *cond.kids[0], !pos, line);
        return;
    }
    if (cond.k != EK::Bin) return;
    const std::string &op0 = cond.a;
    if (op0 == "&&" && pos) {
        apply_guard_o2(b, *cond.kids[0], true, line);
        apply_guard_o2(b, *cond.kids[1], true, line);
        return;
    }
    if (op0 == "||" && !pos) {
        apply_guard_o2(b, *cond.kids[0], false, line);
        apply_guard_o2(b, *cond.kids[1], false, line);
        return;
    }

    // Congruence refinement: `(x % M) == R` or `let r = x % M; if r == R`
    auto extract_mod_var = [&](const Expr &lhs, std::string &var_out, int64_t &mod_out) -> bool {
        if (lhs.k == EK::Bin && lhs.a == "%" && lhs.kids[0]->k == EK::Var &&
            lhs.kids[1]->k == EK::Num && !lhs.kids[1]->is_float) {
            var_out = lhs.kids[0]->a;
            mod_out = lhs.kids[1]->i;
            return true;
        }
        if (lhs.k == EK::Var) {
            auto it = b.env.find(lhs.a);
            if (it != b.env.end()) {
                const Val2 &mv = b.vals[it->second];
                if (mv.k == VK::Mod && mv.a >= 0 && mv.b >= 0 &&
                    b.vals[mv.b].k == VK::Const && !b.vals[mv.a].src_var.empty()) {
                    const std::string &cand_vn = b.vals[mv.a].src_var;
                    auto vit = b.env.find(cand_vn);
                    if (vit != b.env.end() &&
                        b.vals[vit->second].origin == b.vals[mv.a].origin) {
                        var_out = cand_vn;
                        mod_out = b.vals[mv.b].iv.lo;
                        return true;
                    }
                }
            }
        }
        return false;
    };
    if ((op0 == "==" && pos) || (op0 == "!=" && !pos)) {
        auto try_mod_eq = [&](const Expr &lhs, const Expr &rhs) {
            std::string vn;
            int64_t mod_c = 0;
            if (rhs.k == EK::Num && !rhs.is_float && extract_mod_var(lhs, vn, mod_c)) {
                int64_t rem_c = rhs.i;
                if (mod_c >= 2 && rem_c >= 0 && rem_c < mod_c) {
                    auto it = b.env.find(vn);
                    if (it != b.env.end()) {
                        Val2 nv = b.vals[it->second];
                        nv.cg = meet_cong(nv.cg, Cong{false, static_cast<uint64_t>(mod_c),
                                                      static_cast<uint64_t>(rem_c)});
                        nv.name = "%n" + std::to_string(b.seq++);
                        nv.why = "congruence narrow of `" + vn + "` by `" + src_text(cond) + "`";
                        b.env[vn] = b.push(std::move(nv));
                    }
                }
            }
        };
        try_mod_eq(*cond.kids[0], *cond.kids[1]);
        try_mod_eq(*cond.kids[1], *cond.kids[0]);
    }
    if ((op0 == "!=" && pos) || (op0 == "==" && !pos)) {
        auto try_mod2_nz = [&](const Expr &lhs, const Expr &rhs) {
            std::string vn;
            int64_t mod_c = 0;
            if (rhs.k == EK::Num && !rhs.is_float && rhs.i == 0 &&
                extract_mod_var(lhs, vn, mod_c) && mod_c == 2) {
                auto it = b.env.find(vn);
                if (it != b.env.end()) {
                    Val2 nv = b.vals[it->second];
                    nv.cg = meet_cong(nv.cg, Cong{false, 2, 1});
                    nv.name = "%n" + std::to_string(b.seq++);
                    nv.why = "parity narrow of `" + vn + "` by `" + src_text(cond) + "`";
                    b.env[vn] = b.push(std::move(nv));
                }
            }
        };
        try_mod2_nz(*cond.kids[0], *cond.kids[1]);
        try_mod2_nz(*cond.kids[1], *cond.kids[0]);
    }

    // Standard unary interval refinements from `-O1`
    std::vector<Refine> rs;
    refines(cond, pos, rs);
    for (const auto &r : rs) {
        auto it = b.env.find(r.var);
        if (it == b.env.end()) continue;
        IV want = rng(r.has_lo ? r.lo : INT64_MIN, r.has_hi ? r.hi : INT64_MAX);
        Val2 nv = b.vals[it->second];
        nv.iv = meet_iv(nv.iv, want);
        nv.name = "%n" + std::to_string(b.seq++);
        nv.why = "narrow of " + b.vals[it->second].name + " by `" +
                 (pos ? src_text(cond) : "!(" + src_text(cond) + ")") + "` at line " +
                 std::to_string(line);
        int nid = b.push(std::move(nv));
        if (is_empty(b.vals[nid].iv)) {
            b.dbm.inconsistent = true;
            return;
        }
        b.env[r.var] = nid;
    }

    // Relational DBM constraint extraction for `<, <=, >, >=, ==`
    std::function<bool(const Expr &, int &, __int128 &)> extract_term =
        [&](const Expr &e, int &dn, __int128 &offset) -> bool {
        offset = 0;
        if (e.k == EK::Num && !e.is_float) {
            dn = 0;
            offset = static_cast<__int128>(e.i);
            return true;
        }
        if (e.k == EK::Var) {
            auto it = b.env.find(e.a);
            if (it == b.env.end() || b.vals[it->second].dbm_node <= 0) return false;
            dn = b.vals[it->second].dbm_node;
            return true;
        }
        if (e.k == EK::Call && e.a == "len" && e.kids.size() == 1 &&
            e.kids[0]->k == EK::Var && b.vecs.count(e.kids[0]->a)) {
            dn = b.dbm.node("len(" + e.kids[0]->a + ")", /*nonneg_init=*/true);
            return dn > 0;
        }
        // `v + c` or `v - c` when provably non-wrapping
        if (e.k == EK::Bin && (e.a == "+" || e.a == "-") &&
            e.kids[1]->k == EK::Num && !e.kids[1]->is_float) {
            int sub_dn = -1;
            __int128 sub_off = 0;
            if (!extract_term(*e.kids[0], sub_dn, sub_off)) return false;
            IV sub_iv = (sub_dn == 0) ? cst(static_cast<int64_t>(sub_off)) : b.dbm.extract_iv(sub_dn);
            if (sub_iv.top) return false;
            __int128 c = (e.a == "+") ? static_cast<__int128>(e.kids[1]->i)
                                      : -static_cast<__int128>(e.kids[1]->i);
            if (!fits(static_cast<__int128>(sub_iv.lo) + c) ||
                !fits(static_cast<__int128>(sub_iv.hi) + c))
                return false;
            dn = sub_dn;
            offset = sub_off + c;
            return true;
        }
        return false;
    };

    std::string rel = op0;
    if (!pos) {
        if (rel == "<") rel = ">=";
        else if (rel == "<=") rel = ">";
        else if (rel == ">") rel = "<=";
        else if (rel == ">=") rel = "<";
        else if (rel == "!=") rel = "==";
        else rel.clear();
    }
    if (rel == "<" || rel == "<=" || rel == ">" || rel == ">=" || rel == "==") {
        int dl = -1, dr = -1;
        __int128 ol = 0, orhs = 0;
        if (extract_term(*cond.kids[0], dl, ol) && extract_term(*cond.kids[1], dr, orhs)) {
            // (dl + ol) rel (dr + orhs)  <=>  dl - dr <= orhs - ol + adjust
            __int128 rhs_minus_lhs = orhs - ol;
            if (rel == "<") b.dbm.add_diff(dl, dr, rhs_minus_lhs - 1);
            else if (rel == "<=") b.dbm.add_diff(dl, dr, rhs_minus_lhs);
            else if (rel == ">") b.dbm.add_diff(dr, dl, -rhs_minus_lhs - 1);
            else if (rel == ">=") b.dbm.add_diff(dr, dl, -rhs_minus_lhs);
            else if (rel == "==") {
                b.dbm.add_diff(dl, dr, rhs_minus_lhs);
                b.dbm.add_diff(dr, dl, -rhs_minus_lhs);
            }
        }
    }
    b.sync_from_dbm();
}

bool is_step_o2(const Stmt &s, const std::string &name, int &dir_out, int64_t &step_out) {
    if (s.k != SK::Set || s.a != name || !s.e1) return false;
    const Expr &e = *s.e1;
    if (e.k != EK::Bin || (e.a != "+" && e.a != "-") || e.kids.size() != 2) return false;
    if (e.kids[0]->k != EK::Var || e.kids[0]->a != name) return false;
    const Expr &rhs = *e.kids[1];
    if (rhs.k == EK::Num && !rhs.is_float && rhs.i >= 1) {
        dir_out = (e.a == "+") ? 1 : -1;
        step_out = rhs.i;
        return true;
    }
    if (rhs.k == EK::Un && rhs.a == "-" && rhs.kids.size() == 1 &&
        rhs.kids[0]->k == EK::Num && !rhs.kids[0]->is_float && rhs.kids[0]->i >= 1) {
        dir_out = (e.a == "+") ? -1 : 1;
        step_out = rhs.kids[0]->i;
        return true;
    }
    return false;
}

void count_counter_writes_o2(const Stmt &s, const std::string &name, int &cnt) {
    if ((s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::Set || s.k == SK::ForRange ||
         s.k == SK::ForIter) &&
        s.a == name)
        cnt++;
    for (const Stmt *t : view(s.body)) count_counter_writes_o2(*t, name, cnt);
    for (const Stmt *t : view(s.body2)) count_counter_writes_o2(*t, name, cnt);
    if (s.els) count_counter_writes_o2(*s.els, name, cnt);
}

struct Walker2 {
    Builder2 &b;
    int line = 0;
    int loops = 0;

    bool bail() {
        b.bad = true;
        return false;
    }

    void bind_var(const std::string &nm, int vid) {
        b.dbm.forget(nm);
        int dn = b.dbm.node(nm);
        const Val2 &src = b.vals[vid];
        if (dn > 0) {
            b.dbm.set_iv(dn, src.iv);
            if (src.dbm_node >= 0 && src.dbm_node != dn) {
                b.dbm.add_diff(dn, src.dbm_node, 0);
                b.dbm.add_diff(src.dbm_node, dn, 0);
            }
        }
        Val2 copy = src;
        copy.dbm_node = dn;
        copy.origin = -1;
        copy.src_var = nm;
        b.env[nm] = b.push(std::move(copy));
        b.dbm.close();
    }

    int val(const Expr &e) {
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
        case EK::Idx: {
            if (e.kids.size() != 2 || e.kids[0]->k != EK::Var) return -1;
            const std::string &vn = e.kids[0]->a;
            if (!b.vecs.count(vn)) return -1;
            int idx = val(*e.kids[1]);
            if (idx < 0) return -1;
            b.check_vec_bounds(vn, idx, line, &e, nullptr);
            Val2 elem;
            elem.k = VK::Call;
            elem.iv = unknown_iv();
            elem.cg = top_cong();
            elem.line = line;
            elem.name = "%t" + std::to_string(b.seq++);
            elem.why = "element `" + vn + "[...]` at line " + std::to_string(line);
            return b.push(std::move(elem));
        }
        case EK::Un: {
            int x = val(*e.kids[0]);
            if (x < 0) return -1;
            if (e.a == "-") return b.neg(x, line);
            if (e.a == "!") return b.cmp(x, b.konst(0), "!", line);
            if (e.a == "~") {
                Val2 u;
                u.k = VK::Call;
                u.iv = unknown_iv();
                u.cg = top_cong();
                u.line = line;
                u.name = "%t" + std::to_string(b.seq++);
                u.why = "`~` at line " + std::to_string(line);
                return b.push(std::move(u));
            }
            return -1;
        }
        case EK::Bin: {
            const std::string &op = e.a;
            bool is_cmp = op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" ||
                          op == "!=" || op == "&&" || op == "||";
            bool is_div = op == "/" || op == "%";
            bool is_ar = op == "+" || op == "-" || op == "*";
            bool is_bw = op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>";
            if (!is_cmp && !is_div && !is_ar && !is_bw) return -1;
            int a = val(*e.kids[0]);
            if (a < 0) return -1;
            int c = val(*e.kids[1]);
            if (c < 0) return -1;
            if (is_bw) {
                Val2 u;
                u.k = VK::Call;
                u.iv = unknown_iv();
                u.cg = top_cong();
                u.line = line;
                const IV &av = b.vals[a].iv, &cv = b.vals[c].iv;
                if (op == "&") {
                    if (nonneg(cv)) u.iv = rng(0, nonneg(av) ? std::min(av.hi, cv.hi) : cv.hi);
                    else if (nonneg(av)) u.iv = rng(0, av.hi);
                }
                u.name = "%t" + std::to_string(b.seq++);
                u.why = "`" + op + "` at line " + std::to_string(line);
                return b.push(std::move(u));
            }
            if (is_ar) return b.arith(op[0], a, c, line);
            if (is_cmp) return b.cmp(a, c, op, line);
            bool safe = false, nz_only = false;
            std::string reason;
            int id = b.divmod(op == "/", a, c, line, safe, nz_only, reason);
            if (safe || nz_only) b.prove_div(id, op == "/", &e, reason, nz_only);
            return id;
        }
        case EK::Call: {
            if (e.a == "len" && e.kids.size() == 1 && e.kids[0]->k == EK::Var &&
                b.vecs.count(e.kids[0]->a)) {
                return b.len_of(e.kids[0]->a, line);
            }
            for (const auto &arg : e.kids) {
                if (arg->k == EK::Var && b.vecs.count(arg->a)) continue;
                if (val(*arg) < 0) return -1;
            }
            return b.call_val(e.a, line);
        }
        default:
            return -1;
        }
    }

    bool block(const std::vector<const Stmt *> &body) {
        for (const Stmt *s : body) {
            if (b.dbm.inconsistent) return true;
            line = s->line;
            switch (s->k) {
            case SK::Let:
            case SK::LetTmp:
            case SK::Set: {
                if (s->k == SK::Set && s->a.find('.') != std::string::npos) return bail();
                int v = val(*s->e1);
                if (v < 0) return bail();
                bind_var(s->a, v);
                break;
            }
            case SK::SetIdx: {
                if (!s->e1 || s->e1->k != EK::Var || !b.vecs.count(s->e1->a)) return bail();
                int idx = val(*s->e2);
                if (idx < 0) return bail();
                int rhs = val(*s->e3);
                if (rhs < 0) return bail();
                b.check_vec_bounds(s->e1->a, idx, line, nullptr, s);
                break;
            }
            case SK::Return: {
                int v = val(*s->e1);
                if (v < 0) return bail();
                if (!b.dbm.inconsistent) b.record_return(v);
                return true;
            }
            case SK::Expr: {
                if (s->e1->k == EK::Call && s->e1->a == "print") {
                    for (const auto &arg : s->e1->kids) {
                        if (arg->k == EK::Str || (arg->k == EK::Num && arg->is_float)) continue;
                        if (val(*arg) < 0) return bail();
                    }
                    break;
                }
                int v = val(*s->e1);
                if (v < 0) return bail();
                break;
            }
            case SK::If: {
                int c = val(*s->e1);
                if (c < 0) return bail();
                int at = line;
                auto saved_env = b.env;
                DBM saved_dbm = b.dbm;

                apply_guard_o2(b, *s->e1, true, at);
                bool ex_t = b.dbm.inconsistent ? true : block(view(s->body));
                if (b.bad) return false;
                auto env_t = b.env;
                DBM dbm_t = b.dbm;

                b.env = saved_env;
                b.dbm = saved_dbm;
                apply_guard_o2(b, *s->e1, false, at);
                const Stmt *els = real_else(*s);
                bool ex_e = b.dbm.inconsistent ? true : false;
                if (!ex_e && els) {
                    ex_e = els->k == SK::Block ? block(view(els->body)) : block({els});
                    if (b.bad) return false;
                }
                auto env_e = b.env;
                DBM dbm_e = b.dbm;

                if (ex_t && ex_e) {
                    b.env = saved_env;
                    b.dbm = saved_dbm;
                    b.dbm.inconsistent = true;
                    return true;
                }
                if (ex_t) {
                    b.env = env_e;
                    b.dbm = dbm_e;
                    break;
                }
                if (ex_e) {
                    b.env = env_t;
                    b.dbm = dbm_t;
                    break;
                }
                b.env = saved_env;
                b.dbm = DBM::join(dbm_t, dbm_e);
                std::set<std::string> keys;
                for (const auto &kv : env_t) keys.insert(kv.first);
                for (const auto &kv : env_e) keys.insert(kv.first);
                for (const auto &k : keys) {
                    int it = env_t.count(k) ? env_t[k] : (saved_env.count(k) ? saved_env[k] : -1);
                    int ie = env_e.count(k) ? env_e[k] : (saved_env.count(k) ? saved_env[k] : -1);
                    if (it >= 0 && ie >= 0) {
                        if (it == ie) {
                            b.env[k] = it;
                        } else {
                            Val2 pv;
                            pv.k = VK::Phi;
                            pv.a = it;
                            pv.b = ie;
                            pv.line = at;
                            pv.iv = hull_iv(b.vals[it].iv, b.vals[ie].iv);
                            pv.cg = join_cong(b.vals[it].cg, b.vals[ie].cg);
                            pv.dbm_node = b.dbm.node(k);
                            pv.name = "%p" + std::to_string(b.seq++);
                            pv.why = "join of " + b.vals[it].name + " and " + b.vals[ie].name;
                            b.env[k] = b.push(std::move(pv));
                        }
                    } else {
                        b.dbm.forget(k);
                        b.env[k] = b.call_val("<branch-local>", at);
                    }
                }
                b.sync_from_dbm();
                break;
            }
            case SK::Break:
            case SK::Continue:
                if (!loops) return bail();
                return true;
            case SK::While: {
                const Expr &cond = *s->e1;
                if (cond.k != EK::Bin || cond.kids[0]->k != EK::Var) return bail();
                const bool asc_cmp = (cond.a == "<" || cond.a == "<=");
                const bool desc_cmp = (cond.a == ">" || cond.a == ">=");
                if (!asc_cmp && !desc_cmp) return bail();
                const std::string &counter = cond.kids[0]->a;
                auto it = b.env.find(counter);
                if (it == b.env.end()) return bail();
                const Val2 entry = b.vals[it->second];

                std::vector<const Stmt *> lbody = view(s->body);
                int step_dir = 0;
                int64_t step = 0;
                if (lbody.empty() || !is_step_o2(*lbody.back(), counter, step_dir, step))
                    return bail();
                if ((asc_cmp && step_dir != 1) || (desc_cmp && step_dir != -1))
                    return bail();
                if (asc_cmp && entry.iv.top) return bail();

                int cnt_writes = 0;
                for (const Stmt *t : lbody) count_counter_writes_o2(*t, counter, cnt_writes);
                if (cnt_writes != 1) return bail();

                std::set<std::string> assigned, guard_vars;
                for (const Stmt *t : lbody) collect_assigned(*t, assigned);
                collect_vars(*cond.kids[1], guard_vars);
                if (guard_vars.count(counter)) return bail();
                for (const auto &g : guard_vars)
                    if (assigned.count(g)) return bail();

                int bid = val(*cond.kids[1]);
                if (bid < 0) return bail();
                const Val2 bval = b.vals[bid];

                int64_t body_lo = 0, body_hi = 0;
                if (asc_cmp) {
                    if (cond.a == "<" && !bval.iv.top && bval.iv.hi == INT64_MIN) break;
                    int64_t head_hi = (cond.a == "<")
                                          ? (bval.iv.top ? INT64_MAX - 1 : bval.iv.hi - 1)
                                          : (bval.iv.top ? INT64_MAX : bval.iv.hi);
                    if (!fits(static_cast<__int128>(head_hi) + static_cast<__int128>(step)))
                        return bail();
                    if (head_hi < entry.iv.lo) break;
                    body_lo = entry.iv.lo;
                    body_hi = head_hi;
                } else {
                    if (cond.a == ">" && !bval.iv.top && bval.iv.lo == INT64_MAX) break;
                    int64_t head_lo = (cond.a == ">")
                                          ? (bval.iv.top ? INT64_MIN + 1 : bval.iv.lo + 1)
                                          : (bval.iv.top ? INT64_MIN : bval.iv.lo);
                    if (!fits(static_cast<__int128>(head_lo) - static_cast<__int128>(step)))
                        return bail();
                    if (!entry.iv.top && head_lo > entry.iv.hi) break;
                    body_lo = head_lo;
                    body_hi = entry.iv.top ? INT64_MAX : entry.iv.hi;
                }

                b.dbm.close();
                auto pre_dbm = b.dbm;
                const int old_cdn = entry.dbm_node;
                for (const auto &nm : assigned) {
                    if (nm == counter) continue;
                    b.dbm.forget(nm);
                    Val2 uv;
                    uv.k = VK::Call;
                    uv.iv = unknown_iv();
                    uv.cg = top_cong();
                    uv.dbm_node = b.dbm.node(nm);
                    uv.line = line;
                    uv.name = "%t" + std::to_string(b.seq++);
                    uv.why = "loop-carried `" + nm + "` at line " + std::to_string(line);
                    b.env[nm] = b.push(std::move(uv));
                }

                b.dbm.forget(counter);
                int cdn = b.dbm.node(counter);
                if (old_cdn > 0 && cdn > 0 && !pre_dbm.inconsistent) {
                    for (int k = 0; k < pre_dbm.n; ++k) {
                        if (k == old_cdn) continue;
                        if (k > 0 && assigned.count(pre_dbm.names[k])) continue;
                        if (asc_cmp) {
                            // `entry <= counter` preserves `k - counter <= k - entry`.
                            if (pre_dbm.m[k][old_cdn] < DBM_INF)
                                b.dbm.add_diff(k, cdn, pre_dbm.m[k][old_cdn]);
                        } else {
                            // `counter <= entry` preserves `counter - k <= entry - k`.
                            if (pre_dbm.m[old_cdn][k] < DBM_INF)
                                b.dbm.add_diff(cdn, k, pre_dbm.m[old_cdn][k]);
                        }
                    }
                }
                Val2 cv;
                cv.k = VK::Narrow;
                cv.iv = rng(body_lo, body_hi);
                uint64_t gmod = gcd64(entry.cg.m, static_cast<uint64_t>(step));
                int64_t er = static_cast<int64_t>(entry.cg.r);
                cv.cg = norm_cong(gmod, er);
                cv.dbm_node = cdn;
                cv.line = line;
                cv.name = "%n" + std::to_string(b.seq++);
                cv.why = std::string(asc_cmp ? "strided loop `" : "descending strided loop `") +
                         src_text(cond) + "` (step " + (asc_cmp ? "+" : "-") +
                         std::to_string(step) + ") from " + entry.name;
                if (bval.dbm_node >= 0) {
                    if (asc_cmp)
                        b.dbm.add_diff(cdn, bval.dbm_node, cond.a == "<" ? -1 : 0);
                    else
                        b.dbm.add_diff(bval.dbm_node, cdn, cond.a == ">" ? -1 : 0);
                }
                b.env[counter] = b.push(std::move(cv));
                b.sync_from_dbm();

                loops++;
                if (!b.dbm.inconsistent) block(lbody);
                loops--;
                if (b.bad) return false;

                b.dbm = pre_dbm;
                for (const auto &nm : assigned) {
                    b.dbm.forget(nm);
                    if (nm != counter)
                        b.env[nm] = b.call_val("<loop-assigned>", line);
                }
                int post_dn = b.dbm.node(counter);
                if (old_cdn > 0 && post_dn > 0 && !pre_dbm.inconsistent) {
                    for (int k = 0; k < pre_dbm.n; ++k) {
                        if (k == old_cdn) continue;
                        if (k > 0 && assigned.count(pre_dbm.names[k])) continue;
                        if (asc_cmp && pre_dbm.m[k][old_cdn] < DBM_INF)
                            b.dbm.add_diff(k, post_dn, pre_dbm.m[k][old_cdn]);
                        else if (desc_cmp && pre_dbm.m[old_cdn][k] < DBM_INF)
                            b.dbm.add_diff(post_dn, k, pre_dbm.m[old_cdn][k]);
                    }
                }
                Val2 post;
                post.k = VK::Narrow;
                post.iv = asc_cmp ? rng(entry.iv.lo, INT64_MAX)
                                  : (entry.iv.top ? unknown_iv() : rng(INT64_MIN, entry.iv.hi));
                post.cg = norm_cong(gmod, er);
                post.dbm_node = post_dn;
                post.line = line;
                post.name = "%n" + std::to_string(b.seq++);
                post.why = "post-loop `" + counter + "`";
                b.env[counter] = b.push(std::move(post));
                b.sync_from_dbm();
                break;
            }
            case SK::ForRange: {
                const std::string &counter = s->a;
                std::vector<const Stmt *> lbody = view(s->body);
                int cnt_writes = 0;
                for (const Stmt *t : lbody) count_counter_writes_o2(*t, counter, cnt_writes);
                if (cnt_writes != 0) return bail();

                std::set<std::string> assigned, bound_vars;
                for (const Stmt *t : lbody) collect_assigned(*t, assigned);
                collect_vars(*s->e1, bound_vars);
                collect_vars(*s->e2, bound_vars);
                if (bound_vars.count(counter)) return bail();
                for (const auto &g : bound_vars)
                    if (assigned.count(g)) return bail();

                int lo_id = val(*s->e1);
                if (lo_id < 0) return bail();
                int hi_id = val(*s->e2);
                if (hi_id < 0) return bail();
                const Val2 lo_v = b.vals[lo_id];
                const Val2 hi_v = b.vals[hi_id];
                if (lo_v.iv.top) return bail();
                auto saved_counter = b.env.count(counter) ? b.env[counter] : -1;
                if (!hi_v.iv.top && hi_v.iv.hi == INT64_MIN) {
                    if (saved_counter >= 0) b.env[counter] = saved_counter;
                    else b.env.erase(counter);
                    break;
                }
                int64_t head_hi = hi_v.iv.top ? INT64_MAX : hi_v.iv.hi - 1;
                if (head_hi < lo_v.iv.lo) {
                    if (saved_counter >= 0) b.env[counter] = saved_counter;
                    else b.env.erase(counter);
                    break;
                }

                auto pre_dbm = b.dbm;
                for (const auto &nm : assigned) {
                    if (nm == counter) continue;
                    b.dbm.forget(nm);
                    Val2 uv;
                    uv.k = VK::Call;
                    uv.iv = unknown_iv();
                    uv.cg = top_cong();
                    uv.dbm_node = b.dbm.node(nm);
                    uv.line = line;
                    uv.name = "%t" + std::to_string(b.seq++);
                    uv.why = "loop-carried `" + nm + "` at line " + std::to_string(line);
                    b.env[nm] = b.push(std::move(uv));
                }

                b.dbm.forget(counter);
                int cdn = b.dbm.node(counter);
                Val2 cv;
                cv.k = VK::Narrow;
                cv.iv = rng(lo_v.iv.lo, head_hi);
                cv.cg = top_cong();
                cv.dbm_node = cdn;
                cv.line = line;
                cv.name = "%" + counter + "." + std::to_string(b.seq++);
                cv.why = "`for " + counter + " in " + src_text(*s->e1) + ".." + src_text(*s->e2) + "`";
                if (lo_v.dbm_node >= 0) b.dbm.add_diff(lo_v.dbm_node, cdn, 0);   // lo <= counter
                if (hi_v.dbm_node >= 0) b.dbm.add_diff(cdn, hi_v.dbm_node, -1);  // counter <= hi - 1
                b.env[counter] = b.push(std::move(cv));
                b.sync_from_dbm();

                loops++;
                if (!b.dbm.inconsistent) block(lbody);
                loops--;
                if (b.bad) return false;

                b.dbm = pre_dbm;
                b.dbm.forget(counter);
                if (saved_counter >= 0) {
                    Val2 uv;
                    uv.k = VK::Call;
                    uv.iv = unknown_iv();
                    uv.cg = top_cong();
                    uv.dbm_node = b.dbm.node(counter);
                    uv.line = line;
                    uv.name = "%t" + std::to_string(b.seq++);
                    uv.why = "for-loop counter `" + counter + "` at line " + std::to_string(line);
                    b.env[counter] = b.push(std::move(uv));
                } else {
                    b.env.erase(counter);
                }
                for (const auto &nm : assigned) {
                    b.dbm.forget(nm);
                    b.env[nm] = b.call_val("<loop-assigned>", line);
                }
                b.sync_from_dbm();
                break;
            }
            case SK::ForIter: {
                if (!s->e1) return bail();
                bool empty_lit = false;
                IV elem_iv = unknown_iv();
                Cong elem_cg = top_cong();
                if (s->e1->k == EK::Var && b.vecs.count(s->e1->a)) {
                    // Vector parameter iterable.
                } else if (s->e1->k == EK::VecLit) {
                    if (s->e1->kids.empty()) {
                        empty_lit = true;
                    } else {
                        bool first_el = true;
                        for (const auto &el : s->e1->kids) {
                            int eid = val(*el);
                            if (eid < 0) return bail();
                            if (first_el) {
                                elem_iv = b.vals[eid].iv;
                                elem_cg = b.vals[eid].cg;
                                first_el = false;
                            } else {
                                elem_iv = hull_iv(elem_iv, b.vals[eid].iv);
                                elem_cg = join_cong(elem_cg, b.vals[eid].cg);
                            }
                        }
                    }
                } else {
                    if (val(*s->e1) < 0) return bail();
                }
                const std::string &elem = s->a;
                auto saved_elem = b.env.count(elem) ? b.env[elem] : -1;
                if (empty_lit) break;
                std::vector<const Stmt *> lbody = view(s->body);
                int elem_writes = 0;
                for (const Stmt *t : lbody) count_counter_writes_o2(*t, elem, elem_writes);
                std::set<std::string> assigned;
                for (const Stmt *t : lbody) collect_assigned(*t, assigned);
                auto pre_dbm = b.dbm;
                for (const auto &nm : assigned) {
                    b.dbm.forget(nm);
                    b.env[nm] = b.call_val("<loop-assigned>", line);
                }
                b.dbm.forget(elem);
                if (elem_writes == 0 && (!elem_iv.top || elem_cg.m != 1)) {
                    Val2 ev;
                    ev.k = VK::Narrow;
                    ev.iv = elem_iv;
                    ev.cg = elem_cg;
                    ev.dbm_node = b.dbm.node(elem);
                    ev.line = line;
                    ev.name = "%" + elem + "." + std::to_string(b.seq++);
                    ev.why = "`for " + elem + " in [...]` hull at line " + std::to_string(line);
                    b.env[elem] = b.push(std::move(ev));
                    b.sync_from_dbm();
                } else {
                    b.env[elem] = b.call_val("<iter-elem>", line);
                }
                loops++;
                if (!b.dbm.inconsistent) block(lbody);
                loops--;
                if (b.bad) return false;
                b.dbm = pre_dbm;
                b.dbm.forget(elem);
                if (saved_elem >= 0) {
                    b.env[elem] = b.call_val("<iter-elem>", line);
                } else {
                    b.env.erase(elem);
                }
                for (const auto &nm : assigned) {
                    b.dbm.forget(nm);
                    b.env[nm] = b.call_val("<loop-assigned>", line);
                }
                b.sync_from_dbm();
                break;
            }
            default:
                return bail();
            }
        }
        return false;
    }
};

}  // namespace o2

bool analyze_o2(const std::string &fn, const std::vector<Param> &params,
                const std::set<std::string> &vec_params,
                const std::vector<StmtP> &body,
                const std::map<std::string, FnSummary> &summaries,
                Result &out, FnSummary *out_summary) {
    out = Result{};
    if (out_summary) *out_summary = FnSummary{};
    o2::Builder2 b(summaries);
    b.fname = fn;
    b.vecs = vec_params;
    for (const auto &vp : vec_params) {
        b.dbm.node("len(" + vp + ")", /*nonneg_init=*/true);
    }
    for (const auto &p : params) {
        if (vec_params.count(p.name)) continue;
        if (p.def && !(p.def->k == EK::Num && !p.def->is_float)) return false;
        o2::Val2 v;
        v.k = VK::Param;
        v.iv = unknown_iv();
        v.cg = o2::top_cong();
        v.dbm_node = b.dbm.node(p.name);
        v.name = "%" + p.name + ".0";
        v.src_var = p.name;
        v.why = "parameter `" + p.name + "`";
        b.env[p.name] = b.push(std::move(v));
    }
    b.dbm.close();
    o2::Walker2 w{b};
    bool all_ret = w.block(view(body));
    if (b.bad) return false;
    if (!all_ret) {
        // Implicit `return 0;` at end of fast int64_t function if control falls off.
        b.record_return(b.konst(0));
    }
    if (out_summary && b.saw_return && !b.ret_cg.bot && !is_empty(b.ret_iv)) {
        FnSummary s;
        s.valid = true;
        s.top = b.ret_iv.top;
        s.lo = b.ret_iv.lo;
        s.hi = b.ret_iv.hi;
        s.mod = b.ret_cg.m;
        s.rem = b.ret_cg.r;
        *out_summary = s;
    }
    out = std::move(b.out);
    return true;
}

}  // namespace ir
