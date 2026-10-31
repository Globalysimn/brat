#include "parfor.h"

#include <functional>
#include <map>
#include <set>

namespace {

using View = std::vector<const Stmt *>;

const std::set<std::string> kRaise{"raise", "unwrap", "assert", "exit"};
const std::set<std::string> kIo{"print", "eprint", "log.info", "log.warn", "log.error",
                                "trace.span", "read_file", "write_file", "args", "env_get",
                                "time_ms", "sleep_ms", "tcp_listen", "tcp_connect",
                                "tcp_send", "tcp_recv", "tcp_close"};
const std::set<std::string> kIoHeads{"tcp", "log", "trace"};
const std::set<std::string> kAllocMeth{"push", "ins", "set", "copy", "map", "filter",
                                       "chars", "split", "replace", "trim", "upper",
                                       "lower", "join"};
const std::set<std::string> kWriteMeth{"push", "pop", "ins", "del", "set", "sort",
                                       "sort_by", "clear"};
const std::set<std::string> kOrder{"raise", "io", "alloc", "spawn", "ffi"};
const std::set<SK> kDecls{SK::Fn, SK::Impl, SK::Trait, SK::Enum, SK::Extern,
                          SK::ExternStruct, SK::ExportFn};

std::string head_of(const std::string &name) {
    auto dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

View view(const std::vector<StmtP> &body) {
    View out;
    for (const auto &s : body) out.push_back(s.get());
    return out;
}

View else_of(const Stmt &s) {
    if (!s.els) return {};
    if (s.els->k == SK::Block) return view(s.els->body);
    return {s.els.get()};
}

void each_stmt(const View &body, const std::function<void(const Stmt &)> &fn) {
    for (const Stmt *s : body) {
        fn(*s);
        each_stmt(view(s->body), fn);
        each_stmt(view(s->body2), fn);
        each_stmt(else_of(*s), fn);
        for (const auto &f : s->fns) each_stmt(view(f.body), fn);
    }
}

void each_expr(const Expr *e, const std::function<void(const Expr &)> &fn) {
    if (e == nullptr) return;
    fn(*e);
    for (const auto &k : e->kids) each_expr(k.get(), fn);
}

void each_expr_in(const View &body, const std::function<void(const Expr &)> &fn) {
    each_stmt(body, [&](const Stmt &s) {
        for (const Expr *e : {s.e1.get(), s.e2.get(), s.e3.get()}) each_expr(e, fn);
    });
}

// The effect row of one body, without following calls.
std::set<std::string> local_row(const View &body) {
    std::set<std::string> row;
    each_stmt(body, [&](const Stmt &s) {
        if (s.k == SK::Spawn || s.k == SK::Scope) row.insert("spawn");
    });
    each_expr_in(body, [&](const Expr &e) {
        switch (e.k) {
        case EK::Quest: row.insert("raise"); break;
        case EK::VecLit:
        case EK::MapLit:
        case EK::StructLit:
        case EK::Closure: row.insert("alloc"); break;
        case EK::Call:
            if (kRaise.count(e.a)) row.insert("raise");
            else if (kIo.count(e.a) || kIoHeads.count(head_of(e.a))) row.insert("io");
            break;
        case EK::MCall:
            if (kAllocMeth.count(e.b)) row.insert("alloc");
            break;
        default: break;
        }
    });
    return row;
}

using Bodies = std::map<std::string, View>;

Bodies collect_bodies(const std::vector<StmtP> &body) {
    Bodies out;
    for (const auto &sp : body) {
        const Stmt *node = sp.get();
        while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
        if (node->k == SK::Fn && !node->fns.empty()) {
            out[node->fns[0].name.value_or("")] = view(node->fns[0].body);
        } else if (node->k == SK::Impl) {
            for (const auto &f : node->fns) out[node->a + "." + f.name.value_or("")] = view(f.body);
        } else if (node->k == SK::Extern) {
            out[node->ext.name] = {};
        }
    }
    return out;
}

// Union over the call graph until nothing grows: a caller inherits its callees.
std::map<std::string, std::set<std::string>> rows_of(const Bodies &bodies,
                                                     const std::set<std::string> &externs) {
    std::map<std::string, std::set<std::string>> rows;
    std::map<std::string, std::set<std::string>> calls;
    std::map<std::string, std::string> alias;
    for (const auto &[name, body] : bodies) {
        auto dot = name.rfind('.');
        alias[dot == std::string::npos ? name : name.substr(dot + 1)] = name;
        if (externs.count(name)) {
            rows[name].insert("ffi");
            continue;
        }
        rows[name] = local_row(body);
        each_expr_in(body, [&](const Expr &e) {
            if (e.k == EK::Call) calls[name].insert(e.a);
            else if (e.k == EK::MCall) calls[name].insert(e.b);
        });
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &[name, callees] : calls) {
            for (const auto &c : callees) {
                std::string key = rows.count(c) ? c : (alias.count(c) ? alias[c] : "");
                if (key.empty()) continue;
                for (const auto &eff : rows[key])
                    if (!rows[name].count(eff)) {
                        rows[name].insert(eff);
                        changed = true;
                    }
            }
        }
    }
    return rows;
}

std::string joined(const std::set<std::string> &row) {
    std::string out;
    for (const auto &n : kOrder)
        if (row.count(n)) {
            if (!out.empty()) out += ", ";
            out += n;
        }
    return out;
}

bool ordered_empty(const std::set<std::string> &row) {
    for (const auto &n : kOrder)
        if (row.count(n)) return false;
    return true;
}

std::vector<std::string> outer_writes(const View &body, const std::string &var) {
    std::set<std::string> bound{var};
    for (const Stmt *s : body)
        if (s->k == SK::Let || s->k == SK::LetTmp) bound.insert(s->a);
    std::vector<std::string> hits;
    auto note = [&](const std::string &name) {
        if (name.empty() || bound.count(name)) return;
        for (const auto &h : hits)
            if (h == name) return;
        hits.push_back(name);
    };
    each_stmt(body, [&](const Stmt &s) {
        if (s.k == SK::Set) note(s.a);
        if (s.k == SK::SetIdx) {
            each_expr(s.e1.get(), [&](const Expr &e) {
                if (e.k == EK::Var) note(e.a);
            });
        }
        for (const Expr *e : {s.e1.get(), s.e2.get(), s.e3.get()})
            each_expr(e, [&](const Expr &x) {
                if (x.k == EK::MCall && kWriteMeth.count(x.b) && !x.kids.empty() &&
                    x.kids[0]->k == EK::Var) {
                    note(x.kids[0]->a);
                }
            });
    });
    return hits;
}

}  // namespace

std::vector<BorrowDiag> parfor_rules(const std::vector<StmtP> &body, const FnTable &funcs) {
    std::vector<BorrowDiag> out;
    Bodies bodies = collect_bodies(body);
    std::set<std::string> externs;
    for (const auto &sp : body) {
        const Stmt *node = sp.get();
        while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
        if (node->k == SK::Extern) externs.insert(node->ext.name);
    }
    auto rows = rows_of(bodies, externs);

    View top;
    for (const auto &sp : body) top.push_back(sp.get());
    std::function<void(const Expr &)> scan = [&](const Expr &e) {
        if (e.k == EK::ParFor) {
            const int line = static_cast<int>(e.i);
            auto sig = funcs.sigs.find(e.b);
            if (sig == funcs.sigs.end()) {
                out.push_back(BorrowDiag{line,
                    "`" + e.b + "` is not a function: `par for` needs the folding operation "
                    "by name"});
            } else if (sig->second.size() != 2) {
                out.push_back(BorrowDiag{line,
                    "`" + e.b + "` takes " + std::to_string(sig->second.size()) +
                    " parameter(s); a fold needs exactly two (the accumulated value and the "
                    "next one)"});
            } else if (rows.count(e.b) && !ordered_empty(rows[e.b])) {
                out.push_back(BorrowDiag{line,
                    "`" + e.b + "` cannot fold in parallel: it can " + joined(rows[e.b]) +
                    " (a monoid operation has to be pure, or the result depends on the "
                    "thread order)"});
            }
            std::set<std::string> brow = local_row(view(e.fn->body));
            each_expr_in(view(e.fn->body), [&](const Expr &c) {
                if (c.k != EK::Call) return;
                auto dot = c.a.rfind('.');
                std::string bare = dot == std::string::npos ? c.a : c.a.substr(dot + 1);
                if (rows.count(c.a)) {
                    for (const auto &x : rows[c.a]) brow.insert(x);
                } else if (rows.count(bare)) {
                    for (const auto &x : rows[bare]) brow.insert(x);
                }
            });
            if (!ordered_empty(brow)) {
                out.push_back(BorrowDiag{line,
                    "the `par for` body cannot " + joined(brow) +
                    ": every element is computed at the same time"});
            }
            for (const auto &name : outer_writes(view(e.fn->body), e.a)) {
                out.push_back(BorrowDiag{line,
                    "the `par for` body writes `" + name + "`, which lives outside the loop "
                    "(a parallel fold may not touch outer mutable state)"});
            }
        }
        for (const auto &k : e.kids) scan(*k);
    };
    each_expr_in(top, scan);
    return out;
}
