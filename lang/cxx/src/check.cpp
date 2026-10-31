// The type pass. It walks the tree the parser built and reports what does not
// typecheck, with the same words, in the same order, at the same lines as
// `lang/engine/typer.py` — that equality is the whole point of the port.
#include "check.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>


bool Ty::struct_has(const std::string &f, Ty *out) const {
    for (const auto &kv : fields) {
        if (kv.first == f) {
            if (out) *out = kv.second;
            return true;
        }
    }
    return false;
}


static bool field_of(const std::vector<std::pair<std::string, Ty>> &fields,
                     const std::string &name, Ty *out) {
    for (const auto &kv : fields) {
        if (kv.first == name) {
            if (out) *out = kv.second;
            return true;
        }
    }
    return false;
}

static bool same_fields(const std::vector<std::pair<std::string, Ty>> &a,
                        const std::vector<std::pair<std::string, Ty>> &b) {
    if (a.size() != b.size()) return false;
    for (const auto &kv : a) {
        Ty got;
        if (!field_of(b, kv.first, &got) || got != kv.second) return false;
    }
    return true;
}

bool Ty::operator==(const Ty &o) const {
    if (k != o.k) return false;
    switch (k) {
    case Cb: return arity == o.arity && text == o.text;
    case Ptr:
    case Slice:
    case Vec: return *inner == *o.inner;
    // Two structs are the same when their layouts are, and a declared name
    // makes the claim checkable even before the fields are in the table.
    case Struct: return (text.empty() || o.text.empty() || text == o.text) &&
                        same_fields(fields, o.fields);
    case Other: return text == o.text;
    default: return true;
    }
}

std::string type_name(const Ty &t) {
    switch (t.k) {
    case Ty::Cb: return t.text;
    case Ty::Ptr: return "ptr[" + type_name(*t.inner) + "]";
    case Ty::Slice: return "slice[" + type_name(*t.inner) + "]";
    case Ty::Vec: return t.inner->k == Ty::Dyn ? "vec" : "vec[" + type_name(*t.inner) + "]";
    case Ty::Struct: return "struct";
    case Ty::Other: return t.text;
    case Ty::Int: return "i64";
    case Ty::Float: return "f64";
    case Ty::Str: return "str";
    case Ty::Bool: return "bool";
    case Ty::None: return "none";
    case Ty::Tag: return "tag";
    case Ty::Map: return "map";
    case Ty::Dyn: break;
    }
    return "?";
}

bool numeric(const Ty &t) { return t.k == Ty::Int || t.k == Ty::Float || t.k == Ty::Bool; }

static bool is_tuple(const Ty &t) {
    return t.k == Ty::Cb || t.k == Ty::Ptr || t.k == Ty::Slice || t.k == Ty::Vec ||
           t.k == Ty::Struct;
}

static bool scalar(const Ty &t) {
    return t.k == Ty::Int || t.k == Ty::Float || t.k == Ty::Bool || t.k == Ty::None;
}

bool type_ok(const Ty &want, const Ty &got) {
    if (want.k == Ty::Dyn) return true;
    if (got.k == Ty::Dyn || got.k == Ty::Tag || got.k == Ty::None) return true;
    if (want == got) return true;
    if (want.k == Ty::Float && got.k == Ty::Int) return true;
    if (want.k == Ty::Cb && got.k == Ty::Str) return true;
    if (is_tuple(want) && is_tuple(got)) {
        if (want.k == Ty::Struct || got.k == Ty::Struct) {
            return want.k == Ty::Struct && got.k == Ty::Struct;
        }
        if (want.k == got.k) {
            return want.inner->k == Ty::Dyn || got.inner->k == Ty::Dyn ||
                   *want.inner == *got.inner;
        }
        return false;
    }
    if (want.k == Ty::Ptr && got.k == Ty::Int) return true;
    if (want.k == Ty::Int && got.k == Ty::Ptr) return true;
    return false;
}

// The declared `extern struct` fields: `ptr[T]` is dereferenced through this
// table, so a field T never declared is a type error, not a run-time guess.
static std::map<std::string, std::map<std::string, Ty>> *g_structs = nullptr;

static Ty word_type(const std::string &w) {
    if (w == "i64" || w == "i32" || w == "u32" || w == "u64" || w == "usize" || w == "u8") {
        return Ty::of(Ty::Int);
    }
    if (w == "f64" || w == "f32") return Ty::of(Ty::Float);
    if (w == "str") return Ty::of(Ty::Str);
    if (w == "bool") return Ty::of(Ty::Bool);
    if (w == "ptr") return Ty::other("raw");
    if (g_structs && g_structs->count(w)) {
        Ty t;
        t.k = Ty::Struct;
        t.text = w;
        for (const auto &kv : (*g_structs)[w]) t.fields.push_back(kv);
        return t;
    }
    // A name this pass does not know is kept as written, not erased: two
    // different `ptr[T]` must not compare equal by accident.
    if (!w.empty()) return Ty::other(w);
    return Ty::dyn();
}

static int cb_arity(const std::string &ann) {
    auto i = ann.find('(');
    auto j = ann.rfind(')');
    if (i == std::string::npos || j == std::string::npos || j < i) return 0;
    int n = 0;
    std::string inner = ann.substr(i + 1, j - i - 1);
    size_t start = 0;
    while (start <= inner.size()) {
        auto comma = inner.find(',', start);
        std::string part = inner.substr(start, comma == std::string::npos ? std::string::npos
                                                                         : comma - start);
        if (!part.empty() && part.find_first_not_of(" \t") != std::string::npos) n++;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return n;
}

static std::vector<std::string> split_type_list(const std::string &text) {
        std::vector<std::string> parts;
        size_t start = 0;
        int depth = 0;
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '(' || text[i] == '[') {
                depth++;
            } else if (text[i] == ')' || text[i] == ']') {
                depth--;
            } else if (text[i] == ',' && depth == 0) {
                std::string part = text.substr(start, i - start);
                auto first = part.find_first_not_of(" \t\r\n");
                auto last = part.find_last_not_of(" \t\r\n");
                if (first != std::string::npos) parts.push_back(part.substr(first, last - first + 1));
                start = i + 1;
            }
        }
        std::string part = text.substr(start);
        auto first = part.find_first_not_of(" \t\r\n");
        auto last = part.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) parts.push_back(part.substr(first, last - first + 1));
        return parts;
    }

std::string normalize_abi_type(const std::optional<std::string> &text) {
        if (!text) return "none";
        std::string out;
        for (unsigned char ch : *text) {
            if (!std::isspace(ch)) out.push_back(static_cast<char>(ch));
        }
        return out;
    }

std::optional<std::pair<std::vector<std::string>, std::string>>
callback_signature(const std::string &text) {
        if (text.rfind("fn", 0) != 0) return std::nullopt;
        size_t start = text.find('(');
        if (start == std::string::npos) return std::nullopt;
        int depth = 0;
        size_t end = std::string::npos;
        for (size_t i = start; i < text.size(); i++) {
            if (text[i] == '(') depth++;
            else if (text[i] == ')' && --depth == 0) {
                end = i;
                break;
            }
        }
        if (end == std::string::npos) return std::nullopt;
        std::string suffix = text.substr(end + 1);
        auto first = suffix.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::make_pair(split_type_list(text.substr(start + 1, end - start - 1)),
                                                               std::string("none"));
        suffix = suffix.substr(first);
        if (suffix.rfind("->", 0) != 0) return std::nullopt;
        suffix.erase(0, 2);
        auto ret_first = suffix.find_first_not_of(" \t\r\n");
        if (ret_first == std::string::npos) return std::nullopt;
        auto ret_last = suffix.find_last_not_of(" \t\r\n");
        return std::make_pair(split_type_list(text.substr(start + 1, end - start - 1)),
                              suffix.substr(ret_first, ret_last - ret_first + 1));
}

static Ty ann_type(const std::string &a) {
    if (a.rfind("fn", 0) == 0) return Ty::cb(cb_arity(a), a);
    if (a == "i32" || a == "i64" || a == "u64") return Ty::of(Ty::Int);
    if (a == "f64" || a == "f32") return Ty::of(Ty::Float);
    if (a == "bool") return Ty::of(Ty::Bool);
    if (a == "str") return Ty::of(Ty::Str);
    if (a.rfind("ptr[", 0) == 0 && a.back() == ']') {
        return Ty::wrap(Ty::Ptr, word_type(a.substr(4, a.size() - 5)));
    }
    if (a == "ptr") return Ty::of(Ty::Int);
    if (a.rfind("slice[", 0) == 0 && a.back() == ']') {
        return Ty::wrap(Ty::Slice, word_type(a.substr(6, a.size() - 7)));
    }
    if (a == "slice") return Ty::wrap(Ty::Slice, Ty::dyn());
    if (a.rfind("vec[", 0) == 0) {
        std::string inner = a.substr(4);
        while (!inner.empty() && inner.back() == ']') inner.pop_back();
        return Ty::wrap(Ty::Vec, inner.empty() ? Ty::dyn() : ann_type(inner));
    }
    if (a == "vec") return Ty::wrap(Ty::Vec, Ty::dyn());
    if (a == "map" || a.rfind("map[", 0) == 0) return Ty::of(Ty::Map);
    return Ty::dyn();
}

static Ty ann_type(const std::optional<std::string> &a) {
    return a ? ann_type(*a) : Ty::dyn();
}


static void mentions(const Expr &e, std::set<std::string> &acc);

static void mentions_str(const std::string &s, std::set<std::string> &acc) {
    for (size_t i = 0; i + 1 < s.size(); i++) {
        if (s[i] != '{') continue;
        size_t j = i + 1;
        if (j >= s.size() || !(std::isalpha(static_cast<unsigned char>(s[j])) || s[j] == '_')) {
            continue;
        }
        while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) j++;
        acc.insert(s.substr(i + 1, j - i - 1));
    }
}

static void mentions(const std::vector<StmtP> &v, std::set<std::string> &acc);

static void mentions(const Stmt &s, std::set<std::string> &acc) {
    if (s.e1) mentions(*s.e1, acc);
    if (s.e2) mentions(*s.e2, acc);
    if (s.e3) mentions(*s.e3, acc);
    mentions(s.body, acc);
    mentions(s.body2, acc);
    if (s.els) mentions(*s.els, acc);
}

static void mentions(const std::vector<StmtP> &v, std::set<std::string> &acc) {
    for (const auto &s : v) mentions(*s, acc);
}

static void mentions(const Expr &e, std::set<std::string> &acc) {
    if (e.k == EK::Call && e.a == "own") return;   // `own` reads nothing: it copies
    if (e.k == EK::Var) acc.insert(e.a);
    if (e.k == EK::Str || e.k == EK::RStr) mentions_str(e.a, acc);
    // A closure keeps its body in `fn`, not in `kids`, so walking `kids` alone
    // made a captured name invisible to every rule that asks what an expression
    // reads. The arena rule "a closure inside the block captures a value made
    // in it" therefore never fired here while it fired in the Python checker:
    // `brat check` refused the program, `brat run` built and ran it, and the
    // escaped value pointed into an arena that had already been reset.
    if (e.k == EK::Closure && e.fn) mentions(e.fn->body, acc);
    for (const auto &k : e.kids) mentions(*k, acc);
}

static void first_var(const Expr &e, std::string &out, bool &found) {
    if (found) return;
    if (e.k == EK::Var) {
        out = e.a;
        found = true;
        return;
    }
    for (const auto &k : e.kids) {
        first_var(*k, out, found);
        if (found) return;
    }
}

static std::string first_var(const Expr &e) {
    std::string out;
    bool found = false;
    first_var(e, out, found);
    return out;
}

static bool has_node(const Expr &e, EK kind) {
    if (e.k == kind) return true;
    for (const auto &k : e.kids) {
        if (has_node(*k, kind)) return true;
    }
    return false;
}

static bool has_node(const Stmt &s, EK kind) {
    if (s.e1 && has_node(*s.e1, kind)) return true;
    if (s.e2 && has_node(*s.e2, kind)) return true;
    if (s.e3 && has_node(*s.e3, kind)) return true;
    for (const auto &b : s.body) {
        if (has_node(*b, kind)) return true;
    }
    for (const auto &b : s.body2) {
        if (has_node(*b, kind)) return true;
    }
    if (s.els && has_node(*s.els, kind)) return true;
    return false;
}

// `if true { __arena_enter(); defer __arena_leave(); ... }` from parse_arena.
static bool is_arena_block(const Stmt &s) {
    if (s.k != SK::If || !s.e1 || s.e1->k != EK::Bool || !s.e1->flag) return false;
    if (s.body.empty()) return false;
    const Stmt &b = *s.body[0];
    return b.k == SK::Expr && b.e1 && b.e1->k == EK::Call && b.e1->a == "__arena_enter" &&
           b.e1->kids.empty();
}


static Ty fn_ret(const Stmt &s) {
    if (s.k == SK::Extern) return ann_type(s.ext.ret);
    if (!s.fns.empty()) return ann_type(s.fns[0].ret);
    return Ty::dyn();
}

static const Stmt &unpub(const Stmt &s) {
    if (s.k == SK::Pub && !s.body.empty()) return *s.body[0];
    return s;
}

static bool is_fn_like(const Stmt &s) {
    return s.k == SK::Fn || s.k == SK::Extern || s.k == SK::ExportFn;
}

void collect_sigs(const std::vector<StmtP> &prog, Sigs &sigs) {
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if (!is_fn_like(node)) continue;
        FnSig sig;
        const std::vector<Param> *params;
        std::optional<std::string> varargs;
        if (node.k == SK::Extern) {
            params = &node.ext.params;
            varargs = node.ext.varargs;
            sig.ret = ann_type(node.ext.ret);
        } else {
            params = &node.fns[0].params;
            sig.ret = ann_type(node.fns[0].ret);
        }
        for (const auto &p : *params) {
            sig.params.push_back(ann_type(p.ann));
            sig.param_anns.push_back(p.ann);
            if (p.def) sig.ndef++;
        }
        sig.ret_ann = node.k == SK::Extern ? node.ext.ret : node.fns[0].ret;
        if (varargs) {
            sig.varargs = ann_type(*varargs);
            sig.varargs_ann = *varargs;
        }
        const bool is_ext = node.k == SK::Extern;
        sig.extern_fn = is_ext;
        sigs[is_ext ? node.ext.name : node.fns[0].name.value_or("")] = std::move(sig);
    }
}


struct MethDef {
    const char *name;
    std::vector<const char *> args;
    const char *ret;
    bool flex;
};

static const std::vector<MethDef> STR_METH = {
    {"split", {"str"}, "vec:str", true},
    {"lines", {}, "vec:str", false},
    {"trim", {}, "str", false},
    {"lower", {}, "str", false},
    {"upper", {}, "str", false},
    {"starts_with", {"str"}, "bool", false},
    {"ends_with", {"str"}, "bool", false},
    {"contains", {"str"}, "bool", false},
    {"parse_int", {}, "tag", false},
    {"replace", {"str", "str"}, "str", false},
    {"index_of", {"str"}, "dyn", false},
    {"repeat", {"int"}, "str", false},
    {"split_once", {"str"}, "dyn", false},
    {"copy", {}, "str", false},
    {"chars", {}, "vec:str", false},
    {"parse_float", {}, "tag", false},
    {"slice", {"int", "int"}, "str", false},
};

static const std::vector<MethDef> VEC_METH = {
    {"join", {"str"}, "str", false},
    {"push", {"$E"}, "int", false},
    {"pop", {}, "dyn", false},
    {"first", {}, "dyn", false},
    {"last", {}, "dyn", false},
    {"sort", {}, "$S", false},
    {"rev", {}, "$S", false},
    {"ins", {"int", "$E"}, "int", false},
    {"del", {"int"}, "dyn", false},
    {"slice", {"int", "int"}, "$S", false},
    {"sum", {}, "$SUM", false},
    {"min", {}, "dyn", false},
    {"max", {}, "dyn", false},
    {"enumerate", {}, "vec:dyn", false},
    {"copy", {}, "$S", false},
    {"zip", {"dyn"}, "vec:dyn", false},
    {"chunks", {"int"}, "$S", false},
    {"pos", {"dyn"}, "dyn", false},
    {"any", {"dyn"}, "bool", false},
    {"all", {"dyn"}, "bool", false},
    {"find", {"dyn"}, "dyn", false},
    {"fold", {"dyn", "dyn"}, "dyn", false},
    {"sort_by", {"dyn"}, "$S", false},
};

static const std::vector<MethDef> MAP_METH = {
    {"get", {"dyn"}, "dyn", false},
    {"has", {"dyn"}, "bool", false},
    {"keys", {}, "vec:str", false},
    {"del", {"dyn"}, "bool", false},
    {"set", {"dyn", "dyn"}, "dyn", false},
    {"items", {}, "vec:dyn", false},
    {"vals", {}, "vec:dyn", false},
    {"copy", {}, "map", false},
    {"get_or", {"dyn", "dyn"}, "dyn", false},
};

static const std::map<std::string, Ty> BRES = {
    {"len", Ty::of(Ty::Int)}, {"str", Ty::of(Ty::Str)}, {"is_err", Ty::of(Ty::Bool)},
    {"is_none", Ty::of(Ty::Bool)}, {"tagof", Ty::of(Ty::Str)},
    {"read_file", Ty::of(Ty::Tag)}, {"write_file", Ty::of(Ty::Tag)},
    {"is_vec", Ty::of(Ty::Bool)}, {"is_str", Ty::of(Ty::Bool)},
    {"is_map", Ty::of(Ty::Bool)}, {"join_all", Ty::of(Ty::Int)},
    {"Ok", Ty::of(Ty::Tag)}, {"Err", Ty::of(Ty::Tag)}, {"Some", Ty::of(Ty::Tag)},
    {"args", Ty::wrap(Ty::Vec, Ty::of(Ty::Str))}, {"exit", Ty::dyn()},
    {"eprint", Ty::dyn()}, {"raise", Ty::dyn()}, {"time_ms", Ty::of(Ty::Int)},
    {"sleep_ms", Ty::dyn()}, {"env_get", Ty::dyn()},
    {"__arena_enter", Ty::of(Ty::Int)}, {"__arena_leave", Ty::of(Ty::Int)},
    {"re_match", Ty::of(Ty::Bool)},
    {"tcp_listen", Ty::of(Ty::Tag)}, {"tcp_connect", Ty::of(Ty::Tag)},
    {"tcp_accept", Ty::of(Ty::Tag)}, {"tcp_send", Ty::of(Ty::Tag)},
    {"tcp_recv", Ty::of(Ty::Tag)}, {"tcp_close", Ty::of(Ty::Tag)},
    {"sbuf", Ty::of(Ty::Int)}, {"sbuf_add", Ty::of(Ty::Int)},
    {"sbuf_len", Ty::of(Ty::Int)}, {"sbuf_str", Ty::of(Ty::Str)},
    {"chan", Ty::of(Ty::Int)}, {"ptr_null", Ty::of(Ty::Int)},
    {"ptr_is_null", Ty::of(Ty::Bool)}, {"ptr_read_i64", Ty::of(Ty::Int)},
    {"ptr_write_i64", Ty::of(Ty::Int)}, {"ptr_read_f64", Ty::of(Ty::Float)},
    {"ptr_write_f64", Ty::of(Ty::Int)},
    {"ptr_read_f32", Ty::of(Ty::Float)}, {"ptr_write_f32", Ty::of(Ty::Int)},
    {"ptr_read_struct", Ty::dyn()}, {"ptr_write_struct", Ty::of(Ty::Int)},
    {"ptr_read_ptr", Ty::of(Ty::Int)},
    {"ptr_offset", Ty::of(Ty::Int)}, {"ptr_read_str", Ty::of(Ty::Str)},
    {"ptr_write_ptr", Ty::of(Ty::Int)}, {"ptr_of_str", Ty::of(Ty::Int)},
    {"ptr_write_str", Ty::of(Ty::Int)}, {"errno", Ty::of(Ty::Int)},
    {"strerror", Ty::of(Ty::Str)},
};

static Ty from_code(const char *c) {
    std::string s = c;
    if (s == "int") return Ty::of(Ty::Int);
    if (s == "float") return Ty::of(Ty::Float);
    if (s == "str") return Ty::of(Ty::Str);
    if (s == "bool") return Ty::of(Ty::Bool);
    if (s == "tag") return Ty::of(Ty::Tag);
    if (s == "none") return Ty::of(Ty::None);
    if (s == "map") return Ty::of(Ty::Map);
    if (s == "vec:str") return Ty::wrap(Ty::Vec, Ty::of(Ty::Str));
    if (s == "vec:dyn") return Ty::wrap(Ty::Vec, Ty::dyn());
    return Ty::dyn();
}


struct FnTyper {
    const Sigs &sigs;
    Ty ret;
    std::vector<TypeErr> &errs;
    int line = 0;
    std::map<std::string, Ty> env;
    std::vector<std::set<std::string>> tainted;
    int region = 0;
    std::map<std::string, int> regions;
    std::vector<int> region_stack{0};
    // `&x` is the out-parameter of a C function: legitimate in an extern's
    // `ptr[T]` slot, refused everywhere else.
    bool amp_ok = false;

    FnTyper(const Sigs &sigs, Ty ret, std::vector<TypeErr> &errs)
        : sigs(sigs), ret(std::move(ret)), errs(errs) {}

    void note(const std::string &msg) { errs.push_back(TypeErr{line, msg}); }

    int current_region() const { return region_stack.back(); }

    bool inner(const std::string &name) const {
        auto it = regions.find(name);
        int r = it == regions.end() ? 0 : it->second;
        return current_region() != 0 && r != 0 && r == current_region();
    }

    void declare(const std::string &name) {
        if (!name.empty()) regions[name] = current_region();
    }

    void taint(const std::string &name) {
        if (current_region() != 0 && !name.empty() &&
            (regions.count(name) ? regions[name] : 0) != current_region()) {
            if (tainted.empty()) tainted.emplace_back();
            tainted.back().insert(name);
        }
    }

    bool is_tainted(const std::string &name) const {
        if (name.empty()) return false;
        for (const auto &s : tainted) {
            if (s.count(name)) return true;
        }
        return false;
    }

    std::vector<std::string> non_scalar_inner(const std::set<std::string> &names) const {
        std::vector<std::string> out;
        for (const auto &n : names) {
            auto r = regions.find(n);
            int reg = r == regions.end() ? 0 : r->second;
            auto e = env.find(n);
            Ty t = e == env.end() ? Ty::dyn() : e->second;
            if (reg != 0 && !scalar(t)) out.push_back(n);
        }
        return out;
    }

    Ty env_get(const std::string &n) const {
        auto it = env.find(n);
        return it == env.end() ? Ty::dyn() : it->second;
    }

    static bool is_num_op(const std::string &op) {
        static const std::set<std::string> ops = {"-", "*", "/", "%", "==", "!=",
                                                 "<", ">", "<=", ">=", "&&", "||"};
        return ops.count(op) > 0;
    }

    bool escapes(const Expr &e);

    void arena_note(std::string msg, bool ownable = true) {
        msg += ownable
                   ? "  (hint: hand out a heap copy with `own(...)`, e.g. `return Err(own(msg))`)"
                   : "  (hint: keep the value inside the block, or copy it with `own(...)` first)";
        note("arena: " + msg);
    }

    void closure_body(const Expr &e) {
        std::map<std::string, Ty> saved = env;
        Ty saved_ret = ret;
        for (const auto &p : e.fn->params) env[p.name] = ann_type(p.ann);
        // A closure has no annotated return: `fn(x) { return x > 1 }` is a bool
        // predicate, and typing it against the enclosing `ret` rejected it.
        ret = Ty::dyn();
        block(e.fn->body);
        ret = saved_ret;
        env = saved;
    }

    // A `fn` declared inside a body: its own parameters and return type.
    void nested_fn(const Stmt &s) {
        if (s.fns.empty()) return;
        const FnDecl &f = s.fns[0];
        std::map<std::string, Ty> saved = env;
        Ty saved_ret = ret;
        for (const auto &p : f.params) env[p.name] = ann_type(p.ann);
        ret = ann_type(f.ret);
        block(f.body);
        ret = saved_ret;
        env = saved;
    }

    std::string field_names(const Ty &t) {
        if (t.k != Ty::Struct) return "";
        std::string out;
        for (const auto &kv : t.fields) {
            if (!out.empty()) out += ", ";
            out += kv.first;
        }
        return out;
    }

    std::string struct_field_err(const std::string &f, const Ty &bt) {
        std::string names = field_names(bt);
        if (!names.empty()) return "no field `" + f + "` on struct (has " + names + ")";
        return "no field `" + f + "` on struct";
    }

    Ty expr(const Expr &e);
    Ty meth(const Ty &ot, const std::string &name, const std::vector<ExprP> &args,
            size_t start);
    Ty plugin_call(const std::string &name, const std::vector<ExprP> &args, size_t start);
    Ty slice_call(const std::string &name, const std::vector<ExprP> &args, size_t start);
    void check_cb(const Ty &pt, const Expr &arg, const std::string &name, int pos);

    void block(const std::vector<StmtP> &body) {
        for (const auto &s : body) stmt(*s);
    }

    void stmt(const Stmt &s);
};

Ty FnTyper::expr(const Expr &e) {
    switch (e.k) {
    case EK::Num: return e.is_float ? Ty::of(Ty::Float) : Ty::of(Ty::Int);
    case EK::Bool: return Ty::of(Ty::Bool);
    case EK::Str:
    case EK::RStr: return Ty::of(Ty::Str);
    case EK::None: return Ty::of(Ty::None);
    case EK::VecLit: {
        std::vector<Ty> ts;
        for (const auto &k : e.kids) ts.push_back(expr(*k));
        Ty t = Ty::dyn();
        if (!ts.empty()) {
            t = ts[0];
            for (const auto &x : ts) {
                if (!(x == ts[0])) t = Ty::dyn();
            }
        }
        return Ty::wrap(Ty::Vec, t);
    }
    case EK::MapLit:
        for (const auto &k : e.kids) expr(*k);
        return Ty::of(Ty::Map);
    case EK::StructLit: {
        std::vector<std::pair<std::string, Ty>> fields;
        for (size_t i = 0; i < e.kids.size(); i++) {
            Ty t = expr(*e.kids[i]);
            const std::string &name = e.names[i];
            bool replaced = false;
            for (auto &kv : fields) {
                if (kv.first == name) {
                    kv.second = t;
                    replaced = true;
                }
            }
            if (!replaced) fields.emplace_back(name, t);
        }
        return Ty::strct(std::move(fields));
    }
    case EK::Var: return env_get(e.a);
    case EK::Un: {
        Ty t = expr(*e.kids[0]);
        if (e.a == "&") {
            if (e.kids[0]->k != EK::Var) note("`&` takes a binding: `let mut p = &x`");
            else if (!amp_ok)
                note("`&` is an out-parameter: write it where the extern takes a "
                     "`ptr[T]`, as in `fill(&n, 21)`");
            return Ty::wrap(Ty::Ptr, Ty::dyn());
        }
        if (e.a == "!") return Ty::of(Ty::Bool);
        return numeric(t) ? t : Ty::dyn();
    }
    case EK::Move: return expr(*e.kids[0]);
    case EK::Closure:
        closure_body(e);
        return Ty::dyn();
    case EK::ParFor: {
        expr(*e.kids[0]);
        Ty unit = expr(*e.kids[1]);
        closure_body(e);      // the body is a closure of one parameter
        return unit;          // the fold returns the type of its unit
    }
    case EK::Bin: {
        const std::string &op = e.a;
        Ty a = expr(*e.kids[0]);
        Ty b = expr(*e.kids[1]);
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=" ||
            op == "in") {
            return Ty::of(Ty::Bool);
        }
        if (op == "&&" || op == "||") return a == b ? a : Ty::dyn();
        if (op == "+") {
            if (a.k == Ty::Str || b.k == Ty::Str) return Ty::of(Ty::Str);
            if (is_tuple(a) && is_tuple(b)) return Ty::wrap(Ty::Vec, Ty::dyn());
            if (numeric(a) && numeric(b)) {
                return Ty::of((a.k == Ty::Float || b.k == Ty::Float) ? Ty::Float : Ty::Int);
            }
            return Ty::dyn();
        }
        if (numeric(a) && numeric(b)) {
            if (op == "*" && (a.k == Ty::Str || b.k == Ty::Str)) return Ty::of(Ty::Str);
            return Ty::of((a.k == Ty::Float || b.k == Ty::Float) ? Ty::Float : Ty::Int);
        }
        if (op == "*" && (a.k == Ty::Str || b.k == Ty::Str) &&
            (a.k == Ty::Int || b.k == Ty::Int)) {
            return Ty::of(Ty::Str);
        }
        return Ty::dyn();
    }
    case EK::Or: {
        Ty a = expr(*e.kids[0]);
        Ty b = expr(*e.kids[1]);
        return a == b ? a : Ty::dyn();
    }
    case EK::Quest: return expr(*e.kids[0]);
    case EK::MCall: {
        Ty ot = expr(*e.kids[0]);
        for (size_t i = 1; i < e.kids.size(); i++) expr(*e.kids[i]);
        return meth(ot, e.b, e.kids, 1);
    }
    case EK::Field: {
        Ty bt = expr(*e.kids[0]);
        if (bt.k == Ty::Struct) {
            Ty ft;
            if (!bt.struct_has(e.b, &ft)) {
                note(struct_field_err(e.b, bt));
                return Ty::dyn();
            }
            return ft;
        }
        return Ty::dyn();
    }
    case EK::Idx: {
        Ty b = expr(*e.kids[0]);
        expr(*e.kids[1]);
        if (b.k == Ty::Ptr) {
            // `p[i]` where `p: ptr[T]` is the element, not the address: the
            // declaration says how wide the step is and what the layout is.
            Ty elem = *b.inner;
            if (elem.k == Ty::Struct) return elem;
            if (elem.k == Ty::Int) return Ty::of(Ty::Int);
            if (elem.k == Ty::Float) return Ty::of(Ty::Float);
            note("index a `ptr` needs a unit: declare it `ptr[T]` "
                 "with T an extern struct, i32, i64, f32, f64 or u8");
            return Ty::dyn();
        }
        if (b.k == Ty::Vec) return *b.inner;
        if (b.k == Ty::Str) return Ty::of(Ty::Str);
        // A plain `ptr` types as an integer, so indexing one is the same
        // refusal: the element type is what says how wide the step is.
        if (b.k == Ty::Int) {
            note("index a `ptr` needs a unit: declare it `ptr[T]` "
                 "with T an extern struct, i32, i64, f32, f64 or u8");
        }
        return Ty::dyn();
    }
    case EK::Call: {
        const std::string &name = e.a;
        // A known signature types its arguments in the loop below, one slot at a
        // time -- that is where `&x` learns it is a `ptr[T]` argument. Walking
        // them here first would check each one twice, the first time blind.
        if (sigs.find(name) == sigs.end()) {
            for (const auto &k : e.kids) expr(*k);
        }
        if (name == "own") {
            if (e.kids.size() != 1) {
                note("`own` wants 1 arg, got " + std::to_string(e.kids.size()));
                return Ty::dyn();
            }
            return expr(*e.kids[0]);
        }
        auto bres = BRES.find(name);
        if (bres != BRES.end()) return bres->second;
        if (name == "slice_of" || name == "slice_of_ptr" || name == "slice_len" ||
            name == "slice_get" || name == "slice_set" || name == "str_from_ptr" ||
            name == "slice_of_struct" || name == "slice_struct_of") {
            return slice_call(name, e.kids, 0);
        }
        if (name == "lib_open" || name == "lib_sym" || name == "lib_close" || name == "call") {
            return plugin_call(name, e.kids, 0);
        }
        auto sig = sigs.find(name);
        if (sig != sigs.end()) {
            const FnSig &f = sig->second;
            const auto &args = e.kids;
            if (!f.varargs) {
                if (args.size() < f.params.size() - f.ndef || args.size() > f.params.size()) {
                    note("`" + name + "` wants " + std::to_string(f.params.size()) + " args, got " +
                         std::to_string(args.size()));
                }
            } else {
                if (args.size() < f.params.size() - f.ndef) {
                    note("`" + name + "` wants at least " +
                         std::to_string(f.params.size() - f.ndef) + " args, got " +
                         std::to_string(args.size()));
                }
            }
            for (size_t i = 0; i < f.params.size() && i < args.size(); i++) {
                const Ty &pt = f.params[i];
                if (pt.k == Ty::Cb) {
                    check_cb(pt, *args[i], name, static_cast<int>(i) + 1);
                    continue;
                }
                // `&x` is the out-parameter of an extern calling nothing into
                // BRAT: it is only legitimate in a `ptr[T]` slot, and killing
                // the flag right after is the whole check.
                bool prev = amp_ok;
                amp_ok = f.extern_fn && !f.param_anns.empty() && i < f.param_anns.size()
                         && f.param_anns[i] && f.param_anns[i]->rfind("ptr", 0) == 0;
                Ty gt = expr(*args[i]);
                amp_ok = prev;
                if (!type_ok(pt, gt)) {
                    note("arg " + std::to_string(i + 1) + " of `" + name + "`: want " +
                         type_name(pt) + ", got " + type_name(gt));
                }
            }
            if (f.varargs) {
                for (size_t i = f.params.size(); i < args.size(); i++) {
                    Ty gt = expr(*args[i]);
                    // A `str` fills a variadic `..ptr` slot: that is how `%s`
                    // reads it, as `const char *`.
                    if (f.varargs_ann == "ptr" && gt.k == Ty::Str) continue;
                    if (!type_ok(*f.varargs, gt)) {
                        note("arg " + std::to_string(i + 1) + " of `" + name + "`: want " +
                             type_name(*f.varargs) + ", got " + type_name(gt));
                    }
                }
            }
            return f.ret;
        }
        return Ty::dyn();
    }
    }
    return Ty::dyn();
}

bool FnTyper::escapes(const Expr &e) {
    if (current_region() == 0) return false;
    std::set<std::string> ms;
    mentions(e, ms);
    bool any_inner = !non_scalar_inner(ms).empty();
    bool any_tainted = false;
    for (const auto &n : ms) {
        if (is_tainted(n)) any_tainted = true;
    }
    if (!any_inner && !any_tainted) return false;
    if (e.k == EK::Bin) {
        if (is_num_op(e.a)) return false;
        if (e.a == "+") {
            for (size_t i = 0; i < 2 && i < e.kids.size(); i++) {
                Ty t = expr(*e.kids[i]);
                if (t.k == Ty::Int || t.k == Ty::Float) return false;
            }
        }
    }
    if (e.k == EK::Un) return false;
    return !scalar(expr(e));
}

Ty FnTyper::meth(const Ty &ot, const std::string &name, const std::vector<ExprP> &args,
                 size_t start) {
    const std::vector<MethDef> *tab = nullptr;
    if (ot.k == Ty::Str) {
        tab = &STR_METH;
    } else if (ot.k == Ty::Vec) {
        tab = &VEC_METH;
    } else if (ot.k == Ty::Map) {
        tab = &MAP_METH;
    }
    if (!tab) return Ty::dyn();
    if (name == "map" || name == "filter") return Ty::wrap(Ty::Vec, Ty::dyn());
    const MethDef *sig = nullptr;
    for (const auto &m : *tab) {
        if (name == m.name) sig = &m;
    }
    if (!sig) {
        note("no method `" + name + "` for " + type_name(ot));
        return Ty::dyn();
    }
    const size_t n = args.size() - start;
    const size_t want = sig->args.size();
    if (n != want && !(sig->flex && n == want - 1)) {
        note("method `" + name + "` wants " + std::to_string(want) + " args");
        return Ty::dyn();
    }
    for (size_t i = 0; i < want && i < n; i++) {
        std::string code = sig->args[i];
        Ty pt = code == "$E" ? *ot.inner : from_code(sig->args[i]);
        Ty gt = expr(*args[start + i]);
        if (!type_ok(pt, gt)) {
            note("method `" + name + "` arg " + std::to_string(i + 1) + ": want " +
                 type_name(pt) + ", got " + type_name(gt));
        }
    }
    std::string rtype = sig->ret;
    if (rtype == "$SUM") {
        if (ot.k == Ty::Vec && numeric(*ot.inner)) return *ot.inner;
        return Ty::dyn();
    }
    if (rtype == "$S") return ot;
    return from_code(sig->ret);
}

Ty FnTyper::plugin_call(const std::string &name, const std::vector<ExprP> &kids,
                       size_t start) {
    const size_t n = kids.size() - start;
    auto at = [&](size_t i) -> const Expr & { return *kids[start + i]; };
    if (name == "lib_open") {
        if (n != 1) {
            note("`lib_open` wants 1 arg, got " + std::to_string(n));
        } else if (!type_ok(Ty::of(Ty::Str), expr(at(0)))) {
            note("lib_open: want str, got " + type_name(expr(at(0))));
        }
        return Ty::of(Ty::Int);
    }
    if (name == "lib_sym") {
        if (n != 2) {
            note("`lib_sym` wants 2 args, got " + std::to_string(n));
            return Ty::of(Ty::Int);
        }
        if (!type_ok(Ty::of(Ty::Int), expr(at(0)))) {
            note("lib_sym: handle wants ptr, got " + type_name(expr(at(0))));
        }
        if (!type_ok(Ty::of(Ty::Str), expr(at(1)))) {
            note("lib_sym: name wants str, got " + type_name(expr(at(1))));
        }
        return Ty::of(Ty::Int);
    }
    if (name == "lib_close") {
        if (n != 1) {
            note("`lib_close` wants 1 arg, got " + std::to_string(n));
        } else if (!type_ok(Ty::of(Ty::Int), expr(at(0)))) {
            note("lib_close: want ptr, got " + type_name(expr(at(0))));
        }
        return Ty::of(Ty::Int);
    }
    static const std::map<std::string, std::string> SIG_RET = {
        {"i64", "int"}, {"f32", "float"}, {"f64", "float"}, {"str", "str"}, {"ptr", "int"}, {"none", "dyn"}};
    static const std::map<std::string, std::string> SIG_WANT = {
        {"i64", "int"}, {"f32", "float"}, {"f64", "float"}, {"str", "str"}, {"ptr", "int"}};
    if (n < 2) {
        note("`call` wants a function, a signature and the arguments");
        return Ty::dyn();
    }
    const Expr &sig = at(1);
    if (sig.k != EK::Str) {
        note("call: signature must be a string literal");
        return Ty::dyn();
    }
    std::string text = sig.a;
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    if (text.find('(') == std::string::npos || text.empty() || text.back() != ')') {
        note("call: bad signature `" + text + "`");
        return Ty::dyn();
    }
    std::string ret_t = text.substr(0, text.find('('));
    std::string rest = text.substr(text.find('(') + 1);
    rest.pop_back();
    std::vector<std::string> params;
    size_t pos = 0;
    while (pos <= rest.size()) {
        auto comma = rest.find(',', pos);
        std::string p = rest.substr(pos, comma == std::string::npos ? std::string::npos
                                                                   : comma - pos);
        if (!p.empty()) params.push_back(p);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (!SIG_RET.count(ret_t)) {
        note("call: unsupported return type `" + ret_t + "`");
        return Ty::dyn();
    }
    if (!type_ok(Ty::of(Ty::Int), expr(at(0)))) {
        note("call: want ptr, got " + type_name(expr(at(0))));
    }
    Ty ret_ty = from_code(SIG_RET.at(ret_t).c_str());
    if (params.size() != n - 2) {
        note("call: signature wants " + std::to_string(params.size()) + " argument(s), got " +
             std::to_string(n - 2));
        return ret_ty;
    }
    for (size_t i = 0; i < params.size(); i++) {
        if (!SIG_WANT.count(params[i])) {
            note("call: unsupported argument type `" + params[i] + "`");
            continue;
        }
        Ty got = expr(at(i + 2));
        if (!type_ok(from_code(SIG_WANT.at(params[i]).c_str()), got)) {
            note("call: arg " + std::to_string(i + 1) + " wants " + params[i] + ", got " +
                 type_name(got));
        }
    }
    return ret_ty;
}

Ty FnTyper::slice_call(const std::string &name, const std::vector<ExprP> &kids,
                      size_t start) {
    const size_t n = kids.size() - start;
    auto at = [&](size_t i) -> const Expr & { return *kids[start + i]; };
    auto elem = [&](const Expr &node) {
        Ty t = expr(node);
        if (t.k == Ty::Vec || t.k == Ty::Slice) return *t.inner;
        return Ty::dyn();
    };
    size_t want = 0;
    if (name == "slice_of" || name == "slice_of_ptr" || name == "slice_set" ||
        name == "slice_of_struct") {
        want = 3;
    } else if (name == "slice_struct_of") {
        want = 1;
    } else if (name == "slice_len") {
        want = 1;
    } else if (name == "slice_get" || name == "str_from_ptr") {
        want = 2;
    }
    if (n != want) {
        note("`" + name + "` wants " + std::to_string(want) + " args, got " +
             std::to_string(n));
        return Ty::dyn();
    }
    if (name == "str_from_ptr") {
        if (!type_ok(Ty::of(Ty::Int), expr(at(0)))) {
            note("str_from_ptr: want ptr, got " + type_name(expr(at(0))));
        }
        if (!type_ok(Ty::of(Ty::Int), expr(at(1)))) {
            note("str_from_ptr: n wants i64, got " + type_name(expr(at(1))));
        }
        return Ty::of(Ty::Str);
    }
    if (name == "slice_len") return Ty::of(Ty::Int);
    if (name == "slice_of") {
        Ty e = elem(at(0));
        for (size_t i = 1; i <= 2; i++) {
            Ty got = expr(at(i));
            if (!type_ok(Ty::of(Ty::Int), got)) {
                note("slice_of: bound " + std::to_string(i) + " wants i64, got " +
                     type_name(got));
            }
        }
        return Ty::wrap(Ty::Slice, e);
    }
    if (name == "slice_of_struct") {
        // slice_of_struct(p, n, "T"): a window over a table of records. The
        // layout must be declared, or reading an element would be a guess.
        const Expr &sn = at(2);
        if (!(sn.k == EK::Str && g_structs && g_structs->count(sn.a))) {
            note(sn.k != EK::Str ? "slice_of_struct wants a struct name as a string literal"
                                 : "slice_of_struct: " + sn.a + " is not an extern struct");
            return Ty::wrap(Ty::Slice, Ty::dyn());
        }
        Ty st;
        st.k = Ty::Struct;
        for (const auto &kv : (*g_structs)[sn.a]) st.fields.push_back(kv);
        Ty got = expr(at(0));
        if (!type_ok(Ty::wrap(Ty::Ptr, st), got)) {
            note("slice_of_struct: want ptr[" + sn.a + "], got " + type_name(got));
        }
        if (!type_ok(Ty::of(Ty::Int), expr(at(1)))) {
            note("slice_of_struct: n wants i64, got " + type_name(expr(at(1))));
        }
        return Ty::wrap(Ty::Slice, st);
    }
    if (name == "slice_struct_of") {
        // slice_struct_of([...]): a window over elements built in the program.
        // Its element type is whatever the literal holds.
        const Expr &lit = at(0);
        if (lit.k != EK::VecLit) {
            note("slice_struct_of wants a vector literal: a window that is written cannot start empty");
            return Ty::wrap(Ty::Slice, Ty::dyn());
        }
        Ty elem = Ty::dyn();
        for (const auto &kid : lit.kids) {
            elem = expr(*kid);
            break;
        }
        return Ty::wrap(Ty::Slice, elem);
    }
    if (name == "slice_of_ptr") {
        const Expr &kind = at(2);
        if (!(kind.k == EK::Str && (kind.a == "i64" || kind.a == "i32" || kind.a == "f64" ||
                                    kind.a == "f32" || kind.a == "u8"))) {
            note("slice_of_ptr: kind must be the literal \"i64\", \"i32\", \"f64\", "
                 "\"f32\" or \"u8\"");
            return Ty::wrap(Ty::Slice, Ty::dyn());
        }
        Ty want_t = Ty::wrap(Ty::Ptr, word_type(kind.a));
        Ty got = expr(at(0));
        if (!type_ok(want_t, got)) {
            note("slice_of_ptr: want ptr[" + kind.a + "], got " + type_name(got));
        }
        if (!type_ok(Ty::of(Ty::Int), expr(at(1)))) {
            note("slice_of_ptr: n wants i64, got " + type_name(expr(at(1))));
        }
        return Ty::wrap(Ty::Slice, word_type(kind.a));
    }
    Ty e = elem(at(0));
    if (!type_ok(Ty::of(Ty::Int), expr(at(1)))) {
        note(name + ": index wants i64, got " + type_name(expr(at(1))));
    }
    if (name == "slice_get") return e;
    Ty got = expr(at(2));
    if (!type_ok(e, got)) note("slice_set: want " + type_name(e) + ", got " + type_name(got));
    return Ty::of(Ty::Int);
}

void FnTyper::check_cb(const Ty &pt, const Expr &arg, const std::string &name, int pos) {
    if (arg.k != EK::Str) {
        note("arg " + std::to_string(pos) + " of `" + name + "`: callback must be a function name");
        return;
    }
    auto sig = sigs.find(arg.a);
    if (sig == sigs.end()) {
        note("unknown callback `" + arg.a + "`");
        return;
    }
    int got = static_cast<int>(sig->second.params.size());
    if (got != pt.arity) {
        note("callback `" + arg.a + "` takes " + std::to_string(got) + " parameter(s), " + pt.text +
             " wants " + std::to_string(pt.arity));
        return;
    }
    auto expected = callback_signature(pt.text);
    if (!expected || sig->second.param_anns.size() != sig->second.params.size()) {
        note("callback `" + arg.a + "` does not have a verifiable C ABI signature");
        return;
    }
    bool matches = expected->first.size() == sig->second.param_anns.size();
    for (size_t i = 0; matches && i < expected->first.size(); i++) {
        matches = normalize_abi_type(sig->second.param_anns[i]) ==
                  normalize_abi_type(expected->first[i]);
    }
    matches = matches && normalize_abi_type(sig->second.ret_ann) ==
                             normalize_abi_type(expected->second);
    if (!matches) {
        std::string actual = "fn(";
        for (size_t i = 0; i < sig->second.param_anns.size(); i++) {
            if (i) actual += ",";
            actual += normalize_abi_type(sig->second.param_anns[i]);
        }
        actual += ")->" + normalize_abi_type(sig->second.ret_ann);
        note("callback `" + arg.a + "` has signature " + actual +
             ", expected " + pt.text);
    }
}

void FnTyper::stmt(const Stmt &s) {
    if (s.line) line = s.line;
    switch (s.k) {
    case SK::Unsafe: block(s.body); break;
    case SK::Scope: break;   // the reference has no case for `scope`: its body stays unchecked
    case SK::Block: block(s.body); break;
    case SK::Let:
    case SK::LetTmp: {
        Ty want = ann_type(s.ann);
        Ty got = expr(*s.e1);
        if (s.k == SK::Let && want.k != Ty::Dyn && !type_ok(want, got)) {
            note("let `" + s.a + "`: annotated " + type_name(want) + ", got " + type_name(got));
        }
        env[s.a] = want.k != Ty::Dyn ? want : got;
        std::set<std::string> ms;
        mentions(*s.e1, ms);
        bool outer_alias = false;
        for (const auto &n : ms) {
            if (n != s.a && !inner(n) && !scalar(env_get(n))) outer_alias = true;
        }
        if (!outer_alias) declare(s.a);
        if (current_region() != 0) {
            for (const auto &n : ms) {
                if (is_tainted(n)) taint(s.a);
            }
            if (has_node(*s.e1, EK::Closure) && !non_scalar_inner(ms).empty()) {
                arena_note("a closure inside the block captures a value made in it", false);
            }
        }
        break;
    }
    case SK::Set: {
        Ty got = expr(*s.e1);
        std::string target = s.a;
        auto dot = target.find('.');
        if (dot != std::string::npos) target = target.substr(0, dot);
        if (current_region() != 0 && !inner(target) && escapes(*s.e1)) {
            arena_note("a value made inside the block is assigned to `" + target +
                       "`, which lives outside it");
            taint(target);
        }
        if (dot != std::string::npos) {
            std::string head = s.a.substr(0, dot);
            std::string field = s.a.substr(dot + 1);
            auto it = env.find(head);
            Ty bt = it == env.end() ? Ty::dyn() : it->second;
            if (bt.k == Ty::Struct && field.find('.') == std::string::npos) {
                Ty ft;
                if (!bt.struct_has(field, &ft)) {
                    note(struct_field_err(field, bt));
                } else if (!type_ok(ft, got)) {
                    note("assign to `" + s.a + "`: want " + type_name(ft) + ", got " +
                         type_name(got));
                }
            }
        } else {
            auto it = env.find(s.a);
            if (it != env.end() && it->second.k != Ty::Dyn && !type_ok(it->second, got)) {
                note("assign to `" + s.a + "`: want " + type_name(it->second) + ", got " +
                     type_name(got));
            }
        }
        break;
    }
    case SK::SetIdx: {
        expr(*s.e1);
        expr(*s.e2);
        expr(*s.e3);
        std::string root = first_var(*s.e1);
        if (current_region() != 0 && !root.empty() && !inner(root) && escapes(*s.e3)) {
            arena_note("a value made inside the block is stored into `" + root +
                       "`, which lives outside it");
            taint(root);
        }
        break;
    }
    case SK::SetField: {
        Ty bt = expr(*s.e1);
        Ty got = expr(*s.e2);
        std::string root = first_var(*s.e1);
        if (current_region() != 0 && !root.empty() && !inner(root) && escapes(*s.e2)) {
            arena_note("a value made inside the block is stored into `" + root +
                       "`, which lives outside it");
            taint(root);
        }
        if (bt.k == Ty::Struct) {
            Ty ft;
            if (!bt.struct_has(s.a, &ft)) {
                note(struct_field_err(s.a, bt));
            } else if (!type_ok(ft, got)) {
                note("assign to `" + s.a + "`: want " + type_name(ft) + ", got " + type_name(got));
            }
        }
        break;
    }
    case SK::Return: {
        Ty got = expr(*s.e1);
        if (escapes(*s.e1)) arena_note("a value made inside the block is returned out of it");
        if (!type_ok(ret, got)) {
            note("return: want " + type_name(ret) + ", got " + type_name(got));
        }
        break;
    }
    case SK::Expr: {
        expr(*s.e1);
        const Expr &e = *s.e1;
        if (current_region() == 0) break;
        if (e.k == EK::MCall) {
            std::string root = first_var(*e.kids[0]);
            bool any = false;
            for (size_t i = 1; i < e.kids.size(); i++) {
                if (e.kids[i]->k == EK::Closure) continue;
                if (escapes(*e.kids[i])) any = true;
            }
            if (!root.empty() && env.count(root) && !inner(root) && any) {
                arena_note("a value made inside the block is stored into `" + root +
                           "`, which lives outside it");
                taint(root);
            }
        } else if (e.k == EK::Call && sigs.count(e.a)) {
            bool escapes_any = false, outer = false;
            for (const auto &a : e.kids) {
                if (escapes(*a)) escapes_any = true;
                std::string fv = first_var(*a);
                if (!fv.empty() && !inner(fv)) outer = true;
            }
            if (escapes_any && outer) {
                arena_note("a value made inside the block is passed to `" + e.a +
                           "` next to an outer binding");
            }
        } else if (e.k == EK::Closure) {
            std::set<std::string> ms;
            mentions(e, ms);
            if (!non_scalar_inner(ms).empty()) {
                arena_note("a closure inside the block captures a value made in it", false);
            } else {
                for (const auto &n : ms) {
                    if (is_tainted(n)) {
                        arena_note("a closure inside the block captures a value made in it", false);
                        break;
                    }
                }
            }
        }
        break;
    }
    case SK::If: {
        expr(*s.e1);
        if (is_arena_block(s)) {
            region++;
            region_stack.push_back(region);
            tainted.emplace_back();
            block(s.body);
            tainted.pop_back();
            region_stack.pop_back();
        } else {
            block(s.body);
        }
        if (s.els) {
            if (s.els->k == SK::Block) {
                block(s.els->body);
            } else {
                stmt(*s.els);
            }
        }
        break;
    }
    case SK::While:
        expr(*s.e1);
        block(s.body);
        break;
    case SK::ForRange: {
        declare(s.a);
        for (const Expr *b : {s.e1.get(), s.e2.get()}) {
            Ty t = expr(*b);
            if (!(t.k == Ty::Int || t.k == Ty::Bool || t.k == Ty::Dyn)) {
                note("forrange bound: want i64, got " + type_name(t));
            }
        }
        bool had = env.count(s.a) > 0;
        Ty old = env_get(s.a);
        env[s.a] = Ty::of(Ty::Int);
        block(s.body);
        if (!had) env.erase(s.a); else env[s.a] = old;
        break;
    }
    case SK::ForIter: {
        Ty st = expr(*s.e1);
        bool had = env.count(s.a) > 0;
        Ty old = env_get(s.a);
        declare(s.a);
        Ty elem = Ty::dyn();
        if (is_tuple(st)) {
            elem = st.inner ? *st.inner : Ty::dyn();
        } else if (st.k == Ty::Str) {
            elem = Ty::of(Ty::Str);
        }
        env[s.a] = elem;
        block(s.body);
        if (!had) env.erase(s.a); else env[s.a] = old;
        break;
    }
    case SK::Break:
    case SK::Continue:
    case SK::Import:
    case SK::Trait:
    case SK::Enum:
    case SK::Fn:
    case SK::ExportFn:
        nested_fn(s);
        break;
    case SK::Impl:
    case SK::Extern:
    case SK::ExternStruct:
    case SK::PubUse: break;
    case SK::Try: {
        block(s.body);
        bool had = s.opt1 && !s.opt1->empty();
        Ty old = had ? env_get(*s.opt1) : Ty::dyn();
        if (had) {
            declare(*s.opt1);
            env[*s.opt1] = Ty::dyn();
        }
        block(s.body2);
        if (had) env[*s.opt1] = old;
        break;
    }
    case SK::Spawn: {
        if (current_region() != 0) {
            std::set<std::string> ms;
            mentions(s, ms);
            if (!non_scalar_inner(ms).empty()) {
                arena_note("a spawn inside the block captures a value made in it");
            }
            bool t = false;
            for (const auto &n : ms) {
                if (is_tainted(n)) t = true;
            }
            if (t) arena_note("a spawn inside the block captures a value made in it", false);
        }
        block(s.body);
        break;
    }
    case SK::Defer: expr(*s.e1); break;
    case SK::Pub:
        if (!s.body.empty()) stmt(*s.body[0]);
        break;
    }
}

std::vector<TypeErr> type_check(const std::vector<StmtP> &prog, const Sigs &extra) {
    std::vector<TypeErr> errs;
    Sigs sigs = extra;
    // `extern struct T` declares what `ptr[T]` points at, so the table is built
    // before anything is typed -- signatures included: a `ptr[Node]` return is
    // a pointer to Node only if Node is already known.
    std::map<std::string, std::map<std::string, Ty>> structs;
    auto *saved_structs = g_structs;
    g_structs = &structs;
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if (node.k == SK::ExternStruct) structs[node.a];   // a name is enough to exist
    }
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if (node.k != SK::ExternStruct) continue;
        for (const auto &f : node.fields) structs[node.a][f.name] = ann_type(f.ann);
    }
    collect_sigs(prog, sigs);
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if (!is_fn_like(node) || node.k == SK::Extern) continue;
        Ty ret = fn_ret(node);
        if (ret.k == Ty::Dyn) continue;
        FnTyper tp(sigs, ret, errs);
        for (const auto &p : node.fns[0].params) tp.env[p.name] = ann_type(p.ann);
        tp.block(node.fns[0].body);
    }
    FnTyper top(sigs, Ty::dyn(), errs);
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if (node.k == SK::Fn || node.k == SK::ExportFn || node.k == SK::Impl ||
            node.k == SK::Trait || node.k == SK::Enum || node.k == SK::Extern) {
            continue;
        }
        top.stmt(*sp);
    }
    g_structs = saved_structs;
    std::vector<TypeErr> out;
    for (const auto &e : errs) {
        bool dup = false;
        for (const auto &o : out) {
            if (o.line == e.line && o.msg == e.msg) {
                dup = true;
                break;
            }
        }
        if (!dup) out.push_back(e);
    }
    return out;
}
