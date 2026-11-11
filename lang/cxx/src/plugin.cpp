#include "plugin.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace {

using View = StmtView;
constexpr View (*view)(const std::vector<StmtP> &) = &stmt_view;
constexpr View (*else_of)(const Stmt &) = &stmt_else_of;

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

std::string ret_msg(const std::string &name, const std::string &want,
                    const std::string &got) {
    return "call: `" + name + "` returns " + want + ", the signature says " + got;
}

std::string param_own(const std::string &name, size_t i, const std::string &got) {
    return "call: `" + name + "` parameter " + std::to_string(i) +
           " takes ownership (`own ptr`), the signature says " + got;
}

std::string ret_own(const std::string &name, const std::string &got) {
    return "call: `" + name + "` returns an owned pointer (`own ptr`), the signature says " + got;
}

std::string need_move(size_t pos, const std::string &var) {
    return "call: arg " + std::to_string(pos) + " is `own ptr`, pass `move " + var +
           "` to transfer ownership";
}

std::string no_move_borrow(size_t pos) {
    return "call: arg " + std::to_string(pos) + " is `&ptr` (borrowed), remove `move`";
}

std::string no_move_out(size_t pos) {
    return "call: arg " + std::to_string(pos) + " is `out ptr`, remove `move`";
}

std::string out_need_mut_addr(size_t pos, const std::string &v) {
    return "call: arg " + std::to_string(pos) + " is `out ptr`, `&" + v +
           "` requires `let mut " + v + "`";
}

std::string out_need_mut_var(size_t pos, const std::string &v) {
    return "call: arg " + std::to_string(pos) + " is `out ptr`, `" + v +
           "` must be declared `let mut`";
}

std::string used_after_move(const std::string &v, int line) {
    return "`" + v + "` is used after `move` on line " + std::to_string(line);
}

std::string moved_twice(const std::string &v, int line) {
    return "`" + v + "` was already moved on line " + std::to_string(line);
}

std::string own_leak(const std::string &v, int line) {
    return "`" + v + "` is an owned pointer from `call` on line " + std::to_string(line) +
           " and is never moved or returned (hint: pass `move " + v +
           "` to its destructor or `free`)";
}

const std::string kOwnDrop = "call: `own ptr` result is dropped without freeing";

std::string cb_not_name(size_t pos) {
    return "arg " + std::to_string(pos) + " of `call`: callback must be a function name";
}

std::string cb_unknown(const std::string &name) {
    return "unknown callback `" + name + "`";
}

std::string cb_arity(const std::string &name, size_t got, const std::string &want_sig, size_t want) {
    return "callback `" + name + "` takes " + std::to_string(got) + " parameter(s), " +
           want_sig + " wants " + std::to_string(want);
}

std::string cb_sig(const std::string &name, const std::string &got_params,
                   const std::string &got_ret, const std::string &want_sig) {
    return "callback `" + name + "` has signature fn(" + got_params + ")->" + got_ret +
           ", expected " + want_sig;
}

std::string cb_schedule(const std::string &cb, const std::string &callee, const std::string &global) {
    return "`" + cb + "` is registered as a callback of `" + callee +
           "`, which declares `thread(any)`, but it touches `" + global +
           "`; a callback on another thread may not touch the caller's state "
           "(hint: pass it through the callback's parameters, or declare `thread(same)`)";
}

std::string cb_reenter(const std::string &cb, const std::string &callee) {
    return "`" + cb + "` is registered as a callback of `" + callee +
           "` and calls `" + callee +
           "` again; a callee may re-enter a callback unless the declaration says `reenter(no)` "
           "(hint: add `reenter(no)` to the declaration)";
}

std::string cb_close_retained(const std::string &lib, const std::string &cb, int line) {
    return "cannot close library `" + lib + "`: callback `" + cb + "` registered on line " +
           std::to_string(line) + " declares `thread(any)` and may outlive `lib_close`";
}

std::string norm_slot(const std::string &raw) {
    std::string s;
    for (unsigned char c : raw)
        if (!std::isspace(c)) s.push_back(static_cast<char>(c));
    if (s == "ownptr" || s == "moveptr") return "own ptr";
    if (s == "outptr") return "out ptr";
    if (s == "&ptr" || s == "refptr") return "&ptr";
    return s;
}

std::vector<std::string> split_top_commas(const std::string &text) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '(') depth++;
        else if (text[i] == ')') depth--;
        else if (text[i] == ',' && depth == 0) {
            std::string piece = norm_slot(text.substr(start, i - start));
            if (!piece.empty()) out.push_back(std::move(piece));
            start = i + 1;
        }
    }
    std::string tail = norm_slot(text.substr(start));
    if (!tail.empty()) out.push_back(std::move(tail));
    return out;
}

struct CbSlot {
    std::vector<std::string> params;
    std::string ret;
    std::set<std::string> contracts;
};

std::optional<CbSlot> parse_cb_slot(const std::string &slot) {
    std::string s = slot;
    std::vector<std::string> found;
    if (s.rfind("fn(", 0) != 0) return std::nullopt;
    while (true) {
        std::string hit_word, hit_val, hit_suf;
        const std::pair<std::string, std::vector<std::string>> table[] = {
            {"thread", {"same", "any"}}, {"reenter", {"yes", "no"}}
        };
        for (const auto &entry : table) {
            for (const auto &val : entry.second) {
                std::string suf = entry.first + "(" + val + ")";
                if (s.size() >= suf.size() &&
                    s.compare(s.size() - suf.size(), suf.size(), suf) == 0) {
                    hit_word = entry.first;
                    hit_val = val;
                    hit_suf = suf;
                    break;
                }
            }
            if (!hit_suf.empty()) break;
        }
        if (hit_suf.empty()) break;
        s.erase(s.size() - hit_suf.size());
        found.push_back(hit_word + ":" + hit_val);
    }
    const size_t arrow = s.rfind(")->");
    if (s.rfind("fn(", 0) != 0 || arrow == std::string::npos) return std::nullopt;
    const std::string inner = s.substr(3, arrow - 3);
    const std::string ret = s.substr(arrow + 3);
    if (ret.empty()) return std::nullopt;
    CbSlot res;
    if (!inner.empty()) res.params = split_top_commas(inner);
    res.ret = ret;
    for (const auto &c : found) res.contracts.insert(c);
    return res;
}

std::string c_type_str(const std::string &raw) {
    const std::string t = norm_slot(raw);
    if (t == "none") return "none";
    if (t.rfind("fn(", 0) == 0 || t.rfind("ptr[", 0) == 0) return "ptr";
    if (t == "str" || t == "ptr" || t == "own ptr" || t == "&ptr" || t == "out ptr") return "ptr";
    if (t == "f32" || t == "f64") return "f64";
    if (t == "i8" || t == "i16" || t == "i32" || t == "i64" || t == "u8" ||
        t == "u16" || t == "u32" || t == "u64" || t == "isize" || t == "usize" ||
        t == "bool")
        return "i64";
    return t;
}

std::string c_type(const std::optional<std::string> &ann) {
    return c_type_str(ann ? *ann : "");
}

struct ParsedSig {
    std::string ret;
    std::vector<std::string> params;
};

std::optional<ParsedSig> parse_sig(const std::string &text) {
    size_t lo = 0, hi = text.size();
    while (lo < hi && std::isspace(static_cast<unsigned char>(text[lo]))) lo++;
    while (hi > lo && std::isspace(static_cast<unsigned char>(text[hi - 1]))) hi--;
    const std::string raw = text.substr(lo, hi - lo);
    const size_t open = raw.find('(');
    if (open == 0 || open == std::string::npos || raw.empty() || raw.back() != ')')
        return std::nullopt;
    int depth = 0;
    for (size_t i = open; i < raw.size(); i++) {
        if (raw[i] == '(') depth++;
        else if (raw[i] == ')') {
            depth--;
            if (depth == 0 && i + 1 != raw.size()) return std::nullopt;
        }
    }
    if (depth != 0) return std::nullopt;
    ParsedSig s;
    s.ret = norm_slot(raw.substr(0, open));
    if (s.ret.empty()) return std::nullopt;
    const std::string rest = raw.substr(open + 1, raw.size() - open - 2);
    s.params = split_top_commas(rest);
    return s;
}

struct Declared {
    std::vector<std::string> params;
    std::string ret;
    std::vector<bool> pmoves;
    bool ret_owned = false;
};

struct FnEntry {
    std::vector<std::string> pnames;
    std::vector<std::string> panns;
    std::string ret;
    const std::vector<StmtP> *body = nullptr;
};

struct State {
    int next_id = 0;
    std::map<std::string, int> handle;         // binding -> library id
    std::map<int, int> closed;                 // library id -> line of the close
    std::map<std::string, int> tied;           // binding -> library id
    std::map<std::string, std::string> sym;    // binding -> declared name
    std::set<std::string> muts;
    std::map<std::string, int> owned_ptr;
    std::map<std::string, int> moved;
    std::set<std::string> consumed;
    std::map<int, std::vector<std::pair<std::string, int>>> retained_cb;
};

std::string lib_name(const State &st, int id) {
    for (const auto &entry : st.handle)
        if (entry.second == id) return entry.first;
    return "?";
}

bool is_var(const Expr *e) { return e && e->k == EK::Var; }

void walk_stmt_uses(const std::vector<StmtP> &body, std::set<std::string> locals,
                    std::set<std::string> &names, std::set<std::string> &calls,
                    std::set<std::string> &sym_calls);

void walk_expr_uses(const Expr *e, const std::set<std::string> &locals,
                    std::set<std::string> &names,
                    std::set<std::string> &calls, std::set<std::string> &sym_calls) {
    if (!e) return;
    if (e->k == EK::Var) {
        if (!locals.count(e->a)) names.insert(e->a);
    } else if (e->k == EK::Call) {
        if (e->a == kCall && !e->kids.empty() && is_var(e->kids[0].get()))
            sym_calls.insert(e->kids[0]->a);
        else
            calls.insert(e->a);
    }
    for (const auto &k : e->kids) walk_expr_uses(k.get(), locals, names, calls, sym_calls);
    if (e->fn) {
        std::set<std::string> inner_locals = locals;
        for (const auto &p : e->fn->params) inner_locals.insert(p.name);
        walk_stmt_uses(e->fn->body, std::move(inner_locals), names, calls, sym_calls);
    }
}

void walk_stmt_uses(const std::vector<StmtP> &body, std::set<std::string> locals,
                    std::set<std::string> &names, std::set<std::string> &calls,
                    std::set<std::string> &sym_calls) {
    for (const auto &sp : body) {
        if (!sp) continue;
        const Stmt &s = *sp;
        if (s.k == SK::Set && !s.a.empty() && !locals.count(s.a)) names.insert(s.a);
        walk_expr_uses(s.e1.get(), locals, names, calls, sym_calls);
        walk_expr_uses(s.e2.get(), locals, names, calls, sym_calls);
        walk_expr_uses(s.e3.get(), locals, names, calls, sym_calls);
        if ((s.k == SK::Let || s.k == SK::LetTmp) && !s.a.empty()) locals.insert(s.a);
        if (s.k == SK::ForRange || s.k == SK::ForIter) {
            std::set<std::string> body_locals = locals;
            if (!s.a.empty()) body_locals.insert(s.a);
            walk_stmt_uses(s.body, std::move(body_locals), names, calls, sym_calls);
        } else {
            walk_stmt_uses(s.body, locals, names, calls, sym_calls);
        }
        if (s.k == SK::Try && s.opt1 && !s.opt1->empty()) {
            std::set<std::string> catch_locals = locals;
            catch_locals.insert(*s.opt1);
            walk_stmt_uses(s.body2, std::move(catch_locals), names, calls, sym_calls);
        } else {
            walk_stmt_uses(s.body2, locals, names, calls, sym_calls);
        }
        for (const Stmt *el = s.els.get(); el != nullptr; el = el->els.get()) {
            walk_expr_uses(el->e1.get(), locals, names, calls, sym_calls);
            walk_stmt_uses(el->body, locals, names, calls, sym_calls);
        }
    }
}

std::optional<std::string> cb_first_global(const std::string &start,
                                           const std::map<std::string, FnEntry> &fns,
                                           const std::set<std::string> &globals) {
    std::set<std::string> seen;
    std::vector<std::string> stack{start};
    while (!stack.empty()) {
        std::string cur = stack.back();
        stack.pop_back();
        if (seen.count(cur)) continue;
        auto it = fns.find(cur);
        if (it == fns.end() || !it->second.body) continue;
        seen.insert(cur);
        std::set<std::string> locals(it->second.pnames.begin(), it->second.pnames.end());
        std::set<std::string> names, calls, sym_calls;
        walk_stmt_uses(*it->second.body, std::move(locals), names, calls, sym_calls);
        for (const auto &n : names) {
            if (globals.count(n)) return n;
        }
        for (const auto &c : calls)
            if (c != cur) stack.push_back(c);
    }
    return std::nullopt;
}

bool cb_reenters(const std::string &start, const std::string &sym_var,
                 const std::string &sym_name,
                 const std::map<std::string, FnEntry> &fns) {
    std::set<std::string> seen;
    std::vector<std::string> stack{start};
    while (!stack.empty()) {
        std::string cur = stack.back();
        stack.pop_back();
        if (seen.count(cur)) continue;
        auto it = fns.find(cur);
        if (it == fns.end() || !it->second.body) continue;
        seen.insert(cur);
        std::set<std::string> locals, names, calls, sym_calls;
        walk_stmt_uses(*it->second.body, locals, names, calls, sym_calls);
        if (!sym_var.empty() && sym_calls.count(sym_var)) return true;
        if (!sym_name.empty() && calls.count(sym_name)) return true;
        for (const auto &c : calls)
            if (c != cur) stack.push_back(c);
    }
    return false;
}

void dead_use(const std::string &var, std::vector<BorrowDiag> &out, const State &st,
              int line) {
    auto t = st.tied.find(var);
    if (t == st.tied.end()) return;
    auto c = st.closed.find(t->second);
    if (c == st.closed.end()) return;
    out.push_back(BorrowDiag{line, dead(var, lib_name(st, t->second), c->second),
                             std::nullopt});
}

void consume_move(const std::string &var, std::vector<BorrowDiag> &out, State &st, int line) {
    dead_use(var, out, st, line);
    if (!st.owned_ptr.count(var)) return;
    auto mv = st.moved.find(var);
    if (mv != st.moved.end()) {
        out.push_back(BorrowDiag{line, moved_twice(var, mv->second), std::nullopt});
        return;
    }
    st.moved[var] = line;
    st.consumed.insert(var);
}

void bind_var(const std::string &name, const Expr *expr, State &st, int line) {
    if (!expr || name.empty()) return;
    if (is_var(expr)) {
        const std::string &src = expr->a;
        auto h = st.handle.find(src);
        if (h != st.handle.end()) st.handle[name] = h->second;
        auto t = st.tied.find(src);
        if (t != st.tied.end()) st.tied[name] = t->second;
        auto s = st.sym.find(src);
        if (s != st.sym.end()) st.sym[name] = s->second;
        return;
    }
    if (expr->k == EK::Move && !expr->kids.empty() && is_var(expr->kids[0].get())) {
        const std::string &src = expr->kids[0]->a;
        auto t = st.tied.find(src);
        if (t != st.tied.end()) st.tied[name] = t->second;
        if (st.owned_ptr.count(src)) {
            st.consumed.insert(src);
            st.owned_ptr[name] = line;
        }
        return;
    }
    if (expr->k != EK::Call) return;
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
    if (callee == kCall && args.size() >= 2) {
        if (!args[1] || args[1]->k != EK::Str) return;
        auto sig = parse_sig(args[1]->a);
        if (sig && (sig->ret == "ptr" || sig->ret == "own ptr")) {
            if (is_var(args[0].get())) {
                auto t = st.tied.find(args[0]->a);
                if (t != st.tied.end()) st.tied[name] = t->second;
            }
            if (sig->ret == "own ptr") st.owned_ptr[name] = line;
        }
    }
}

void expr_check(const Expr *e, const std::map<std::string, Declared> &decls,
                const std::map<std::string, FnEntry> &fns,
                const std::set<std::string> &globals,
                std::vector<BorrowDiag> &out, State &st, int line);

void check_cb_slot(size_t pos, const std::string &slot, const Expr *arg,
                   const std::string &sym_var, const std::string &sym_name,
                   std::optional<int> lib,
                   const std::map<std::string, FnEntry> &fns,
                   const std::set<std::string> &globals,
                   std::vector<BorrowDiag> &out, State &st, int line) {
    auto cb_info = parse_cb_slot(slot);
    if (!cb_info) return;
    if (!arg || arg->k != EK::Str) {
        out.push_back(BorrowDiag{line, cb_not_name(pos), std::nullopt});
        return;
    }
    const std::string &cb_name = arg->a;
    auto fit = fns.find(cb_name);
    if (fit == fns.end()) {
        out.push_back(BorrowDiag{line, cb_unknown(cb_name), std::nullopt});
        return;
    }
    std::string clean_params;
    for (size_t i = 0; i < cb_info->params.size(); i++) {
        if (i) clean_params += ",";
        clean_params += cb_info->params[i];
    }
    std::string clean_slot = "fn(" + clean_params + ")->" + cb_info->ret;
    const FnEntry &fe = fit->second;
    if (fe.panns.size() != cb_info->params.size()) {
        out.push_back(BorrowDiag{
            line, cb_arity(cb_name, fe.panns.size(), clean_slot, cb_info->params.size()),
            std::nullopt});
        return;
    }
    if (fe.panns != cb_info->params || fe.ret != cb_info->ret) {
        std::string got_p;
        for (size_t i = 0; i < fe.panns.size(); i++) {
            if (i) got_p += ",";
            got_p += fe.panns[i];
        }
        out.push_back(BorrowDiag{
            line, cb_sig(cb_name, got_p, fe.ret, clean_slot), std::nullopt});
        return;
    }
    const std::string label = !sym_name.empty() ? sym_name : (!sym_var.empty() ? sym_var : "call");
    if (cb_info->contracts.count("thread:any")) {
        if (auto touch = cb_first_global(cb_name, fns, globals))
            out.push_back(BorrowDiag{line, cb_schedule(cb_name, label, *touch), std::nullopt});
        if (lib) st.retained_cb[*lib].push_back({cb_name, line});
    }
    if (!cb_info->contracts.count("reenter:no") && cb_reenters(cb_name, sym_var, sym_name, fns))
        out.push_back(BorrowDiag{line, cb_reenter(cb_name, label), std::nullopt});
}

std::set<size_t> plugin_call(const std::vector<ExprP> &args,
                             const std::map<std::string, Declared> &decls,
                             const std::map<std::string, FnEntry> &fns,
                             const std::set<std::string> &globals,
                             std::vector<BorrowDiag> &out, State &st, int line) {
    std::set<size_t> handled;
    if (args.size() < 2) return handled;
    std::string sym_var;
    std::optional<int> lib;
    if (is_var(args[0].get())) {
        sym_var = args[0]->a;
        auto t = st.tied.find(sym_var);
        if (t != st.tied.end()) lib = t->second;
    }
    if (!sym_var.empty() && lib) {
        auto c = st.closed.find(*lib);
        if (c != st.closed.end()) {
            out.push_back(BorrowDiag{line, dead(sym_var, lib_name(st, *lib), c->second),
                                     std::nullopt});
            return handled;
        }
    }
    if (!args[1] || args[1]->k != EK::Str) return handled;
    auto sig = parse_sig(args[1]->a);
    if (!sig) return handled;
    std::string sym_name;
    if (!sym_var.empty()) {
        auto s = st.sym.find(sym_var);
        if (s != st.sym.end()) sym_name = s->second;
    }

    for (size_t idx = 0; idx < sig->params.size() && idx + 2 < args.size(); idx++) {
        const std::string &slot = sig->params[idx];
        const Expr *arg = args[idx + 2].get();
        const size_t pos = idx + 1;
        if (slot == "own ptr") {
            handled.insert(idx);
            if (arg && arg->k == EK::Move && !arg->kids.empty() && is_var(arg->kids[0].get())) {
                consume_move(arg->kids[0]->a, out, st, line);
            } else if (is_var(arg)) {
                dead_use(arg->a, out, st, line);
                out.push_back(BorrowDiag{line, need_move(pos, arg->a), std::nullopt});
            } else {
                expr_check(arg, decls, fns, globals, out, st, line);
            }
        } else if (slot == "&ptr") {
            handled.insert(idx);
            if (arg && arg->k == EK::Move)
                out.push_back(BorrowDiag{line, no_move_borrow(pos), std::nullopt});
            else
                expr_check(arg, decls, fns, globals, out, st, line);
        } else if (slot == "out ptr") {
            handled.insert(idx);
            if (arg && arg->k == EK::Move) {
                out.push_back(BorrowDiag{line, no_move_out(pos), std::nullopt});
            } else if (arg && arg->k == EK::Un && arg->a == "&" && !arg->kids.empty() &&
                       is_var(arg->kids[0].get())) {
                const std::string &v = arg->kids[0]->a;
                dead_use(v, out, st, line);
                auto mv = st.moved.find(v);
                if (mv != st.moved.end())
                    out.push_back(BorrowDiag{line, used_after_move(v, mv->second), std::nullopt});
                else if (!st.muts.count(v))
                    out.push_back(BorrowDiag{line, out_need_mut_addr(pos, v), std::nullopt});
            } else if (is_var(arg)) {
                const std::string &v = arg->a;
                dead_use(v, out, st, line);
                auto mv = st.moved.find(v);
                if (mv != st.moved.end())
                    out.push_back(BorrowDiag{line, used_after_move(v, mv->second), std::nullopt});
                else if (!st.muts.count(v))
                    out.push_back(BorrowDiag{line, out_need_mut_var(pos, v), std::nullopt});
            } else {
                expr_check(arg, decls, fns, globals, out, st, line);
            }
        } else if (slot.rfind("fn(", 0) == 0) {
            handled.insert(idx);
            check_cb_slot(pos, slot, arg, sym_var, sym_name, lib, fns, globals, out, st, line);
        }
    }

    if (sym_name.empty()) return handled;
    auto declared = decls.find(sym_name);
    if (declared == decls.end()) return handled;
    const Declared &want = declared->second;
    if (sig->params.size() != want.params.size()) {
        out.push_back(BorrowDiag{
            line, arity(sym_name, want.params.size(), sig->params.size()),
            std::nullopt});
        return handled;
    }
    for (size_t i = 0; i < sig->params.size(); i++) {
        const std::string got = c_type_str(sig->params[i]);
        if (got != want.params[i]) {
            out.push_back(BorrowDiag{
                line, param(sym_name, i + 1, want.params[i], got), std::nullopt});
            return handled;
        }
        if (i < want.pmoves.size() && want.pmoves[i] && want.params[i] == "ptr" &&
            sig->params[i] != "own ptr") {
            out.push_back(BorrowDiag{
                line, param_own(sym_name, i + 1, sig->params[i]), std::nullopt});
            return handled;
        }
    }
    const std::string got = c_type_str(sig->ret);
    if (got != want.ret) {
        out.push_back(BorrowDiag{line, ret_msg(sym_name, want.ret, got), std::nullopt});
        return handled;
    }
    if (want.ret_owned && want.ret == "ptr" && sig->ret != "own ptr")
        out.push_back(BorrowDiag{line, ret_own(sym_name, sig->ret), std::nullopt});
    return handled;
}

void body_check(const View &body, const std::map<std::string, Declared> &decls,
                const std::map<std::string, FnEntry> &fns,
                const std::set<std::string> &globals,
                std::vector<BorrowDiag> &out, State &st, int hint);
void finish_body(const State &st, std::vector<BorrowDiag> &out);

void expr_check(const Expr *e, const std::map<std::string, Declared> &decls,
                const std::map<std::string, FnEntry> &fns,
                const std::set<std::string> &globals,
                std::vector<BorrowDiag> &out, State &st, int line) {
    if (!e) return;
    if (e->k == EK::Var) {
        dead_use(e->a, out, st, line);
        auto mv = st.moved.find(e->a);
        if (mv != st.moved.end())
            out.push_back(BorrowDiag{line, used_after_move(e->a, mv->second), std::nullopt});
        return;
    }
    if (e->k == EK::Move) {
        if (!e->kids.empty() && is_var(e->kids[0].get()))
            consume_move(e->kids[0]->a, out, st, line);
        else if (!e->kids.empty())
            expr_check(e->kids[0].get(), decls, fns, globals, out, st, line);
        return;
    }
    if (e->k != EK::Call) {
        for (const auto &kid : e->kids)
            expr_check(kid.get(), decls, fns, globals, out, st, line);
        if (e->fn) {
            State sub;
            sub.handle = st.handle;
            sub.tied = st.tied;
            sub.sym = st.sym;
            sub.closed = st.closed;
            sub.moved = st.moved;
            sub.retained_cb = st.retained_cb;
            sub.next_id = st.next_id;
            for (const auto &p : e->fn->params)
                if (p.mode == Mode::Mut && !p.name.empty()) sub.muts.insert(p.name);
            body_check(view(e->fn->body), decls, fns, globals, out, sub, line);
            finish_body(sub, out);
        }
        return;
    }
    const std::string &callee = e->a;
    const std::vector<ExprP> &args = e->kids;
    if (callee == kClose) {
        if (args.size() == 1 && is_var(args[0].get())) {
            auto h = st.handle.find(args[0]->a);
            if (h != st.handle.end()) {
                auto c = st.closed.find(h->second);
                if (c != st.closed.end()) {
                    out.push_back(BorrowDiag{line, twice(args[0]->a, c->second),
                                             std::nullopt});
                } else {
                    auto rc = st.retained_cb.find(h->second);
                    if (rc != st.retained_cb.end() && !rc->second.empty()) {
                        out.push_back(BorrowDiag{
                            line,
                            cb_close_retained(args[0]->a, rc->second[0].first, rc->second[0].second),
                            std::nullopt});
                    } else {
                        st.closed[h->second] = line;
                    }
                }
            }
        }
        for (const auto &a : args) expr_check(a.get(), decls, fns, globals, out, st, line);
        return;
    }
    if (callee == kSym) {
        if (!args.empty() && is_var(args[0].get())) {
            auto h = st.handle.find(args[0]->a);
            if (h != st.handle.end()) {
                auto c = st.closed.find(h->second);
                if (c != st.closed.end())
                    out.push_back(BorrowDiag{line, closed_value(args[0]->a, c->second),
                                             std::nullopt});
            }
        }
        for (const auto &a : args) expr_check(a.get(), decls, fns, globals, out, st, line);
        return;
    }
    if (callee == kCall && args.size() >= 2) {
        auto handled = plugin_call(args, decls, fns, globals, out, st, line);
        for (size_t i = 2; i < args.size(); i++) {
            if (handled.count(i - 2)) continue;
            expr_check(args[i].get(), decls, fns, globals, out, st, line);
        }
        return;
    }
    for (const auto &a : args) expr_check(a.get(), decls, fns, globals, out, st, line);
}

bool view_exits(const View &v) {
    if (v.empty() || !v.back()) return false;
    const SK k = v.back()->k;
    return k == SK::Return || k == SK::Break || k == SK::Continue;
}

void merge_into(State &dst, const State &a, const State &b) {
    dst.next_id = std::max(a.next_id, b.next_id);
    for (const auto &kv : a.closed) dst.closed.insert(kv);
    for (const auto &kv : b.closed) dst.closed.insert(kv);
    for (const auto &kv : a.moved) dst.moved.insert(kv);
    for (const auto &kv : b.moved) dst.moved.insert(kv);
    for (const auto &kv : a.owned_ptr) dst.owned_ptr.insert(kv);
    for (const auto &kv : b.owned_ptr) dst.owned_ptr.insert(kv);
    std::set<std::string> all_own;
    for (const auto &kv : dst.owned_ptr) all_own.insert(kv.first);
    dst.consumed.clear();
    for (const std::string &nm : all_own) {
        const bool in_a = !a.owned_ptr.count(nm) || a.consumed.count(nm);
        const bool in_b = !b.owned_ptr.count(nm) || b.consumed.count(nm);
        if (in_a && in_b) dst.consumed.insert(nm);
    }
    for (const State *src : {&a, &b}) {
        for (const auto &kv : src->retained_cb) {
            auto &vec = dst.retained_cb[kv.first];
            for (const auto &it : kv.second)
                if (std::find(vec.begin(), vec.end(), it) == vec.end()) vec.push_back(it);
        }
    }
}

void body_check(const View &body, const std::map<std::string, Declared> &decls,
                const std::map<std::string, FnEntry> &fns,
                const std::set<std::string> &globals,
                std::vector<BorrowDiag> &out, State &st, int hint);

void check_two_arms(const View &av, const View &bv,
                    const std::map<std::string, Declared> &decls,
                    const std::map<std::string, FnEntry> &fns,
                    const std::set<std::string> &globals,
                    std::vector<BorrowDiag> &out, State &st, int line) {
    State a_st = st;
    body_check(av, decls, fns, globals, out, a_st, line);
    State b_st = st;
    b_st.next_id = a_st.next_id;
    body_check(bv, decls, fns, globals, out, b_st, line);
    const bool a_exit = view_exits(av);
    const bool b_exit = view_exits(bv);
    auto emit_arm_leaks = [&](const State &arm) {
        for (const auto &kv : arm.owned_ptr) {
            if (!arm.consumed.count(kv.first) && !st.owned_ptr.count(kv.first))
                out.push_back(BorrowDiag{kv.second, own_leak(kv.first, kv.second), std::nullopt});
        }
    };
    if (a_exit && !b_exit) {
        emit_arm_leaks(a_st);
        st = std::move(b_st);
    } else if (b_exit && !a_exit) {
        emit_arm_leaks(b_st);
        st = std::move(a_st);
    } else {
        merge_into(st, a_st, b_st);
    }
}

void body_check(const View &body, const std::map<std::string, Declared> &decls,
                const std::map<std::string, FnEntry> &fns,
                const std::set<std::string> &globals,
                std::vector<BorrowDiag> &out, State &st, int hint) {
    for (const Stmt *s : body) {
        if (!s) continue;
        const int line = s->line ? s->line : (hint ? hint : 1);
        switch (s->k) {
        case SK::Let:
        case SK::LetTmp:
            if (s->mut && !s->a.empty()) st.muts.insert(s->a);
            expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            bind_var(s->a, s->e1.get(), st, line);
            break;
        case SK::Set:
            expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            if (!s->a.empty()) {
                auto ow = st.owned_ptr.find(s->a);
                if (ow != st.owned_ptr.end() && !st.consumed.count(s->a)) {
                    out.push_back(BorrowDiag{ow->second, own_leak(s->a, ow->second), std::nullopt});
                }
                st.moved.erase(s->a);
                st.owned_ptr.erase(s->a);
                st.consumed.erase(s->a);
                st.tied.erase(s->a);
                st.sym.erase(s->a);
                st.handle.erase(s->a);
                bind_var(s->a, s->e1.get(), st, line);
            }
            break;
        case SK::SetField:
            expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            expr_check(s->e2.get(), decls, fns, globals, out, st, line);
            break;
        case SK::SetIdx:
            expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            expr_check(s->e2.get(), decls, fns, globals, out, st, line);
            expr_check(s->e3.get(), decls, fns, globals, out, st, line);
            break;
        case SK::Return:
            if (s->e1) {
                if (is_var(s->e1.get()) && st.owned_ptr.count(s->e1->a))
                    st.consumed.insert(s->e1->a);
                else if (s->e1->k == EK::Move && !s->e1->kids.empty() &&
                         is_var(s->e1->kids[0].get()) &&
                         st.owned_ptr.count(s->e1->kids[0]->a))
                    st.consumed.insert(s->e1->kids[0]->a);
                expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            }
            break;
        case SK::Expr:
            if (s->e1 && s->e1->k == EK::Call && s->e1->a == kCall &&
                s->e1->kids.size() >= 2 && s->e1->kids[1] &&
                s->e1->kids[1]->k == EK::Str) {
                auto ps = parse_sig(s->e1->kids[1]->a);
                if (ps && ps->ret == "own ptr")
                    out.push_back(BorrowDiag{line, kOwnDrop, std::nullopt});
            }
            expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            break;
        case SK::Defer:
            expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            break;
        case SK::If:
            expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            check_two_arms(view(s->body), else_of(*s), decls, fns, globals, out, st, line);
            break;
        case SK::While:
            expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            body_check(view(s->body), decls, fns, globals, out, st, line);
            break;
        case SK::ForRange:
            expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            expr_check(s->e2.get(), decls, fns, globals, out, st, line);
            body_check(view(s->body), decls, fns, globals, out, st, line);
            break;
        case SK::ForIter:
            expr_check(s->e1.get(), decls, fns, globals, out, st, line);
            body_check(view(s->body), decls, fns, globals, out, st, line);
            break;
        case SK::Spawn:
        case SK::Scope:
        case SK::Unsafe:
        case SK::Block:
            body_check(view(s->body), decls, fns, globals, out, st, line);
            break;
        case SK::Try:
            check_two_arms(view(s->body), view(s->body2), decls, fns, globals, out, st, line);
            break;
        default:
            break;
        }
    }
}

void finish_body(const State &st, std::vector<BorrowDiag> &out) {
    std::vector<std::pair<int, std::string>> order;
    for (const auto &kv : st.owned_ptr) {
        if (!st.consumed.count(kv.first))
            order.push_back({kv.second, kv.first});
    }
    std::sort(order.begin(), order.end());
    for (const auto &it : order)
        out.push_back(BorrowDiag{it.first, own_leak(it.second, it.first), std::nullopt});
}

const Stmt *inner(const Stmt &s) {
    const Stmt *p = &s;
    while (p->k == SK::Pub && !p->body.empty()) p = p->body[0].get();
    return p;
}

}  // namespace

std::vector<BorrowDiag> plugin_check(const std::vector<StmtP> &body) {
    std::vector<BorrowDiag> out;
    std::map<std::string, Declared> decls;
    std::map<std::string, FnEntry> fns;
    std::set<std::string> globals;
    std::vector<std::pair<View, std::set<std::string>>> bodies;
    View top;
    std::function<void(const std::vector<StmtP> &)> collect_top =
        [&](const std::vector<StmtP> &stmts) {
            for (const auto &sp : stmts) {
                const Stmt *node = inner(*sp);
                if (node->k == SK::Block) {
                    collect_top(node->body);
                    continue;
                }
                if (node->k == SK::Extern) {
                    Declared d;
                    for (const Param &p : node->ext.params) {
                        d.params.push_back(c_type(p.ann));
                        d.pmoves.push_back(p.mode == Mode::Move);
                    }
                    d.ret = c_type(node->ext.ret);
                    d.ret_owned = node->ext.owned;
                    decls[node->ext.name] = d;
                    continue;
                }
                if (node->k == SK::ExportFn || node->k == SK::Fn) {
                    if (node->fns.empty()) continue;
                    const FnDecl &f = node->fns[0];
                    const std::string fname = f.name ? *f.name : node->a;
                    FnEntry fe;
                    std::set<std::string> init_muts;
                    for (const Param &p : f.params) {
                        fe.pnames.push_back(p.name);
                        fe.panns.push_back(p.ann ? norm_slot(*p.ann) : "");
                        if (p.mode == Mode::Mut) init_muts.insert(p.name);
                    }
                    fe.ret = (f.ret && !f.ret->empty()) ? norm_slot(*f.ret) : "none";
                    fe.body = &f.body;
                    fns[fname] = fe;
                    if (node->k == SK::ExportFn) {
                        Declared d;
                        for (const Param &p : f.params) {
                            d.params.push_back(c_type(p.ann));
                            d.pmoves.push_back(p.mode == Mode::Move);
                        }
                        d.ret = c_type(f.ret);
                        decls[fname] = d;
                    }
                    bodies.push_back({view(f.body), std::move(init_muts)});
                    continue;
                }
                if (node->k == SK::Impl) {
                    for (const FnDecl &f : node->fns) {
                        std::set<std::string> init_muts;
                        for (const Param &p : f.params)
                            if (p.mode == Mode::Mut) init_muts.insert(p.name);
                        bodies.push_back({view(f.body), std::move(init_muts)});
                    }
                    continue;
                }
                if (is_decl_stmt(node->k)) continue;
                if (node->k == SK::Let && !node->a.empty()) globals.insert(node->a);
                top.push_back(node);
            }
        };
    collect_top(body);
    bodies.push_back({top, {}});   // a script is one body, like `main`
    for (const auto &entry : bodies) {
        State st;
        st.muts = entry.second;
        body_check(entry.first, decls, fns, globals, out, st, 0);
        finish_body(st, out);
    }
    return out;
}
