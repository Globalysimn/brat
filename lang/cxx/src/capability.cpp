#include "capability.h"

#include <algorithm>
#include <map>
#include <set>

namespace {

using View = std::vector<const Stmt *>;

const std::set<std::string> kInsert{"push", "ins", "set"};
// Builtins that write through the pointer or slice they are given: the place the
// argument names is written, even though no `let` in the program says so.
const std::set<std::string> kPtrWrite{"ptr_write_i64", "ptr_write_f64", "ptr_write_f32",
                                      "ptr_write_u8", "ptr_write_str", "ptr_write_ptr",
                                      "ptr_write_struct", "slice_set", "fill"};
const std::set<std::string> kMutate{"pop", "sort", "sort_by", "del", "clear"};
const std::set<SK> kDecls{SK::Fn, SK::Impl, SK::Trait, SK::Enum, SK::Extern,
                          SK::ExternStruct, SK::ExportFn};

std::string cycle_msg(const std::string &value, const std::string &container) {
    return "reference cycle: `" + value + "` goes into `" + container +
           "`, which already owns it (refcounting cannot reclaim a cycle)";
}

std::string race_msg(const std::string &name) {
    return "`" + name +
           "` is written inside `spawn` and touched again with no `join_all()` in "
           "between (hint: send the value through a channel)";
}

std::string race_read_msg(const std::string &name) {
    return "`" + name +
           "` is read inside `spawn` and written here with no `join_all()` in "
           "between (hint: `join_all()` first, or send the value through a "
           "channel)";
}

std::string loop_msg(const std::string &name) {
    return "`" + name +
           "` is written inside `spawn` launched in a loop that never joins "
           "(hint: join inside the loop, or send through a channel)";
}

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

// The variable a place starts at; empty when the expression computes a value.
std::string root(const Expr *e) {
    while (e) {
        switch (e->k) {
        case EK::Var: return e->a;
        case EK::Un:
            // `&x` is the place of x, not a fresh value.
            if (e->a != "&" || e->kids.empty()) return {};
            e = e->kids[0].get();
            continue;
        case EK::Move:
        case EK::Quest:
        case EK::Field:
        case EK::Idx:
            if (e->kids.empty()) return {};
            e = e->kids[0].get();
            continue;
        default: return {};
        }
    }
    return {};
}

void each_node(const Expr *e, std::vector<const Expr *> &out) {
    if (!e) return;
    out.push_back(e);
    for (const auto &k : e->kids) each_node(k.get(), out);
}

struct FnSummary {
    std::vector<std::string> params;
    std::set<size_t> mut_idx;
    std::set<std::pair<size_t, size_t>> ins_pairs;
    View body;
    std::map<std::string, std::string> alias;
};

using FnSummaries = std::map<std::string, FnSummary>;

std::vector<const Expr *> stmt_exprs(const Stmt &s) {
    switch (s.k) {
    case SK::SetIdx: return {s.e1.get(), s.e2.get(), s.e3.get()};
    case SK::Set:
    case SK::Let:
    case SK::LetTmp: return {s.e1.get()};
    case SK::If:
    case SK::While: return {s.e1.get()};
    case SK::ForRange: return {s.e1.get(), s.e2.get()};
    case SK::ForIter: return {s.e1.get()};
    default: return {s.e1.get()};
    }
}

struct Access {
    std::set<std::string> read, write;
};

Access touched(const Stmt &s, const FnSummaries *fns = nullptr) {
    Access out;
    std::vector<const Expr *> exprs = stmt_exprs(s);
    std::vector<const Expr *> nodes;
    for (const Expr *e : exprs) each_node(e, nodes);
    for (const Expr *n : nodes) {
        if (n->k == EK::MCall) {
            std::string r = n->kids.empty() ? std::string() : root(n->kids[0].get());
            const FnSummary *fs = nullptr;
            if (fns) {
                auto it = fns->find(n->b);
                if (it != fns->end()) fs = &it->second;
            }
            if (!r.empty()) {
                if (kInsert.count(n->b) || kMutate.count(n->b) || (fs && fs->mut_idx.count(0)))
                    out.write.insert(r);
                else
                    out.read.insert(r);
            }
            if (fs) {
                for (size_t idx : fs->mut_idx) {
                    if (idx < n->kids.size()) {
                        std::string ar = root(n->kids[idx].get());
                        if (!ar.empty()) out.write.insert(ar);
                    }
                }
            }
        } else if (n->k == EK::Call && kPtrWrite.count(n->a)) {
            if (!n->kids.empty()) {
                std::string ar = root(n->kids[0].get());
                if (!ar.empty()) out.write.insert(ar);   // C writes through it
            }
        } else if (n->k == EK::Move) {
            std::string r = root(n);
            if (!r.empty()) out.write.insert(r);         // the storage is handed away
        } else if (n->k == EK::Call && fns) {
            auto it = fns->find(n->a);
            if (it != fns->end()) {
                for (size_t idx : it->second.mut_idx) {
                    if (idx < n->kids.size()) {
                        std::string ar = root(n->kids[idx].get());
                        if (!ar.empty()) out.write.insert(ar);
                    }
                }
            }
        } else if (n->k == EK::Var || n->k == EK::Field || n->k == EK::Idx) {
            std::string r = root(n);
            if (!r.empty()) out.read.insert(r);
        }
    }
    if (s.k == SK::SetIdx) {
        std::string r = root(s.e1.get());
        if (!r.empty()) out.write.insert(r);
    }
    // A plain `x = 2` rebinds the binding and does not touch the object, so it
    // is not a write this pass reports. Sound only because `spawn` copies the
    // frame: the child holds a snapshot of every name its body mentions, so a
    // parent that rebinds a name is not touching what the child reads. A
    // mutation through the object (`push`, `v[i] = x`, `ptr_write_*`, `move`)
    // still is a write.
    if (s.k == SK::SetField) {
        std::string r = root(s.e1.get());
        if (!r.empty()) out.write.insert(r);
    }
    for (const auto &w : out.write) out.read.erase(w);
    return out;
}

void declared(const View &body, std::set<std::string> &out) {
    for (const Stmt *s : body) {
        if (!s) continue;
        if (s->k == SK::Let || s->k == SK::LetTmp || s->k == SK::ForRange || s->k == SK::ForIter)
            out.insert(s->a);
        if (s->k == SK::Try && s->opt1 && !s->opt1->empty()) out.insert(*s->opt1);
        declared(view(s->body), out);
        declared(view(s->body2), out);
        for (const auto &f : s->fns) declared(view(f.body), out);
    }
}

void alias_map(const View &body, std::map<std::string, std::string> &out) {
    for (const Stmt *s : body) {
        if (!s) continue;
        if (s->k == SK::Let && s->e1) {
            std::string r = root(s->e1.get());
            if (!r.empty() && r != s->a) out[s->a] = r;
        }
        if (s->k == SK::Try && s->opt1) out.erase(*s->opt1);
        alias_map(view(s->body), out);
        alias_map(view(s->body2), out);
    }
}

std::string resolve(std::string name, const std::map<std::string, std::string> &alias) {
    for (size_t guard = 0; guard < 64; guard++) {
        auto it = alias.find(name);
        if (it == alias.end()) return name;
        name = it->second;
    }
    return name;
}

std::vector<std::pair<const Expr *, const Expr *>> inserts(const Stmt &s,
                                                           const FnSummaries *fns = nullptr) {
    std::vector<std::pair<const Expr *, const Expr *>> out;
    if (s.k == SK::SetIdx && s.e1 && s.e3) out.push_back({s.e1.get(), s.e3.get()});
    std::vector<const Expr *> nodes;
    for (const Expr *e : stmt_exprs(s)) each_node(e, nodes);
    for (const Expr *n : nodes) {
        if (n->k == EK::MCall) {
            if (kInsert.count(n->b) && n->kids.size() >= 2) {
                const Expr *value =
                    n->b == "set" ? n->kids.back().get() : n->kids[1].get();
                out.push_back({n->kids[0].get(), value});
            } else if (fns) {
                auto it = fns->find(n->b);
                if (it != fns->end()) {
                    for (const auto &[ci, vi] : it->second.ins_pairs) {
                        if (ci < n->kids.size() && vi < n->kids.size())
                            out.push_back({n->kids[ci].get(), n->kids[vi].get()});
                    }
                }
            }
        } else if (n->k == EK::Call && fns) {
            auto it = fns->find(n->a);
            if (it != fns->end()) {
                for (const auto &[ci, vi] : it->second.ins_pairs) {
                    if (ci < n->kids.size() && vi < n->kids.size())
                        out.push_back({n->kids[ci].get(), n->kids[vi].get()});
                }
            }
        }
    }
    return out;
}

struct Owner {
    std::map<std::string, std::string> parent;

    // True when `c` already reaches `v`: inserting v into c would close a cycle.
    bool owns(const std::string &c, const std::string &v) const {
        std::string cur = c;
        for (size_t guard = 0; guard < 4096 && !cur.empty(); guard++) {
            if (cur == v) return true;
            auto it = parent.find(cur);
            if (it == parent.end()) return false;
            cur = it->second;
        }
        return false;
    }
};

void cycles(const View &body, std::vector<BorrowDiag> &out, Owner &own,
            const std::map<std::string, std::string> &alias,
            const FnSummaries *fns = nullptr) {
    for (const Stmt *s : body) {
        if (!s) continue;
        for (const auto &[container, value] : inserts(*s, fns)) {
            std::string rc = root(container), rv = root(value);
            if (rc.empty() || rv.empty()) continue;
            rc = resolve(rc, alias);
            rv = resolve(rv, alias);
            if (own.owns(rc, rv))
                out.push_back(BorrowDiag{s->line, cycle_msg(rv, rc), std::nullopt});
            else if (rc != rv && !own.parent.count(rv))
                own.parent[rv] = rc;
        }
        switch (s->k) {
        case SK::If:
        case SK::While:
        case SK::ForRange:
        case SK::ForIter:
        case SK::Spawn:
        case SK::Scope:
        case SK::Try:
            cycles(view(s->body), out, own, alias, fns);
            cycles(view(s->body2), out, own, alias, fns);
            cycles(else_of(*s), out, own, alias, fns);
            break;
        case SK::Fn:
        case SK::ExportFn:
            for (const auto &f : s->fns) {
                Owner fresh;
                std::map<std::string, std::string> inner;
                alias_map(view(f.body), inner);
                cycles(view(f.body), out, fresh, inner, fns);
            }
            break;
        default:
            break;
        }
    }
}

struct Event {
    bool spawn = false;
    bool join = false;
    std::set<std::string> writes, reads;
    int line = 0;
    const void *loop = nullptr;
    size_t member_begin = 0, member_end = 0;
};

// One of the two threads writes a place the other touches: write/read and
// write/write both need a barrier, two reads do not.
bool touches(const Event &a, const Event &b) {
    for (const auto &w : a.writes)
        if (b.writes.count(w) || b.reads.count(w)) return true;
    for (const auto &r : a.reads)
        if (b.writes.count(r)) return true;
    return false;
}

bool wrote_side(const Event &a, const Event &b) {
    for (const auto &w : a.writes)
        if (b.writes.count(w) || b.reads.count(w)) return true;
    return false;
}

std::string first_touched(const Event &a, const Event &b) {
    for (const auto &w : a.writes)
        if (b.writes.count(w) || b.reads.count(w)) return w;
    for (const auto &r : a.reads)
        if (b.writes.count(r)) return r;
    return {};
}

void collect(const View &body, std::vector<Event> &events,
             std::vector<std::pair<size_t, size_t>> &loops, const void *loop,
             const FnSummaries *fns = nullptr) {
    for (const Stmt *s : body) {
        if (!s) continue;
        if (s->k == SK::Spawn) {
            size_t before = events.size();
            collect(view(s->body), events, loops, loop, fns);
            std::set<std::string> locals;
            declared(view(s->body), locals);
            Event ev;
            ev.spawn = true;
            ev.line = s->line;
            ev.loop = loop;
            for (size_t i = before; i < events.size(); i++)
                for (const auto &w : events[i].writes)
                    if (!locals.count(w)) ev.writes.insert(w);
            for (size_t i = before; i < events.size(); i++)
                for (const auto &r : events[i].reads)
                    if (!locals.count(r)) ev.reads.insert(r);
            ev.member_begin = before + 1;
            ev.member_end = events.size() + 1;
            events.insert(events.begin() + static_cast<long>(before), ev);
            for (auto &[a, b] : loops) {
                a++;
                b++;
            }
            continue;
        }
        Access ac = touched(*s, fns);
        Event ev;
        ev.writes = ac.write;
        ev.reads = ac.read;
        ev.line = s->line;
        ev.loop = loop;
        ev.join = (s->k == SK::Expr && s->e1 && s->e1->k == EK::Call && s->e1->a == "join_all");
        events.push_back(ev);
        if (s->k == SK::While || s->k == SK::ForRange || s->k == SK::ForIter) {
            size_t start = events.size();
            collect(view(s->body), events, loops, s, fns);
            loops.emplace_back(start, events.size());
        } else if (s->k == SK::If) {
            collect(view(s->body), events, loops, loop, fns);
            collect(else_of(*s), events, loops, loop, fns);
        } else if (s->k == SK::Scope || s->k == SK::Try) {
            collect(view(s->body), events, loops, loop, fns);
            collect(view(s->body2), events, loops, loop, fns);
        }
    }
}

void races(const View &body, std::vector<BorrowDiag> &out,
           const FnSummaries *fns = nullptr) {
    std::vector<Event> events;
    std::vector<std::pair<size_t, size_t>> loops;
    collect(body, events, loops, nullptr, fns);
    std::map<std::string, std::string> alias;
    alias_map(body, alias);
    for (Event &e : events) {
        std::set<std::string> w, r;
        for (const auto &x : e.writes) w.insert(resolve(x, alias));
        for (const auto &x : e.reads) r.insert(resolve(x, alias));
        e.writes = w;
        e.reads = r;
    }
    for (size_t i = 0; i < events.size(); i++) {
        const Event &e = events[i];
        if (!e.spawn || (e.writes.empty() && e.reads.empty())) continue;
        for (size_t j = i + 1; j < events.size(); j++) {
            if (j >= e.member_begin && j < e.member_end) continue;   // its own body
            const Event &other = events[j];
            if (e.loop != other.loop && other.spawn) continue;
            if (!touches(e, other)) continue;
            bool joined = false;
            for (size_t k = i + 1; k <= j && k < events.size(); k++)
                if (events[k].join) {
                    joined = true;
                    break;
                }
            if (joined) continue;
            const std::string who = first_touched(e, other);
            out.push_back(BorrowDiag{other.line ? other.line : e.line,
                                     wrote_side(e, other) ? race_msg(who)
                                                          : race_read_msg(who),
                                     std::nullopt});
            break;
        }
        if (e.writes.empty()) continue;   // the loop rule is about writes only
        for (const auto &[start, end] : loops) {
            if (i < start || i >= end) continue;
            bool joined = false;
            for (size_t k = i + 1; k < end; k++)
                if (events[k].join) {
                    joined = true;
                    break;
                }
            if (!joined) out.push_back(BorrowDiag{e.line, loop_msg(*e.writes.begin()), std::nullopt});
            break;
        }
    }
}

void all_stmts(const View &body, std::vector<const Stmt *> &out) {
    for (const Stmt *s : body) {
        if (!s) continue;
        out.push_back(s);
        all_stmts(view(s->body), out);
        all_stmts(view(s->body2), out);
        all_stmts(else_of(*s), out);
    }
}

void register_fns(const View &body, FnSummaries &fns) {
    for (const Stmt *s : body) {
        if (!s) continue;
        const Stmt *node = s;
        while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
        if (node->k == SK::Extern) {
            // Same reason as `capability.py`: a foreign function that declares
            // `writes(p)` is a writer of the place handed to it, and is the only
            // way this pass can see the write a `free` or a `memcpy` does.
            FnSummary sum;
            for (size_t i = 0; i < node->ext.params.size(); i++)
                sum.params.push_back(node->ext.params[i].name);
            std::set<std::string> written;
            for (const auto &eff : node->ext.effects)
                if (eff.rfind("writes:", 0) == 0) written.insert(eff.substr(7));
            for (size_t i = 0; i < sum.params.size(); i++)
                if (written.count(sum.params[i])) sum.mut_idx.insert(i);
            if (!node->ext.name.empty()) fns[node->ext.name] = std::move(sum);
        }
        if (node->k == SK::Fn || node->k == SK::ExportFn || node->k == SK::Impl) {
            for (const auto &f : node->fns) {
                if (!f.name || f.name->empty()) continue;
                FnSummary sum;
                for (size_t i = 0; i < f.params.size(); i++) {
                    sum.params.push_back(f.params[i].name);
                    if (f.params[i].mode == Mode::Mut) sum.mut_idx.insert(i);
                }
                sum.body = view(f.body);
                alias_map(sum.body, sum.alias);
                fns[*f.name] = std::move(sum);
                register_fns(view(f.body), fns);
            }
        }
        register_fns(view(node->body), fns);
        register_fns(view(node->body2), fns);
        register_fns(else_of(*node), fns);
    }
}

FnSummaries build_fn_summaries(const std::vector<StmtP> &body) {
    FnSummaries fns;
    register_fns(view(body), fns);
    for (int pass = 0; pass < 3; pass++) {
        for (auto &[name, info] : fns) {
            std::map<std::string, size_t> pindex;
            for (size_t i = 0; i < info.params.size(); i++) pindex[info.params[i]] = i;
            std::vector<const Stmt *> stmts;
            all_stmts(info.body, stmts);
            for (const Stmt *s : stmts) {
                for (const auto &[c, v] : inserts(*s, &fns)) {
                    std::string rc = root(c), rv = root(v);
                    if (rc.empty() || rv.empty()) continue;
                    rc = resolve(rc, info.alias);
                    rv = resolve(rv, info.alias);
                    auto ic = pindex.find(rc);
                    auto iv = pindex.find(rv);
                    if (ic != pindex.end() && iv != pindex.end() && ic->second != iv->second) {
                        info.ins_pairs.insert({ic->second, iv->second});
                        info.mut_idx.insert(ic->second);
                    }
                }
            }
        }
    }
    return fns;
}

}  // namespace

// The callback contracts of `capability.py`: the same two rules, the same two
// sentences, because a program that one checker accepts and the other refuses is
// a program whose meaning depends on which binary a person ran first.
std::string schedule_msg(const std::string &cb, const std::string &callee,
                         const std::string &global) {
    return "`" + cb + "` is registered as a callback of `" + callee +
           "`, which declares `thread(any)`, but it touches `" + global +
           "`; a callback on another thread may not touch the caller's state (hint: "
           "pass it through the callback's parameters, or declare `thread(same)`)";
}

std::string reenter_msg(const std::string &cb, const std::string &callee) {
    return "`" + cb + "` is registered as a callback of `" + callee + "` and calls `" +
           callee +
           "` again; a callee may re-enter a callback unless the declaration says "
           "`reenter(no)` (hint: add `reenter(no)` to the declaration)";
}

struct CallbackSlot {
    std::set<size_t> indices;
    std::vector<std::string> contracts;
};

std::map<std::string, CallbackSlot> callback_slots(const View &stmts) {
    std::map<std::string, CallbackSlot> out;
    for (const Stmt *s : stmts) {
        if (!s) continue;
        const Stmt *node = s;
        while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
        if (node->k != SK::Extern) continue;
        CallbackSlot slot;
        for (size_t i = 0; i < node->ext.params.size(); i++) {
            const auto &ann = node->ext.params[i].ann;
            if (ann && ann->rfind("fn(", 0) == 0) slot.indices.insert(i);
        }
        if (slot.indices.empty()) continue;
        slot.contracts = node->ext.contracts;
        out[node->ext.name] = std::move(slot);
    }
    return out;
}

// (names read, functions called) anywhere in a body.
void uses(const View &body, std::set<std::string> &names, std::set<std::string> &calls) {
    std::vector<const Stmt *> stmts;
    all_stmts(body, stmts);
    for (const Stmt *s : stmts) {
        std::vector<const Expr *> nodes;
        for (const Expr *e : stmt_exprs(*s)) each_node(e, nodes);
        for (const Expr *n : nodes) {
            if (n->k == EK::Call) calls.insert(n->a);
            else if (n->k == EK::Var) names.insert(n->a);
        }
    }
}

// The first global a function reaches, following calls; empty when it is clean.
std::string first_global_touch(const std::string &start, const FnSummaries &fns,
                               const std::set<std::string> &globals) {
    std::set<std::string> seen;
    std::vector<std::string> stack{start};
    while (!stack.empty()) {
        const std::string cur = stack.back();
        stack.pop_back();
        if (!seen.insert(cur).second) continue;
        auto it = fns.find(cur);
        if (it == fns.end()) continue;
        std::set<std::string> names, calls;
        uses(it->second.body, names, calls);
        std::set<std::string> local(it->second.params.begin(), it->second.params.end());
        declared(it->second.body, local);
        for (const auto &n : names) {
            if (local.count(n)) continue;
            if (globals.count(n)) return n;      // sorted: the smallest, both sides
        }
        for (const auto &c : calls)
            if (c != cur) stack.push_back(c);
    }
    return {};
}

bool reaches(const std::string &start, const std::string &target, const FnSummaries &fns) {
    std::set<std::string> seen;
    std::vector<std::string> stack{start};
    while (!stack.empty()) {
        const std::string cur = stack.back();
        stack.pop_back();
        if (!seen.insert(cur).second) continue;
        auto it = fns.find(cur);
        if (it == fns.end()) continue;
        std::set<std::string> names, calls;
        uses(it->second.body, names, calls);
        if (calls.count(target)) return true;
        for (const auto &c : calls)
            if (c != cur) stack.push_back(c);
    }
    return false;
}

std::vector<BorrowDiag> callback_contracts(const View &stmts, const View &top,
                                           const FnSummaries &fns) {
    std::vector<BorrowDiag> out;
    const auto slots = callback_slots(stmts);
    if (slots.empty()) return out;
    std::set<std::string> globals;
    for (const Stmt *s : top)
        if (s && s->k == SK::Let) globals.insert(s->a);
    std::vector<View> bodies{top};
    for (const auto &[name, info] : fns)
        if (!info.body.empty()) bodies.push_back(info.body);
    for (const auto &body : bodies) {
        std::vector<const Stmt *> stmts_of;
        all_stmts(body, stmts_of);
        for (const Stmt *s : stmts_of) {
            std::vector<const Expr *> nodes;
            for (const Expr *e : stmt_exprs(*s)) each_node(e, nodes);
            for (const Expr *n : nodes) {
                if (n->k != EK::Call) continue;
                auto slot = slots.find(n->a);
                if (slot == slots.end()) continue;
                for (size_t i : slot->second.indices) {
                    if (i >= n->kids.size()) continue;
                    const Expr *arg = n->kids[i].get();
                    if (!arg || arg->k != EK::Str) continue;
                    const std::string cb = arg->a;
                    if (!fns.count(cb)) continue;      // the callback gate owns that
                    const auto &contracts = slot->second.contracts;
                    if (std::find(contracts.begin(), contracts.end(), "thread:any") !=
                        contracts.end()) {
                        const std::string touch = first_global_touch(cb, fns, globals);
                        if (!touch.empty())
                            out.push_back(BorrowDiag{s->line, schedule_msg(cb, n->a, touch),
                                                     std::nullopt});
                    }
                    if (std::find(contracts.begin(), contracts.end(), "reenter:no") ==
                            contracts.end() &&
                        reaches(cb, n->a, fns))
                        out.push_back(BorrowDiag{s->line, reenter_msg(cb, n->a),
                                                 std::nullopt});
                }
            }
        }
    }
    return out;
}

std::vector<BorrowDiag> sharing_check(const std::vector<StmtP> &body) {
    std::vector<BorrowDiag> out;
    View top;
    FnSummaries fns = build_fn_summaries(body);
    for (const auto &sp : body) {
        const Stmt *node = sp.get();
        while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
        if (kDecls.count(node->k)) {
            if (node->k == SK::Fn || node->k == SK::ExportFn || node->k == SK::Impl) {
                for (const auto &f : node->fns) {
                    Owner own;
                    std::map<std::string, std::string> alias;
                    alias_map(view(f.body), alias);
                    cycles(view(f.body), out, own, alias, &fns);
                    races(view(f.body), out, &fns);
                }
            }
            continue;
        }
        top.push_back(sp.get());
    }
    Owner own;
    std::map<std::string, std::string> alias;
    alias_map(top, alias);
    cycles(top, out, own, alias, &fns);   // a script is one body, like `main`
    races(top, out, &fns);
    for (const auto &d : callback_contracts(view(body), top, fns)) out.push_back(d);
    return out;
}
