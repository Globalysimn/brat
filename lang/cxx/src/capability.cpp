#include "capability.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace {

using View = std::vector<const Stmt *>;

const std::set<std::string> kInsert{"push", "ins", "set"};
const std::set<std::string> kPtrWrite{"ptr_write_i64", "ptr_write_f64", "ptr_write_f32",
                                      "ptr_write_u8", "ptr_write_str", "ptr_write_ptr",
                                      "ptr_write_struct", "slice_set", "fill"};
const std::set<std::string> kPtrRead{"ptr_read_i64", "ptr_read_f64", "ptr_read_f32",
                                     "ptr_read_u8", "ptr_read_str", "ptr_read_ptr",
                                     "ptr_read_struct"};
const std::set<std::string> kPtrAliasCalls{"ptr_offset", "slice_of", "slice_of_ptr",
                                           "slice_of_struct"};
const std::set<std::string> kResourceClose{"tcp_close", "lib_close"};
const std::set<std::string> kMutate{"pop", "sort", "sort_by", "del", "clear"};
const std::set<std::string> kNumOps{"-", "*", "/", "%", "&", "|", "^", "<<", ">>",
                                    "==", "!=", "<", ">", "<=", ">=", "&&", "||"};
const std::set<std::string> kScalarCalls{"own", "len", "is_ok", "is_err", "is_some", "is_none",
                                         "is_vec", "is_str", "is_map", "is_bool", "is_int",
                                         "is_float", "__stag_is", "__shas", "ptr_is_null",
                                         "ptr_read_i64", "ptr_read_f64", "ptr_read_f32",
                                         "re_match"};
const std::set<SK> kDecls{SK::Fn, SK::Impl, SK::Trait, SK::Enum, SK::Struct, SK::Extern,
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

std::string unjoined_msg(const std::string &name) {
    return "`" + name +
           "` is touched inside `spawn` in a function that returns without "
           "`join_all()` (hint: call `join_all()` before returning, or use "
           "`scope { ... }`)";
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

std::string root(const Expr *e) {
    while (e) {
        switch (e->k) {
        case EK::Var: return e->a;
        case EK::Un:
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

bool is_nested_place(const Expr *e) {
    while (e && (e->k == EK::Move || e->k == EK::Quest))
        e = e->kids.empty() ? nullptr : e->kids[0].get();
    return e && (e->k == EK::Idx || e->k == EK::Field);
}

std::string strip_ws(const std::string &s) {
    std::string out;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    return out;
}

std::set<size_t> plugin_write_indices(const Expr *sig_expr) {
    std::set<size_t> out;
    if (!sig_expr || sig_expr->k != EK::Str) return out;
    std::string raw = sig_expr->a;
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.erase(raw.begin());
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
    size_t op = raw.find('(');
    if (op == std::string::npos || op == 0 || raw.back() != ')') return out;
    std::string rest = raw.substr(op + 1, raw.size() - op - 2);
    int depth = 0;
    size_t start = 0, idx = 0;
    for (size_t i = 0; i < rest.size(); i++) {
        if (rest[i] == '(') depth++;
        else if (rest[i] == ')') depth--;
        else if (rest[i] == ',' && depth == 0) {
            std::string sl = strip_ws(rest.substr(start, i - start));
            if (sl == "outptr" || sl == "ownptr" || sl == "moveptr") out.insert(idx);
            idx++;
            start = i + 1;
        }
    }
    std::string tail = strip_ws(rest.substr(start));
    if (tail == "outptr" || tail == "ownptr" || tail == "moveptr") out.insert(idx);
    return out;
}

void all_stmts(const View &body, std::vector<const Stmt *> &out);
std::vector<const Expr *> stmt_exprs(const Stmt &s);

void each_node(const Expr *e, std::vector<const Expr *> &out) {
    if (!e) return;
    out.push_back(e);
    for (const auto &k : e->kids) each_node(k.get(), out);
    if ((e->k == EK::Closure || e->k == EK::ParFor) && e->fn) {
        std::vector<const Stmt *> inner;
        all_stmts(view(e->fn->body), inner);
        for (const Stmt *st : inner) {
            for (const Expr *se : stmt_exprs(*st)) each_node(se, out);
        }
    }
}

void non_own_vars(const Expr *e, std::set<std::string> &acc) {
    if (!e) return;
    switch (e->k) {
    case EK::Num:
    case EK::Str:
    case EK::RStr:
    case EK::Bool:
    case EK::None:
    case EK::Un:
        return;
    case EK::Bin:
        if (kNumOps.count(e->a)) return;
        break;
    case EK::Call:
        if (kScalarCalls.count(e->a)) return;
        break;
    case EK::Var:
        if (!e->a.empty()) acc.insert(e->a);
        return;
    default:
        break;
    }
    for (const auto &k : e->kids) non_own_vars(k.get(), acc);
}

struct FnSummary {
    std::vector<std::string> params;
    std::set<size_t> mut_idx;
    std::set<size_t> ptr_idx;
    std::set<std::pair<size_t, size_t>> ins_pairs;
    std::set<std::pair<std::string, size_t>> global_ins;
    std::set<std::string> global_writes;
    std::set<std::string> global_reads;
    std::set<size_t> ret_params;
    View body;
    std::map<std::string, std::string> alias;
};

using FnSummaries = std::map<std::string, FnSummary>;

std::vector<const Expr *> stmt_exprs(const Stmt &s) {
    switch (s.k) {
    case SK::SetIdx: return {s.e1.get(), s.e2.get(), s.e3.get()};
    case SK::SetField: return {s.e1.get(), s.e2.get()};
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

struct TouchDetail {
    std::set<std::string> read, write, nested_write, ptr_touch;
    std::vector<std::pair<std::string, std::string>> chan_sends;
    std::set<std::string> chan_recvs;
};

TouchDetail touch_detail(const Stmt &s, const FnSummaries *fns = nullptr) {
    TouchDetail out;
    std::vector<const Expr *> exprs = stmt_exprs(s);
    std::vector<const Expr *> nodes;
    for (const Expr *e : exprs) each_node(e, nodes);
    for (const Expr *n : nodes) {
        if (!n) continue;
        if ((n->k == EK::Closure || n->k == EK::ParFor) && n->fn) {
            std::vector<const Stmt *> inner;
            all_stmts(view(n->fn->body), inner);
            for (const Stmt *ist : inner) {
                if (ist->k == SK::Set && !ist->a.empty()) {
                    auto dot = ist->a.find('.');
                    out.write.insert(dot == std::string::npos ? ist->a : ist->a.substr(0, dot));
                } else if (ist->k == SK::SetIdx || ist->k == SK::SetField) {
                    std::string ir = root(ist->e1.get());
                    if (!ir.empty()) {
                        out.write.insert(ir);
                        if (is_nested_place(ist->e1.get())) out.nested_write.insert(ir);
                    }
                }
            }
        }
        if (n->k == EK::MCall) {
            std::string r = n->kids.empty() ? std::string() : root(n->kids[0].get());
            if (n->b == "send" && n->kids.size() >= 2) {
                std::string cr = root(n->kids[0].get());
                std::string vr = root(n->kids[1].get());
                if (!cr.empty() && !vr.empty()) out.chan_sends.push_back({cr, vr});
            } else if (n->b == "recv" && !n->kids.empty()) {
                std::string cr = root(n->kids[0].get());
                if (!cr.empty()) out.chan_recvs.insert(cr);
            }
            const FnSummary *fs = nullptr;
            if (fns) {
                auto it = fns->find(n->b);
                if (it != fns->end()) fs = &it->second;
            }
            if (!r.empty()) {
                if (kInsert.count(n->b) || kMutate.count(n->b) || (fs && fs->mut_idx.count(0))) {
                    out.write.insert(r);
                    if (!n->kids.empty() && is_nested_place(n->kids[0].get()))
                        out.nested_write.insert(r);
                } else {
                    out.read.insert(r);
                }
            }
            if (fs) {
                for (size_t idx : fs->mut_idx) {
                    if (idx < n->kids.size()) {
                        std::string ar = root(n->kids[idx].get());
                        if (!ar.empty()) {
                            out.write.insert(ar);
                            if (is_nested_place(n->kids[idx].get())) out.nested_write.insert(ar);
                        }
                    }
                }
                for (const auto &gw : fs->global_writes) out.write.insert(gw);
                for (const auto &gr : fs->global_reads) out.read.insert(gr);
            }
        } else if (n->k == EK::Call && n->a == "call") {
            if (n->kids.size() >= 2) {
                auto widxs = plugin_write_indices(n->kids[1].get());
                for (size_t idx = 0; idx + 2 < n->kids.size(); idx++) {
                    std::string ar = root(n->kids[idx + 2].get());
                    if (!ar.empty()) {
                        out.ptr_touch.insert(ar);
                        if (widxs.count(idx)) out.write.insert(ar);
                        else out.read.insert(ar);
                    }
                }
            }
        } else if (n->k == EK::Call && kPtrWrite.count(n->a)) {
            if (!n->kids.empty()) {
                std::string ar = root(n->kids[0].get());
                if (!ar.empty()) {
                    out.write.insert(ar);
                    out.ptr_touch.insert(ar);
                }
            }
        } else if (n->k == EK::Call && kPtrRead.count(n->a)) {
            if (!n->kids.empty()) {
                std::string ar = root(n->kids[0].get());
                if (!ar.empty()) {
                    out.read.insert(ar);
                    out.ptr_touch.insert(ar);
                }
            }
        } else if (n->k == EK::Call && kResourceClose.count(n->a)) {
            if (!n->kids.empty()) {
                std::string ar = root(n->kids[0].get());
                if (!ar.empty()) {
                    out.write.insert(ar);
                    if (n->a == "lib_close") out.ptr_touch.insert(ar);
                }
            }
        } else if (n->k == EK::Move) {
            std::string r = root(n);
            if (!r.empty()) out.write.insert(r);
        } else if (n->k == EK::Call && fns && fns->count(n->a)) {
            const auto &fs = fns->at(n->a);
            for (size_t idx : fs.mut_idx) {
                if (idx < n->kids.size()) {
                    std::string ar = root(n->kids[idx].get());
                    if (!ar.empty()) {
                        out.write.insert(ar);
                        if (is_nested_place(n->kids[idx].get())) out.nested_write.insert(ar);
                    }
                }
            }
            for (size_t idx : fs.ptr_idx) {
                if (idx < n->kids.size()) {
                    std::string ar = root(n->kids[idx].get());
                    if (!ar.empty()) out.ptr_touch.insert(ar);
                }
            }
            for (const auto &gw : fs.global_writes) out.write.insert(gw);
            for (const auto &gr : fs.global_reads) out.read.insert(gr);
        } else if (n->k == EK::Var || n->k == EK::Field || n->k == EK::Idx) {
            std::string r = root(n);
            if (!r.empty()) out.read.insert(r);
        }
    }
    if (s.k == SK::SetIdx) {
        std::string r = root(s.e1.get());
        if (!r.empty()) {
            out.write.insert(r);
            if (is_nested_place(s.e1.get())) out.nested_write.insert(r);
        }
    }
    if (s.k == SK::SetField) {
        std::string r = root(s.e1.get());
        if (!r.empty()) {
            out.write.insert(r);
            if (is_nested_place(s.e1.get())) out.nested_write.insert(r);
        }
    }
    for (const auto &w : out.write) out.read.erase(w);
    return out;
}

void declared_expr(const Expr *e, std::set<std::string> &out);

void declared(const View &body, std::set<std::string> &out) {
    for (const Stmt *s : body) {
        if (!s) continue;
        if (s->k == SK::Let || s->k == SK::LetTmp || s->k == SK::ForRange || s->k == SK::ForIter)
            out.insert(s->a);
        if (s->k == SK::Try && s->opt1 && !s->opt1->empty()) out.insert(*s->opt1);
        for (const Expr *e : stmt_exprs(*s)) declared_expr(e, out);
        declared(view(s->body), out);
        declared(view(s->body2), out);
        declared(else_of(*s), out);
        for (const auto &f : s->fns) declared(view(f.body), out);
    }
}

void declared_expr(const Expr *e, std::set<std::string> &out) {
    if (!e) return;
    if ((e->k == EK::Closure || e->k == EK::ParFor) && e->fn) {
        if (e->k == EK::ParFor && !e->a.empty()) out.insert(e->a);
        for (const auto &p : e->fn->params) out.insert(p.name);
        declared(view(e->fn->body), out);
    }
    for (const auto &k : e->kids) declared_expr(k.get(), out);
}

using StructFields = std::map<std::string, std::map<std::string, std::string>>;

const std::set<std::string> kAcyclicScalars{
    "bool", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64",
    "usize", "isize", "f32", "f64", "str", "none"};

std::string trim_type(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool is_acyclic_type(const std::string &raw, const std::set<std::string> &acyc_structs) {
    std::string t = trim_type(raw);
    if (kAcyclicScalars.count(t) > 0 || acyc_structs.count(t) > 0) return true;
    if (t.size() > 8 && t.rfind("Option[", 0) == 0 && t.back() == ']') {
        return is_acyclic_type(t.substr(7, t.size() - 8), acyc_structs);
    }
    if (t.size() > 8 && t.rfind("Result[", 0) == 0 && t.back() == ']') {
        std::string inner = t.substr(7, t.size() - 8);
        int depth = 0;
        for (size_t i = 0; i < inner.size(); i++) {
            if (inner[i] == '[' || inner[i] == '(') depth++;
            else if (inner[i] == ']' || inner[i] == ')') depth--;
            else if (inner[i] == ',' && depth == 0) {
                return is_acyclic_type(inner.substr(0, i), acyc_structs) &&
                       is_acyclic_type(inner.substr(i + 1), acyc_structs);
            }
        }
        return false;
    }
    return false;
}

std::set<std::string> compute_acyclic_structs(const StructFields &sf) {
    std::set<std::string> acyc;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &[raw_name, fields] : sf) {
            std::string sname = raw_name.rfind("__enum__:", 0) == 0
                                    ? raw_name.substr(9)
                                    : raw_name;
            if (acyc.count(sname)) continue;
            bool all_ok = true;
            for (const auto &[_, fann] : fields) {
                if (!is_acyclic_type(fann, acyc)) {
                    all_ok = false;
                    break;
                }
            }
            if (all_ok) {
                acyc.insert(sname);
                changed = true;
            }
        }
    }
    return acyc;
}

std::string unwrap_cap_adt(const std::string &raw) {
    std::string a = trim_type(raw);
    if (a.size() > 8 && a.rfind("Option[", 0) == 0 && a.back() == ']')
        return trim_type(a.substr(7, a.size() - 8));
    if (a.size() > 8 && a.rfind("Result[", 0) == 0 && a.back() == ']') {
        std::string inner = a.substr(7, a.size() - 8);
        int depth = 0;
        for (size_t i = 0; i < inner.size(); i++) {
            if (inner[i] == '[' || inner[i] == '(') depth++;
            else if (inner[i] == ']' || inner[i] == ')') depth--;
            else if (inner[i] == ',' && depth == 0)
                return trim_type(inner.substr(0, i));
        }
        return trim_type(inner);
    }
    return a;
}

std::optional<std::string> infer_cap_type(const Expr *e,
                                          const std::map<std::string, std::string> &vars,
                                          const StructFields &sf) {
    while (e && e->k == EK::Move)
        e = e->kids.empty() ? nullptr : e->kids[0].get();
    if (!e) return std::nullopt;
    if (e->k == EK::Quest) {
        auto sub = infer_cap_type(e->kids.empty() ? nullptr : e->kids[0].get(), vars, sf);
        if (!sub) return std::nullopt;
        return unwrap_cap_adt(*sub);
    }
    switch (e->k) {
    case EK::Num: return e->is_float ? std::string("f64") : std::string("i64");
    case EK::Bool: return std::string("bool");
    case EK::Str:
    case EK::RStr: return std::string("str");
    case EK::None: return std::string("none");
    case EK::Un:
        if (e->a == "!") return std::string("bool");
        if (e->a == "-" || e->a == "~") return std::string("i64");
        return std::nullopt;
    case EK::Bin:
        if (kNumOps.count(e->a)) return std::string("i64");
        if (e->a == "+" && e->kids.size() == 2) {
            auto l = infer_cap_type(e->kids[0].get(), vars, sf);
            auto r = infer_cap_type(e->kids[1].get(), vars, sf);
            if (l && (*l == "i64" || *l == "f64" || *l == "str")) return l;
            if (r && (*r == "i64" || *r == "f64" || *r == "str")) return r;
        }
        return std::nullopt;
    case EK::Call:
        if (kScalarCalls.count(e->a) || e->a == "str") return std::string("i64");
        if (e->a == "Some" && e->kids.size() == 1) {
            auto in = infer_cap_type(e->kids[0].get(), vars, sf);
            if (in && !in->empty()) return "Option[" + *in + "]";
        }
        if (e->a == "unwrap" && e->kids.size() == 1) {
            auto in = infer_cap_type(e->kids[0].get(), vars, sf);
            if (in) return unwrap_cap_adt(*in);
        }
        if (e->a == "unwrap_or" && e->kids.size() == 2) {
            auto in = infer_cap_type(e->kids[0].get(), vars, sf);
            if (in) {
                std::string u = unwrap_cap_adt(*in);
                if (!u.empty() && u != *in && u != "dyn") return u;
            }
            return infer_cap_type(e->kids[1].get(), vars, sf);
        }
        return std::nullopt;
    case EK::Var: {
        auto it = vars.find(e->a);
        return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    case EK::StructLit:
        if (sf.count(e->a)) return e->a;
        return std::nullopt;
    case EK::Idx: {
        auto bt = infer_cap_type(e->kids.empty() ? nullptr : e->kids[0].get(), vars, sf);
        if (!bt) return std::nullopt;
        std::string a = trim_type(*bt);
        if (a.rfind("vec[", 0) == 0 && a.back() == ']')
            return trim_type(a.substr(4, a.size() - 5));
        if (a.rfind("map[", 0) == 0 && a.back() == ']') {
            std::string inner = a.substr(4, a.size() - 5);
            int depth = 0;
            size_t split = std::string::npos;
            for (size_t i = 0; i < inner.size(); i++) {
                if (inner[i] == '[') depth++;
                else if (inner[i] == ']') depth--;
                else if (inner[i] == ',' && depth == 0) split = i;
            }
            return trim_type(split == std::string::npos ? inner : inner.substr(split + 1));
        }
        return std::nullopt;
    }
    case EK::Field: {
        auto bt = infer_cap_type(e->kids.empty() ? nullptr : e->kids[0].get(), vars, sf);
        if (!bt) return std::nullopt;
        auto sit = sf.find(trim_type(*bt));
        if (sit == sf.end()) return std::nullopt;
        auto fit = sit->second.find(e->b);
        if (fit == sit->second.end()) return std::nullopt;
        return trim_type(fit->second);
    }
    default:
        return std::nullopt;
    }
}

void collect_var_types(const View &body, std::map<std::string, std::string> &vars,
                       const StructFields &sf) {
    for (const Stmt *s : body) {
        if (!s) continue;
        if ((s->k == SK::Let || s->k == SK::LetTmp) && !s->a.empty()) {
            if (auto inf = infer_cap_type(s->e1.get(), vars, sf)) {
                vars[s->a] = *inf;
            } else if (s->ann && !s->ann->empty() &&
                       (!s->e1 || s->e1->k != EK::Var || vars.count(s->e1->a))) {
                vars[s->a] = trim_type(*s->ann);
            }
        } else if (s->k == SK::ForRange && !s->a.empty()) {
            vars[s->a] = "i64";
        } else if (s->k == SK::ForIter && !s->a.empty()) {
            if (auto bt = infer_cap_type(s->e1.get(), vars, sf)) {
                std::string a = trim_type(*bt);
                if (a.rfind("vec[", 0) == 0 && a.back() == ']')
                    vars[s->a] = trim_type(a.substr(4, a.size() - 5));
                else if (a == "str")
                    vars[s->a] = "str";
            }
        }
        collect_var_types(view(s->body), vars, sf);
        collect_var_types(view(s->body2), vars, sf);
        collect_var_types(else_of(*s), vars, sf);
    }
}

bool is_acyclic_expr(const Expr *e, const std::map<std::string, std::string> &vars,
                     const StructFields &sf, const std::set<std::string> &acyc_structs) {
    auto t = infer_cap_type(e, vars, sf);
    return t && is_acyclic_type(*t, acyc_structs);
}

void alias_map(const View &body, std::map<std::string, std::string> &out) {
    for (const Stmt *s : body) {
        if (!s) continue;
        if (s->k == SK::Let && s->e1) {
            std::string r = root(s->e1.get());
            if (r.empty() && s->e1->k == EK::Call && kPtrAliasCalls.count(s->e1->a) &&
                !s->e1->kids.empty())
                r = root(s->e1->kids[0].get());
            if (!r.empty() && r != s->a) out[s->a] = r;
        }
        if (s->k == SK::Try && s->opt1) out.erase(*s->opt1);
        alias_map(view(s->body), out);
        alias_map(view(s->body2), out);
        alias_map(else_of(*s), out);
    }
}

void cycle_alias_map(const View &body, std::map<std::string, std::string> &out,
                     const std::map<std::string, std::string> &vars,
                     const StructFields &sf, const std::set<std::string> &acyc_structs) {
    for (const Stmt *s : body) {
        if (!s) continue;
        if ((s->k == SK::Let || s->k == SK::LetTmp) && s->e1) {
            if (!is_acyclic_expr(s->e1.get(), vars, sf, acyc_structs) &&
                !(vars.count(s->a) && is_acyclic_type(vars.at(s->a), acyc_structs))) {
                std::string r = root(s->e1.get());
                if (r.empty() && s->e1->k == EK::Call && kPtrAliasCalls.count(s->e1->a) &&
                    !s->e1->kids.empty())
                    r = root(s->e1->kids[0].get());
                if (!r.empty() && r != s->a) out[s->a] = r;
            }
        }
        if (s->k == SK::Try && s->opt1) out.erase(*s->opt1);
        cycle_alias_map(view(s->body), out, vars, sf, acyc_structs);
        cycle_alias_map(view(s->body2), out, vars, sf, acyc_structs);
        cycle_alias_map(else_of(*s), out, vars, sf, acyc_structs);
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

struct InsEdge {
    std::string c_override;   // non-empty when lhs is a synthetic variable name (let / global_ins)
    const Expr *c_expr = nullptr;
    const Expr *v_expr = nullptr;
};

std::vector<const Expr *> lit_elements(const Expr *e, const FnSummaries *fns = nullptr) {
    std::vector<const Expr *> out;
    if (!e) return out;
    if (e->k == EK::VecLit || e->k == EK::StructLit) {
        for (const auto &k : e->kids) out.push_back(k.get());
    } else if (e->k == EK::MapLit) {
        for (size_t i = 1; i < e->kids.size(); i += 2) out.push_back(e->kids[i].get());
    } else if (e->k == EK::Call && fns && fns->count(e->a)) {
        for (size_t i : fns->at(e->a).ret_params)
            if (i < e->kids.size()) out.push_back(e->kids[i].get());
    } else if (e->k == EK::MCall && fns && fns->count(e->b)) {
        for (size_t i : fns->at(e->b).ret_params)
            if (i < e->kids.size()) out.push_back(e->kids[i].get());
    }
    return out;
}

std::vector<InsEdge> inserts(const Stmt &s, const FnSummaries *fns = nullptr) {
    std::vector<InsEdge> out;
    if (s.k == SK::SetIdx && s.e1 && s.e3) out.push_back({"", s.e1.get(), s.e3.get()});
    else if (s.k == SK::SetField && s.e1 && s.e2) out.push_back({"", s.e1.get(), s.e2.get()});
    else if (s.k == SK::Let && !s.a.empty() && s.e1) {
        for (const Expr *elem : lit_elements(s.e1.get(), fns)) {
            std::string rv = root(elem);
            if (!rv.empty() && rv != s.a) out.push_back({s.a, nullptr, elem});
        }
    }
    std::vector<const Expr *> nodes;
    for (const Expr *e : stmt_exprs(s)) each_node(e, nodes);
    for (const Expr *n : nodes) {
        if (!n) continue;
        if (n->k == EK::MCall) {
            if (kInsert.count(n->b) && n->kids.size() >= 2) {
                const Expr *value =
                    n->b == "set" ? n->kids.back().get() : n->kids[1].get();
                out.push_back({"", n->kids[0].get(), value});
            } else if (fns && fns->count(n->b)) {
                const auto &fs = fns->at(n->b);
                for (const auto &[ci, vi] : fs.ins_pairs) {
                    if (ci < n->kids.size() && vi < n->kids.size())
                        out.push_back({"", n->kids[ci].get(), n->kids[vi].get()});
                }
                for (const auto &[g, vi] : fs.global_ins) {
                    if (vi < n->kids.size())
                        out.push_back({g, nullptr, n->kids[vi].get()});
                }
            }
        } else if (n->k == EK::Call && fns && fns->count(n->a)) {
            const auto &fs = fns->at(n->a);
            for (const auto &[ci, vi] : fs.ins_pairs) {
                if (ci < n->kids.size() && vi < n->kids.size())
                    out.push_back({"", n->kids[ci].get(), n->kids[vi].get()});
            }
            for (const auto &[g, vi] : fs.global_ins) {
                if (vi < n->kids.size())
                    out.push_back({g, nullptr, n->kids[vi].get()});
            }
        }
    }
    return out;
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

using ContainsMap = std::map<std::string, std::set<std::string>>;

ContainsMap containment_graph(const View &body, const std::map<std::string, std::string> &alias,
                              const FnSummaries *fns = nullptr,
                              const std::map<std::string, std::string> *vars = nullptr,
                              const StructFields *sf = nullptr,
                              const std::set<std::string> *acyc = nullptr) {
    ContainsMap contains;
    std::vector<const Stmt *> stmts;
    all_stmts(body, stmts);
    for (const Stmt *s : stmts) {
        for (const auto &ed : inserts(*s, fns)) {
            if (vars && sf && acyc && is_acyclic_expr(ed.v_expr, *vars, *sf, *acyc)) continue;
            std::string rc = ed.c_override.empty() ? root(ed.c_expr) : ed.c_override;
            std::string rv = root(ed.v_expr);
            if (rc.empty() || rv.empty()) continue;
            rc = resolve(rc, alias);
            rv = resolve(rv, alias);
            if (rc != rv) contains[rc].insert(rv);
        }
    }
    return contains;
}

std::set<std::string> descendants(const std::string &name, const ContainsMap &contains) {
    std::set<std::string> out;
    auto it0 = contains.find(name);
    if (it0 == contains.end()) return out;
    std::vector<std::string> stack(it0->second.begin(), it0->second.end());
    size_t steps = 0;
    while (!stack.empty() && steps < 256) {
        std::string cur = stack.back();
        stack.pop_back();
        steps++;
        if (cur == name || out.count(cur)) continue;
        out.insert(cur);
        auto it = contains.find(cur);
        if (it != contains.end())
            stack.insert(stack.end(), it->second.begin(), it->second.end());
    }
    return out;
}

struct Owner {
    std::map<std::string, std::string> parent;

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
            const FnSummaries *fns,
            const std::map<std::string, std::string> &vars,
            const StructFields &sf,
            const std::set<std::string> &acyc) {
    for (const Stmt *s : body) {
        if (!s) continue;
        for (const auto &ed : inserts(*s, fns)) {
            if (is_acyclic_expr(ed.v_expr, vars, sf, acyc)) continue;
            std::string rc = ed.c_override.empty() ? root(ed.c_expr) : ed.c_override;
            std::string rv = root(ed.v_expr);
            if (rc.empty() || rv.empty()) continue;
            if (vars.count(rv) && is_acyclic_type(vars.at(rv), acyc)) continue;
            rc = resolve(rc, alias);
            rv = resolve(rv, alias);
            if (vars.count(rv) && is_acyclic_type(vars.at(rv), acyc)) continue;
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
            cycles(view(s->body), out, own, alias, fns, vars, sf, acyc);
            cycles(view(s->body2), out, own, alias, fns, vars, sf, acyc);
            cycles(else_of(*s), out, own, alias, fns, vars, sf, acyc);
            break;
        case SK::Fn:
        case SK::ExportFn:
            for (const auto &f : s->fns) {
                Owner fresh;
                std::map<std::string, std::string> fvars = vars;
                for (const auto &p : f.params) {
                    if (p.ann && !p.ann->empty()) fvars[p.name] = trim_type(*p.ann);
                }
                collect_var_types(view(f.body), fvars, sf);
                std::map<std::string, std::string> inner;
                cycle_alias_map(view(f.body), inner, fvars, sf, acyc);
                cycles(view(f.body), out, fresh, inner, fns, fvars, sf, acyc);
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
    std::set<std::string> writes, reads, nested_writes, ptr_touches;
    std::vector<std::pair<std::string, std::string>> chan_sends;
    std::set<std::string> chan_recvs;
    int line = 0;
    const void *loop = nullptr;
    size_t member_begin = 0, member_end = 0;
};

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
    std::set<std::string> clash;
    for (const auto &w : a.writes)
        if (b.writes.count(w) || b.reads.count(w)) clash.insert(w);
    for (const auto &r : a.reads)
        if (b.writes.count(r)) clash.insert(r);
    return clash.empty() ? std::string() : *clash.begin();
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
            for (size_t i = before; i < events.size(); i++) {
                for (const auto &w : events[i].writes)
                    if (!locals.count(w)) ev.writes.insert(w);
                for (const auto &r : events[i].reads)
                    if (!locals.count(r)) ev.reads.insert(r);
                for (const auto &nw : events[i].nested_writes)
                    if (!locals.count(nw)) ev.nested_writes.insert(nw);
                for (const auto &pt : events[i].ptr_touches)
                    if (!locals.count(pt)) ev.ptr_touches.insert(pt);
                for (const auto &cr : events[i].chan_recvs)
                    if (!locals.count(cr)) ev.chan_recvs.insert(cr);
            }
            ev.member_begin = before + 1;
            ev.member_end = events.size() + 1;
            events.insert(events.begin() + static_cast<long>(before), ev);
            for (auto &[a, b] : loops) {
                a++;
                b++;
            }
            continue;
        }
        TouchDetail td = touch_detail(*s, fns);
        Event ev;
        ev.writes = std::move(td.write);
        ev.reads = std::move(td.read);
        ev.nested_writes = std::move(td.nested_write);
        ev.ptr_touches = std::move(td.ptr_touch);
        ev.chan_sends = std::move(td.chan_sends);
        ev.chan_recvs = std::move(td.chan_recvs);
        ev.line = s->line;
        ev.loop = loop;
        ev.join = (s->k == SK::Expr && s->e1 && s->e1->k == EK::Call && s->e1->a == "join_all");
        events.push_back(std::move(ev));
        if (s->k == SK::While || s->k == SK::ForRange || s->k == SK::ForIter) {
            size_t start = events.size();
            collect(view(s->body), events, loops, s, fns);
            loops.emplace_back(start, events.size());
        } else if (s->k == SK::If) {
            collect(view(s->body), events, loops, loop, fns);
            collect(else_of(*s), events, loops, loop, fns);
        } else if (s->k == SK::Scope) {
            collect(view(s->body), events, loops, loop, fns);
            Event end_ev;
            end_ev.join = true;
            end_ev.line = s->line;
            end_ev.loop = loop;
            events.push_back(std::move(end_ev));
        } else if (s->k == SK::Try) {
            collect(view(s->body), events, loops, loop, fns);
            collect(view(s->body2), events, loops, loop, fns);
        }
    }
}

void races(const View &body, std::vector<BorrowDiag> &out,
           const FnSummaries *fns = nullptr, bool is_fn_body = false) {
    std::vector<Event> events;
    std::vector<std::pair<size_t, size_t>> loops;
    std::map<std::string, std::string> alias;
    alias_map(body, alias);
    ContainsMap contains = containment_graph(body, alias, fns);
    collect(body, events, loops, nullptr, fns);

    std::map<std::string, std::set<std::string>> chan_payloads;
    for (const Event &e : events) {
        for (const auto &[cr, vr] : e.chan_sends) {
            std::string rc = resolve(cr, alias), rv = resolve(vr, alias);
            chan_payloads[rc].insert(rv);
            for (const auto &d : descendants(rv, contains)) chan_payloads[rc].insert(d);
        }
    }

    for (Event &e : events) {
        std::set<std::string> w, r, nw, pt;
        for (const auto &x : e.writes) w.insert(resolve(x, alias));
        for (const auto &x : e.reads) r.insert(resolve(x, alias));
        for (const auto &x : e.nested_writes) nw.insert(resolve(x, alias));
        for (const auto &x : e.ptr_touches) pt.insert(resolve(x, alias));
        for (const auto &cr : e.chan_recvs) {
            auto it = chan_payloads.find(resolve(cr, alias));
            if (it != chan_payloads.end()) r.insert(it->second.begin(), it->second.end());
        }
        for (const auto &x : nw)
            for (const auto &d : descendants(x, contains)) w.insert(d);
        std::vector<std::string> rsnap(r.begin(), r.end());
        for (const auto &x : rsnap)
            for (const auto &d : descendants(x, contains)) r.insert(d);
        for (const auto &wx : w) r.erase(wx);
        e.writes = std::move(w);
        e.reads = std::move(r);
        e.ptr_touches = std::move(pt);
    }

    for (size_t i = 0; i < events.size(); i++) {
        const Event &e = events[i];
        if (!e.spawn || (e.writes.empty() && e.reads.empty() && e.ptr_touches.empty())) continue;
        bool raced = false;
        for (size_t j = i + 1; j < events.size(); j++) {
            if (j >= e.member_begin && j < e.member_end) continue;
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
            raced = true;
            break;
        }
        if (!e.writes.empty()) {
            for (const auto &[start, end] : loops) {
                if (i < start || i >= end) continue;
                bool joined = false;
                for (size_t k = i + 1; k < end; k++)
                    if (events[k].join) {
                        joined = true;
                        break;
                    }
                if (!joined) {
                    out.push_back(BorrowDiag{e.line, loop_msg(*e.writes.begin()), std::nullopt});
                    raced = true;
                }
                break;
            }
        }
        if (is_fn_body && !raced && (!e.writes.empty() || !e.ptr_touches.empty())) {
            bool joined = false;
            for (size_t k = i + 1; k < events.size(); k++)
                if (events[k].join) {
                    joined = true;
                    break;
                }
            if (!joined) {
                std::set<std::string> comb = e.writes;
                comb.insert(e.ptr_touches.begin(), e.ptr_touches.end());
                out.push_back(BorrowDiag{e.line, unjoined_msg(*comb.begin()), std::nullopt});
            }
        }
    }
}

void register_fns(const View &body, FnSummaries &fns) {
    for (const Stmt *s : body) {
        if (!s) continue;
        const Stmt *node = s;
        while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
        if (node->k == SK::Extern) {
            FnSummary sum;
            std::set<std::string> written;
            for (const auto &eff : node->ext.effects)
                if (eff.rfind("writes:", 0) == 0) written.insert(eff.substr(7));
            for (size_t i = 0; i < node->ext.params.size(); i++) {
                const auto &p = node->ext.params[i];
                sum.params.push_back(p.name);
                if (written.count(p.name) || p.mode == Mode::Move) sum.mut_idx.insert(i);
                if (p.ann && (*p.ann == "ptr" || p.ann->rfind("ptr[", 0) == 0))
                    sum.ptr_idx.insert(i);
            }
            if (!node->ext.name.empty()) fns[node->ext.name] = std::move(sum);
        }
        if (node->k == SK::Fn || node->k == SK::ExportFn || node->k == SK::Impl) {
            for (const auto &f : node->fns) {
                if (!f.name || f.name->empty()) continue;
                FnSummary sum;
                for (size_t i = 0; i < f.params.size(); i++) {
                    sum.params.push_back(f.params[i].name);
                    if (f.params[i].mode == Mode::Mut || f.params[i].mode == Mode::Move)
                        sum.mut_idx.insert(i);
                    if (f.params[i].ann &&
                        (*f.params[i].ann == "ptr" || f.params[i].ann->rfind("ptr[", 0) == 0))
                        sum.ptr_idx.insert(i);
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

FnSummaries build_fn_summaries(const std::vector<StmtP> &body,
                               const std::set<std::string> &globals) {
    FnSummaries fns;
    register_fns(view(body), fns);
    for (int pass = 0; pass < 6; pass++) {
        for (auto &[name, info] : fns) {
            if (info.body.empty()) continue;
            std::map<std::string, size_t> pindex;
            for (size_t i = 0; i < info.params.size(); i++) pindex[info.params[i]] = i;
            std::set<std::string> local(info.params.begin(), info.params.end());
            declared(info.body, local);
            ContainsMap contains = containment_graph(info.body, info.alias, &fns);
            ContainsMap rev_contains;
            for (const auto &[par, kids] : contains)
                for (const auto &ch : kids) rev_contains[ch].insert(par);

            std::vector<const Stmt *> stmts;
            all_stmts(info.body, stmts);
            for (const Stmt *s : stmts) {
                TouchDetail td = touch_detail(*s, &fns);
                for (const auto &w : td.write) {
                    std::string rw = resolve(w, info.alias);
                    auto ip = pindex.find(rw);
                    if (ip != pindex.end()) info.mut_idx.insert(ip->second);
                    else if (globals.count(rw) && !local.count(rw)) info.global_writes.insert(rw);
                }
                for (const auto &r : td.read) {
                    std::string rr = resolve(r, info.alias);
                    if (globals.count(rr) && !local.count(rr)) info.global_reads.insert(rr);
                }
                for (const auto &p : td.ptr_touch) {
                    std::string rp = resolve(p, info.alias);
                    auto ip = pindex.find(rp);
                    if (ip != pindex.end()) info.ptr_idx.insert(ip->second);
                }
                for (const auto &[cr, vr] : td.chan_sends) {
                    std::string rc = resolve(cr, info.alias);
                    std::string rv = resolve(vr, info.alias);
                    auto iv = pindex.find(rv);
                    if (globals.count(rc) && !local.count(rc) && iv != pindex.end()) {
                        info.global_ins.insert({rc, iv->second});
                        info.global_writes.insert(rc);
                    }
                }
                if (s->k == SK::Set && !s->a.empty() && globals.count(s->a) &&
                    !local.count(s->a) && s->e1) {
                    info.global_writes.insert(s->a);
                    std::set<std::string> vs;
                    non_own_vars(s->e1.get(), vs);
                    for (const auto &vn : vs) {
                        std::string rv = resolve(vn, info.alias);
                        auto iv = pindex.find(rv);
                        if (iv != pindex.end()) info.global_ins.insert({s->a, iv->second});
                        for (const auto &ch : descendants(rv, contains)) {
                            auto ic = pindex.find(ch);
                            if (ic != pindex.end()) info.global_ins.insert({s->a, ic->second});
                        }
                    }
                }
                if (s->k == SK::Return && s->e1) {
                    std::set<std::string> vs;
                    non_own_vars(s->e1.get(), vs);
                    for (const auto &vn : vs) {
                        std::string rv = resolve(vn, info.alias);
                        auto iv = pindex.find(rv);
                        if (iv != pindex.end()) info.ret_params.insert(iv->second);
                        for (const auto &ch : descendants(rv, contains)) {
                            auto ic = pindex.find(ch);
                            if (ic != pindex.end()) info.ret_params.insert(ic->second);
                        }
                    }
                }
                for (const auto &ed : inserts(*s, &fns)) {
                    std::string rc = ed.c_override.empty() ? root(ed.c_expr) : ed.c_override;
                    std::string rv = root(ed.v_expr);
                    if (rc.empty() || rv.empty()) continue;
                    rc = resolve(rc, info.alias);
                    rv = resolve(rv, info.alias);
                    auto ic = pindex.find(rc);
                    auto iv = pindex.find(rv);
                    if (ic != pindex.end() && iv != pindex.end() && ic->second != iv->second) {
                        info.ins_pairs.insert({ic->second, iv->second});
                        info.mut_idx.insert(ic->second);
                    }
                    if (globals.count(rc) && !local.count(rc)) {
                        info.global_writes.insert(rc);
                        if (iv != pindex.end()) info.global_ins.insert({rc, iv->second});
                        for (const auto &ch : descendants(rv, contains)) {
                            auto ich = pindex.find(ch);
                            if (ich != pindex.end()) info.global_ins.insert({rc, ich->second});
                        }
                    }
                    auto it_rev = rev_contains.find(rc);
                    if (it_rev != rev_contains.end() && iv != pindex.end()) {
                        for (const auto &anc : it_rev->second) {
                            auto ianc = pindex.find(anc);
                            if (ianc != pindex.end() && ianc->second != iv->second)
                                info.ins_pairs.insert({ianc->second, iv->second});
                        }
                    }
                }
            }
        }
    }
    return fns;
}

}  // namespace

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

void uses(const View &body, std::set<std::string> &names, std::set<std::string> &calls) {
    std::vector<const Stmt *> stmts;
    all_stmts(body, stmts);
    for (const Stmt *s : stmts) {
        std::vector<const Expr *> nodes;
        for (const Expr *e : stmt_exprs(*s)) each_node(e, nodes);
        for (const Expr *n : nodes) {
            if (!n) continue;
            if (n->k == EK::Call) calls.insert(n->a);
            else if (n->k == EK::Var) names.insert(n->a);
        }
    }
}

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
            if (globals.count(n)) return n;
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
    for (const Stmt *s : top) {
        if (!s) continue;
        const Stmt *n = s;
        while (n->k == SK::Pub && !n->body.empty()) n = n->body[0].get();
        if (n->k == SK::Let && !n->a.empty()) globals.insert(n->a);
    }
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
                if (!n || n->k != EK::Call) continue;
                auto slot = slots.find(n->a);
                if (slot == slots.end()) continue;
                for (size_t i : slot->second.indices) {
                    if (i >= n->kids.size()) continue;
                    const Expr *arg = n->kids[i].get();
                    if (!arg || arg->k != EK::Str) continue;
                    const std::string cb = arg->a;
                    if (!fns.count(cb)) continue;
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

std::vector<BorrowDiag> sharing_check(
    const std::vector<StmtP> &body,
    const std::map<std::string, std::map<std::string, std::string>> &extra_structs) {
    std::vector<BorrowDiag> out;
    View top;
    std::set<std::string> top_globals;
    StructFields sf = extra_structs;
    for (const auto &sp : body) {
        if (!sp) continue;
        const Stmt *n = sp.get();
        while (n->k == SK::Pub && !n->body.empty()) n = n->body[0].get();
        if (n->k == SK::Let && !n->a.empty()) top_globals.insert(n->a);
        if (n->k == SK::Struct || n->k == SK::ExternStruct) {
            sf[n->a];
            for (const auto &f : n->fields) {
                sf[n->a][f.name] = f.ann ? *f.ann : "dyn";
            }
        } else if (n->k == SK::Enum) {
            auto &ef = sf["__enum__:" + n->a];
            size_t slot = 0;
            for (const auto &v : n->variants) {
                for (size_t fi = 0; fi < v.fields.size(); fi++) {
                    ef["f" + std::to_string(slot++)] =
                        (fi < v.field_anns.size() && v.field_anns[fi] && !v.field_anns[fi]->empty())
                            ? *v.field_anns[fi]
                            : "dyn";
                }
            }
        }
    }
    const std::set<std::string> acyc = compute_acyclic_structs(sf);
    FnSummaries fns = build_fn_summaries(body, top_globals);
    for (const auto &sp : body) {
        const Stmt *node = sp.get();
        while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
        if (kDecls.count(node->k)) {
            if (node->k == SK::Fn || node->k == SK::ExportFn || node->k == SK::Impl) {
                for (const auto &f : node->fns) {
                    Owner own;
                    std::map<std::string, std::string> fvars;
                    for (const auto &p : f.params) {
                        if (p.ann && !p.ann->empty()) fvars[p.name] = trim_type(*p.ann);
                        else if (node->k == SK::Impl && p.name == "self" &&
                                 (sf.count(node->a) || sf.count("__enum__:" + node->a)))
                            fvars[p.name] = node->a;
                    }
                    collect_var_types(view(f.body), fvars, sf);
                    std::map<std::string, std::string> alias;
                    cycle_alias_map(view(f.body), alias, fvars, sf, acyc);
                    cycles(view(f.body), out, own, alias, &fns, fvars, sf, acyc);
                    races(view(f.body), out, &fns, true);
                }
            }
            continue;
        }
        top.push_back(sp.get());
    }
    Owner own;
    std::map<std::string, std::string> top_vars;
    collect_var_types(top, top_vars, sf);
    std::map<std::string, std::string> alias;
    cycle_alias_map(top, alias, top_vars, sf, acyc);
    cycles(top, out, own, alias, &fns, top_vars, sf, acyc);
    races(top, out, &fns, false);
    for (const auto &d : callback_contracts(view(body), top, fns)) out.push_back(d);
    return out;
}
