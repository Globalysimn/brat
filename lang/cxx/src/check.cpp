// Gradual static type checker over parsed ASTs: checks typed bindings,
// function signatures, struct/trait rules, and `arena` escape boundaries.
#include "check.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>


bool Ty::struct_has(const std::string &f, Ty *out) const {
    for (const auto &field : fields) {
        if (field.name == f) {
            if (out) *out = field.type;
            return true;
        }
    }
    return false;
}


static bool field_of(const std::vector<TyField> &fields, const std::string &name, Ty *out) {
    for (const auto &field : fields) {
        if (field.name == name) {
            if (out) *out = field.type;
            return true;
        }
    }
    return false;
}

static bool same_fields(const std::vector<TyField> &a, const std::vector<TyField> &b) {
    if (a.size() != b.size()) return false;
    for (const auto &field : a) {
        Ty got;
        if (!field_of(b, field.name, &got) || got != field.type) return false;
    }
    return true;
}

bool Ty::operator==(const Ty &o) const {
    if (k != o.k) return false;
    switch (k) {
    case Cb: return arity == o.arity && text == o.text;
    case Ptr:
    case Slice:
    case Vec:
    case Option: return *inner == *o.inner;
    case Result: return *inner == *o.inner && *inner2 == *o.inner2;
    // Two structs are the same when their layouts are, and a declared name
    // makes the claim checkable even before the fields are in the table.
    case Struct: return (text.empty() || o.text.empty() || text == o.text) &&
                        same_fields(fields, o.fields);
    case Enum:
    case Other: return text == o.text;
    default: return true;
    }
}

struct EnumVariantInfo {
    std::string orig_name;
    std::vector<Ty> field_tys;
    std::vector<std::optional<std::string>> field_anns;
};

struct EnumInfo {
    std::vector<std::string> order;
    std::map<std::string, EnumVariantInfo> variants;
};

// The declared `extern struct`, native `struct`, and `enum` tables.
static std::map<std::string, std::map<std::string, Ty>> *g_structs = nullptr;
static std::set<std::string> *g_unions = nullptr;
static std::set<std::string> *g_native_structs = nullptr;
static std::map<std::string, EnumInfo> *g_enums = nullptr;

struct ScopedTypeTables {
    std::map<std::string, std::map<std::string, Ty>> *prev_structs;
    std::set<std::string> *prev_unions;
    std::set<std::string> *prev_native;
    std::map<std::string, EnumInfo> *prev_enums;
    ScopedTypeTables(std::map<std::string, std::map<std::string, Ty>> *s,
                     std::set<std::string> *u,
                     std::set<std::string> *n,
                     std::map<std::string, EnumInfo> *e)
        : prev_structs(g_structs), prev_unions(g_unions),
          prev_native(g_native_structs), prev_enums(g_enums) {
        g_structs = s;
        g_unions = u;
        g_native_structs = n;
        g_enums = e;
    }
    ~ScopedTypeTables() {
        g_structs = prev_structs;
        g_unions = prev_unions;
        g_native_structs = prev_native;
        g_enums = prev_enums;
    }
};

std::string type_name(const Ty &t) {
    switch (t.k) {
    case Ty::Cb: return t.text;
    case Ty::Ptr: return "ptr[" + type_name(*t.inner) + "]";
    case Ty::Slice: return "slice[" + type_name(*t.inner) + "]";
    case Ty::Vec: return t.inner->k == Ty::Dyn ? "vec" : "vec[" + type_name(*t.inner) + "]";
    case Ty::Option:
        return t.inner->k == Ty::Dyn ? "Option" : "Option[" + type_name(*t.inner) + "]";
    case Ty::Result:
        return (t.inner->k == Ty::Dyn && t.inner2->k == Ty::Dyn)
                   ? "Result"
                   : "Result[" + type_name(*t.inner) + ", " + type_name(*t.inner2) + "]";
    case Ty::Enum: return t.text;
    case Ty::Struct:
        if (!t.text.empty() && g_native_structs && g_native_structs->count(t.text))
            return t.text;
        return "struct";
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

static bool slot_ok(const Ty &want, const Ty &got) {
    if (want.k == Ty::Dyn || got.k == Ty::Dyn) return true;
    return type_ok(want, got);
}

static Ty join_ty(const Ty &a, const Ty &b) {
    if (a == b) return a;
    if (a.k == Ty::None && b.k == Ty::Option) return b;
    if (b.k == Ty::None && a.k == Ty::Option) return a;
    if (a.k == Ty::Option && b.k == Ty::Option) {
        if (a.inner->k == Ty::Dyn) return b;
        if (b.inner->k == Ty::Dyn) return a;
        Ty j = join_ty(*a.inner, *b.inner);
        return j.k == Ty::Dyn ? Ty::dyn() : Ty::option(j);
    }
    if (a.k == Ty::Result && b.k == Ty::Result) {
        Ty jok = a.inner->k == Ty::Dyn ? *b.inner
                                       : (b.inner->k == Ty::Dyn ? *a.inner : join_ty(*a.inner, *b.inner));
        Ty jerr = a.inner2->k == Ty::Dyn
                      ? *b.inner2
                      : (b.inner2->k == Ty::Dyn ? *a.inner2 : join_ty(*a.inner2, *b.inner2));
        if (a.inner->k != Ty::Dyn && b.inner->k != Ty::Dyn && jok.k == Ty::Dyn) return Ty::dyn();
        if (a.inner2->k != Ty::Dyn && b.inner2->k != Ty::Dyn && jerr.k == Ty::Dyn) return Ty::dyn();
        return Ty::result(jok, jerr);
    }
    if (a.k == Ty::Vec && b.k == Ty::Vec) {
        if (a.inner->k == Ty::Dyn) return b;
        if (b.inner->k == Ty::Dyn) return a;
        return Ty::wrap(Ty::Vec, join_ty(*a.inner, *b.inner));
    }
    return Ty::dyn();
}

bool type_ok(const Ty &want, const Ty &got) {
    if (want.k == Ty::Dyn) return true;
    if (want.k == Ty::Result) {
        if (got.k == Ty::Dyn || got.k == Ty::Tag) return true;
        if (got.k == Ty::Enum && g_enums && g_enums->count(got.text)) {
            const auto &ei = g_enums->at(got.text);
            if (ei.variants.size() == 2 && ei.variants.count("ok") && ei.variants.count("err")) {
                const auto &vok = ei.variants.at("ok");
                const auto &verr = ei.variants.at("err");
                if (vok.field_tys.size() == 1 && verr.field_tys.size() == 1)
                    return slot_ok(*want.inner, vok.field_tys[0]) &&
                           slot_ok(*want.inner2, verr.field_tys[0]);
            }
        }
        if (got.k != Ty::Result) return false;
        return slot_ok(*want.inner, *got.inner) && slot_ok(*want.inner2, *got.inner2);
    }
    if (want.k == Ty::Option) {
        if (got.k == Ty::Dyn || got.k == Ty::Tag || got.k == Ty::None) return true;
        if (got.k != Ty::Option) return false;
        return slot_ok(*want.inner, *got.inner);
    }
    if (want.k == Ty::Enum) {
        if (got.k == Ty::Dyn || got.k == Ty::Tag) return true;
        if (got.k == Ty::Enum) return want.text == got.text;
        if (g_enums && g_enums->count(want.text)) {
            const auto &ei = g_enums->at(want.text);
            if (got.k == Ty::None) {
                auto vit = ei.variants.find("none");
                return vit != ei.variants.end() && vit->second.field_tys.empty();
            }
            if (got.k == Ty::Option) {
                auto vsome = ei.variants.find("some");
                if (vsome == ei.variants.end() || vsome->second.field_tys.size() != 1) return false;
                return slot_ok(vsome->second.field_tys[0], *got.inner);
            }
            if (got.k == Ty::Result) {
                const bool ok_needed = got.inner->k != Ty::Dyn;
                const bool err_needed = got.inner2->k != Ty::Dyn;
                if (!ok_needed && !err_needed)
                    return ei.variants.count("ok") > 0 || ei.variants.count("err") > 0;
                if (ok_needed) {
                    auto vok = ei.variants.find("ok");
                    if (vok == ei.variants.end() || vok->second.field_tys.size() != 1 ||
                        !slot_ok(vok->second.field_tys[0], *got.inner))
                        return false;
                }
                if (err_needed) {
                    auto verr = ei.variants.find("err");
                    if (verr == ei.variants.end() || verr->second.field_tys.size() != 1 ||
                        !slot_ok(verr->second.field_tys[0], *got.inner2))
                        return false;
                }
                return true;
            }
        }
        return false;
    }
    if (got.k == Ty::Dyn || got.k == Ty::Tag || got.k == Ty::None) return true;
    if (want == got) return true;
    if (want.k == Ty::Float && got.k == Ty::Int) return true;
    if (want.k == Ty::Cb && got.k == Ty::Str) return true;
    if (want.k == Ty::Struct && want.is_union && got.k == Ty::Map) return true;
    if (is_tuple(want) && is_tuple(got)) {
        if (want.k == Ty::Struct || got.k == Ty::Struct) {
            if (want.k != Ty::Struct || got.k != Ty::Struct) return false;
            if (g_native_structs &&
                ((!want.text.empty() && g_native_structs->count(want.text)) ||
                 (!got.text.empty() && g_native_structs->count(got.text)))) {
                return want.text == got.text;
            }
            return true;
        }
        if (want.k == got.k) {
            if (want.k == Ty::Vec) return slot_ok(*want.inner, *got.inner);
            return want.inner->k == Ty::Dyn || got.inner->k == Ty::Dyn ||
                   *want.inner == *got.inner;
        }
        return false;
    }
    if (want.k == Ty::Ptr && got.k == Ty::Int) return true;
    if (want.k == Ty::Int && got.k == Ty::Ptr) return true;
    return false;
}

static Ty word_type(const std::string &w) {
    if (w == "i8" || w == "u8" || w == "i16" || w == "u16" || w == "i32" || w == "u32" ||
        w == "i64" || w == "u64" || w == "usize" || w == "isize") {
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
        t.is_union = g_unions && g_unions->count(w) > 0;
        for (const auto &kv : (*g_structs)[w]) t.fields.push_back({kv.first, kv.second});
        return t;
    }
    if (g_enums && g_enums->count(w)) return Ty::enm(w);
    // A name this pass does not know is kept as written, not erased: two
    // different `ptr[T]` must not compare equal by accident.
    if (!w.empty()) return Ty::other(w);
    return Ty::dyn();
}

std::optional<std::pair<std::vector<std::string>, std::string>>
callback_signature(const std::string &text);

static int cb_arity(const std::string &ann) {
    auto parsed = callback_signature(ann);
    return parsed ? static_cast<int>(parsed->first.size()) : 0;
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
    if (a == "i8" || a == "u8" || a == "i16" || a == "u16" || a == "i32" || a == "u32" ||
        a == "i64" || a == "u64" || a == "usize" || a == "isize")
        return Ty::of(Ty::Int);
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
    if (a.rfind("vec[", 0) == 0 && a.back() == ']') {
        std::string inner = a.substr(4, a.size() - 5);
        return Ty::wrap(Ty::Vec, inner.empty() ? Ty::dyn() : ann_type(inner));
    }
    if (a == "vec") return Ty::wrap(Ty::Vec, Ty::dyn());
    if (a == "Option") return Ty::option(Ty::dyn());
    if (a.rfind("Option[", 0) == 0 && a.back() == ']') {
        std::string inner = a.substr(7, a.size() - 8);
        return Ty::option(inner.empty() ? Ty::dyn() : ann_type(inner));
    }
    if (a == "Result") return Ty::result(Ty::dyn(), Ty::dyn());
    if (a.rfind("Result[", 0) == 0 && a.back() == ']') {
        auto parts = split_type_list(a.substr(7, a.size() - 8));
        Ty ok = parts.size() >= 1 ? ann_type(parts[0]) : Ty::dyn();
        Ty err = parts.size() >= 2 ? ann_type(parts[1]) : Ty::dyn();
        return Ty::result(std::move(ok), std::move(err));
    }
    if (a == "map" || a.rfind("map[", 0) == 0) return Ty::of(Ty::Map);
    if (auto lb = a.find('['); lb != std::string::npos && !a.empty() && a.back() == ']') {
        std::string elem = a.substr(0, lb);
        std::string cnt = a.substr(lb + 1, a.size() - lb - 2);
        bool all_dig = !cnt.empty();
        for (char ch : cnt) if (!std::isdigit(static_cast<unsigned char>(ch))) all_dig = false;
        if (!elem.empty() && all_dig) return Ty::wrap(Ty::Vec, ann_type(elem));
    }
    if (g_unions && g_unions->count(a)) return word_type(a);
    if (g_native_structs && g_native_structs->count(a)) return word_type(a);
    if (g_enums && g_enums->count(a)) return Ty::enm(a);
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
    for (const auto &fd : s.fns) mentions(fd.body, acc);
}

static void mentions(const std::vector<StmtP> &v, std::set<std::string> &acc) {
    for (const auto &s : v) mentions(*s, acc);
}

static void mentions(const Expr &e, std::set<std::string> &acc) {
    if (e.k == EK::Call && e.a == "own") return;   // `own` reads nothing: it copies
    if (e.k == EK::Var) acc.insert(e.a);
    if (e.k == EK::Str || e.k == EK::RStr) mentions_str(e.a, acc);
    if ((e.k == EK::Closure || e.k == EK::ParFor) && e.fn) mentions(e.fn->body, acc);
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

// `if true { $arena_enter(); defer $arena_leave(); ... }` from parse_arena.
static bool is_arena_block(const Stmt &s) {
    if (s.k != SK::If || !s.e1 || s.e1->k != EK::Bool || !s.e1->flag) return false;
    if (s.body.empty()) return false;
    const Stmt &b = *s.body[0];
    return b.k == SK::Expr && b.e1 && b.e1->k == EK::Call && b.e1->a == "$arena_enter" &&
           b.e1->kids.empty();
}

static bool contains_arena_stmts(const std::vector<StmtP> &stmts);

static bool contains_arena_expr(const ExprP &ep) {
    if (!ep) return false;
    for (const auto &k : ep->kids) {
        if (contains_arena_expr(k)) return true;
    }
    if (ep->fn && contains_arena_stmts(ep->fn->body)) return true;
    return false;
}

static bool contains_arena_stmt(const Stmt &s) {
    if (is_arena_block(s)) return true;
    if (contains_arena_expr(s.e1) || contains_arena_expr(s.e2) || contains_arena_expr(s.e3)) return true;
    if (contains_arena_stmts(s.body) || contains_arena_stmts(s.body2)) return true;
    if (s.els && contains_arena_stmt(*s.els)) return true;
    for (const auto &fd : s.fns) {
        if (contains_arena_stmts(fd.body)) return true;
    }
    return false;
}

static bool contains_arena_stmts(const std::vector<StmtP> &stmts) {
    for (const auto &sp : stmts) {
        if (sp && contains_arena_stmt(*sp)) return true;
    }
    return false;
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

static bool readonly_mcall_name(const std::string &m) {
    static const std::set<std::string> ro = {
        "get", "has", "keys", "vals", "len", "contains", "starts_with", "ends_with",
        "index_of", "split", "split_once", "replace", "slice", "join", "zip", "pos",
        "chunks", "enumerate", "copy", "trim", "lower", "upper", "lines", "chars",
        "repeat", "parse_int", "parse_float", "first", "last", "min", "max", "sum",
        "rev", "map", "filter", "fold", "each", "any", "all", "find", "count",
        "sort_by", "flat_map", "take", "skip",
        "unwrap", "unwrap_or", "is_ok", "is_err", "is_some", "is_none"
    };
    return ro.count(m) > 0;
}

static bool pure_read_builtin(const std::string &name) {
    static const std::set<std::string> ro = {
        "len", "str", "print", "eprint", "is_ok", "is_err", "is_some", "is_none",
        "is_vec", "is_str", "is_map", "is_bool", "is_int", "is_float", "tagof",
        "__stag_is", "__shas", "own", "sbuf_add", "tcp_send", "write_file",
        "append_file", "fs_exists", "fs_is_dir", "fs_file_size", "fs_mkdir_all",
        "fs_list_dir", "fs_remove_dir", "fs_open", "fs_close", "fs_read_line",
        "write_bytes", "json_stringify"
    };
    return ro.count(name) > 0;
}

static bool scalar_only_call(const std::string &name) {
    static const std::set<std::string> sc = {
        "own", "len", "is_ok", "is_err", "is_some", "is_none", "is_vec", "is_str",
        "is_map", "is_bool", "is_int", "is_float", "__stag_is", "__shas",
        "ptr_is_null", "ptr_read_i64", "ptr_read_f64", "ptr_read_f32", "ptr_read_u8",
        "re_match", "fs_exists", "fs_is_dir", "sbuf_len", "slice_len", "time_ms", "mono_ns", "errno"
    };
    return sc.count(name) > 0;
}

static void non_scalar_flow_vars(const Expr *e, std::set<std::string> &acc) {
    if (!e) return;
    if (e->k == EK::Num || e->k == EK::Bool || e->k == EK::None || e->k == EK::Un) return;
    if (e->k == EK::Bin) {
        static const std::set<std::string> num_ops = {
            "-", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||", "in"};
        if (num_ops.count(e->a)) return;
    }
    if (e->k == EK::Call && scalar_only_call(e->a)) return;
    if (e->k == EK::Var) {
        if (!e->a.empty()) acc.insert(e->a);
        return;
    }
    if (e->k == EK::Str || e->k == EK::RStr) {
        mentions_str(e->a, acc);
        return;
    }
    if ((e->k == EK::Closure || e->k == EK::ParFor) && e->fn) {
        std::set<std::string> inner_ms;
        mentions(e->fn->body, inner_ms);
        std::set<std::string> bnd;
        if (e->k == EK::ParFor && !e->a.empty()) bnd.insert(e->a);
        for (const auto &p : e->fn->params) bnd.insert(p.name);
        for (const auto &m : inner_ms) {
            if (!bnd.count(m)) acc.insert(m);
        }
    }
    for (const auto &k : e->kids) non_scalar_flow_vars(k.get(), acc);
}

static std::set<size_t> deps_of_expr(const Expr *e,
                                     const std::map<std::string, std::set<size_t>> &deps) {
    std::set<size_t> out;
    std::set<std::string> vs;
    non_scalar_flow_vars(e, vs);
    for (const auto &v : vs) {
        auto it = deps.find(v);
        if (it != deps.end()) out.insert(it->second.begin(), it->second.end());
    }
    return out;
}

static void scan_sink_expr(const Expr *e, const std::set<std::string> &sinks,
                           const std::set<std::string> &locals,
                           std::map<std::string, std::set<size_t>> &deps,
                           std::set<size_t> &sink_params,
                           std::vector<SinkEdge> &edges);

static void scan_sink_stmts(const std::vector<StmtP> &stmts, const std::set<std::string> &sinks,
                            std::set<std::string> locals,
                            std::map<std::string, std::set<size_t>> &deps,
                            std::set<size_t> &sink_params,
                            std::vector<SinkEdge> &edges) {
    for (const auto &sp : stmts) {
        if (!sp) continue;
        const Stmt &s = *sp;
        if (s.k == SK::Let || s.k == SK::LetTmp) {
            scan_sink_expr(s.e1.get(), sinks, locals, deps, sink_params, edges);
            auto d = deps_of_expr(s.e1.get(), deps);
            if (!s.a.empty()) {
                locals.insert(s.a);
                deps[s.a].insert(d.begin(), d.end());
            }
            continue;
        }
        if (s.k == SK::Set) {
            scan_sink_expr(s.e1.get(), sinks, locals, deps, sink_params, edges);
            auto d = deps_of_expr(s.e1.get(), deps);
            std::string head = s.a;
            auto dot = head.find('.');
            if (dot != std::string::npos) head = head.substr(0, dot);
            if (!head.empty()) {
                if (sinks.count(head) && !locals.count(head))
                    sink_params.insert(d.begin(), d.end());
                else
                    deps[head].insert(d.begin(), d.end());
            }
            continue;
        }
        if (s.k == SK::SetIdx || s.k == SK::SetField) {
            scan_sink_expr(s.e1.get(), sinks, locals, deps, sink_params, edges);
            scan_sink_expr(s.e2.get(), sinks, locals, deps, sink_params, edges);
            scan_sink_expr(s.e3.get(), sinks, locals, deps, sink_params, edges);
            const Expr *val_e = s.k == SK::SetIdx ? s.e3.get() : s.e2.get();
            auto d = deps_of_expr(val_e, deps);
            std::string root = s.e1 ? first_var(*s.e1) : "";
            if (!root.empty()) {
                if (sinks.count(root) && !locals.count(root))
                    sink_params.insert(d.begin(), d.end());
                else
                    deps[root].insert(d.begin(), d.end());
            }
            continue;
        }
        scan_sink_expr(s.e1.get(), sinks, locals, deps, sink_params, edges);
        scan_sink_expr(s.e2.get(), sinks, locals, deps, sink_params, edges);
        scan_sink_expr(s.e3.get(), sinks, locals, deps, sink_params, edges);
        if (s.k == SK::ForRange || s.k == SK::ForIter) {
            std::set<std::string> sub = locals;
            if (!s.a.empty()) sub.insert(s.a);
            if (s.k == SK::ForIter && !s.a.empty()) {
                auto d = deps_of_expr(s.e1.get(), deps);
                deps[s.a].insert(d.begin(), d.end());
            }
            scan_sink_stmts(s.body, sinks, std::move(sub), deps, sink_params, edges);
            continue;
        }
        if (s.k == SK::Try) {
            scan_sink_stmts(s.body, sinks, locals, deps, sink_params, edges);
            std::set<std::string> sub = locals;
            if (s.opt1 && !s.opt1->empty()) sub.insert(*s.opt1);
            scan_sink_stmts(s.body2, sinks, std::move(sub), deps, sink_params, edges);
            continue;
        }
        if (!s.body.empty()) scan_sink_stmts(s.body, sinks, locals, deps, sink_params, edges);
        if (!s.body2.empty()) scan_sink_stmts(s.body2, sinks, locals, deps, sink_params, edges);
        if (s.els) {
            if (s.els->k == SK::Block) {
                scan_sink_stmts(s.els->body, sinks, locals, deps, sink_params, edges);
            } else {
                scan_sink_expr(s.els->e1.get(), sinks, locals, deps, sink_params, edges);
                scan_sink_stmts(s.els->body, sinks, locals, deps, sink_params, edges);
            }
        }
        for (const auto &fd : s.fns) {
            std::set<std::string> sub = locals;
            for (const auto &p : fd.params) sub.insert(p.name);
            scan_sink_stmts(fd.body, sinks, std::move(sub), deps, sink_params, edges);
        }
    }
}

static void scan_sink_expr(const Expr *e, const std::set<std::string> &sinks,
                           const std::set<std::string> &locals,
                           std::map<std::string, std::set<size_t>> &deps,
                           std::set<size_t> &sink_params,
                           std::vector<SinkEdge> &edges) {
    if (!e) return;
    if (e->k == EK::MCall && !e->kids.empty() && !readonly_mcall_name(e->b)) {
        std::string root = first_var(*e->kids[0]);
        std::set<size_t> d;
        for (size_t i = 1; i < e->kids.size(); i++) {
            auto di = deps_of_expr(e->kids[i].get(), deps);
            d.insert(di.begin(), di.end());
        }
        if (e->b == "send") {
            sink_params.insert(d.begin(), d.end());
        } else if (!root.empty()) {
            if (sinks.count(root) && !locals.count(root))
                sink_params.insert(d.begin(), d.end());
            else
                deps[root].insert(d.begin(), d.end());
        }
    } else if (e->k == EK::Call) {
        bool has_sink_arg = false;
        std::set<size_t> all_d;
        for (size_t j = 0; j < e->kids.size(); j++) {
            auto dj = deps_of_expr(e->kids[j].get(), deps);
            all_d.insert(dj.begin(), dj.end());
            if (!e->a.empty() && !dj.empty()) edges.push_back(SinkEdge{e->a, j, dj});
            std::string fv = first_var(*e->kids[j]);
            if (!fv.empty() && sinks.count(fv) && !locals.count(fv)) has_sink_arg = true;
        }
        if (has_sink_arg && !pure_read_builtin(e->a))
            sink_params.insert(all_d.begin(), all_d.end());
    }
    if ((e->k == EK::Closure || e->k == EK::ParFor) && e->fn) {
        std::set<std::string> sub = locals;
        if (e->k == EK::ParFor && !e->a.empty()) sub.insert(e->a);
        for (const auto &p : e->fn->params) sub.insert(p.name);
        scan_sink_stmts(e->fn->body, sinks, std::move(sub), deps, sink_params, edges);
    }
    for (const auto &k : e->kids) scan_sink_expr(k.get(), sinks, locals, deps, sink_params, edges);
}

static void compute_sink_summary(const std::vector<StmtP> &body,
                                 const std::vector<Param> &params,
                                 const std::set<std::string> &sinks,
                                 std::set<size_t> &sink_params,
                                 std::vector<SinkEdge> &edges) {
    std::map<std::string, std::set<size_t>> deps;
    std::set<std::string> locals;
    for (size_t i = 0; i < params.size(); i++) {
        locals.insert(params[i].name);
        deps[params[i].name].insert(i);
    }
    for (int pass = 0; pass < 2; pass++) {
        edges.clear();
        scan_sink_stmts(body, sinks, locals, deps, sink_params, edges);
    }
}

static bool has_chan_call(const Expr *e) {
    if (!e) return false;
    if (e->k == EK::Call && e->a == "chan") return true;
    for (const auto &k : e->kids) {
        if (has_chan_call(k.get())) return true;
    }
    return false;
}

void collect_sigs(const std::vector<StmtP> &prog, Sigs &sigs) {
    std::set<std::string> mod_sinks;
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if ((node.k == SK::Let || node.k == SK::LetTmp) && !node.a.empty()) {
            if (!node.konst || has_chan_call(node.e1.get())) mod_sinks.insert(node.a);
        }
    }
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if (node.k == SK::Impl) {
            for (const FnDecl &fn : node.fns) {
                if (!fn.name) continue;
                sigs["__impl__." + *fn.name] = FnSig{};
                const bool has_self = !fn.params.empty() && fn.params[0].name == "self";
                FnSig msig;
                msig.ret = ann_type(fn.ret);
                msig.ret_ann = fn.ret;
                for (size_t pi = has_self ? 1 : 0; pi < fn.params.size(); pi++) {
                    msig.params.push_back(ann_type(fn.params[pi].ann));
                    msig.param_anns.push_back(fn.params[pi].ann);
                    if (fn.params[pi].def) msig.ndef++;
                }
                if (has_self) {
                    sigs["__method__." + node.a + "." + *fn.name] = std::move(msig);
                } else {
                    sigs["__static__." + node.a + "." + *fn.name] = std::move(msig);
                }
            }
            continue;
        }
        if (node.k == SK::Trait) {
            for (const Sig &g : node.sigs) sigs["__impl__." + g.name] = FnSig{};
            continue;
        }
        if (sp->k == SK::Pub && node.k == SK::Enum) {
            for (const auto &v : node.variants) {
                if (v.name == "Ok" || v.name == "Err" || v.name == "Some" || v.name == "None")
                    continue;
                FnSig csig;
                csig.is_ctor = true;
                csig.ret = Ty::enm(node.a);
                csig.ret_ann = node.a;
                for (size_t fi = 0; fi < v.fields.size(); fi++) {
                    std::optional<std::string> fa =
                        fi < v.field_anns.size() ? v.field_anns[fi] : std::nullopt;
                    csig.params.push_back(ann_type(fa));
                    csig.param_anns.push_back(fa);
                }
                sigs[v.name] = std::move(csig);
            }
            continue;
        }
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
        if (!is_ext && !node.fns.empty()) {
            compute_sink_summary(node.fns[0].body, *params, mod_sinks, sig.sink_params,
                                 sig.sink_edges);
        }
        sigs[is_ext ? node.ext.name : node.fns[0].name.value_or("")] = std::move(sig);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &kv : sigs) {
            for (const auto &ed : kv.second.sink_edges) {
                auto it = sigs.find(ed.callee);
                if (it != sigs.end() && it->second.sink_params.count(ed.arg_idx)) {
                    for (size_t cp : ed.caller_params) {
                        if (kv.second.sink_params.insert(cp).second) changed = true;
                    }
                }
            }
        }
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
    {"parse_int", {}, "res:int:str", false},
    {"replace", {"str", "str"}, "str", false},
    {"index_of", {"str"}, "dyn", false},
    {"repeat", {"int"}, "str", false},
    {"split_once", {"str"}, "dyn", false},
    {"copy", {}, "str", false},
    {"chars", {}, "vec:str", false},
    {"parse_float", {}, "res:float:str", false},
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
    {"len", Ty::of(Ty::Int)}, {"str", Ty::of(Ty::Str)},
    {"is_ok", Ty::of(Ty::Bool)}, {"is_err", Ty::of(Ty::Bool)},
    {"is_some", Ty::of(Ty::Bool)}, {"is_none", Ty::of(Ty::Bool)},
    {"tagof", Ty::of(Ty::Str)},
    {"__stag_is", Ty::of(Ty::Bool)}, {"__shas", Ty::of(Ty::Bool)},
    {"read_file", Ty::result(Ty::of(Ty::Str), Ty::of(Ty::Str))},
    {"write_file", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"append_file", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"read_bytes", Ty::result(Ty::wrap(Ty::Vec, Ty::of(Ty::Int)), Ty::of(Ty::Str))},
    {"write_bytes", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"proc_run", Ty::result(Ty::of(Ty::Map), Ty::of(Ty::Str))},
    {"utf8_bytes", Ty::wrap(Ty::Vec, Ty::of(Ty::Int))},
    {"utf8_str", Ty::result(Ty::of(Ty::Str), Ty::of(Ty::Str))},
    {"json_parse", Ty::result(Ty::dyn(), Ty::of(Ty::Str))},
    {"json_stringify", Ty::of(Ty::Str)},
    {"fs_exists", Ty::of(Ty::Bool)},
    {"fs_is_dir", Ty::of(Ty::Bool)},
    {"fs_file_size", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"fs_mkdir_all", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"fs_list_dir", Ty::result(Ty::wrap(Ty::Vec, Ty::of(Ty::Str)), Ty::of(Ty::Str))},
    {"fs_remove_dir", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"fs_open", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"fs_read_line", Ty::result(Ty::option(Ty::of(Ty::Str)), Ty::of(Ty::Str))},
    {"fs_close", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"stdin_read_line", Ty::result(Ty::option(Ty::of(Ty::Str)), Ty::of(Ty::Str))},
    {"fs_rename", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"fs_remove", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"sha256_bytes", Ty::result(Ty::wrap(Ty::Vec, Ty::of(Ty::Int)), Ty::of(Ty::Str))},
    {"random_bytes", Ty::result(Ty::wrap(Ty::Vec, Ty::of(Ty::Int)), Ty::of(Ty::Str))},
    {"https_request_raw", Ty::result(Ty::of(Ty::Str), Ty::of(Ty::Str))},
    {"sql_open", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"sql_close", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"sql_exec", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"sql_query", Ty::result(Ty::wrap(Ty::Vec, Ty::of(Ty::Map)), Ty::of(Ty::Str))},
    {"is_vec", Ty::of(Ty::Bool)}, {"is_str", Ty::of(Ty::Bool)},
    {"is_map", Ty::of(Ty::Bool)}, {"is_bool", Ty::of(Ty::Bool)},
    {"is_int", Ty::of(Ty::Bool)}, {"is_float", Ty::of(Ty::Bool)},
    {"join_all", Ty::of(Ty::Int)},
    {"Ok", Ty::of(Ty::Tag)}, {"Err", Ty::of(Ty::Tag)}, {"Some", Ty::of(Ty::Tag)},
    {"args", Ty::wrap(Ty::Vec, Ty::of(Ty::Str))},
    {"args_bytes", Ty::wrap(Ty::Vec, Ty::wrap(Ty::Vec, Ty::of(Ty::Int)))},
    {"exit", Ty::dyn()}, {"eprint", Ty::dyn()}, {"raise", Ty::dyn()},
    {"time_ms", Ty::of(Ty::Int)}, {"mono_ns", Ty::of(Ty::Int)},
    {"sleep_ms", Ty::dyn()}, {"env_get", Ty::dyn()}, {"env_get_bytes", Ty::dyn()},
    {"$arena_enter", Ty::of(Ty::Int)}, {"$arena_leave", Ty::of(Ty::Int)},
    {"re_match", Ty::of(Ty::Bool)}, {"re_find", Ty::option(Ty::of(Ty::Str))},
    {"re_captures", Ty::option(Ty::wrap(Ty::Vec, Ty::of(Ty::Str)))},
    {"re_replace", Ty::of(Ty::Str)},
    {"tcp_listen", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"tcp_connect", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"tcp_accept", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"tcp_send", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"tcp_recv", Ty::result(Ty::of(Ty::Str), Ty::of(Ty::Str))},
    {"tcp_close", Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str))},
    {"sbuf", Ty::dyn()}, {"sbuf_add", Ty::of(Ty::Int)},
    {"sbuf_len", Ty::of(Ty::Int)}, {"sbuf_str", Ty::result(Ty::of(Ty::Str), Ty::of(Ty::Str))},
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
    if (s == "res:int:str") return Ty::result(Ty::of(Ty::Int), Ty::of(Ty::Str));
    if (s == "res:float:str") return Ty::result(Ty::of(Ty::Float), Ty::of(Ty::Str));
    return Ty::dyn();
}


struct FnTyper {
    const Sigs &sigs;
    Ty ret;
    std::vector<TypeErr> &errs;
    int line = 0;
    std::map<std::string, Ty> env;
    std::set<std::string> annotated_vars;
    std::map<std::string, std::string> active_tags;
    std::set<std::string> outer_globals;
    std::vector<std::set<std::string>> tainted;
    int region = 0;
    std::map<std::string, int> decl_regions;
    std::map<std::string, int> regions;
    std::set<std::string> arena_ptrs;
    std::map<std::string, std::set<size_t>> closure_sinks;
    std::map<std::string, std::set<size_t>> local_fn_sinks;
    std::vector<int> region_stack{0};
    std::set<const Expr *> arena_checked;
    bool arena_only = false;
    bool arity_and_arena = false;
    bool silent = false;
    // `&x` is the out-parameter of a C function: legitimate in an extern's
    // `ptr[T]` slot, refused everywhere else.
    bool amp_ok = false;
    std::set<std::string> *cur_block_lets = nullptr;

    FnTyper(const Sigs &sigs, Ty ret, std::vector<TypeErr> &errs)
        : sigs(sigs), ret(std::move(ret)), errs(errs) {}

    void note(const std::string &msg) {
        if (silent) return;
        if (arena_only && msg.rfind("arena: ", 0) != 0) return;
        if (arity_and_arena && msg.rfind("arena: ", 0) != 0 &&
            msg.rfind("non-exhaustive match:", 0) != 0 &&
            msg.rfind("variant `", 0) != 0 &&
            !(msg.rfind("method `", 0) == 0 && msg.find(" wants ") != std::string::npos &&
              msg.find(" args") != std::string::npos)) return;
        errs.push_back(TypeErr{line, msg});
    }

    int current_region() const { return region_stack.back(); }

    int decl_region(const std::string &name) const {
        auto it = decl_regions.find(name);
        return it == decl_regions.end() ? 0 : it->second;
    }

    int val_region(const std::string &name) const {
        auto it = regions.find(name);
        return it == regions.end() ? 0 : it->second;
    }

    bool inner(const std::string &name) const {
        return current_region() != 0 && decl_region(name) >= current_region();
    }

    void declare(const std::string &name, int vreg = -1) {
        if (name.empty()) return;
        int cur = current_region();
        decl_regions[name] = cur;
        regions[name] = vreg < 0 ? cur : vreg;
    }

    void taint(const std::string &name) {
        if (current_region() != 0 && !name.empty() && !inner(name)) {
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

    bool is_non_scalar_var(const std::string &n) const {
        if (arena_ptrs.count(n)) return true;
        return !scalar(env_get(n));
    }

    std::vector<std::string> non_scalar_inner(const std::set<std::string> &names,
                                              int min_reg = 1) const {
        std::vector<std::string> out;
        for (const auto &n : names) {
            if (val_region(n) >= min_reg && is_non_scalar_var(n)) out.push_back(n);
        }
        return out;
    }

    Ty env_get(const std::string &n) const {
        auto it = env.find(n);
        return it == env.end() ? Ty::dyn() : it->second;
    }

    Ty peek_expr(const Expr &e) {
        bool saved = silent;
        silent = true;
        Ty t = expr(e);
        silent = saved;
        return t;
    }

    static bool is_num_op(const std::string &op) {
        static const std::set<std::string> ops = {"-", "*", "/", "%", "&", "|", "^", "<<", ">>",
                                                 "==", "!=", "<", ">", "<=", ">=", "&&", "||"};
        return ops.count(op) > 0;
    }

    int expr_region(const Expr &e);
    bool escapes(const Expr &e, int min_reg = -1);

    static bool readonly_mcall(const std::string &m) {
        return readonly_mcall_name(m);
    }

    bool closure_captures_arena(const Expr &e) const {
        if (e.k != EK::Closure) return false;
        std::set<std::string> ms;
        mentions(e, ms);
        if (!non_scalar_inner(ms, 1).empty()) return true;
        for (const auto &n : ms) {
            if (is_tainted(n)) return true;
        }
        return false;
    }

    std::set<size_t> body_sink_params(const std::vector<StmtP> &body,
                                      const std::vector<Param> &params) const {
        std::set<std::string> sinks = outer_globals;
        for (const auto &kv : env) {
            if (!inner(kv.first)) sinks.insert(kv.first);
        }
        std::set<size_t> sp;
        std::vector<SinkEdge> edges;
        compute_sink_summary(body, params, sinks, sp, edges);
        for (const auto &ed : edges) {
            if (auto it = sigs.find(ed.callee);
                it != sigs.end() && it->second.sink_params.count(ed.arg_idx)) {
                sp.insert(ed.caller_params.begin(), ed.caller_params.end());
            } else if (auto lit = local_fn_sinks.find(ed.callee);
                       lit != local_fn_sinks.end() && lit->second.count(ed.arg_idx)) {
                sp.insert(ed.caller_params.begin(), ed.caller_params.end());
            } else if (auto cit = closure_sinks.find(ed.callee);
                       cit != closure_sinks.end() && cit->second.count(ed.arg_idx)) {
                sp.insert(ed.caller_params.begin(), ed.caller_params.end());
            } else if (!sigs.count(ed.callee) && env.count(ed.callee) && !inner(ed.callee)) {
                sp.insert(ed.caller_params.begin(), ed.caller_params.end());
            }
        }
        return sp;
    }

    void check_arena_call_expr(const Expr &e) {
        if (current_region() == 0 || !arena_checked.insert(&e).second) return;
        if (e.k == EK::MCall) {
            if (readonly_mcall(e.b)) return;
            std::string root = first_var(*e.kids[0]);
            int max_arg_reg = 0;
            for (size_t i = 1; i < e.kids.size(); i++) {
                if (e.kids[i]->k == EK::Closure) {
                    if (closure_captures_arena(*e.kids[i])) max_arg_reg = current_region();
                    continue;
                }
                int r = expr_region(*e.kids[i]);
                if (r > max_arg_reg) max_arg_reg = r;
            }
            if (max_arg_reg > 0 && e.b == "send" && max_arg_reg >= current_region()) {
                if (!root.empty() && (env.count(root) || outer_globals.count(root)) &&
                    !inner(root)) {
                    arena_note("a value made inside the block is stored into `" + root +
                               "`, which lives outside it");
                    taint(root);
                } else {
                    arena_note("a value made inside the block is sent through a channel");
                }
            } else if (!root.empty() && max_arg_reg > 0) {
                if ((env.count(root) || outer_globals.count(root)) && !inner(root) &&
                    max_arg_reg >= current_region()) {
                    arena_note("a value made inside the block is stored into `" + root +
                               "`, which lives outside it");
                    taint(root);
                } else if (inner(root) && max_arg_reg > val_region(root)) {
                    regions[root] = max_arg_reg;
                }
            }
        } else if (e.k == EK::Call && e.a != "own") {
            if ((e.a == "slice_set" || e.a == "ptr_write_str" || e.a == "ptr_write_ptr" ||
                 e.a == "ptr_write_struct") &&
                e.kids.size() >= 2) {
                std::string root = first_var(*e.kids[0]);
                const Expr &val_arg = *e.kids.back();
                if (!root.empty() && !inner(root) && escapes(val_arg)) {
                    arena_note("a value made inside the block is stored into `" + root +
                               "`, which lives outside it");
                    taint(root);
                }
                return;
            }
            if (sigs.count(e.a) || env.count(e.a) || local_fn_sinks.count(e.a)) {
                bool escapes_any = false, outer = false;
                for (const auto &a : e.kids) {
                    if (escapes(*a) || closure_captures_arena(*a)) escapes_any = true;
                    std::string fv = first_var(*a);
                    if (!fv.empty() && !inner(fv)) outer = true;
                }
                if (!escapes_any) return;
                if (outer) {
                    arena_note("a value made inside the block is passed to `" + e.a +
                               "` next to an outer binding");
                    return;
                }
                bool hit_sink = false;
                bool unknown_outer_cb = !sigs.count(e.a) && !local_fn_sinks.count(e.a) &&
                                        !closure_sinks.count(e.a) && env.count(e.a) &&
                                        !inner(e.a);
                for (size_t j = 0; j < e.kids.size(); j++) {
                    if (!(escapes(*e.kids[j]) || closure_captures_arena(*e.kids[j]))) continue;
                    if (unknown_outer_cb ||
                        (sigs.count(e.a) && sigs.at(e.a).sink_params.count(j)) ||
                        (local_fn_sinks.count(e.a) && local_fn_sinks.at(e.a).count(j)) ||
                        (closure_sinks.count(e.a) && closure_sinks.at(e.a).count(j))) {
                        hit_sink = true;
                        break;
                    }
                }
                if (hit_sink) {
                    arena_note("a value made inside the block is passed to `" + e.a +
                               "`, which may store into outer state");
                }
            }
        }
    }

    void arena_note(std::string msg, bool ownable = true) {
        msg += ownable
                   ? "  (hint: hand out a heap copy with `own(...)`, e.g. `return Err(own(msg))`)"
                   : "  (hint: keep the value inside the block, or copy it with `own(...)` first)";
        note("arena: " + msg);
    }

    void closure_body(const Expr &e, int elem_reg = 0, Ty elem_ty = Ty::dyn(),
                      size_t elem_idx = 0) {
        std::map<std::string, Ty> saved = env;
        std::set<std::string> saved_ann = annotated_vars;
        std::map<std::string, std::string> saved_tags = active_tags;
        std::map<std::string, int> saved_decl = decl_regions;
        std::map<std::string, int> saved_reg = regions;
        Ty saved_ret = ret;
        for (size_t i = 0; i < e.fn->params.size(); i++) {
            const auto &p = e.fn->params[i];
            Ty pt = ann_type(p.ann);
            if (pt.k == Ty::Dyn && elem_reg > 0 && i == elem_idx) pt = elem_ty;
            env[p.name] = pt;
            if (p.ann) annotated_vars.insert(p.name);
            else annotated_vars.erase(p.name);
            active_tags.erase(p.name);
            decl_regions[p.name] = current_region();
            regions[p.name] = (elem_reg > 0 && i == elem_idx && !scalar(pt)) ? elem_reg : 0;
        }
        // A closure has no annotated return: `fn(x) { return x > 1 }` is a bool
        // predicate, and typing it against the enclosing `ret` rejected it.
        ret = Ty::dyn();
        block(e.fn->body);
        ret = saved_ret;
        regions = std::move(saved_reg);
        decl_regions = std::move(saved_decl);
        active_tags = std::move(saved_tags);
        annotated_vars = std::move(saved_ann);
        env = std::move(saved);
    }

    // A `fn` declared inside a body: its own parameters and return type.
    void nested_fn(const Stmt &s) {
        if (s.fns.empty()) return;
        const FnDecl &f = s.fns[0];
        if (f.name && !f.name->empty()) {
            auto sp = body_sink_params(f.body, f.params);
            if (!sp.empty()) local_fn_sinks[*f.name] = std::move(sp);
            else local_fn_sinks.erase(*f.name);
        }
        std::map<std::string, Ty> saved = env;
        std::set<std::string> saved_ann = annotated_vars;
        std::map<std::string, std::string> saved_tags = active_tags;
        std::map<std::string, int> saved_decl = decl_regions;
        std::map<std::string, int> saved_reg = regions;
        Ty saved_ret = ret;
        for (const auto &p : f.params) {
            env[p.name] = ann_type(p.ann);
            if (p.ann) annotated_vars.insert(p.name);
            else annotated_vars.erase(p.name);
            active_tags.erase(p.name);
            decl_regions[p.name] = current_region();
            regions[p.name] = 0;
        }
        ret = ann_type(f.ret);
        block(f.body);
        ret = saved_ret;
        regions = std::move(saved_reg);
        decl_regions = std::move(saved_decl);
        active_tags = std::move(saved_tags);
        annotated_vars = std::move(saved_ann);
        env = std::move(saved);
    }

    std::string field_names(const Ty &t) {
        if (t.k != Ty::Struct) return "";
        std::string out;
        for (const auto &field : t.fields) {
            if (!out.empty()) out += ", ";
            out += field.name;
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

    static bool extract_tagof_eq(const Expr *c, std::string &var_out, std::string &tag_out) {
        if (!c || c->k != EK::Bin || c->a != "==" || c->kids.size() != 2) return false;
        const Expr *lhs = c->kids[0].get();
        const Expr *rhs = c->kids[1].get();
        auto match_side = [&](const Expr *call_e, const Expr *str_e) -> bool {
            if (!call_e || !str_e || str_e->k != EK::Str) return false;
            if (call_e->k == EK::Call && call_e->a == "tagof" && call_e->kids.size() == 1 &&
                call_e->kids[0]->k == EK::Var) {
                var_out = call_e->kids[0]->a;
                tag_out = str_e->a;
                return true;
            }
            return false;
        };
        return match_side(lhs, rhs) || match_side(rhs, lhs);
    }

    static bool extract_pred_var(const Expr *c, const char *pred, std::string &var_out) {
        if (!c) return false;
        if (c->k == EK::Call && c->a == pred && c->kids.size() == 1 &&
            c->kids[0]->k == EK::Var) {
            var_out = c->kids[0]->a;
            return true;
        }
        if (c->k == EK::MCall && c->b == pred && c->kids.size() == 1 &&
            c->kids[0]->k == EK::Var) {
            var_out = c->kids[0]->a;
            return true;
        }
        return false;
    }

    static bool body_assigns(const std::vector<StmtP> &stmts, const std::string &var);
    static bool stmt_assigns(const Stmt &s, const std::string &var) {
        if (s.k == SK::Set && (s.a == var || s.a.rfind(var + ".", 0) == 0)) return true;
        if (body_assigns(s.body, var) || body_assigns(s.body2, var)) return true;
        if (s.els && stmt_assigns(*s.els, var)) return true;
        return false;
    }

    void check_match_exhaustive(const std::vector<StmtP> &stmts, size_t idx) {
        if (idx + 1 >= stmts.size() || !stmts[idx] || !stmts[idx + 1]) return;
        const Stmt &lt = *stmts[idx];
        if (lt.k != SK::LetTmp || !lt.from_match) return;
        Ty scrut = env_get(lt.a);
        const bool is_enum = (scrut.k == Ty::Enum && g_enums && g_enums->count(scrut.text));
        if (!is_enum && scrut.k != Ty::Option && scrut.k != Ty::Result) return;
        const Stmt *cur = stmts[idx + 1].get();
        if (!cur || cur->k != SK::If) return;
        std::set<std::string> seen;
        while (cur && cur->k == SK::If) {
            if (cur->pat_uncond) {
                std::string v, tag;
                if (extract_tagof_eq(cur->e1.get(), v, tag) && v == lt.a)
                    seen.insert(tag);
                else if (extract_pred_var(cur->e1.get(), "is_none", v) && v == lt.a)
                    seen.insert("none");
            }
            cur = cur->els.get();
        }
        if (!cur || cur->k != SK::Block || !cur->from_match) return;
        std::string missing;
        auto add_missing = [&](const std::string &tag, const std::string &disp) {
            if (!seen.count(tag)) {
                if (!missing.empty()) missing += ", ";
                missing += disp;
            }
        };
        if (scrut.k == Ty::Option) {
            add_missing("some", "Some");
            add_missing("none", "None");
        } else if (scrut.k == Ty::Result) {
            add_missing("ok", "Ok");
            add_missing("err", "Err");
        } else {
            const auto &ei = g_enums->at(scrut.text);
            for (const auto &tag : ei.order) {
                auto vit = ei.variants.find(tag);
                add_missing(tag, vit != ei.variants.end() ? vit->second.orig_name : tag);
            }
        }
        if (!missing.empty()) {
            int saved_line = line;
            if (lt.line) line = lt.line;
            note("non-exhaustive match: missing " + missing);
            line = saved_line;
        }
    }

    void block(const std::vector<StmtP> &body) {
        auto saved_env = env;
        auto saved_ann = annotated_vars;
        auto saved_tags = active_tags;
        auto saved_decl = decl_regions;
        auto saved_reg = regions;
        auto saved_aptrs = arena_ptrs;
        auto saved_sinks = closure_sinks;
        std::set<std::string> local_lets;
        auto *saved_blets = cur_block_lets;
        cur_block_lets = &local_lets;
        for (size_t i = 0; i < body.size(); i++) {
            stmt(*body[i]);
            check_match_exhaustive(body, i);
        }
        cur_block_lets = saved_blets;
        for (const auto &v : local_lets) {
            auto eit = saved_env.find(v);
            if (eit != saved_env.end()) env[v] = eit->second;
            else env.erase(v);
            if (saved_ann.count(v)) annotated_vars.insert(v);
            else annotated_vars.erase(v);
            auto tit = saved_tags.find(v);
            if (tit != saved_tags.end()) active_tags[v] = tit->second;
            else active_tags.erase(v);
            auto dit = saved_decl.find(v);
            if (dit != saved_decl.end()) decl_regions[v] = dit->second;
            else decl_regions.erase(v);
            auto rit = saved_reg.find(v);
            if (rit != saved_reg.end()) regions[v] = rit->second;
            else regions.erase(v);
            if (saved_aptrs.count(v)) arena_ptrs.insert(v);
            else arena_ptrs.erase(v);
            auto sit = saved_sinks.find(v);
            if (sit != saved_sinks.end()) closure_sinks[v] = sit->second;
            else closure_sinks.erase(v);
        }
    }

    void stmt(const Stmt &s);
};

bool FnTyper::body_assigns(const std::vector<StmtP> &stmts, const std::string &var) {
    for (const auto &sp : stmts) {
        if (sp && stmt_assigns(*sp, var)) return true;
    }
    return false;
}

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
            for (size_t i = 1; i < ts.size(); i++) t = join_ty(t, ts[i]);
        }
        return Ty::wrap(Ty::Vec, t);
    }
    case EK::MapLit:
        for (const auto &k : e.kids) expr(*k);
        return Ty::of(Ty::Map);
    case EK::StructLit: {
        std::vector<TyField> fields;
        for (size_t i = 0; i < e.kids.size(); i++) {
            Ty t = expr(*e.kids[i]);
            const std::string &name = e.names[i];
            bool replaced = false;
            for (auto &field : fields) {
                if (field.name == name) {
                    field.type = t;
                    replaced = true;
                }
            }
            if (!replaced) fields.push_back({name, t});
        }
        if (g_unions && g_unions->count(e.a) && g_structs && g_structs->count(e.a)) {
            const auto &ufields = (*g_structs)[e.a];
            for (const auto &field : fields) {
                auto fit = ufields.find(field.name);
                if (fit == ufields.end()) {
                    note("extern union `" + e.a + "` has no arm `" + field.name + "`");
                } else if (!type_ok(fit->second, field.type)) {
                    note("arm `" + field.name + "` of extern union `" + e.a + "` wants " +
                         type_name(fit->second) + ", got " + type_name(field.type));
                }
            }
            if (e.kids.size() != 1) {
                note("extern union `" + e.a + "` literal initializes " +
                     std::to_string(e.kids.size()) + " arms (want 1)");
            }
            return word_type(e.a);
        }
        if (g_native_structs && g_native_structs->count(e.a) && g_structs && g_structs->count(e.a)) {
            const auto &sfields = (*g_structs)[e.a];
            std::set<std::string> seen_names;
            for (const auto &fname : e.names) {
                if (!seen_names.insert(fname).second)
                    note("duplicate field `" + fname + "` in struct `" + e.a + "`");
            }
            std::string known;
            for (auto it = sfields.begin(); it != sfields.end(); ++it) {
                if (it != sfields.begin()) known += ", ";
                known += it->first;
            }
            for (const auto &field : fields) {
                auto fit = sfields.find(field.name);
                if (fit == sfields.end()) {
                    note("no field `" + field.name + "` on struct (has " + known + ")");
                } else if (!type_ok(fit->second, field.type)) {
                    note("field `" + field.name + "` of struct `" + e.a + "` wants " +
                         type_name(fit->second) + ", got " + type_name(field.type));
                }
            }
            for (const auto &kv : sfields) {
                if (!seen_names.count(kv.first))
                    note("missing field `" + kv.first + "` in struct `" + e.a + "`");
            }
            return word_type(e.a);
        }
        Ty out_st = Ty::strct(std::move(fields));
        out_st.text = e.a;
        return out_st;
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
        if (e.a == "~") {
            if (t.k != Ty::Dyn && t.k != Ty::Int)
                note("unary `~` wants i64, got " + type_name(t));
            return Ty::of(Ty::Int);
        }
        return numeric(t) ? t : Ty::dyn();
    }
    case EK::Move: return expr(*e.kids[0]);
    case EK::Closure:
        closure_body(e);
        return Ty::dyn();
    case EK::ParFor: {
        expr(*e.kids[0]);
        Ty unit = expr(*e.kids[1]);
        closure_body(e);
        return unit;
    }
    case EK::Bin: {
        const std::string &op = e.a;
        Ty a = expr(*e.kids[0]);
        Ty b = expr(*e.kids[1]);
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=" ||
            op == "in") {
            return Ty::of(Ty::Bool);
        }
        if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
            if (a.k != Ty::Dyn && a.k != Ty::Int)
                note("operator `" + op + "` wants i64, got " + type_name(a));
            if (b.k != Ty::Dyn && b.k != Ty::Int)
                note("operator `" + op + "` wants i64, got " + type_name(b));
            return Ty::of(Ty::Int);
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
        if (a.k == Ty::None) return b;
        return join_ty(a, b);
    }
    case EK::Quest: {
        Ty inner = expr(*e.kids[0]);
        if (inner.k == Ty::Option || inner.k == Ty::Result) return *inner.inner;
        if (inner.k == Ty::Enum && g_enums && g_enums->count(inner.text)) {
            const auto &ei = g_enums->at(inner.text);
            auto vok = ei.variants.find("ok");
            if (vok != ei.variants.end() && vok->second.field_tys.size() == 1)
                return vok->second.field_tys[0];
        }
        return inner;
    }
    case EK::MCall: {
        if ((e.kids[0]->k == EK::Call && e.kids[0]->kids.empty()) ||
            (e.kids[0]->k == EK::Var && !env.count(e.kids[0]->a))) {
            auto sit = sigs.find("__static__." + e.kids[0]->a + "." + e.b);
            if (sit != sigs.end()) {
                const FnSig &f = sit->second;
                const size_t n = e.kids.size() - 1;
                const size_t ndef = f.ndef > 0 ? static_cast<size_t>(f.ndef) : 0;
                const size_t min_n = f.params.size() >= ndef ? f.params.size() - ndef : 0;
                if (n < min_n || n > f.params.size()) {
                    note("`" + e.kids[0]->a + "." + e.b + "` wants " +
                         std::to_string(f.params.size()) + " args, got " + std::to_string(n));
                }
                for (size_t i = 0; i < n; i++) {
                    Ty gt = expr(*e.kids[i + 1]);
                    if (i < f.params.size() && !type_ok(f.params[i], gt)) {
                        note("arg " + std::to_string(i + 1) + " of `" + e.kids[0]->a + "." + e.b +
                             "`: want " + type_name(f.params[i]) + ", got " + type_name(gt));
                    }
                }
                check_arena_call_expr(e);
                return f.ret;
            }
        }
        Ty ot = expr(*e.kids[0]);
        int recv_reg = current_region() != 0 ? expr_region(*e.kids[0]) : 0;
        Ty elem_ty = (ot.k == Ty::Vec && ot.inner) ? *ot.inner
                                                   : (ot.k == Ty::Str ? Ty::of(Ty::Str) : Ty::dyn());
        static const std::set<std::string> hof_m = {
            "each", "map", "filter", "flat_map", "find", "any", "all", "count", "sort_by", "fold"
        };
        for (size_t i = 1; i < e.kids.size(); i++) {
            if (recv_reg > 0 && hof_m.count(e.b) && e.kids[i]->k == EK::Closure && e.kids[i]->fn) {
                closure_body(*e.kids[i], recv_reg, elem_ty, e.b == "fold" ? 1 : 0);
            } else {
                expr(*e.kids[i]);
            }
        }
        check_arena_call_expr(e);
        return meth(ot, e.b, e.kids, 1);
    }
    case EK::Field: {
        if ((e.kids[0]->k == EK::Call && e.kids[0]->kids.empty()) ||
            (e.kids[0]->k == EK::Var && !env.count(e.kids[0]->a))) {
            auto sit = sigs.find("__static__." + e.kids[0]->a + "." + e.b);
            if (sit != sigs.end() && sit->second.is_ctor && sit->second.params.empty()) {
                return sit->second.ret;
            }
        }
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
        if (e.kids.empty() && !name.empty() && name[0] >= 'A' && name[0] <= 'Z' &&
            env.count(name)) {
            return env_get(name);
        }
        if (name == "Some" || name == "Ok" || name == "Err") {
            if (e.kids.size() != 1) {
                note("`" + name + "` wants 1 arg, got " + std::to_string(e.kids.size()));
                for (const auto &k : e.kids) expr(*k);
                check_arena_call_expr(e);
                return name == "Some" ? Ty::option(Ty::dyn())
                                      : Ty::result(Ty::dyn(), Ty::dyn());
            }
            Ty inner = expr(*e.kids[0]);
            check_arena_call_expr(e);
            if (name == "Some") return Ty::option(std::move(inner));
            if (name == "Ok") return Ty::result(std::move(inner), Ty::dyn());
            return Ty::result(Ty::dyn(), std::move(inner));
        }
        if (name == "unwrap") {
            if (e.kids.size() != 1) {
                note("`unwrap` wants 1 arg, got " + std::to_string(e.kids.size()));
                for (const auto &k : e.kids) expr(*k);
                check_arena_call_expr(e);
                return Ty::dyn();
            }
            Ty arg = expr(*e.kids[0]);
            check_arena_call_expr(e);
            if (arg.k == Ty::Option || arg.k == Ty::Result) return *arg.inner;
            if (arg.k == Ty::Dyn || arg.k == Ty::Tag) return Ty::dyn();
            if (arg.k == Ty::Enum && g_enums && g_enums->count(arg.text)) {
                const auto &ei = g_enums->at(arg.text);
                auto vok = ei.variants.find("ok");
                if (vok != ei.variants.end() && vok->second.field_tys.size() == 1)
                    return vok->second.field_tys[0];
                auto vsome = ei.variants.find("some");
                if (vsome != ei.variants.end() && vsome->second.field_tys.size() == 1)
                    return vsome->second.field_tys[0];
            }
            note("`unwrap` wants Option or Result, got " + type_name(arg));
            return Ty::dyn();
        }
        if (name == "unwrap_or") {
            if (e.kids.size() != 2) {
                note("`unwrap_or` wants 2 args, got " + std::to_string(e.kids.size()));
                for (const auto &k : e.kids) expr(*k);
                check_arena_call_expr(e);
                return Ty::dyn();
            }
            Ty arg = expr(*e.kids[0]);
            Ty fb = expr(*e.kids[1]);
            check_arena_call_expr(e);
            if (arg.k == Ty::Dyn || arg.k == Ty::Tag) return Ty::dyn();
            if (arg.k == Ty::None) return fb;
            Ty pay = Ty::dyn();
            bool valid = false;
            if (arg.k == Ty::Option || arg.k == Ty::Result) {
                pay = *arg.inner;
                valid = true;
            } else if (arg.k == Ty::Enum && g_enums && g_enums->count(arg.text)) {
                const auto &ei = g_enums->at(arg.text);
                if (ei.variants.count("ok") || ei.variants.count("err") ||
                    ei.variants.count("some") || ei.variants.count("none")) {
                    valid = true;
                    auto vok = ei.variants.find("ok");
                    auto vsome = ei.variants.find("some");
                    if (vok != ei.variants.end() && vok->second.field_tys.size() == 1)
                        pay = vok->second.field_tys[0];
                    else if (vsome != ei.variants.end() && vsome->second.field_tys.size() == 1)
                        pay = vsome->second.field_tys[0];
                }
            }
            if (!valid) {
                note("`unwrap_or` wants Option or Result, got " + type_name(arg));
                return Ty::dyn();
            }
            if (pay.k == Ty::Dyn) return fb;
            if (fb.k != Ty::Dyn && !type_ok(pay, fb)) {
                note("arg 2 of `unwrap_or`: want " + type_name(pay) + ", got " + type_name(fb));
                return pay;
            }
            return pay;
        }
        if (name == "is_ok" || name == "is_err" || name == "is_some" || name == "is_none") {
            if (e.kids.size() != 1) {
                note("`" + name + "` wants 1 arg, got " + std::to_string(e.kids.size()));
                for (const auto &k : e.kids) expr(*k);
                check_arena_call_expr(e);
                return Ty::of(Ty::Bool);
            }
            expr(*e.kids[0]);
            check_arena_call_expr(e);
            return Ty::of(Ty::Bool);
        }
        if (name == "__at" && e.kids.size() == 2 && e.kids[0]->k == EK::Var &&
            e.kids[1]->k == EK::Num && !e.kids[1]->is_float) {
            const std::string &v = e.kids[0]->a;
            const int64_t idx = e.kids[1]->i - 1;
            expr(*e.kids[0]);
            expr(*e.kids[1]);
            auto ait = active_tags.find(v);
            if (ait != active_tags.end() && idx >= 0) {
                Ty scrut = env_get(v);
                const std::string &tag = ait->second;
                if (scrut.k == Ty::Option && tag == "some" && idx == 0) return *scrut.inner;
                if (scrut.k == Ty::Result && tag == "ok" && idx == 0) return *scrut.inner;
                if (scrut.k == Ty::Result && tag == "err" && idx == 0) return *scrut.inner2;
                if (scrut.k == Ty::Enum && g_enums && g_enums->count(scrut.text)) {
                    const auto &ei = g_enums->at(scrut.text);
                    auto vit = ei.variants.find(tag);
                    if (vit != ei.variants.end() &&
                        static_cast<size_t>(idx) < vit->second.field_tys.size()) {
                        return vit->second.field_tys[static_cast<size_t>(idx)];
                    }
                }
            }
            check_arena_call_expr(e);
            return Ty::dyn();
        }
        // A known signature types its arguments in the loop below, one slot at a
        // time -- that is where `&x` learns it is a `ptr[T]` argument. Walking
        // them here first would check each one twice, the first time blind.
        if (sigs.find(name) == sigs.end() && name != "lib_open" && name != "lib_sym" &&
            name != "lib_close" && name != "call") {
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
        if (bres != BRES.end()) {
            check_arena_call_expr(e);
            return bres->second;
        }
        if (name == "slice_of" || name == "slice_of_ptr" || name == "slice_len" ||
            name == "slice_get" || name == "slice_set" || name == "str_from_ptr" ||
            name == "slice_of_struct" || name == "slice_struct_of") {
            Ty st = slice_call(name, e.kids, 0);
            check_arena_call_expr(e);
            return st;
        }
        if (name == "lib_open" || name == "lib_sym" || name == "lib_close" || name == "call") {
            Ty pt = plugin_call(name, e.kids, 0);
            check_arena_call_expr(e);
            return pt;
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
            check_arena_call_expr(e);
            return f.ret;
        }
        check_arena_call_expr(e);
        return Ty::dyn();
    }
    }
    return Ty::dyn();
}

int FnTyper::expr_region(const Expr &e) {
    int cur = current_region();
    if (cur == 0) return 0;
    switch (e.k) {
    case EK::Num:
    case EK::Bool:
    case EK::None:
    case EK::Str:
    case EK::Un:
        return 0;
    case EK::RStr:
        return 0;
    case EK::VecLit:
    case EK::MapLit:
    case EK::StructLit:
        return cur;
    case EK::Var:
        if (is_tainted(e.a)) return cur;
        if (!is_non_scalar_var(e.a)) return 0;
        return val_region(e.a);
    case EK::Closure: {
        std::set<std::string> ms;
        mentions(e, ms);
        int r = 0;
        for (const auto &n : ms) {
            if (is_tainted(n)) return cur;
            if (is_non_scalar_var(n)) r = std::max(r, val_region(n));
        }
        return r;
    }
    case EK::ParFor:
        return scalar(peek_expr(e)) ? 0 : cur;
    case EK::Bin: {
        if (e.a == "*" && e.kids.size() >= 2) {
            if (peek_expr(*e.kids[0]).k == Ty::Str || peek_expr(*e.kids[1]).k == Ty::Str)
                return cur;
            return 0;
        }
        if (is_num_op(e.a) || e.a == "in") return 0;
        if (e.a == "+" && e.kids.size() >= 2) {
            Ty ta = peek_expr(*e.kids[0]);
            Ty tb = peek_expr(*e.kids[1]);
            if ((ta.k == Ty::Int || ta.k == Ty::Float) && tb.k != Ty::Str) return 0;
            if ((tb.k == Ty::Int || tb.k == Ty::Float) && ta.k != Ty::Str) return 0;
            return cur;
        }
        return 0;
    }
    case EK::Or: {
        if (scalar(peek_expr(e))) return 0;
        int r0 = e.kids.size() > 0 ? expr_region(*e.kids[0]) : 0;
        int r1 = e.kids.size() > 1 ? expr_region(*e.kids[1]) : 0;
        return std::max(r0, r1);
    }
    case EK::Quest:
    case EK::Move:
    case EK::Field:
    case EK::Idx:
        if (scalar(peek_expr(e))) return 0;
        return e.kids.empty() ? 0 : expr_region(*e.kids[0]);
    case EK::MCall: {
        if (scalar(peek_expr(e))) return 0;
        static const std::set<std::string> elem_m = {
            "get", "get_or", "pop", "first", "last", "min", "max", "find", "del",
            "unwrap", "unwrap_or", "recv"
        };
        if (elem_m.count(e.b)) {
            int r = 0;
            for (const auto &k : e.kids) r = std::max(r, expr_region(*k));
            return r;
        }
        return cur;
    }
    case EK::Call: {
        if (e.a == "own") return 0;
        if (e.a == "ptr_of_str" && !e.kids.empty()) return expr_region(*e.kids[0]);
        if ((e.a == "slice_of" || e.a == "slice_get") && !e.kids.empty()) {
            if (e.a == "slice_get" && scalar(peek_expr(e))) return 0;
            return expr_region(*e.kids[0]);
        }
        if (e.a == "unwrap" || e.a == "unwrap_or" || e.a == "__at") {
            if (scalar(peek_expr(e))) return 0;
            int r = 0;
            for (const auto &k : e.kids) r = std::max(r, expr_region(*k));
            return r;
        }
        if (!e.a.empty() && e.a[0] >= 'A' && e.a[0] <= 'Z' &&
            (!sigs.count(e.a) || sigs.at(e.a).is_ctor)) {
            int r = 0;
            for (const auto &k : e.kids) r = std::max(r, expr_region(*k));
            return r;
        }
        if (scalar(peek_expr(e))) return 0;
        return cur;
    }
    }
    return 0;
}

bool FnTyper::escapes(const Expr &e, int min_reg) {
    int cur = current_region();
    if (cur == 0) return false;
    int threshold = min_reg < 0 ? cur : min_reg;
    return expr_region(e) >= threshold;
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
    if (!tab) {
        if (ot.k == Ty::Struct && !ot.text.empty() && g_native_structs &&
            g_native_structs->count(ot.text)) {
            auto mit = sigs.find("__method__." + ot.text + "." + name);
            if (mit != sigs.end()) {
                const FnSig &f = mit->second;
                const size_t n = args.size() - start;
                const size_t ndef = f.ndef > 0 ? static_cast<size_t>(f.ndef) : 0;
                const size_t min_n = f.params.size() >= ndef ? f.params.size() - ndef : 0;
                if (n < min_n || n > f.params.size()) {
                    note("method `" + name + "` wants " + std::to_string(f.params.size()) + " args");
                    return f.ret;
                }
                for (size_t i = 0; i < n && i < f.params.size(); i++) {
                    Ty gt = expr(*args[start + i]);
                    if (!type_ok(f.params[i], gt)) {
                        note("method `" + name + "` arg " + std::to_string(i + 1) + ": want " +
                             type_name(f.params[i]) + ", got " + type_name(gt));
                    }
                }
                return f.ret;
            }
            if (name == "copy") {
                if (args.size() - start != 0) note("method `copy` wants 0 args");
                return ot;
            }
            for (const auto &m : MAP_METH) {
                if (m.name == name) { tab = &MAP_METH; break; }
            }
            if (!tab) {
                note("no method `" + name + "` for " + ot.text);
                return Ty::dyn();
            }
        }
    }
    if (!tab) {
        auto dispatch_adt_builtin = [&](const Ty &recv) -> std::optional<Ty> {
            const size_t n = args.size() - start;
            if (name == "is_ok" || name == "is_err" || name == "is_some" || name == "is_none") {
                if (n != 0) note("method `" + name + "` wants 0 args");
                return Ty::of(Ty::Bool);
            }
            if (name == "unwrap") {
                if (n != 0) note("method `unwrap` wants 0 args");
                if (recv.k == Ty::Option || recv.k == Ty::Result) return *recv.inner;
                if (recv.k == Ty::Enum && g_enums && g_enums->count(recv.text)) {
                    const auto &ei = g_enums->at(recv.text);
                    auto vok = ei.variants.find("ok");
                    if (vok != ei.variants.end() && vok->second.field_tys.size() == 1)
                        return vok->second.field_tys[0];
                    auto vsome = ei.variants.find("some");
                    if (vsome != ei.variants.end() && vsome->second.field_tys.size() == 1)
                        return vsome->second.field_tys[0];
                }
                return std::nullopt;
            }
            if (name == "unwrap_or") {
                if (n != 1) {
                    note("method `unwrap_or` wants 1 args");
                    return Ty::dyn();
                }
                Ty fb = expr(*args[start]);
                if (recv.k == Ty::None) return fb;
                Ty pay = Ty::dyn();
                bool valid = false;
                if (recv.k == Ty::Option || recv.k == Ty::Result) {
                    pay = *recv.inner;
                    valid = true;
                } else if (recv.k == Ty::Enum && g_enums && g_enums->count(recv.text)) {
                    const auto &ei = g_enums->at(recv.text);
                    if (ei.variants.count("ok") || ei.variants.count("err") ||
                        ei.variants.count("some") || ei.variants.count("none")) {
                        valid = true;
                        auto vok = ei.variants.find("ok");
                        auto vsome = ei.variants.find("some");
                        if (vok != ei.variants.end() && vok->second.field_tys.size() == 1)
                            pay = vok->second.field_tys[0];
                        else if (vsome != ei.variants.end() && vsome->second.field_tys.size() == 1)
                            pay = vsome->second.field_tys[0];
                    }
                }
                if (!valid) return std::nullopt;
                if (pay.k == Ty::Dyn) return fb;
                if (fb.k != Ty::Dyn && !type_ok(pay, fb)) {
                    note("method `unwrap_or` arg 1: want " + type_name(pay) + ", got " +
                         type_name(fb));
                }
                return pay;
            }
            return std::nullopt;
        };
        if (ot.k == Ty::Enum && !ot.text.empty() && g_enums && g_enums->count(ot.text)) {
            auto mit = sigs.find("__method__." + ot.text + "." + name);
            if (mit != sigs.end()) {
                const FnSig &f = mit->second;
                const size_t n = args.size() - start;
                const size_t ndef = f.ndef > 0 ? static_cast<size_t>(f.ndef) : 0;
                const size_t min_n = f.params.size() >= ndef ? f.params.size() - ndef : 0;
                if (n < min_n || n > f.params.size()) {
                    note("method `" + name + "` wants " + std::to_string(f.params.size()) + " args");
                    return f.ret;
                }
                for (size_t i = 0; i < n && i < f.params.size(); i++) {
                    Ty gt = expr(*args[start + i]);
                    if (!type_ok(f.params[i], gt)) {
                        note("method `" + name + "` arg " + std::to_string(i + 1) + ": want " +
                             type_name(f.params[i]) + ", got " + type_name(gt));
                    }
                }
                return f.ret;
            }
            if (auto adt = dispatch_adt_builtin(ot)) return *adt;
            note("no method `" + name + "` for " + ot.text);
            return Ty::dyn();
        }
        if (ot.k == Ty::Option || ot.k == Ty::Result) {
            if (auto adt = dispatch_adt_builtin(ot)) return *adt;
            note("no method `" + name + "` for " + type_name(ot));
            return Ty::dyn();
        }
        if (ot.k == Ty::None &&
            (name == "unwrap_or" || name == "is_ok" || name == "is_err" ||
             name == "is_some" || name == "is_none")) {
            if (auto adt = dispatch_adt_builtin(ot)) return *adt;
        }
    }
    if (!tab) {
        if (ot.k == Ty::Dyn || ot.k == Ty::Tag) {
            const bool user_defined = sigs.count("__impl__." + name) != 0;
            if (!user_defined) {
                static const std::map<std::string, std::pair<size_t, size_t>> builtin_arity = {
                    {"split", {0, 1}}, {"lines", {0, 0}}, {"trim", {0, 0}}, {"lower", {0, 0}},
                    {"upper", {0, 0}}, {"starts_with", {1, 1}}, {"ends_with", {1, 1}},
                    {"contains", {1, 1}}, {"parse_int", {0, 0}}, {"replace", {2, 2}},
                    {"index_of", {1, 1}}, {"repeat", {1, 1}}, {"split_once", {1, 1}},
                    {"parse_float", {0, 0}}, {"slice", {2, 2}}, {"join", {1, 1}},
                    {"push", {1, 1}}, {"pop", {0, 0}}, {"first", {0, 0}}, {"last", {0, 0}},
                    {"sort", {0, 0}}, {"rev", {0, 0}}, {"ins", {2, 2}}, {"del", {1, 1}},
                    {"sum", {0, 0}}, {"min", {0, 0}}, {"max", {0, 0}}, {"enumerate", {0, 0}},
                    {"zip", {1, 1}}, {"chunks", {1, 1}}, {"pos", {1, 1}}, {"get", {1, 1}},
                    {"has", {1, 1}}, {"keys", {0, 0}}, {"set", {2, 2}}, {"items", {0, 0}},
                    {"vals", {0, 0}}, {"get_or", {2, 2}}, {"copy", {0, 0}}, {"chars", {0, 0}},
                    {"send", {1, 1}}, {"recv", {0, 0}}, {"close", {0, 0}},
                    {"map", {1, 1}}, {"filter", {1, 1}}, {"any", {1, 1}}, {"all", {1, 1}},
                    {"find", {1, 1}}, {"sort_by", {1, 1}}, {"fold", {2, 2}},
                    {"unwrap", {0, 0}}, {"unwrap_or", {1, 1}}, {"is_ok", {0, 0}},
                    {"is_err", {0, 0}}, {"is_some", {0, 0}}, {"is_none", {0, 0}},
                };
                auto it = builtin_arity.find(name);
                if (it != builtin_arity.end()) {
                    const size_t n = args.size() - start;
                    if (n < it->second.first || n > it->second.second) {
                        note("method `" + name + "` wants " + std::to_string(it->second.second) + " args");
                    }
                }
            }
        }
        return Ty::dyn();
    }
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
        } else {
            Ty got = expr(at(0));
            if (!type_ok(Ty::of(Ty::Str), got))
                note("lib_open: want str, got " + type_name(got));
        }
        return Ty::of(Ty::Int);
    }
    if (name == "lib_sym") {
        if (n != 2) {
            note("`lib_sym` wants 2 args, got " + std::to_string(n));
            return Ty::of(Ty::Int);
        }
        Ty got0 = expr(at(0));
        if (!type_ok(Ty::of(Ty::Int), got0)) {
            note("lib_sym: handle wants ptr, got " + type_name(got0));
        }
        Ty got1 = expr(at(1));
        if (!type_ok(Ty::of(Ty::Str), got1)) {
            note("lib_sym: name wants str, got " + type_name(got1));
        }
        return Ty::of(Ty::Int);
    }
    if (name == "lib_close") {
        if (n != 1) {
            note("`lib_close` wants 1 arg, got " + std::to_string(n));
        } else {
            Ty got = expr(at(0));
            if (!type_ok(Ty::of(Ty::Int), got))
                note("lib_close: want ptr, got " + type_name(got));
        }
        return Ty::of(Ty::Int);
    }
    static const std::map<std::string, std::string> SIG_RET = {
        {"i64", "int"}, {"f32", "float"}, {"f64", "float"}, {"str", "str"},
        {"ptr", "int"}, {"own ptr", "int"}, {"none", "dyn"}};
    static const std::map<std::string, std::string> SIG_WANT = {
        {"i64", "int"}, {"f32", "float"}, {"f64", "float"}, {"str", "str"},
        {"ptr", "int"}, {"own ptr", "int"}, {"&ptr", "int"}, {"out ptr", "int"}};
    auto norm_slot = [](const std::string &raw) -> std::string {
        std::string s;
        for (unsigned char c : raw)
            if (!std::isspace(c)) s.push_back(static_cast<char>(c));
        if (s == "ownptr" || s == "moveptr") return "own ptr";
        if (s == "outptr") return "out ptr";
        if (s == "&ptr" || s == "refptr") return "&ptr";
        return s;
    };
    if (n < 2) {
        note("`call` wants a function, a signature and the arguments");
        return Ty::dyn();
    }
    const Expr &sig = at(1);
    if (sig.k != EK::Str) {
        note("call: signature must be a string literal");
        return Ty::dyn();
    }
    size_t lo = 0, hi = sig.a.size();
    while (lo < hi && std::isspace(static_cast<unsigned char>(sig.a[lo]))) lo++;
    while (hi > lo && std::isspace(static_cast<unsigned char>(sig.a[hi - 1]))) hi--;
    std::string raw = sig.a.substr(lo, hi - lo);
    auto bad_sig = [&]() -> Ty {
        std::string compact = sig.a;
        compact.erase(std::remove(compact.begin(), compact.end(), ' '), compact.end());
        note("call: bad signature `" + compact + "`");
        return Ty::dyn();
    };
    size_t open_i = raw.find('(');
    if (open_i == 0 || open_i == std::string::npos || raw.empty() || raw.back() != ')')
        return bad_sig();
    int pdepth = 0;
    for (size_t i = open_i; i < raw.size(); i++) {
        if (raw[i] == '(') pdepth++;
        else if (raw[i] == ')') {
            pdepth--;
            if (pdepth == 0 && i + 1 != raw.size()) return bad_sig();
        }
    }
    if (pdepth != 0) return bad_sig();
    std::string ret_t = norm_slot(raw.substr(0, open_i));
    std::string rest = raw.substr(open_i + 1, raw.size() - open_i - 2);
    std::vector<std::string> params;
    int depth = 0;
    size_t start_i = 0;
    for (size_t i = 0; i < rest.size(); i++) {
        if (rest[i] == '(') depth++;
        else if (rest[i] == ')') depth--;
        else if (rest[i] == ',' && depth == 0) {
            std::string piece = norm_slot(rest.substr(start_i, i - start_i));
            if (!piece.empty()) params.push_back(piece);
            start_i = i + 1;
        }
    }
    std::string tail = norm_slot(rest.substr(start_i));
    if (!tail.empty()) params.push_back(tail);
    if (!SIG_RET.count(ret_t)) {
        note("call: unsupported return type `" + ret_t + "`");
        return Ty::dyn();
    }
    Ty fn_ty = expr(at(0));
    if (!type_ok(Ty::of(Ty::Int), fn_ty)) {
        note("call: want ptr, got " + type_name(fn_ty));
    }
    Ty ret_ty = from_code(SIG_RET.at(ret_t).c_str());
    if (params.size() != n - 2) {
        note("call: signature wants " + std::to_string(params.size()) + " argument(s), got " +
             std::to_string(n - 2));
        return ret_ty;
    }
    for (size_t i = 0; i < params.size(); i++) {
        if (params[i].rfind("fn(", 0) == 0 && params[i].find(")->") != std::string::npos) {
            continue;
        }
        if (!SIG_WANT.count(params[i])) {
            note("call: unsupported argument type `" + params[i] + "`");
            continue;
        }
        bool prev_amp = amp_ok;
        amp_ok = (params[i] == "out ptr");
        Ty got = expr(at(i + 2));
        amp_ok = prev_amp;
        bool is_ptr_slot = (params[i] == "ptr" || params[i] == "own ptr" ||
                            params[i] == "&ptr" || params[i] == "out ptr");
        if (is_ptr_slot && got.k == Ty::Ptr) continue;
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
        Ty got0 = expr(at(0));
        if (!type_ok(Ty::of(Ty::Int), got0)) {
            note("str_from_ptr: want ptr, got " + type_name(got0));
        }
        Ty got1 = expr(at(1));
        if (!type_ok(Ty::of(Ty::Int), got1)) {
            note("str_from_ptr: n wants i64, got " + type_name(got1));
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
        if (!(sn.k == EK::Str && g_structs && g_structs->count(sn.a) &&
              !(g_native_structs && g_native_structs->count(sn.a)))) {
            note(sn.k != EK::Str ? "slice_of_struct wants a struct name as a string literal"
                                 : "slice_of_struct: " + sn.a + " is not an extern struct");
            return Ty::wrap(Ty::Slice, Ty::dyn());
        }
        Ty st;
        st.k = Ty::Struct;
        for (const auto &kv : (*g_structs)[sn.a]) st.fields.push_back({kv.first, kv.second});
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
            Ty kt = expr(*kid);
            if (elem.k == Ty::Dyn) {
                elem = kt;
            } else if (kt.k != Ty::Dyn && !type_ok(elem, kt)) {
                note("slice_struct_of: want " + type_name(elem) + ", got " + type_name(kt));
            }
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
        Ty got_n = expr(at(1));
        if (!type_ok(Ty::of(Ty::Int), got_n)) {
            note("slice_of_ptr: n wants i64, got " + type_name(got_n));
        }
        return Ty::wrap(Ty::Slice, word_type(kind.a));
    }
    Ty e = elem(at(0));
    Ty got_idx = expr(at(1));
    if (!type_ok(Ty::of(Ty::Int), got_idx)) {
        note(name + ": index wants i64, got " + type_name(got_idx));
    }
    if (name == "slice_get") return e;
    Ty got = expr(at(2));
    if (!type_ok(e, got)) note("slice_set: want " + type_name(e) + ", got " + type_name(got));
    return Ty::of(Ty::Int);
}

void FnTyper::check_cb(const Ty &pt, const Expr &arg, const std::string &name, int pos) {
    if (arg.k != EK::Str) {
        bool sentinel = (arg.k == EK::Num && !arg.is_float && arg.i == 0) ||
                        (arg.k == EK::Un && arg.a == "-" && arg.kids.size() == 1 &&
                         arg.kids[0] && arg.kids[0]->k == EK::Num &&
                         !arg.kids[0]->is_float && arg.kids[0]->i == 1);
        if (sentinel) return;
        expr(arg);
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
    case SK::Scope:
        if (current_region() != 0 || contains_arena_stmts(s.body)) {
            bool saved_ao = arena_only;
            arena_only = true;
            block(s.body);
            arena_only = saved_ao;
        }
        break;
    case SK::Block: block(s.body); break;
    case SK::Let:
    case SK::LetTmp: {
        Ty want = ann_type(s.ann);
        Ty got = expr(*s.e1);
        if (s.k == SK::Let && want.k != Ty::Dyn && !type_ok(want, got)) {
            note("let `" + s.a + "`: annotated " + type_name(want) + ", got " + type_name(got));
        }
        Ty vty = want.k != Ty::Dyn ? want : got;
        if (cur_block_lets && !s.a.empty()) cur_block_lets->insert(s.a);
        env[s.a] = vty;
        if (s.ann && want.k != Ty::Dyn) annotated_vars.insert(s.a);
        else annotated_vars.erase(s.a);
        active_tags.erase(s.a);
        std::set<std::string> ms;
        mentions(*s.e1, ms);
        bool outer_alias = false;
        int alias_decl = current_region();
        for (const auto &n : ms) {
            if (n != s.a && !inner(n) && is_non_scalar_var(n)) {
                outer_alias = true;
                alias_decl = std::min(alias_decl, decl_region(n));
            }
        }
        int er = expr_region(*s.e1);
        bool is_arena_ptr = current_region() != 0 && er > 0 && scalar(vty);
        if (is_arena_ptr) arena_ptrs.insert(s.a);
        else arena_ptrs.erase(s.a);
        if (!s.a.empty()) {
            decl_regions[s.a] = outer_alias ? alias_decl : current_region();
            regions[s.a] = (scalar(vty) && !is_arena_ptr) ? 0 : er;
            if (s.e1->k == EK::Closure && s.e1->fn) {
                auto sp = body_sink_params(s.e1->fn->body, s.e1->fn->params);
                if (!sp.empty()) closure_sinks[s.a] = std::move(sp);
                else closure_sinks.erase(s.a);
            } else {
                closure_sinks.erase(s.a);
            }
        }
        if (current_region() != 0) {
            for (const auto &n : ms) {
                if (is_tainted(n)) taint(s.a);
            }
            if (has_node(*s.e1, EK::Closure) && !non_scalar_inner(ms, 1).empty()) {
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
        active_tags.erase(target);
        int er = expr_region(*s.e1);
        if (current_region() != 0) {
            if (!inner(target) && escapes(*s.e1)) {
                arena_note("a value made inside the block is assigned to `" + target +
                           "`, which lives outside it");
                taint(target);
            } else if (inner(target) && er > val_region(target)) {
                regions[target] = er;
                if (scalar(env_get(target))) arena_ptrs.insert(target);
            }
            if (has_node(*s.e1, EK::Closure)) {
                std::set<std::string> ms;
                mentions(*s.e1, ms);
                if (!non_scalar_inner(ms, 1).empty()) {
                    arena_note("a closure inside the block captures a value made in it", false);
                }
            }
        }
        if (dot == std::string::npos && s.e1->k == EK::Closure && s.e1->fn) {
            auto sp = body_sink_params(s.e1->fn->body, s.e1->fn->params);
            if (!sp.empty()) closure_sinks[target].insert(sp.begin(), sp.end());
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
            if (it != env.end() && it->second.k != Ty::Dyn) {
                if (!annotated_vars.count(s.a) && it->second.k == Ty::None &&
                    got.k != Ty::None) {
                    it->second = got.k == Ty::Option ? got : Ty::dyn();
                } else if (!type_ok(it->second, got)) {
                    note("assign to `" + s.a + "`: want " + type_name(it->second) + ", got " +
                         type_name(got));
                } else if (!annotated_vars.count(s.a)) {
                    if (it->second.k == Ty::Result && got.k == Ty::Result) {
                        it->second = join_ty(it->second, got);
                    } else if (it->second.k == Ty::Option && got.k == Ty::Option) {
                        it->second = join_ty(it->second, got);
                    }
                }
            }
        }
        break;
    }
    case SK::SetIdx: {
        expr(*s.e1);
        expr(*s.e2);
        expr(*s.e3);
        std::string root = first_var(*s.e1);
        if (current_region() != 0 && !root.empty()) {
            if (!inner(root) && escapes(*s.e3)) {
                arena_note("a value made inside the block is stored into `" + root +
                           "`, which lives outside it");
                taint(root);
            } else if (inner(root)) {
                int er = expr_region(*s.e3);
                if (er > val_region(root)) regions[root] = er;
            }
        }
        break;
    }
    case SK::SetField: {
        Ty bt = expr(*s.e1);
        Ty got = expr(*s.e2);
        std::string root = first_var(*s.e1);
        if (current_region() != 0 && !root.empty()) {
            if (!inner(root) && escapes(*s.e2)) {
                arena_note("a value made inside the block is stored into `" + root +
                           "`, which lives outside it");
                taint(root);
            } else if (inner(root)) {
                int er = expr_region(*s.e2);
                if (er > val_region(root)) regions[root] = er;
            }
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
        if (escapes(*s.e1, 1)) arena_note("a value made inside the block is returned out of it");
        if (!type_ok(ret, got)) {
            note("return: want " + type_name(ret) + ", got " + type_name(got));
        }
        break;
    }
    case SK::Expr: {
        expr(*s.e1);
        const Expr &e = *s.e1;
        if (current_region() == 0) break;
        if (e.k == EK::Closure) {
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
        std::string tag_var, tag_name;
        std::string pred_var;
        bool has_tag_scope = false;
        std::optional<std::string> prev_tag;
        bool has_narrow = false;
        std::string narrow_var;
        Ty prev_narrow_ty;
        if (!is_arena_block(s)) {
            if (extract_tagof_eq(s.e1.get(), tag_var, tag_name)) {
                Ty scrut = env_get(tag_var);
                if (scrut.k == Ty::Enum && g_enums && g_enums->count(scrut.text)) {
                    const auto &ei = g_enums->at(scrut.text);
                    auto vit = ei.variants.find(tag_name);
                    if (vit == ei.variants.end()) {
                        note("enum `" + scrut.text + "` has no variant `" + tag_name + "`");
                    } else if (s.pat_arity >= 0 &&
                               static_cast<size_t>(s.pat_arity) != vit->second.field_tys.size()) {
                        note("variant `" + vit->second.orig_name + "` of `" + scrut.text +
                             "` wants " + std::to_string(vit->second.field_tys.size()) +
                             " field(s), got " + std::to_string(s.pat_arity));
                    }
                } else if (scrut.k == Ty::Option) {
                    if (tag_name != "some" && tag_name != "none") {
                        note("`" + type_name(scrut) + "` has no variant `" + tag_name + "`");
                    } else if (s.pat_arity >= 0) {
                        const size_t want_n = tag_name == "some" ? 1 : 0;
                        if (static_cast<size_t>(s.pat_arity) != want_n)
                            note("variant `" + std::string(tag_name == "some" ? "Some" : "None") +
                                 "` wants " + std::to_string(want_n) + " field(s), got " +
                                 std::to_string(s.pat_arity));
                    }
                } else if (scrut.k == Ty::Result) {
                    if (tag_name != "ok" && tag_name != "err") {
                        note("`" + type_name(scrut) + "` has no variant `" + tag_name + "`");
                    } else if (s.pat_arity >= 0 && s.pat_arity != 1) {
                        note("variant `" + std::string(tag_name == "ok" ? "Ok" : "Err") +
                             "` wants 1 field(s), got " + std::to_string(s.pat_arity));
                    }
                }
                auto ait = active_tags.find(tag_var);
                if (ait != active_tags.end()) prev_tag = ait->second;
                active_tags[tag_var] = tag_name;
                has_tag_scope = true;
            } else if (extract_pred_var(s.e1.get(), "is_err", pred_var)) {
                Ty scrut = env_get(pred_var);
                if (scrut.k == Ty::Result && !body_assigns(s.body, pred_var)) {
                    narrow_var = pred_var;
                    prev_narrow_ty = scrut;
                    env[pred_var] = Ty::result(Ty::dyn(), *scrut.inner2);
                    has_narrow = true;
                }
            } else if (extract_pred_var(s.e1.get(), "is_none", pred_var)) {
                Ty scrut = env_get(pred_var);
                if (scrut.k == Ty::Option && !body_assigns(s.body, pred_var)) {
                    narrow_var = pred_var;
                    prev_narrow_ty = scrut;
                    env[pred_var] = Ty::of(Ty::None);
                    has_narrow = true;
                }
            }
        }
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
        if (has_tag_scope) {
            if (prev_tag) active_tags[tag_var] = *prev_tag;
            else active_tags.erase(tag_var);
        }
        if (has_narrow) env[narrow_var] = prev_narrow_ty;
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
        declare(s.a, 0);
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
        Ty elem = Ty::dyn();
        if (is_tuple(st)) {
            elem = st.inner ? *st.inner : Ty::dyn();
        } else if (st.k == Ty::Str) {
            elem = Ty::of(Ty::Str);
        }
        int er = current_region() != 0
                     ? (st.k == Ty::Str ? current_region()
                                        : (!scalar(elem) ? expr_region(*s.e1) : 0))
                     : 0;
        declare(s.a, er);
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
        break;
    case SK::Fn:
    case SK::ExportFn:
        nested_fn(s);
        break;
    case SK::Impl:
    case SK::Struct:
    case SK::Extern:
    case SK::ExternStruct:
    case SK::PubUse: break;
    case SK::Try: {
        block(s.body);
        const bool has_bind = s.opt1 && !s.opt1->empty();
        bool had_outer = false;
        Ty old = Ty::dyn();
        if (has_bind) {
            auto it = env.find(*s.opt1);
            if (it != env.end()) {
                had_outer = true;
                old = it->second;
            }
            declare(*s.opt1);
            env[*s.opt1] = Ty::dyn();
        }
        block(s.body2);
        if (has_bind) {
            if (had_outer) env[*s.opt1] = old;
            else env.erase(*s.opt1);
        }
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

static std::string lower_tag_name(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

static std::string pad4_index(size_t idx) {
    std::string s = std::to_string(idx);
    if (s.size() < 4) s.insert(0, 4 - s.size(), '0');
    return s;
}

static void register_enum_ctors(const std::map<std::string, EnumInfo> &enums, Sigs &sigs) {
    std::map<std::string, std::string> ctor_owner;
    std::set<std::string> ambiguous;
    for (const auto &[ename, ei] : enums) {
        for (const auto &tag : ei.order) {
            auto vit = ei.variants.find(tag);
            if (vit == ei.variants.end()) continue;
            const std::string &vname = vit->second.orig_name;
            if (vname == "Ok" || vname == "Err" || vname == "Some" || vname == "None")
                continue;
            auto existing = sigs.find(vname);
            if (existing != sigs.end() && !existing->second.is_ctor) continue;
            auto oit = ctor_owner.find(vname);
            if (oit != ctor_owner.end() && oit->second != ename) {
                ambiguous.insert(vname);
                sigs.erase(vname);
                continue;
            }
            if (ambiguous.count(vname)) continue;
            ctor_owner[vname] = ename;
            FnSig cs;
            cs.is_ctor = true;
            cs.ret = Ty::enm(ename);
            cs.ret_ann = ename;
            cs.params = vit->second.field_tys;
            cs.param_anns = vit->second.field_anns;
            sigs[vname] = std::move(cs);
        }
    }
}

void collect_exports(const std::vector<StmtP> &prog,
                     std::map<std::string, Ty> &consts,
                     std::map<std::string, std::map<std::string, Ty>> &structs) {
    std::set<std::string> union_names;
    std::set<std::string> native_names;
    std::map<std::string, EnumInfo> pub_enums;
    ScopedTypeTables tables_guard(&structs, &union_names, &native_names, &pub_enums);
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if (node.k == SK::ExternStruct) {
            structs[node.a];
            if (node.opt1 && *node.opt1 == "union") {
                union_names.insert(node.a);
                structs["__unions__"][node.a] = Ty::dyn();
            }
        } else if (node.k == SK::Struct && sp->k == SK::Pub) {
            structs[node.a];
            native_names.insert(node.a);
            structs["__native__"][node.a] = Ty::dyn();
        } else if (node.k == SK::Enum && sp->k == SK::Pub) {
            pub_enums[node.a];
            structs["__enums__"][node.a] = Ty::dyn();
        }
    }
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if (node.k == SK::ExternStruct || (node.k == SK::Struct && sp->k == SK::Pub)) {
            for (const auto &f : node.fields) structs[node.a][f.name] = ann_type(f.ann);
        } else if (node.k == SK::Enum && sp->k == SK::Pub) {
            EnumInfo &ei = pub_enums[node.a];
            ei.order.clear();
            ei.variants.clear();
            for (size_t vi = 0; vi < node.variants.size(); vi++) {
                const auto &v = node.variants[vi];
                std::string tag = (v.name == "None" && v.fields.empty())
                                      ? "none"
                                      : lower_tag_name(v.name);
                EnumVariantInfo evi;
                evi.orig_name = v.name;
                evi.field_anns = v.field_anns;
                std::string key = "__enum_var__:" + node.a + ":" + pad4_index(vi) + ":" + tag;
                structs[key]["__orig__"] = Ty::other(v.name);
                structs[key]["__arity__"] = Ty::cb(static_cast<int>(v.fields.size()), "");
                for (size_t fi = 0; fi < v.fields.size(); fi++) {
                    std::optional<std::string> fa =
                        fi < v.field_anns.size() ? v.field_anns[fi] : std::nullopt;
                    Ty fty = ann_type(fa);
                    evi.field_tys.push_back(fty);
                    structs[key]["f" + std::to_string(fi)] = fty;
                    structs[key]["a" + std::to_string(fi)] = Ty::other(fa.value_or(""));
                }
                ei.order.push_back(tag);
                ei.variants[tag] = std::move(evi);
            }
        }
    }
    std::vector<TypeErr> dummy;
    Sigs probe_sigs;
    register_enum_ctors(pub_enums, probe_sigs);
    FnTyper probe(probe_sigs, Ty::dyn(), dummy);
    probe.env = consts;
    for (const auto &sp : prog) {
        if (sp->k != SK::Pub || sp->body.empty()) continue;
        const Stmt &in = *sp->body[0];
        if ((in.k == SK::Let || in.k == SK::LetTmp) && in.konst) {
            Ty want = ann_type(in.ann);
            Ty got = in.e1 ? probe.expr(*in.e1) : Ty::dyn();
            Ty cty = want.k != Ty::Dyn ? want : got;
            consts[in.a] = cty;
            probe.env[in.a] = cty;
        }
    }
}

std::vector<TypeErr> type_check(const std::vector<StmtP> &prog, const Sigs &extra,
                                const std::map<std::string, Ty> &extra_consts,
                                const std::map<std::string, std::map<std::string, Ty>> &extra_structs) {
    std::vector<TypeErr> errs;
    Sigs sigs = extra;
    std::map<std::string, std::map<std::string, Ty>> structs = extra_structs;
    std::set<std::string> unions;
    std::set<std::string> native_structs;
    std::map<std::string, EnumInfo> enums;
    if (auto uit = structs.find("__unions__"); uit != structs.end()) {
        for (const auto &kv : uit->second) unions.insert(kv.first);
        structs.erase(uit);
    }
    if (auto nit = structs.find("__native__"); nit != structs.end()) {
        for (const auto &kv : nit->second) native_structs.insert(kv.first);
        structs.erase(nit);
    }
    if (auto eit = structs.find("__enums__"); eit != structs.end()) {
        for (const auto &kv : eit->second) enums[kv.first];
        structs.erase(eit);
    }
    for (auto it = structs.begin(); it != structs.end();) {
        if (it->first.rfind("__enum_var__:", 0) == 0) {
            const std::string rest = it->first.substr(13);
            size_t c1 = rest.find(':');
            size_t c2 = c1 == std::string::npos ? std::string::npos : rest.find(':', c1 + 1);
            if (c1 != std::string::npos && c2 != std::string::npos) {
                std::string ename = rest.substr(0, c1);
                std::string tag = rest.substr(c2 + 1);
                EnumVariantInfo evi;
                auto oit = it->second.find("__orig__");
                evi.orig_name = oit != it->second.end() ? oit->second.text : tag;
                auto ait = it->second.find("__arity__");
                int arity = ait != it->second.end() ? ait->second.arity : 0;
                for (int fi = 0; fi < arity; fi++) {
                    auto raw_it = it->second.find("a" + std::to_string(fi));
                    std::optional<std::string> fa;
                    if (raw_it != it->second.end() && !raw_it->second.text.empty())
                        fa = raw_it->second.text;
                    evi.field_anns.push_back(fa);
                    auto fit = it->second.find("f" + std::to_string(fi));
                    evi.field_tys.push_back(fit != it->second.end() ? fit->second : Ty::dyn());
                }
                enums[ename].order.push_back(tag);
                enums[ename].variants[tag] = std::move(evi);
            }
            it = structs.erase(it);
        } else {
            ++it;
        }
    }
    ScopedTypeTables tables_guard(&structs, &unions, &native_structs, &enums);
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if (node.k == SK::ExternStruct) {
            structs[node.a];
            if (node.opt1 && *node.opt1 == "union") unions.insert(node.a);
        } else if (node.k == SK::Struct) {
            structs[node.a];
            native_structs.insert(node.a);
        } else if (node.k == SK::Enum) {
            enums[node.a];
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        for (const auto &sp : prog) {
            const Stmt &node = unpub(*sp);
            if (node.k == SK::ExternStruct || node.k == SK::Struct) {
                for (const auto &f : node.fields) structs[node.a][f.name] = ann_type(f.ann);
            } else if (node.k == SK::Enum) {
                EnumInfo &ei = enums[node.a];
                ei.order.clear();
                ei.variants.clear();
                for (const auto &v : node.variants) {
                    std::string tag = (v.name == "None" && v.fields.empty())
                                          ? "none"
                                          : lower_tag_name(v.name);
                    EnumVariantInfo evi;
                    evi.orig_name = v.name;
                    evi.field_anns = v.field_anns;
                    for (size_t fi = 0; fi < v.fields.size(); fi++) {
                        std::optional<std::string> fa =
                            fi < v.field_anns.size() ? v.field_anns[fi] : std::nullopt;
                        evi.field_tys.push_back(ann_type(fa));
                    }
                    ei.order.push_back(tag);
                    ei.variants[tag] = std::move(evi);
                }
            }
        }
        for (auto &[ename, ei] : enums) {
            for (auto &[tag, evi] : ei.variants) {
                for (size_t fi = 0; fi < evi.field_anns.size() && fi < evi.field_tys.size(); fi++) {
                    if (evi.field_anns[fi]) evi.field_tys[fi] = ann_type(evi.field_anns[fi]);
                }
            }
        }
    }
    collect_sigs(prog, sigs);
    for (auto &[fname, f] : sigs) {
        if (f.is_ctor && fname.rfind("__static__.", 0) != 0) continue;
        for (size_t k = 0; k < f.params.size() && k < f.param_anns.size(); k++) {
            if (f.param_anns[k]) f.params[k] = ann_type(*f.param_anns[k]);
        }
        if (f.ret_ann) f.ret = ann_type(*f.ret_ann);
        if (f.varargs && !f.varargs_ann.empty()) f.varargs = ann_type(f.varargs_ann);
    }
    register_enum_ctors(enums, sigs);
    std::set<std::string> outer_globals;
    for (const auto &kv : extra_consts) outer_globals.insert(kv.first);
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if ((node.k == SK::Let || node.k == SK::LetTmp) && !node.a.empty()) {
            outer_globals.insert(node.a);
        }
    }
    std::map<std::string, Ty> const_env = extra_consts;
    {
        std::vector<TypeErr> dummy;
        FnTyper cprobe(sigs, Ty::dyn(), dummy);
        cprobe.env = const_env;
        for (const auto &sp : prog) {
            const Stmt &node = unpub(*sp);
            if ((node.k == SK::Let || node.k == SK::LetTmp) && node.konst) {
                Ty want = ann_type(node.ann);
                Ty got = node.e1 ? cprobe.expr(*node.e1) : Ty::dyn();
                Ty cty = want.k != Ty::Dyn ? want : got;
                const_env[node.a] = cty;
                cprobe.env[node.a] = cty;
            }
        }
    }
    auto walk_union_expr = [&](auto &self, auto &walk_s, const ExprP &ep, FnTyper &tp) -> void {
        if (!ep) return;
        if (ep->k == EK::StructLit && unions.count(ep->a)) {
            tp.expr(*ep);
            return;
        }
        for (const auto &k : ep->kids) self(self, walk_s, k, tp);
        if (ep->fn) walk_s(walk_s, ep->fn->body, tp);
    };
    auto walk_union_stmt = [&](auto &self_st, auto &walk_s, const Stmt &st, FnTyper &tp) -> void {
        if (st.line) tp.line = st.line;
        walk_union_expr(walk_union_expr, walk_s, st.e1, tp);
        walk_union_expr(walk_union_expr, walk_s, st.e2, tp);
        walk_union_expr(walk_union_expr, walk_s, st.e3, tp);
        walk_s(walk_s, st.body, tp);
        walk_s(walk_s, st.body2, tp);
        if (st.els) self_st(self_st, walk_s, *st.els, tp);
        for (const auto &fd : st.fns) walk_s(walk_s, fd.body, tp);
    };
    auto walk_union_stmts = [&](auto &self, const std::vector<StmtP> &stmts, FnTyper &tp) -> void {
        for (const auto &st : stmts) {
            if (st) walk_union_stmt(walk_union_stmt, self, *st, tp);
        }
    };
    for (const auto &sp : prog) {
        const Stmt &node = unpub(*sp);
        if (node.k == SK::Impl) {
            const bool is_native_impl = native_structs.count(node.a) > 0;
            const bool is_enum_impl = enums.count(node.a) > 0;
            for (const auto &m : node.fns) {
                Ty mret = ann_type(m.ret);
                FnTyper tp(sigs, mret, errs);
                tp.env = const_env;
                tp.outer_globals = outer_globals;
                bool has_typed_param = false;
                for (const auto &p : m.params) {
                    if ((is_native_impl || is_enum_impl) && p.name == "self" && !p.ann) {
                        tp.env[p.name] = is_enum_impl ? Ty::enm(node.a) : word_type(node.a);
                        tp.annotated_vars.insert(p.name);
                        has_typed_param = true;
                    } else {
                        Ty pt = ann_type(p.ann);
                        tp.env[p.name] = pt;
                        if (p.ann && pt.k != Ty::Dyn) {
                            tp.annotated_vars.insert(p.name);
                            has_typed_param = true;
                        }
                    }
                }
                if (!unions.empty()) walk_union_stmts(walk_union_stmts, m.body, tp);
                if ((is_native_impl || is_enum_impl) && (mret.k != Ty::Dyn || has_typed_param)) {
                    tp.block(m.body);
                } else {
                    tp.arity_and_arena = true;
                    tp.block(m.body);
                    tp.arity_and_arena = false;
                }
            }
            continue;
        }
        if (!is_fn_like(node) || node.k == SK::Extern) continue;
        Ty ret = fn_ret(node);
        FnTyper tp(sigs, ret, errs);
        tp.env = const_env;
        tp.outer_globals = outer_globals;
        bool has_typed_param = false;
        for (const auto &p : node.fns[0].params) {
            Ty pt = ann_type(p.ann);
            tp.env[p.name] = pt;
            if (p.ann && pt.k != Ty::Dyn) {
                tp.annotated_vars.insert(p.name);
                has_typed_param = true;
            }
        }
        if (ret.k == Ty::Dyn && !has_typed_param) {
            if (!unions.empty()) walk_union_stmts(walk_union_stmts, node.fns[0].body, tp);
            tp.arity_and_arena = true;
            tp.block(node.fns[0].body);
            tp.arity_and_arena = false;
            continue;
        }
        tp.block(node.fns[0].body);
    }
    FnTyper top(sigs, Ty::dyn(), errs);
    top.env = const_env;
    top.outer_globals = outer_globals;
    for (size_t idx = 0; idx < prog.size(); idx++) {
        const auto &sp = prog[idx];
        const Stmt &node = unpub(*sp);
        if (node.k == SK::Fn || node.k == SK::ExportFn || node.k == SK::Impl ||
            node.k == SK::Trait || node.k == SK::Enum || node.k == SK::Struct ||
            node.k == SK::Extern) {
            continue;
        }
        top.stmt(*sp);
        top.check_match_exhaustive(prog, idx);
    }
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
