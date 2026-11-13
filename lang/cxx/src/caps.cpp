#include "caps.h"

#include "builtins.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using View = StmtView;
constexpr View (*view)(const std::vector<StmtP> &) = &stmt_view;
constexpr View (*else_of)(const Stmt &) = &stmt_else_of;

// True when control cannot fall out of the body at the bottom: the same rule as
// `always_exits` in the ownership pass. A branch that returns does not join into
// the state after the `if`, because on that path there is no after -- and joining
// it made free-on-the-error-branch-then-return look like a second free.
bool exits(const View &body) {
    if (body.empty()) return false;
    const Stmt &last = *body.back();
    // Only `return` leaves the function: a `break`/`continue` path still ends
    // up after the loop, where the place is moved.
    if (last.k == SK::Return) return true;
    if (last.k == SK::If) {
        if (!last.els) return false;
        return exits(view(last.body)) && exits(else_of(last));
    }
    if (last.k == SK::Try) {
        return exits(view(last.body)) && exits(view(last.body2));
    }
    return false;
}

std::vector<Mode> modes_of(const FnDecl &f) {
    std::vector<Mode> m;
    m.reserve(f.params.size());
    for (const Param &p : f.params) m.push_back(p.mode);
    return m;
}

// The lattice. Every value is a summary of what the place still allows, so two
// paths meeting at a join keep the weaker one: a branch that only reads cannot
// talk a place out of a write that happens on the other branch.
enum Cap { kNone = 0, kRead = 1, kMoved = 2, kWrite = 3, kTop = 4 };

int join(int a, int b) {
    if (a == b) return a;
    if (a == kTop || b == kTop) return kTop;
    if (a == kNone) return b;
    if (b == kNone) return a;
    if (a == kMoved || b == kMoved) return kMoved;
    if ((a == kRead && b == kWrite) || (a == kWrite && b == kRead)) return kRead;
    return kWrite;
}

std::string moved_use(const std::string &p) { return "use of moved `" + p + "`"; }
std::string moved_hint(const std::string &verb, const std::string &p,
                       const std::string &advice) {
    return "use of " + verb + " `" + p + "` (hint: " + advice + ")";
}

// What the advice says depends on how the value left: a move, a channel send
// that handed it to the receiver.
std::string advice_of(const std::string &verb) {
    if (verb == "sent")
        return "a channel send hands the value to the receiver; send a copy if you need it back";
    return "the value was moved; use a copy or re-bind";
}

const std::set<std::string> kClosers{"tcp_close", "lib_close"};
std::string escapes_return(const std::string &p) { return "ref `" + p + "` escapes via return"; }
std::string escapes_spawn(const std::string &p) { return "ref `" + p + "` escapes via spawn"; }
std::string two_writers(const std::string &p, const std::string &a, const std::string &b) {
    return "double mut borrow of `" + p + "`: `" + a + "` clashes with `" + b + "`";
}
std::string write_while_read(const std::string &p, const std::string &who) {
    return "cannot mutate immutable `" + p + "` while borrowed by `" + who + "`";
}
std::string assign_while_read(const std::string &p, const std::string &who) {
    return "cannot assign `" + p + "` while borrowed by `" + who + "`";
}
std::string move_while_read(const std::string &p, const std::string &who) {
    return "cannot move `" + p + "` while borrowed by `" + who + "`";
}

struct Holder {
    std::string name;
    int line = 0;
    int cap = kNone;
};

struct State {
    std::map<std::string, int> caps;                        // place -> capability
    std::map<std::string, std::vector<Holder>> who;          // place -> who claims it
    std::map<std::string, std::string> alias;                // binding -> the place it denotes
    std::map<std::string, std::pair<std::string, int>> second;  // place -> a second writer
    std::set<std::string> copies;                            // bindings holding a Copy value
    std::set<std::string> nums;                              // bindings holding a number
    std::set<std::string> params;                            // storage belongs to the caller
    std::set<std::string> muts;                              // bindings declared `let mut`
    struct Verb {
        std::string how;      // "moved" or "sent"
        bool root = false;    // the whole place went, not one path inside it
        int line = 0;
    };
    std::map<std::string, Verb> verb;                        // place -> how it went
    std::set<std::string> consts;                            // `const`: the walk owns it
    std::map<std::string, std::string> anns;                 // binding -> declared type

    std::string place(std::string name) const {
        for (int i = 0; i < 32; i++) {
            auto it = alias.find(name);
            if (it == alias.end()) break;
            name = it->second;
        }
        return name;
    }

    int cap(const std::string &p) const {
        auto it = caps.find(p);
        return it == caps.end() ? kNone : it->second;
    }

    // Take the place at `cap` for `name`; an earlier name keeps its own claim.
    void claim(const std::string &raw, int c, const std::string &name, int line) {
        std::string p = place(raw);
        caps[p] = c;
        std::vector<Holder> held;
        for (const Holder &h : who[p])
            if (h.name != name) held.push_back(h);
        held.push_back({name, line, c});
        who[p] = std::move(held);
    }

    // Drop what was learned about a place and everything under it. A loop body
    // declares its own bindings, and those are fresh at the top of every
    // iteration: carrying `s: moved` out of pass one into pass two is what made
    // `let s = ...; let t = move s` inside a loop read as a use-after-move.
    void forget(const std::string &place) {
        for (auto it = caps.begin(); it != caps.end();) {
            if (it->first == place || it->first.compare(0, place.size() + 1, place + ".") == 0)
                it = caps.erase(it);
            else
                ++it;
        }
        for (auto it = who.begin(); it != who.end();) {
            if (it->first == place || it->first.compare(0, place.size() + 1, place + ".") == 0)
                it = who.erase(it);
            else
                ++it;
        }
        for (auto it = verb.begin(); it != verb.end();) {
            if (it->first == place || it->first.compare(0, place.size() + 1, place + ".") == 0)
                it = verb.erase(it);
            else
                ++it;
        }
        for (auto it = second.begin(); it != second.end();) {
            if (it->first == place || it->first.compare(0, place.size() + 1, place + ".") == 0)
                it = second.erase(it);
            else
                ++it;
        }
        for (auto it = alias.begin(); it != alias.end();) {
            if (it->second == place || it->second.compare(0, place.size() + 1, place + ".") == 0)
                it = alias.erase(it);
            else
                ++it;
        }
    }

    void join_from(const State &other) {
        for (const auto &kv : other.verb) verb.insert({kv.first, kv.second});
        std::set<std::string> places;
        for (const auto &kv : caps) places.insert(kv.first);
        for (const auto &kv : other.caps) places.insert(kv.first);
        for (const std::string &p : places) {
            int merged = join(cap(p), other.cap(p));
            if (merged == kNone) {
                caps.erase(p);
                who.erase(p);
                continue;
            }
            caps[p] = merged;
            auto o = other.who.find(p);
            if (o != other.who.end()) {
                auto &vec = who[p];
                for (const Holder &h : o->second) {
                    bool dup = false;
                    for (const Holder &cur_h : vec) {
                        if (cur_h.name == h.name && cur_h.cap == h.cap) { dup = true; break; }
                    }
                    if (!dup) vec.push_back(h);
                }
            }
        }
    }
};

// The verdict for a use of something gone: the text, and where it went. A move
// of the whole place carries the advice and points at the line that took it; a
// move of one path inside it carries neither.
std::pair<std::string, std::optional<std::pair<int, std::string>>> gone_message(
    const State &st, const std::string &place, const std::string &under) {
    auto it = st.verb.find(under);
    if (it != st.verb.end() && it->second.root)
        return {moved_hint(it->second.how, under, advice_of(it->second.how)),
                std::make_pair(it->second.line,
                               "`" + under + "` " + it->second.how + " here")};
    (void)place;
    return {moved_use(under), std::nullopt};
}

std::optional<std::string> place_of(const Expr *e) {
    if (e == nullptr) return std::nullopt;
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

std::vector<std::string> prefixes_of(std::string p) {
    std::vector<std::string> out{p};
    while (p.find('.') != std::string::npos) {
        p = p.substr(0, p.rfind('.'));
        out.push_back(p);
    }
    return out;
}

// Moving `p.x` leaves `p.y` alone, so only a prefix makes a place gone.
std::optional<std::string> moved_under(const State &st, const std::string &p) {
    for (const std::string &pre : prefixes_of(p))
        if (st.cap(pre) == kMoved) return pre;
    return std::nullopt;
}

// `v[0]` is a value when `v` is declared `vec[i64]`: SPEC 10, Copy elements.
bool elem_copy(const State &st, const Expr *base) {
    while (base && base->k == EK::Quest && !base->kids.empty()) base = base->kids[0].get();
    if (base == nullptr) return false;
    if (base->k == EK::Move || base->k == EK::Field || base->k == EK::Idx)
        return elem_copy(st, base->kids[0].get());
    if (base->k != EK::Var) return false;
    auto it = st.anns.find(base->a);
    if (it == st.anns.end()) return false;
    const std::string el = container_elem_ann(it->second);
    return !el.empty() && is_copy_scalar_ann(el);
}

// A move of `p` cannot happen while something inside `p` has been moved out.
std::optional<std::string> moved_below(const State &st, const std::string &p) {
    for (const auto &kv : st.caps)
        if (kv.first.rfind(p + ".", 0) == 0 && kv.second == kMoved) return kv.first;
    return std::nullopt;
}

std::vector<std::string> relatives(const State &st, const std::string &p) {
    std::vector<std::string> out = prefixes_of(p);
    for (const auto &kv : st.caps)
        if (kv.first.rfind(p + ".", 0) == 0) out.push_back(kv.first);
    return out;
}

std::optional<Holder> live_reader(const State &st, const std::string &p,
                                  const std::optional<std::string> &writer) {
    for (const std::string &rel : relatives(st, p)) {
        auto it = st.who.find(rel);
        if (it == st.who.end()) continue;
        for (const Holder &h : it->second)
            if (h.cap == kRead && !(writer && h.name == *writer)) return h;
    }
    return std::nullopt;
}

std::optional<Holder> live_writer(const State &st, const std::string &p,
                                  const std::optional<std::string> &writer) {
    for (const std::string &rel : relatives(st, p)) {
        auto it = st.who.find(rel);
        if (it == st.who.end()) continue;
        for (const Holder &h : it->second)
            if (h.cap == kWrite && !(writer && h.name == *writer)) return h;
    }
    return std::nullopt;
}

bool is_alias(const Expr *e) {
    return e && (e->k == EK::Var || e->k == EK::Move || e->k == EK::Field || e->k == EK::Idx);
}

bool copy_expr(const Expr *e) {
    return e && (e->k == EK::Num || e->k == EK::Bool || e->k == EK::None || e->k == EK::RStr);
}

std::string root_name(const std::string &p) { return p.substr(0, p.find('.')); }


// `let` bindings and loop variables of this block. Nested blocks are their own
// scopes and hand nothing back, so the walk does not descend: a loop-carried
// value can only be one this level declares.
static std::set<std::string> bound_in(const View &body) {
    std::set<std::string> out;
    for (const Stmt *sp : body) {
        if (!sp) continue;
        const Stmt &s = *sp;
        // `let [a, b] = pair` is lowered to one `Let` per name, so a plain `Let`
        // covers destructuring without a second spelling here.
        if ((s.k == SK::Let || s.k == SK::ForRange || s.k == SK::ForIter) && !s.a.empty())
            out.insert(s.a);
    }
    return out;
}

// names used inside a subtree, for the last-use table and for `spawn`

void used_stmt(const Stmt &s, std::vector<std::string> &acc);

void used_expr(const Expr *e, std::vector<std::string> &acc) {
    if (e == nullptr) return;
    if (e->k == EK::Var) {
        acc.push_back(e->a);
        return;
    }
    for (const auto &k : e->kids) used_expr(k.get(), acc);
    if (e->fn) {
        for (const auto &sp : e->fn->body)
            if (sp) used_stmt(*sp, acc);
    }
}

void used_stmt(const Stmt &s, std::vector<std::string> &acc) {
    used_expr(s.e1.get(), acc);
    used_expr(s.e2.get(), acc);
    used_expr(s.e3.get(), acc);
    for (const auto &t : s.body) used_stmt(*t, acc);
    for (const auto &t : s.body2) used_stmt(*t, acc);
    if (s.els) used_stmt(*s.els, acc);
    if (s.k == SK::ExportFn || s.k == SK::Fn) {
        for (const FnDecl &f : s.fns)
            for (const auto &t : f.body) used_stmt(*t, acc);
    }
}

// Statement order, nested bodies included: the same order the walk counts in.
void flatten(const View &stmts, std::vector<const Stmt *> &order) {
    for (const Stmt *s : stmts) {
        if (s == nullptr) continue;
        order.push_back(s);
        if (is_decl_stmt(s->k)) continue;
        flatten(view(s->body), order);
        flatten(view(s->body2), order);
        if (s->els) flatten(else_of(*s), order);
    }
}

std::map<std::string, int> index_uses(const View &body) {
    std::vector<const Stmt *> order;
    flatten(body, order);
    std::map<std::string, int> last;
    int index = 0;
    for (const Stmt *s : order) {
        index += 1;
        std::vector<std::string> names;
        used_stmt(*s, names);
        for (const std::string &n : names) last[n] = index;
    }
    return last;
}


struct Walk {
    std::map<std::string, std::vector<Mode>> modes;   // fn -> parameter modes
    std::map<std::string, std::map<std::string, std::string>> struct_fields;
    std::map<std::string, std::string> fn_rets;
    std::map<std::string, int> last;                  // name -> last use index
    std::vector<BorrowDiag> out;
    int index = 0;
    int cur = 0;                                      // line of the current statement

    // A number by construction. `+` and `*` also build strings and vectors, so they
    // count only over operands that are numbers themselves.
    bool numeric(const State &st, const Expr *e) const {
        if (e == nullptr) return false;
        switch (e->k) {
        case EK::Num: return true;
        case EK::Var: return st.nums.count(e->a) > 0;
        case EK::Un: return e->a == "-" || e->a == "~";
        case EK::Bin: {
            const std::string &op = e->a;
            if (op == "+" || op == "*")
                return numeric(st, e->kids[0].get()) && numeric(st, e->kids[1].get());
            return op == "-" || op == "/" || op == "%" || op == "&" || op == "|" || op == "^" ||
                   op == "<<" || op == ">>";
        }
        case EK::Call:
            return (e->a == "len" || e->a == "slice_len" || e->a == "sbuf_len" ||
                    e->a == "time_ms" || e->a == "mono_ns" || e->a == "errno") &&
                   modes.count(e->a) == 0;
        default: return false;
        }
    }

    // A number or a bool whatever the operands hold, so `let y = x` of it is a copy.
    bool scalar_result(const State &st, const Expr *e) const {
        if (e == nullptr) return false;
        if (e->k == EK::Un) return e->a != "&";
        if (e->k == EK::Bin) return is_scalar_result_op(e->a) || numeric(st, e);
        if (e->k == EK::Call)
            return e->a != "own" && is_scalar_only_builtin(e->a) && modes.count(e->a) == 0;
        return false;
    }

    std::string infer_ann(const State &st, const Expr *e) const {
        while (e && e->k == EK::Move && !e->kids.empty())
            e = e->kids[0].get();
        if (e == nullptr) return "";
        if (e->k == EK::Quest) {
            if (e->kids.empty()) return "";
            return ::unwrap_adt_ann(infer_ann(st, e->kids[0].get()));
        }
        if (e->k == EK::Var) {
            auto it = st.anns.find(e->a);
            return it == st.anns.end() ? "" : it->second;
        }
        if (e->k == EK::StructLit && !e->a.empty()) return e->a;
        if (e->k == EK::Call) {
            if (e->a == "own" && e->kids.size() == 1)
                return infer_ann(st, e->kids[0].get());
            if (e->a == "is_ok" || e->a == "is_err" || e->a == "is_some" || e->a == "is_none")
                return "bool";
            if (e->a == "Some" && e->kids.size() == 1) {
                std::string in = infer_ann(st, e->kids[0].get());
                return in.empty() ? "" : "Option[" + in + "]";
            }
            if (e->a == "Ok" && e->kids.size() == 1) {
                std::string in = infer_ann(st, e->kids[0].get());
                return in.empty() ? "" : "Result[" + in + ", dyn]";
            }
            if (e->a == "unwrap" && e->kids.size() == 1) {
                std::string in = infer_ann(st, e->kids[0].get());
                std::string u = unwrap_adt_ann(in);
                return u == in ? "" : u;
            }
            if (e->a == "unwrap_or" && e->kids.size() == 2) {
                std::string in = infer_ann(st, e->kids[0].get());
                std::string u = unwrap_adt_ann(in);
                if (!u.empty() && u != in && u != "dyn") return u;
                return infer_ann(st, e->kids[1].get());
            }
            if (e->a == "fs_exists" || e->a == "fs_is_dir") return "bool";
            if (e->a == "read_file") return "Result[str, str]";
            if (e->a == "fs_list_dir") return "Result[vec[str], str]";
            if (e->a == "fs_read_line" || e->a == "stdin_read_line")
                return "Result[Option[str], str]";
            if (e->a == "append_file" || e->a == "write_file" ||
                e->a == "fs_open" || e->a == "fs_close" ||
                e->a == "fs_file_size" || e->a == "fs_mkdir_all" || e->a == "fs_remove_dir")
                return "Result[i64, str]";
            if (e->a == "re_find") return "Option[str]";
            auto it = fn_rets.find(e->a);
            return it == fn_rets.end() ? "" : it->second;
        }
        if (e->k == EK::MCall && !e->kids.empty()) {
            if (e->b == "is_ok" || e->b == "is_err" || e->b == "is_some" || e->b == "is_none")
                return "bool";
            if (e->b == "parse_int") return "Result[i64, str]";
            if (e->b == "parse_float") return "Result[f64, str]";
            if (e->b == "copy" && e->kids.size() == 1)
                return infer_ann(st, e->kids[0].get());
            if ((e->kids[0]->k == EK::Call && e->kids[0]->kids.empty()) ||
                (e->kids[0]->k == EK::Var && !st.anns.count(e->kids[0]->a))) {
                auto sit = fn_rets.find(e->kids[0]->a + "." + e->b);
                if (sit != fn_rets.end()) return sit->second;
            }
            std::string rty = infer_ann(st, e->kids[0].get());
            if (!rty.empty()) {
                if (e->b == "unwrap") {
                    std::string u = unwrap_adt_ann(rty);
                    if (u != rty) return u;
                }
                if (e->b == "unwrap_or") {
                    std::string u = unwrap_adt_ann(rty);
                    if (!u.empty() && u != rty && u != "dyn") return u;
                    if (e->kids.size() >= 2) return infer_ann(st, e->kids[1].get());
                }
                auto mit = fn_rets.find(rty + "." + e->b);
                if (mit != fn_rets.end()) return mit->second;
            }
            return "";
        }
        if (e->k == EK::Idx && !e->kids.empty()) {
            std::string a = infer_ann(st, e->kids[0].get());
            if (a.size() > 5 && a.rfind("vec[", 0) == 0 && a.back() == ']') {
                a = a.substr(4, a.size() - 5);
            } else if (a.rfind("map[", 0) == 0 && a.back() == ']') {
                std::string inner = a.substr(4, a.size() - 5);
                size_t comma = std::string::npos;
                int depth = 0;
                for (size_t i = 0; i < inner.size(); i++) {
                    char c = inner[i];
                    if (c == '[' || c == '(') depth++;
                    else if ((c == ']' || c == ')') && depth > 0) depth--;
                    else if (c == ',' && depth == 0) { comma = i; break; }
                }
                a = comma == std::string::npos ? inner : inner.substr(comma + 1);
            } else {
                return "";
            }
            auto first = a.find_first_not_of(" \t");
            if (first == std::string::npos) return "";
            a.erase(0, first);
            while (!a.empty() && (a.back() == ' ' || a.back() == '\t')) a.pop_back();
            return a;
        }
        if (e->k == EK::Field && !e->kids.empty()) {
            std::string a = infer_ann(st, e->kids[0].get());
            auto sit = struct_fields.find(a);
            if (sit != struct_fields.end()) {
                auto fit = sit->second.find(e->b);
                if (fit != sit->second.end()) return fit->second;
            }
            return "";
        }
        return "";
    }

    void note(int line, const std::string &msg,
              std::optional<std::pair<int, std::string>> rel = std::nullopt) {
        BorrowDiag d;
        d.line = line;
        d.msg = msg;
        if (rel) d.note = BorrowNote{rel->first, rel->second};
        out.push_back(std::move(d));
    }

    void block(const View &body, State &st) {
        for (const Stmt *s : body) {
            stmt(*s, st);
            expire(st);
        }
    }

    // A view's last use has passed: the place goes back to its owner.
    void expire(State &st) {
        for (auto it = st.who.begin(); it != st.who.end();) {
            std::vector<Holder> kept;
            for (const Holder &h : it->second) {
                auto last_it = last.find(h.name);
                int l = last_it == last.end() ? 0 : last_it->second;
                if (h.cap != kRead || l > index) kept.push_back(h);
            }
            if (!kept.empty()) {
                bool any_read = false, any_moved = false;
                int top = kNone;
                for (const Holder &h : kept) {
                    if (h.cap == kRead) any_read = true;
                    if (h.cap == kMoved) any_moved = true;
                    top = std::max(top, h.cap);
                }
                it->second = std::move(kept);
                if (any_moved) st.caps[it->first] = kMoved;
                else if (!any_read) st.caps[it->first] = top;
                ++it;
                continue;
            }
            st.caps.erase(it->first);
            it = st.who.erase(it);
        }
    }

    void stmt(const Stmt &s, State &st) {
        index += 1;
        if (s.line) cur = s.line;
        if (is_decl_stmt(s.k)) return;          // every body is walked on its own
        switch (s.k) {
        case SK::Let: return let_stmt(s, st);
        case SK::Set: return assign(std::optional<std::string>(s.a), s.e1.get(), st, cur);
        case SK::SetIdx: return assign(place_of(s.e1.get()), s.e3.get(), st, cur);
        case SK::SetField: {
            auto base = place_of(s.e1.get());
            assign(base ? std::optional<std::string>(*base + "." + s.a) : std::nullopt,
                   s.e2.get(), st, cur);
            return;
        }
        case SK::Expr: return expr(s.e1.get(), st, true, cur);
        case SK::Return: return ret(s, st);
        case SK::If: {
            expr(s.e1.get(), st, false, cur);
            View then_body = view(s.body);
            View else_body = else_of(s);
            State a = st;
            block(then_body, a);
            for (const std::string &name : bound_in(then_body)) a.forget(name);
            State b = st;
            block(else_body, b);
            for (const std::string &name : bound_in(else_body)) b.forget(name);
            if (!exits(then_body)) st.join_from(a);
            if (!exits(else_body)) st.join_from(b);
            return;
        }
        case SK::While:
        case SK::ForRange:
        case SK::ForIter: return loop(s, st);
        case SK::Try: {
            View try_body = view(s.body);
            View catch_body = view(s.body2);
            View else_body = else_of(s);
            State a = st;
            block(try_body, a);
            for (const std::string &name : bound_in(try_body)) a.forget(name);
            State b = st;
            if (s.opt1 && !s.opt1->empty()) b.forget(*s.opt1);
            block(catch_body, b);
            for (const std::string &name : bound_in(catch_body)) b.forget(name);
            if (s.opt1 && !s.opt1->empty()) b.forget(*s.opt1);
            State c = st;
            block(else_body, c);
            for (const std::string &name : bound_in(else_body)) c.forget(name);
            if (!exits(try_body)) st.join_from(a);
            if (!exits(catch_body)) st.join_from(b);
            if (!exits(else_body)) st.join_from(c);
            return;
        }
        case SK::Spawn:
        case SK::Scope: return spawn(s, st);
        case SK::Unsafe: {
            // `unsafe` is the program saying it knows better than the checker,
            // so nothing inside it decides a verdict outside it.
            State shadow = st;
            block(view(s.body), shadow);
            return;
        }
        case SK::Defer:
            // Deferred code runs when the scope ends, after everything that
            // follows it: claiming a move here would refuse the ordinary uses
            // in between.
            return;
        default:
            expr(s.e1.get(), st, false, cur);
            expr(s.e2.get(), st, false, cur);
            expr(s.e3.get(), st, false, cur);
            block(view(s.body), st);
            block(view(s.body2), st);
            return;
        }
    }

    void loop(const Stmt &s, State &st) {
        expr(s.e1.get(), st, false, cur);
        if (s.k == SK::ForRange) expr(s.e2.get(), st, false, cur);
        // A body runs zero or more times, so its effect joins with "not taken";
        // the lattice has height five, which two passes reach. The body's own
        // bindings are dropped from the seed: they are fresh every iteration.
        std::set<std::string> declared = bound_in(view(s.body));
        if ((s.k == SK::ForRange || s.k == SK::ForIter) && !s.a.empty())
            declared.insert(s.a);
        const int loop_base = index;
        for (int i = 0; i < 2; i++) {
            index = loop_base;
            State once = st;
            for (const std::string &name : declared) once.forget(name);
            if (s.k == SK::ForRange && !s.a.empty()) once.copies.insert(s.a);
            block(view(s.body), once);
            for (const std::string &name : declared) once.forget(name);
            st.join_from(once);
        }
    }

    void ret(const Stmt &s, State &st) {
        auto place = place_of(s.e1.get());
        if (place && st.params.count(root_name(st.place(*place)))) {
            expr(s.e1.get(), st, false, cur);   // a parameter borrow is not an escape
            return;
        }
        if (place) {
            auto holder = live_reader(st, st.place(*place), std::nullopt);
            if (holder && !holder->name.empty() && holder->name != "?")
                note(cur, escapes_return(holder->name));
        }
        expr(s.e1.get(), st, false, cur);
    }

    void spawn(const Stmt &s, State &st) {
        // A child copies the frame: its writes are the sharing pass's business,
        // but a view read inside it would outlive the parent frame.
        std::vector<std::string> names;
        for (const auto &t : s.body) used_stmt(*t, names);
        for (const std::string &n : names) {
            auto holder = live_reader(st, st.place(n), std::nullopt);
            if (holder && holder->name == n) note(cur, escapes_spawn(n));
        }
        State child = st;
        block(view(s.body), child);
    }

    void let_stmt(const Stmt &s, State &st) {
        const std::string &name = s.a;
        const Expr *e = s.e1.get();
        const bool mut = s.mut;
        std::string ann = s.ann.value_or("");
        if (ann.empty()) ann = infer_ann(st, e);
        expr(e, st, false, cur);
        bool copylike = is_copy_scalar_ann(ann) || copy_expr(e) || scalar_result(st, e) ||
                        (e && e->k == EK::Var && st.copies.count(e->a)) ||
                        (e && e->k == EK::Idx && elem_copy(st, e->kids[0].get()));
        const bool number = is_numeric_ann(ann) || numeric(st, e);
        auto place = place_of(e);
        if (!place || !is_alias(e) || root_name(st.place(*place)) != name) {
            st.forget(name);
            st.anns.erase(name);
            st.muts.erase(name);
            st.copies.erase(name);
            st.nums.erase(name);
            st.consts.erase(name);
        }
        if (!ann.empty()) st.anns[name] = ann;
        if (mut) st.muts.insert(name);   // a mutable binding is a writer of its place
        // `i64`, `f64`, `bool` and a binding that holds one are copies: no view,
        // no borrow, no second place.
        if (copylike) {
            st.copies.insert(name);
            if (number) st.nums.insert(name);
            return;
        }
        if (!place || !is_alias(e)) {
            if (s.konst) st.consts.insert(name);
            return;                            // a fresh value: `name` owns it
        }
        if (e->k == EK::Move) {
            auto inside = moved_below(st, st.place(*place));
            if (inside) note(cur, moved_use(st.place(*place)));
            return;                           // the move claimed the place
        }
        const std::string owner = st.place(*place);
        if (st.consts.count(owner)) return;    // a const alias: the walk explains it
        if (st.cap(owner) == kMoved || st.cap(*place) == kMoved) {
            note(cur, moved_use(name));
            return;
        }
        if (mut) {
            auto holder = live_reader(st, *place, name);
            if (holder) {
                // Two names over one buffer, one of them exclusive: a mutable
                // target is a second writer, an immutable one is a refused write.
                if (st.muts.count(owner))
                    note(cur, two_writers(owner, name, holder->name),
                         std::make_pair(holder->line,
                                        "first borrow `" + holder->name + "` here"));
                else
                    note(cur, write_while_read(owner, holder->name),
                         std::make_pair(holder->line,
                                        "borrowed by `" + holder->name + "` here"));
            }
            auto other = live_writer(st, *place, name);
            if (other && other->name != owner && other->name != name)
                note(cur, two_writers(owner, name, other->name),
                     std::make_pair(other->line, "first borrow `" + other->name + "` here"));
            st.alias[name] = owner;
            st.claim(owner, kWrite, name, cur);
            // The owner is still a mutable place of its own: two names, one buffer.
            if (!st.second.count(owner)) st.second[owner] = {name, cur};
        } else {
            st.alias[name] = owner;
            st.claim(owner, kRead, name, cur);
        }
    }

    void assign(std::optional<std::string> place, const Expr *value, State &st, int line) {
        expr(value, st, false, line);
        if (!place) return;
        const std::string p = st.place(*place);
        auto holder = live_reader(st, p, root_name(p));
        if (holder)
            note(line, assign_while_read(p, holder->name),
                 std::make_pair(holder->line, "borrowed by `" + holder->name + "` here"));
        if (p.find('.') != std::string::npos) {
            // Writing *into* a value is a use of it, unless the write lands on
            // exactly what was moved out: that is how a moved field comes back
            // (`let a = move p.x` then `p.x = 9`).
            auto gone = moved_under(st, p);
            if (gone && *gone != p) {
                auto [msg, rel] = gone_message(st, root_name(p), *gone);
                note(line, msg, rel);
                return;
            }
        }
        // Replacing `p` restores `p` and any moved sub-places under it.
        const std::string sub = p + ".";
        for (auto it = st.caps.begin(); it != st.caps.end();) {
            if ((it->first == p || it->first.compare(0, sub.size(), sub) == 0) &&
                it->second == kMoved) {
                st.who.erase(it->first);
                st.verb.erase(it->first);
                it = st.caps.erase(it);
            } else {
                ++it;
            }
        }
        st.claim(p, kWrite, root_name(p), line);
    }

    void expr(const Expr *e, State &st, bool mutating, int line) {
        if (e == nullptr) return;
        switch (e->k) {
        case EK::Var: {
            const std::string place = st.place(e->a);
            auto gone = moved_under(st, place);
            if (gone) {
                auto [msg, rel] = gone_message(st, place, *gone);
                note(line, msg, rel);
            }
            return;
        }
        case EK::Field:
        case EK::Idx:
        case EK::Quest: {
            auto place = place_of(e);
            if (place) {
                auto gone = moved_under(st, *place);
                if (gone) {
                    auto [msg, rel] = gone_message(st, *place, *gone);
                    note(line, msg, rel);
                }
                if (e->k == EK::Idx && e->kids.size() > 1) expr(e->kids[1].get(), st, mutating, line);
                return;
            }
            for (const auto &k : e->kids) expr(k.get(), st, mutating, line);
            return;
        }
        case EK::Move: {
            auto place = place_of(e->kids[0].get());
            if (place && !place->empty()) {
                const std::string p = st.place(*place);
                if (st.cap(p) == kMoved) note(line, gone_message(st, p, p).first);
                auto holder = live_reader(st, p, std::nullopt);
                if (holder) {
                    // A move out of a borrowed place does not happen: the walk
                    // refuses it and the alias stays usable.
                    note(line, move_while_read(p, holder->name),
                         std::make_pair(holder->line,
                                        "borrowed by `" + holder->name + "` here"));
                    return;
                }
                st.claim(p, kMoved, p, line);
                st.verb[p] = State::Verb{"moved", p.find('.') == std::string::npos, line};
            }
            return;
        }
        case EK::Closure: {
            if (e->fn) {
                State inner = st;
                for (const Param &p : e->fn->params) inner.forget(p.name);
                block(view(e->fn->body), inner);
            }
            return;
        }
        case EK::ParFor: {
            for (const auto &k : e->kids) expr(k.get(), st, mutating, line);
            if (e->fn) {
                State inner = st;
                if (!e->a.empty()) inner.forget(e->a);
                block(view(e->fn->body), inner);
            }
            return;
        }
        case EK::MCall: {
            if (e->b == "send") {
                for (size_t i = 1; i < e->kids.size(); i++) {
                    auto place = place_of(e->kids[i].get());
                    const Expr *arg = e->kids[i].get();
                    bool copied = copy_expr(arg) ||
                                  (arg->k == EK::Var && st.copies.count(arg->a)) ||
                                  is_copy_scalar_ann(infer_ann(st, arg));
                    if (i == 1 && place && !copied) {
                        const std::string p = st.place(*place);
                        if (moved_under(st, p)) note(line, gone_message(st, p, p).first);
                        st.claim(p, kMoved, p, line);
                        st.verb[p] = State::Verb{"sent", p.find('.') == std::string::npos, line};
                        continue;
                    }
                    expr(e->kids[i].get(), st, mutating, line);
                }
                return;
            }
            if (is_cap_mutator(e->b)) {
                auto place = place_of(e->kids[0].get());
                if (place && !place->empty()) {
                    const std::string p = st.place(*place);
                    if (st.cap(p) == kMoved) note(line, moved_use(p));
                    auto sec = st.second.find(p);
                    std::string named = (e->kids[0]->k == EK::Var) ? e->kids[0]->a : p;
                    if (sec != st.second.end() && sec->second.first != named)
                        note(line, two_writers(p, named, sec->second.first),
                             std::make_pair(sec->second.second,
                                            "first borrow `" + sec->second.first + "` here"));
                    auto holder = live_reader(st, p, root_name(p));
                    if (holder)
                        note(line, write_while_read(p, holder->name),
                             std::make_pair(holder->line,
                                            "borrowed by `" + holder->name + "` here"));
                    st.claim(p, kWrite, root_name(p), line);
                }
                for (size_t i = 1; i < e->kids.size(); i++) expr(e->kids[i].get(), st, mutating, line);
                return;
            }
            for (const auto &k : e->kids) expr(k.get(), st, mutating, line);
            return;
        }
        case EK::Call: {
            auto it = modes.find(e->a);
            for (size_t i = 0; i < e->kids.size(); i++) {
                auto place = place_of(e->kids[i].get());
                if (it != modes.end() && i < it->second.size() &&
                    it->second[i] == Mode::Move && place) {
                    const std::string p = st.place(*place);
                    if (moved_below(st, p)) {
                        note(line, moved_use(p));
                        continue;
                    }
                    if (moved_under(st, p)) note(line, gone_message(st, p, p).first);
                    st.claim(p, kMoved, p, line);
                    st.verb[p] = State::Verb{"moved", p.find('.') == std::string::npos, line};
                    continue;
                }
                expr(e->kids[i].get(), st, mutating, line);
            }
            return;
        }
        default:
            for (const auto &k : e->kids) expr(k.get(), st, mutating, line);
            return;
        }
    }
};

void one_body(const View &stmts, const std::vector<Param> &params,
              const std::map<std::string, std::vector<Mode>> &modes,
              const std::map<std::string, std::map<std::string, std::string>> &struct_fields,
              const std::map<std::string, std::string> &fn_rets,
              std::vector<BorrowDiag> &out,
              const std::string &impl_type = "") {
    Walk walk;
    walk.modes = modes;
    walk.struct_fields = struct_fields;
    walk.fn_rets = fn_rets;
    walk.last = index_uses(stmts);
    State st;
    for (const Param &p : params) {
        st.params.insert(p.name);
        std::string ann = p.ann.value_or("");
        if (ann.empty() && p.name == "self" && !impl_type.empty() &&
            (struct_fields.count(impl_type) || struct_fields.count("__enum__:" + impl_type))) {
            ann = impl_type;
        }
        if (!ann.empty()) {
            st.anns[p.name] = ann;
            if (is_copy_scalar_ann(ann)) st.copies.insert(p.name);
            if (is_numeric_ann(ann)) st.nums.insert(p.name);
        }
    }
    walk.block(stmts, st);
    for (BorrowDiag &d : walk.out) out.push_back(std::move(d));
}

// The modes a call resolves to inside `f`: a fn declared there hides a
// top-level fn of the same name.
std::map<std::string, std::vector<Mode>> modes_inside(
    const FnDecl &f, std::map<std::string, std::vector<Mode>> modes) {
    std::vector<const Stmt *> nested;
    nested_fns(f.body, nested);
    for (const Stmt *n : nested)
        if (n->fns[0].name) modes[*n->fns[0].name] = modes_of(n->fns[0]);
    return modes;
}

}  // namespace

std::vector<BorrowDiag> cap_verdicts(
    const std::vector<StmtP> &body,
    const std::map<std::string, std::vector<ParamMode>> &sigs,
    const std::map<std::string, std::map<std::string, std::string>> &struct_fields,
    const std::map<std::string, std::string> &fn_rets) {
    std::map<std::string, std::vector<Mode>> modes;
    for (const auto &kv : sigs) {
        std::vector<Mode> m;
        for (const ParamMode &p : kv.second) m.push_back(p.mode);
        modes[kv.first] = std::move(m);
    }
    for (const auto &sp : body) {
        const Stmt *node = sp.get();
        while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
        if ((node->k == SK::Fn || node->k == SK::ExportFn) && !node->fns.empty()) {
            const FnDecl &f = node->fns[0];
            if (f.name) modes[*f.name] = modes_of(f);
        } else if (node->k == SK::Extern && !node->ext.name.empty()) {
            std::vector<Mode> m;
            for (const Param &p : node->ext.params) m.push_back(p.mode);
            modes[node->ext.name] = std::move(m);
        } else if (node->k == SK::Impl) {
            for (const FnDecl &f : node->fns)
                if (f.name) modes.insert({*f.name, modes_of(f)});
        }
    }
    std::vector<BorrowDiag> out;
    View top;
    for (const auto &sp : body) {
        const Stmt *node = sp.get();
        while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
        if ((node->k == SK::Fn || node->k == SK::ExportFn) && !node->fns.empty()) {
            const FnDecl &f = node->fns[0];
            one_body(view(f.body), f.params, modes_inside(f, modes), struct_fields, fn_rets, out);
            continue;
        }
        if (is_decl_stmt(node->k)) {
            if (node->k == SK::Impl) {
                for (const FnDecl &f : node->fns) {
                    one_body(view(f.body), f.params, modes_inside(f, modes), struct_fields,
                             fn_rets, out, node->a);
                }
            }
            continue;
        }
        top.push_back(sp.get());
    }
    one_body(top, {}, modes, struct_fields, fn_rets, out);
    // The same finding twice is one finding.
    std::vector<BorrowDiag> unique;
    std::set<std::pair<int, std::string>> seen;
    for (const BorrowDiag &d : out)
        if (seen.insert({d.line, d.msg}).second) unique.push_back(d);
    return unique;
}
