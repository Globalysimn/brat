#include "parfor.h"

#include <functional>
#include <map>
#include <set>

namespace {

using View = std::vector<const Stmt *>;

const std::set<std::string> kRaise{"raise", "unwrap", "assert", "exit"};
const std::set<std::string> kIo{"print", "eprint", "log.info", "log.warn", "log.error",
                                "trace.span", "read_file", "write_file", "append_file", "read_bytes",
                                "write_bytes", "proc_run",
                                "fs_exists", "fs_is_dir", "fs_file_size", "fs_mkdir_all",
                                "fs_list_dir", "fs_remove_dir",
                                "fs_open", "fs_read_line", "fs_close", "stdin_read_line",
                                "fs_rename", "fs_remove",
                                "random_bytes", "https_request_raw",
                                "sql_open", "sql_close", "sql_exec", "sql_query",
                                "args", "args_bytes", "env_get", "env_get_bytes",
                                "time_ms", "mono_ns", "sleep_ms", "tcp_listen", "tcp_connect",
                                "tcp_send", "tcp_recv", "tcp_close"};
const std::set<std::string> kIoHeads{"tcp", "log", "trace"};
const std::set<std::string> kAllocMeth{"push", "ins", "set", "copy", "map", "filter",
                                       "chars", "split", "replace", "trim", "upper",
                                       "lower", "join"};
const std::set<std::string> kWriteMeth{"push", "pop", "ins", "del", "set", "sort",
                                       "sort_by", "clear", "send", "close"};
const std::set<std::string> kPtrWrite{"ptr_write_i64", "ptr_write_i32", "ptr_write_f64",
                                      "ptr_write_f32", "ptr_write_u8", "ptr_write_str",
                                      "ptr_write_ptr", "ptr_write_struct", "slice_set"};
const std::set<std::string> kPluginFfi{
    "lib_open", "lib_sym", "lib_close", "call",
    "ptr_alloc_i64", "ptr_alloc_f64", "ptr_alloc_f32", "ptr_alloc_u8",
    "ptr_alloc_ptr", "ptr_alloc_struct", "ptr_free",
    "ptr_read_i64", "ptr_read_i32", "ptr_read_f64", "ptr_read_f32",
    "ptr_read_u8", "ptr_read_str", "ptr_read_ptr", "ptr_read_struct",
    "ptr_write_i64", "ptr_write_i32", "ptr_write_f64", "ptr_write_f32",
    "ptr_write_u8", "ptr_write_str", "ptr_write_ptr", "ptr_write_struct",
    "str_from_ptr", "slice_of", "slice_of_ptr", "slice_of_struct", "slice_struct_of",
    "slice_len", "slice_get", "slice_set"};
const std::set<std::string> kOrder{"raise", "io", "alloc", "spawn", "ffi"};
const std::set<SK> kDecls{SK::Fn, SK::Impl, SK::Trait, SK::Enum, SK::Struct, SK::Extern,
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

void each_expr_in(const View &body, const std::function<void(const Expr &)> &fn);

void each_expr(const Expr *e, const std::function<void(const Expr &)> &fn) {
    if (e == nullptr) return;
    fn(*e);
    for (const auto &k : e->kids) each_expr(k.get(), fn);
    if ((e->k == EK::Closure || e->k == EK::ParFor) && e->fn)
        each_expr_in(view(e->fn->body), fn);
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
            else if (kPluginFfi.count(e.a)) row.insert("ffi");
            else if (kIo.count(e.a) || kIoHeads.count(head_of(e.a))) row.insert("io");
            break;
        case EK::MCall:
            if (e.b == "unwrap") row.insert("raise");
            if (kAllocMeth.count(e.b)) row.insert("alloc");
            if (e.b == "send" || e.b == "close" || e.b == "recv") row.insert("io");
            break;
        default: break;
        }
    });
    return row;
}

struct BodyInfo {
    View body;
    std::set<std::string> params;
};

using Bodies = std::map<std::string, BodyInfo>;

Bodies collect_bodies(const std::vector<StmtP> &body) {
    Bodies out;
    auto pnames = [](const std::vector<Param> &ps) {
        std::set<std::string> s;
        for (const auto &p : ps) s.insert(p.name);
        return s;
    };
    for (const auto &sp : body) {
        const Stmt *node = sp.get();
        while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
        if ((node->k == SK::Fn || node->k == SK::ExportFn) && !node->fns.empty()) {
            out[node->fns[0].name.value_or("")] = {view(node->fns[0].body), pnames(node->fns[0].params)};
        } else if (node->k == SK::Impl) {
            for (const auto &f : node->fns)
                out[node->a + "." + f.name.value_or("")] = {view(f.body), pnames(f.params)};
        } else if (node->k == SK::Extern) {
            out[node->ext.name] = {{}, {}};
        }
    }
    return out;
}

// Union over the call graph until nothing grows: a caller inherits its callees.
std::map<std::string, std::set<std::string>>
rows_of(const Bodies &bodies, const std::set<std::string> &externs,
        std::map<std::string, std::vector<std::string>> &by_short) {
    std::map<std::string, std::set<std::string>> rows;
    std::map<std::string, std::set<std::string>> fcalls;
    std::map<std::string, std::set<std::string>> mcalls;
    for (const auto &[name, info] : bodies) {
        auto dot = name.rfind('.');
        if (dot != std::string::npos) by_short[name.substr(dot + 1)].push_back(name);
        if (externs.count(name)) {
            rows[name].insert("ffi");
            continue;
        }
        rows[name] = local_row(info.body);
        each_expr_in(info.body, [&](const Expr &e) {
            if (e.k == EK::Call) fcalls[name].insert(e.a);
            else if (e.k == EK::MCall) mcalls[name].insert(e.b);
        });
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &[name, info] : bodies) {
            (void)info;
            auto merge = [&](const std::string &key) {
                if (!rows.count(key)) return;
                for (const auto &eff : rows[key])
                    if (!rows[name].count(eff)) {
                        rows[name].insert(eff);
                        changed = true;
                    }
            };
            for (const auto &c : fcalls[name]) merge(c);
            for (const auto &m : mcalls[name]) {
                if (by_short.count(m)) {
                    for (const auto &k : by_short[m]) merge(k);
                } else {
                    merge(m);
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

std::vector<std::string> outer_writes(const View &body, const std::set<std::string> &local,
                                      const FnTable *funcs = nullptr) {
    std::set<std::string> bound = local;
    each_stmt(body, [&](const Stmt &s) {
        if (s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::ForRange || s.k == SK::ForIter)
            bound.insert(s.a);
    });
    std::vector<std::string> hits;
    auto note = [&](const std::string &name) {
        if (name.empty() || bound.count(name)) return;
        for (const auto &h : hits)
            if (h == name) return;
        hits.push_back(name);
    };
    each_stmt(body, [&](const Stmt &s) {
        if (s.k == SK::Set) note(head_of(s.a));
        if (s.k == SK::SetIdx || s.k == SK::SetField) {
            each_expr(s.e1.get(), [&](const Expr &e) {
                if (e.k == EK::Var) note(e.a);
            });
        }
        for (const Expr *e : {s.e1.get(), s.e2.get(), s.e3.get()})
            each_expr(e, [&](const Expr &x) {
                bool umut = funcs != nullptr && funcs->umeth.count(x.b) && funcs->umeth.at(x.b);
                if (x.k == EK::MCall && (kWriteMeth.count(x.b) || umut) && !x.kids.empty()) {
                    each_expr(x.kids[0].get(), [&](const Expr &r) {
                        if (r.k == EK::Var) note(r.a);
                    });
                } else if (x.k == EK::Call && kPtrWrite.count(x.a) && !x.kids.empty()) {
                    each_expr(x.kids[0].get(), [&](const Expr &r) {
                        if (r.k == EK::Var) note(r.a);
                    });
                } else if (x.k == EK::Call && funcs != nullptr && funcs->sigs.count(x.a)) {
                    const auto &ps = funcs->sigs.at(x.a);
                    for (size_t i = 0; i < ps.size() && i < x.kids.size(); i++) {
                        if (ps[i].mode == Mode::Mut) {
                            each_expr(x.kids[i].get(), [&](const Expr &r) {
                                if (r.k == EK::Var) note(r.a);
                            });
                        }
                    }
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
    std::map<std::string, std::vector<std::string>> by_short;
    auto rows = rows_of(bodies, externs, by_short);
    std::map<std::string, std::vector<std::string>> fn_gwrites;
    std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>> fn_edges;
    for (const auto &[name, info] : bodies) {
        if (externs.count(name)) continue;
        fn_gwrites[name] = outer_writes(info.body, info.params, &funcs);
        each_expr_in(info.body, [&](const Expr &e) {
            if (e.k == EK::Call) fn_edges[name].first.insert(e.a);
            else if (e.k == EK::MCall) fn_edges[name].second.insert(e.b);
        });
    }
    bool gw_chg = true;
    while (gw_chg) {
        gw_chg = false;
        for (const auto &[name, edges] : fn_edges) {
            auto &cur = fn_gwrites[name];
            auto add_from = [&](const std::string &k) {
                auto it = fn_gwrites.find(k);
                if (it == fn_gwrites.end()) return;
                for (const auto &w : it->second) {
                    bool seen = false;
                    for (const auto &h : cur) if (h == w) { seen = true; break; }
                    if (!seen) { cur.push_back(w); gw_chg = true; }
                }
            };
            for (const auto &c : edges.first) add_from(c);
            for (const auto &m : edges.second)
                if (by_short.count(m))
                    for (const auto &k : by_short[m]) add_from(k);
        }
    }

    View top;
    for (const auto &sp : body) top.push_back(sp.get());
    std::function<void(const Expr &)> scan = [&](const Expr &e) {
        if (e.k == EK::ParFor) {
            const int line = static_cast<int>(e.i);
            auto sig = funcs.sigs.find(e.b);
            if (sig == funcs.sigs.end()) {
                out.push_back(BorrowDiag{line,
                    "`" + e.b + "` is not a function: `par for` needs the folding operation "
                    "by name",
                    std::nullopt});
            } else if (sig->second.size() != 2) {
                out.push_back(BorrowDiag{line,
                    "`" + e.b + "` takes " + std::to_string(sig->second.size()) +
                    " parameter(s); a fold needs exactly two (the accumulated value and the "
                    "next one)",
                    std::nullopt});
            } else {
                if (rows.count(e.b) && !ordered_empty(rows[e.b])) {
                    out.push_back(BorrowDiag{line,
                        "`" + e.b + "` cannot fold in parallel: it can " + joined(rows[e.b]) +
                        " (a monoid operation has to be pure, or the result depends on the "
                        "thread order)",
                        std::nullopt});
                }
                if (fn_gwrites.count(e.b)) {
                    for (const auto &name : fn_gwrites[e.b]) {
                        out.push_back(BorrowDiag{line,
                            "the `par for` body writes `" + name +
                            "`, which lives outside the loop "
                            "(a parallel fold may not touch outer mutable state)",
                            std::nullopt});
                    }
                }
            }
            std::set<std::string> brow = local_row(view(e.fn->body));
            std::set<std::string> b_fc, b_mc;
            each_expr_in(view(e.fn->body), [&](const Expr &c) {
                if (c.k == EK::Call) {
                    b_fc.insert(c.a);
                    if (rows.count(c.a))
                        for (const auto &x : rows[c.a]) brow.insert(x);
                } else if (c.k == EK::MCall) {
                    b_mc.insert(c.b);
                    if (by_short.count(c.b)) {
                        for (const auto &k : by_short[c.b])
                            for (const auto &x : rows[k]) brow.insert(x);
                    } else if (rows.count(c.b)) {
                        for (const auto &x : rows[c.b]) brow.insert(x);
                    }
                }
            });
            if (!ordered_empty(brow)) {
                out.push_back(BorrowDiag{line,
                    "the `par for` body cannot " + joined(brow) +
                    ": every element is computed at the same time",
                    std::nullopt});
            }
            auto writes = outer_writes(view(e.fn->body), {e.a}, &funcs);
            auto add_gw = [&](const std::string &k) {
                if (!fn_gwrites.count(k)) return;
                for (const auto &w : fn_gwrites[k]) {
                    if (w == e.a) continue;
                    bool seen = false;
                    for (const auto &h : writes) if (h == w) { seen = true; break; }
                    if (!seen) writes.push_back(w);
                }
            };
            for (const auto &c : b_fc) add_gw(c);
            for (const auto &m : b_mc)
                if (by_short.count(m))
                    for (const auto &k : by_short[m]) add_gw(k);
            for (const auto &name : writes) {
                out.push_back(BorrowDiag{line,
                    "the `par for` body writes `" + name + "`, which lives outside the loop "
                    "(a parallel fold may not touch outer mutable state)",
                    std::nullopt});
            }
        }
    };
    each_expr_in(top, scan);
    return out;
}
